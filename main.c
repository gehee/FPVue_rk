#define MODULE_TAG "fpvue"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <rockchip/rk_mpi.h>
#include <semaphore.h>

#include "main.h"
#include "drm.h"
#include "osd.h"
#include "rtp.h"

#include "mavlink/common/mavlink.h"
#include "mavlink.h"

#include "time_util.h"

#define READ_BUF_SIZE (1024*1024) // SZ_1M https://github.com/rockchip-linux/mpp/blob/ed377c99a733e2cdbcc457a6aa3f0fcd438a9dff/osal/inc/mpp_common.h#L179
#define MAX_FRAMES 24		// min 16 and 20+ recommended (mpp/readme.txt)

#define CODEC_ALIGN(x, a)   (((x)+(a)-1)&~((a)-1))

pthread_t tid_udp, tid_frame, tid_display, tid_osd, tid_mavlink;

pthread_mutex_t video_mutex;
pthread_cond_t video_cond;

struct {
	MppCtx		  ctx;
	MppApi		  *mpi;
	
	struct timespec first_frame_ts;

	MppBufferGroup	frm_grp;
	struct {
		int prime_fd;
		int fb_id;
		uint32_t handle;
        // only used in copy mode
        void* memory_mmap;
        int memory_mmap_size;
	} frame_to_drm[MAX_FRAMES];
} mpi;

struct timespec frame_stats[1000];
struct timespec frame_stats_byfd[1000];

struct modeset_output *output;
int frm_eos = 0;
int drm_fd = 0;

// OSD Vars
struct video_stats {
	int current_framerate;
	uint64_t current_latency;
	uint64_t max_latency;
	uint64_t min_latency;
};
struct video_stats osd_stats;
int bw_curr = 0;
long long bw_stats[10];


// 'Live buffer hack' originally created by https://github.com/Consti10
void initialize_output_buffers_ion(MppFrame  frame){

    int ret;
    int i;
    // new resolution
    assert(!mpi.frm_grp);

    output->video_frm_width = CODEC_ALIGN(mpp_frame_get_width(frame),16);
    output->video_frm_height = CODEC_ALIGN(mpp_frame_get_height(frame),16);
    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    assert((fmt == MPP_FMT_YUV420SP) || (fmt == MPP_FMT_YUV420SP_10BIT));

    printf("Frame info changed %d(%d)x%d(%d)\n", output->video_frm_width, hor_stride, output->video_frm_height, ver_stride);

    output->video_fb_x = 0;
    output->video_fb_y = 0;
    output->video_fb_width = output->mode.hdisplay;
    output->video_fb_height =output->mode.vdisplay;

    osd_vars.video_width = output->video_frm_width;
    osd_vars.video_height = output->video_frm_height;
    // create new external frame group and allocate (commit flow) new DRM buffers and DRM FB
    assert(!mpi.frm_grp);
    ret = mpp_buffer_group_get_external(&mpi.frm_grp,  MPP_BUFFER_TYPE_ION);
    assert(!ret);

    int lol_width=0;
    int lol_height=0;
    // Specify how many actual buffer(s) to create
    int n_drm_prime_buffers=1;
    for(i=0;i<n_drm_prime_buffers;i++){
        // new DRM buffer
        struct drm_mode_create_dumb dmcd;
        memset(&dmcd, 0, sizeof(dmcd));
        dmcd.bpp = fmt==MPP_FMT_YUV420SP?8:10;
        dmcd.width = hor_stride;
        dmcd.height = ver_stride*2; // documentation say not v*2/3 but v*2 (additional info included)
        do {
            ret = ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        assert(!ret);
        //assert(dmcd.pitch==(fmt==MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8));
        //assert(dmcd.size==(fmt == MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8)*ver_stride*2);
        mpi.frame_to_drm[i].handle = dmcd.handle;

        // commit DRM buffer to frame group
        struct drm_prime_handle dph;
        memset(&dph, 0, sizeof(struct drm_prime_handle));
        dph.handle = dmcd.handle;
        dph.fd = -1;
        dph.flags  = DRM_CLOEXEC | DRM_RDWR;
        do {
            ret = ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        assert(!ret);
        //
        void * primed_framebuffer=mmap(
                0, dmcd.size,    PROT_READ | PROT_WRITE, MAP_SHARED,
                dph.fd, 0);
        if (primed_framebuffer == NULL || primed_framebuffer == MAP_FAILED) {
            printf(
                    "Could not map buffer exported through PRIME : %s (%d)\n"
                    "Buffer : %p\n",
                    strerror(ret), ret,
                    primed_framebuffer
            );
            assert(false);
        }
        mpi.frame_to_drm[i].memory_mmap=primed_framebuffer;
        //
        mpi.frame_to_drm[i].prime_fd = dph.fd; // dups fd
        lol_width=dmcd.width;
        lol_height=dmcd.height;
        // allocate DRM FB from DRM buffer
        uint32_t handles[4], pitches[4], offsets[4];
        memset(handles, 0, sizeof(handles));
        memset(pitches, 0, sizeof(pitches));
        memset(offsets, 0, sizeof(offsets));
        handles[0] = mpi.frame_to_drm[i].handle;
        offsets[0] = 0;
        pitches[0] = hor_stride;
        handles[1] = mpi.frame_to_drm[i].handle;
        offsets[1] = pitches[0] * ver_stride;
        pitches[1] = pitches[0];

        ret = drmModeAddFB2(drm_fd, output->video_frm_width, output->video_frm_height, DRM_FORMAT_NV12, handles, pitches, offsets, (uint32_t*)&mpi.frame_to_drm[i].fb_id, 0);
        assert(!ret);
    }

	// Can be different than n drm prime buffers.
	// If for example only one drm prime buffer was created, we pass the same buffer fd to mpp on each mpp buffer.
	for (i=0; i<16; i++) {
		MppBufferInfo info;
		memset(&info, 0, sizeof(info));
		info.type =  MPP_BUFFER_TYPE_ION;
		info.size = lol_width*lol_height;
		info.index = i;
		// We pass the same buffer multiple time(s) if needed
		int buffer_index = i % n_drm_prime_buffers;
		int this_drm_prime_fd=mpi.frame_to_drm[buffer_index].prime_fd;
		info.fd = this_drm_prime_fd;
		ret = mpp_buffer_commit(mpi.frm_grp, &info);
		assert(!ret);
		if (this_drm_prime_fd != info.fd) {
			// I have no idea why this happens ...
			printf("mpp changed buffer fd from %d to %d\n",this_drm_prime_fd,info.fd);
		}
	}


    // register external frame group
    ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
    ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

	output->video_cur_fb_id = mpi.frame_to_drm[0].fb_id;
}

int displayed_frames = 0;
uint64_t displayed_frames_start = 0;
uint64_t last_vblank = 0;
static sem_t vblank_start;
static sem_t vblank_end;

int init_done = 0;
int decoding = 0;
int got_frame = 0;

static void modeset_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    unsigned int crtc_id, void *data) {
	displayed_frames++;
	printf("VBLANK START\n");
	sem_post(&vblank_start);
	if (decoding == 1) {
	  sem_wait(&vblank_end);
	}
	printf("VBLANK END\n");
	int flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
	int ret = drmModeAtomicCommit(fd,  output->request, flags, NULL);
	if (ret < 0) {
		fprintf(stderr, "atomic commit failed, %m\n", errno);
		//return;
	}
	output->pflip_pending = true;
	uint64_t now = get_time_ms();
	if ((now - displayed_frames_start) > 1000) {
		printf("Feeding screen @ %d fps\n",displayed_frames);
		displayed_frames = 0;
		displayed_frames_start = now;
	}
}

// signal

int signal_flag = 0;

void sig_handler(int signum)
{
	printf("Received signal %d\n", signum);
	signal_flag++;
	mavlink_thread_signal++;
	osd_thread_signal++;
}

void *__DISPLAY_THREAD__(void *param)
{
	int ret;
	fd_set fds;
	time_t start, cur;
	drmEventContext ev;

	srand(time(&start));
	FD_ZERO(&fds);
	memset(&ev, 0, sizeof(ev));

	ev.version = 3;
	ev.page_flip_handler2 = modeset_page_flip_event;

	ret = modeset_perform_modeset(drm_fd, output);
	assert(ret >= 0);

	/* wait for VBLANK or input events */
	while (!signal_flag ) {
		FD_SET(0, &fds);
		FD_SET(drm_fd, &fds);

		ret = select(drm_fd + 1, &fds, NULL, NULL, NULL);
		if (ret < 0) {
			fprintf(stderr, "select() failed with %d: %m\n", errno);
			break;
		} else if (FD_ISSET(0, &fds)) {
			fprintf(stderr, "exit due to user-input\n");
			break;
		} else if (FD_ISSET(drm_fd, &fds)) {
			/* read the fd looking for events and handle each event
			 * by calling modeset_page_flip_event() */
			drmHandleEvent(drm_fd, &ev);
		}
	}
	printf("Display thread done.\n");
}



// __FRAME_THREAD__
//
// - allocate DRM buffers and DRM FB based on frame size
// - pick frame in blocking mode and output to screen overlay

void *__FRAME_THREAD__(void *param)
{
	int ret;
	int i;	
	MppFrame  frame  = NULL;
	int decoded_frames = 0;
	uint64_t decoded_frames_start = 0;

	while (!frm_eos) {
		struct timespec ts, ats;
		
		assert(!frame);
		ret = mpi.mpi->decode_get_frame(mpi.ctx, &frame);
		assert(!ret);
		clock_gettime(CLOCK_MONOTONIC, &ats);
		if (frame) {
			int poc = mpp_frame_get_poc(frame);
			printf("%u: DEC decodeded poc=%d\n", get_time_ms(), poc);
			decoding = 1;
			sem_post(&vblank_end);
			got_frame = 1;
			if (mpp_frame_get_info_change(frame)) {
				// new resolution
				initialize_output_buffers_ion(frame);
				ret = pthread_create(&tid_display, NULL, __DISPLAY_THREAD__, NULL);
				assert(!ret);
				init_done = 1;
			} else {
				// regular frame received
				if (!mpi.first_frame_ts.tv_sec) {
					ts = ats;
					mpi.first_frame_ts = ats;
				}
				MppBuffer buffer = mpp_frame_get_buffer(frame);
			}
			frm_eos = mpp_frame_get_eos(frame);
			mpp_frame_deinit(&frame);
			frame = NULL;

			decoded_frames++;
			uint64_t now = get_time_ms();
			if ((now - decoded_frames_start) > 1000) {
				printf("Decoding frames @ %d fps\n",decoded_frames);
				decoded_frames = 0;
				decoded_frames_start = now;
			}
		} else assert(0);
	}
	printf("Frame thread done.\n");
}


int enable_dvr = 0;
char * dvr_file;

typedef struct {
	MppPacket *packet;
	int port;
	uint8_t* nal_buffer;
} udp_thread_params;

// __UDP_THREAD__
void *__UDP_THREAD__(void *param) {
	udp_thread_params *p = param;
	int port = p->port;
	MppPacket *packet = p->packet;
	uint8_t* nal_buffer = p->nal_buffer;
	// Create socket
  	int socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in address;
	memset(&address, 0x00, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	bind(socketFd, (struct sockaddr*)&address, sizeof(struct sockaddr_in));

	if (fcntl(socketFd, F_SETFL, O_NONBLOCK) == -1) {
		printf("ERROR: Unable to set non-blocking mode\n");
		return nullptr;
	}

	printf("listening on socket %d\n", port);

	uint8_t* rx_buffer = malloc(1024 * 1024);
    
	int nalStart = 0;
	int poc = 0;
	int ret = 0;
	struct timespec recv_ts;
	long long bytesReceived = 0; 
	uint8_t* nal;

	FILE *out_h265 = NULL;
	if (enable_dvr) {
		if ((out_h265 = fopen(dvr_file,"w")) == NULL){
			printf("ERROR: unable to open %s\n", dvr_file);
		}
	}

	sem_init(&vblank_start, 1, 1); // Initial value 1, indicating the semaphore is available
	sem_init(&vblank_end, 1, 1);

	struct timespec bw_start, bw_end;
	clock_gettime(CLOCK_MONOTONIC, &bw_start);
	while (!signal_flag) {
		ssize_t rx = recv(socketFd, rx_buffer+8, 4096, 0);
		clock_gettime(CLOCK_MONOTONIC, &bw_end);
		uint64_t time_us=(bw_end.tv_sec - bw_start.tv_sec)*1000000ll + ((bw_end.tv_nsec - bw_start.tv_nsec)/1000ll) % 1000000ll;
		if (rx > 0) {
			bytesReceived += rx;
		}
		if (time_us >= 1000000) {
			bw_start = bw_end;
			osd_vars.bw_curr = (osd_vars.bw_curr + 1) % 10;
			osd_vars.bw_stats[osd_vars.bw_curr] = bytesReceived;
			bytesReceived = 0;
		}
		if (rx <= 0) {
			usleep(1);
			continue;
		}
		if (nal) {
			clock_gettime(CLOCK_MONOTONIC, &recv_ts);
		}
		uint32_t rtp_header = 0;
		if (rx_buffer[8] & 0x80 && rx_buffer[9] & 0x60) {
			rtp_header = 12;
		}

		// Decode frame
		uint32_t nal_size = 0;
		nal = decode_frame(rx_buffer + 8, rx, rtp_header, nal_buffer, &nal_size);
		if (!nal) {
			continue;
		}

		if (nal_size < 5) {
			printf("> Broken frame\n");
			break;
		}

	
		uint8_t nal_type_hevc = (nal[4] >> 1) & 0x3F;
		if (nalStart==0 && nal_type_hevc == 1) { //hevc
			continue;
		}
		nalStart = 1;
		if (nal_type_hevc == 19) {
			poc = 0;
		}
		frame_stats[poc]=recv_ts;

		mpp_packet_set_pos(packet, nal); // only needed once
		mpp_packet_set_length(packet, nal_size);
		// send packet until it succeeds

		// if (has_lock == 0) {
		// 	printf("DEC locking...\n");
		// 	pthread_mutex_lock(&mutex);
		// 	printf("DEC locked.\n");
		// 	has_lock = 1;
		// } else {
		// 	printf("DEC still locked.\n");
		// }
		if (init_done==1 && got_frame == 1){
			// sync on vblank
			//printf("%u: gonna wait\n", get_time_ms());
			// pthread_mutex_lock(&mutex);
			// while (!ready) { // Wait until ready becomes true
			// 	pthread_cond_wait(&cond, &mutex); // Release mutex and wait
			// }
			// ready = 0;
			// //printf("%u: ready=0...\n", get_time_ms());
			// pthread_mutex_unlock(&mutex);


			sem_wait(&vblank_start);
			got_frame = 0;
		}
		printf("%u: decode_put_packet poc=%d\n", get_time_ms(), poc);
		while (!signal_flag && MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
				//printf("decode_put_packet failure=%m\n", errno);
				usleep(1000);
		}
		poc ++;
		if (out_h265 != NULL) {
			fwrite(nal, nal_size, 1, out_h265);
		}
	};
	mpp_packet_set_eos(packet);
	mpp_packet_set_pos(packet, nal_buffer);
	mpp_packet_set_length(packet, 0);
	while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
		usleep(10000);
	}

	if (out_h265 != NULL) {
		fclose(out_h265);
	}
}

void printHelp() {
  printf(
    "\n\t\tFPVue FPV Decoder for Rockchip (%s)\n"
    "\n"
    "  Usage:\n"
    "    fpvue [Arguments]\n"
    "\n"
    "  Arguments:\n"
    "    -p [Port]      - Listen port                            (Default: 5600)\n"
    "\n"
    "    --osd          - Enable OSD and specifies its element   (Default: video,wfbng)\n"
    "\n"
    "    --dvr          - Save the video feed (no osd) to the provided filename\n"
    "\n"
    "    --screen-mode      - Override default screen mode. ex:1920x1080@120\n"
    "\n", __DATE__
  );
}

// main

int main(int argc, char **argv)
{
	int ret;	
	int i, j;
	int enable_mavlink = 0;
	uint16_t listen_port = 5600;
	uint16_t mavlink_port = 14550;
	uint16_t mode_width = 0;
	uint16_t mode_height = 0;
	uint32_t mode_vrefresh = 0;

	// Load console arguments
	__BeginParseConsoleArguments__(printHelp) 
	
	__OnArgument("-p") {
		listen_port = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--osd") {
		osd_vars.enable = 1;
		char* elements = __ArgValue;
		if (!strcmp(elements, "")) {
			osd_vars.enable_video = 1;
			osd_vars.enable_wfbng = 1;
			enable_mavlink = 1;
		} else {
			char * element = strtok(elements, ",");
			while( element != NULL ) {
				if (!strcmp(element, "video")) {
					osd_vars.enable_video = 1;
				} else if (!strcmp(element, "wfbng")) {
					osd_vars.enable_wfbng = 1;
					enable_mavlink = 1;
				} else if (!strcmp(element, "framecounter")) {
					osd_vars.enable_frame_counter = 1;
				}
				element = strtok(NULL, ",");
			}
		}
		init_icons();
		continue;
	}

	__OnArgument("--mavlink-port") {
		mavlink_port = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--dvr") {
		enable_dvr = 1;
		dvr_file = __ArgValue;
		continue;
	}

	__OnArgument("--screen-mode") {
		char* mode = __ArgValue;
		mode_width = atoi(strtok(mode, "x"));
		mode_height = atoi(strtok(NULL, "@"));
		mode_vrefresh = atoi(strtok(NULL, "@"));
		continue;
	}

	__EndParseConsoleArguments__
		
	MppCodingType mpp_type = MPP_VIDEO_CodingHEVC;
	ret = mpp_check_support_format(MPP_CTX_DEC, mpp_type);
	assert(!ret);

	////////////////////////////////// SIGNAL SETUP

	signal(SIGINT, sig_handler);
	signal(SIGPIPE, sig_handler);
	
	//////////////////////////////////  DRM SETUP
	ret = modeset_open(&drm_fd, "/dev/dri/card0");
	if (ret < 0) {
		printf("modeset_open() =  %d\n", ret);
	}
	assert(drm_fd >= 0);

	output = (struct modeset_output *)malloc(sizeof(struct modeset_output));

	output->request = drmModeAtomicAlloc();
	assert(output->request);
	ret = modeset_prepare(drm_fd, output, mode_width, mode_height, mode_vrefresh);
	assert(!ret);
	
	////////////////////////////////// MPI SETUP
	MppPacket packet;

	uint8_t* nal_buffer = malloc(1024 * 1024);
	assert(nal_buffer);
	ret = mpp_packet_init(&packet, nal_buffer, READ_BUF_SIZE);
	assert(!ret);

	ret = mpp_create(&mpi.ctx, &mpi.mpi);
	assert(!ret);

	ret = mpp_init(mpi.ctx, MPP_CTX_DEC, mpp_type);
	assert(!ret);

	// blocked/wait read of frame in thread
	int param = MPP_POLL_BLOCK;
	ret = mpi.mpi->control(mpi.ctx, MPP_SET_OUTPUT_BLOCK, &param);
	assert(!ret);

 	//////////////////// THREADS SETUP
	ret = pthread_create(&tid_frame, NULL, __FRAME_THREAD__, NULL);
	assert(!ret);
	if (osd_vars.enable) {
		ret = pthread_create(&tid_osd, NULL, __OSD_THREAD__, output);
		assert(!ret);
	}
	if (osd_vars.enable && enable_mavlink) {
		ret = pthread_create(&tid_mavlink, NULL, __MAVLINK_THREAD__, NULL);
		assert(!ret);
	}

	udp_thread_params *args = malloc(sizeof *args);
    args->port = listen_port;
    args->packet = packet;
	args->nal_buffer = nal_buffer;
	ret = pthread_create(&tid_udp, NULL, __UDP_THREAD__, args);
	assert(!ret);

	////////////////////////////////////////////// MPI CLEANUP

	ret = pthread_join(tid_udp, NULL);
	assert(!ret);

	ret = pthread_join(tid_frame, NULL);
	assert(!ret);

	if (tid_display > 0) {
		ret = pthread_join(tid_display, NULL);
		assert(!ret);
	}	

	if (enable_mavlink) {
		ret = pthread_join(tid_mavlink, NULL);
		assert(!ret);
	}

	if (osd_vars.enable) {
		ret = pthread_join(tid_osd, NULL);
		assert(!ret);
	}

	ret = mpi.mpi->reset(mpi.ctx);
	assert(!ret);

	if (mpi.frm_grp) {
		ret = mpp_buffer_group_put(mpi.frm_grp);
		assert(!ret);
		mpi.frm_grp = NULL;
		for (i=0; i<MAX_FRAMES; i++) {
			ret = drmModeRmFB(drm_fd, mpi.frame_to_drm[i].fb_id);
			assert(!ret);
			struct drm_mode_destroy_dumb dmdd;
			memset(&dmdd, 0, sizeof(dmdd));
			dmdd.handle = mpi.frame_to_drm[i].handle;
			do {
				ret = ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdd);
			} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
			assert(!ret);
		}
	}
		
	mpp_packet_deinit(&packet);
	mpp_destroy(mpi.ctx);
	free(nal_buffer);
	
	////////////////////////////////////////////// DRM CLEANUP
	restore_planes_zpos(drm_fd, output);
	drmModeSetCrtc(drm_fd,
			       output->saved_crtc->crtc_id,
			       output->saved_crtc->buffer_id,
			       output->saved_crtc->x,
			       output->saved_crtc->y,
			       &output->connector.id,
			       1,
			       &output->saved_crtc->mode);
	drmModeFreeCrtc(output->saved_crtc);
	drmModeAtomicFree(output->request);
	modeset_cleanup(drm_fd, output);
	close(drm_fd);

	return 0;
}