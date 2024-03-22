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

#include "main.h"
#include "drm.h"
#include "osd.h"
#include "rtp.h"

#include "mavlink/common/mavlink.h"
#include "mavlink.h"

#define READ_BUF_SIZE (1024*1024) // SZ_1M https://github.com/rockchip-linux/mpp/blob/ed377c99a733e2cdbcc457a6aa3f0fcd438a9dff/osal/inc/mpp_common.h#L179
#define MAX_FRAMES 24		// min 16 and 20+ recommended (mpp/readme.txt)

#define CODEC_ALIGN(x, a)   (((x)+(a)-1)&~((a)-1))

pthread_t tid_frame, tid_display, tid_osd, tid_mavlink;

struct {
	MppCtx		  ctx;
	MppApi		  *mpi;
	
	struct timespec first_frame_ts;

	MppBufferGroup	frm_grp;
	struct {
		int prime_fd;
		int fb_id;
		uint32_t handle;
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


/*
 * modeset_page_flip_event() changes. Now that we are using page_flip_handler2,
 * we also receive the CRTC that is responsible for this event. When using the
 * atomic API we commit multiple CRTC's at once, so we need the information of
 * what output caused the event in order to schedule a new page-flip for it.
 */


int last_video_fb = 0;

int displayed_frames = 0;
uint64_t latency_avg[200];
struct timespec osd_vars_start, osd_vars_end;

static void modeset_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    unsigned int crtc_id, void *data)
{
	output->pflip_pending = false;
	int ret;

	// Update OSD vars
	if (last_video_fb != output->video_cur_fb_id) {
		//printf("updating osd vars\n");
		displayed_frames++;
		clock_gettime(CLOCK_MONOTONIC, &osd_vars_end);
		uint64_t time_us = (osd_vars_end.tv_sec - osd_vars_start.tv_sec)*1000000ll + ((osd_vars_end.tv_nsec - osd_vars_start.tv_nsec)/1000ll) % 1000000ll;
		if (time_us >= 1000000) {
			uint64_t sum = 0;
			for (int i = 0; i < displayed_frames; ++i) {
				sum += latency_avg[i];
			}
			osd_vars.latency_avg = sum / (displayed_frames);
			osd_vars.current_framerate = displayed_frames;

		 	//printf("display latency=%.2f ms (%.2f, %.2f), framerate=%d fps\n", osd_vars.latency_avg/1000.0, osd_vars.latency_max/1000.0, osd_vars.latency_min/1000.0, displayed_frames);	
			
			osd_vars_start = osd_vars_end;
			displayed_frames = 0;
			osd_vars.latency_max = 0;
			osd_vars.latency_min = 1844674407370955161;
		}
		struct timespec rtime = frame_stats_byfd[output->video_cur_fb_id];
		uint64_t ltc = (osd_vars_end.tv_sec - rtime.tv_sec)*1000000ll + ((osd_vars_end.tv_nsec - rtime.tv_nsec)/1000ll) % 1000000ll;
		if(ltc < osd_vars.latency_min ){
			osd_vars.latency_min = ltc;
		}if(ltc > osd_vars.latency_max ){
			osd_vars.latency_max = ltc;
		}
		latency_avg[displayed_frames] = ltc;
	}

	drmModeAtomicSetCursor(output->request, 0);
	if (osd_vars.enable) {
		int osd_fb = output->osd_bufs[output->osd_buf_switch].fb;
		ret = set_drm_object_property(output->request, &output->osd_plane, "FB_ID", osd_fb);
		assert(ret>0);	
	}
	ret = set_drm_object_property(output->request, &output->video_plane, "FB_ID", output->video_cur_fb_id);
	assert(ret>0);
	
	int flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
	ret = drmModeAtomicCommit(fd,  output->request, flags, NULL);
	if (ret < 0) {
		fprintf(stderr, "atomic commit failed, %m\n", errno);
		return;
	}
	output->pflip_pending = true;
	last_video_fb=output->video_cur_fb_id;
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

	/* 3 is the first version that allow us to use page_flip_handler2, which
	 * is just like page_flip_handler but with the addition of passing the
	 * crtc_id as argument to the function that will handle page-flip events
	 * (in our case, modeset_page_flip_event()). This is good because we can
	 * find out for what output the page-flip happened.
	 *
	 * The usage of page_flip_handler2 is the reason why we needed to verify
	 * the support for DRM_CAP_CRTC_IN_VBLANK_EVENT.
	 */
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
	int decoded_frame = 0;
	uint64_t latency_avg[200];
	uint64_t min_latency = 1844674407370955161; // almost MAX_uint64_t
	uint64_t max_latency = 0;
	struct timespec fps_start, fps_end;

	while (!frm_eos) {
		struct timespec ts, ats;
		
		assert(!frame);
		ret = mpi.mpi->decode_get_frame(mpi.ctx, &frame);
		assert(!ret);
		clock_gettime(CLOCK_MONOTONIC, &ats);
		if (frame) {
			if (mpp_frame_get_info_change(frame)) {
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
				output->video_fb_width = output->video_crtc_width;
				output->video_fb_height = output->video_crtc_height;		

				osd_vars.video_width = output->video_frm_width;
				osd_vars.video_height = output->video_frm_height;

				// create new external frame group and allocate (commit flow) new DRM buffers and DRM FB
				assert(!mpi.frm_grp);
				ret = mpp_buffer_group_get_external(&mpi.frm_grp, MPP_BUFFER_TYPE_DRM);
				assert(!ret);			
	
				for (i=0; i<MAX_FRAMES; i++) {
					
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
					assert(dmcd.pitch==(fmt==MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8));
					assert(dmcd.size==(fmt == MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8)*ver_stride*2);
					mpi.frame_to_drm[i].handle = dmcd.handle;
					
					// commit DRM buffer to frame group
					struct drm_prime_handle dph;
					memset(&dph, 0, sizeof(struct drm_prime_handle));
					dph.handle = dmcd.handle;
					dph.fd = -1;
					do {
						ret = ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
					} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
					assert(!ret);
					MppBufferInfo info;
					memset(&info, 0, sizeof(info));
					info.type = MPP_BUFFER_TYPE_DRM;
					info.size = dmcd.width*dmcd.height;
					info.fd = dph.fd;
					ret = mpp_buffer_commit(mpi.frm_grp, &info);
					assert(!ret);
					mpi.frame_to_drm[i].prime_fd = info.fd; // dups fd						
					if (dph.fd != info.fd) {
						ret = close(dph.fd);
						assert(!ret);
					}

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
					ret = drmModeAddFB2(drm_fd, output->video_frm_width, output->video_frm_height, DRM_FORMAT_NV12, handles, pitches, offsets, &mpi.frame_to_drm[i].fb_id, 0);
					assert(!ret);
				}

				// register external frame group
				ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
				ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

				output->video_cur_fb_id = mpi.frame_to_drm[0].fb_id;
				
				ret = pthread_create(&tid_display, NULL, __DISPLAY_THREAD__, NULL);
				assert(!ret);

			} else {
				// regular frame received
				if (!mpi.first_frame_ts.tv_sec) {
					ts = ats;
					mpi.first_frame_ts = ats;
				}

				MppBuffer buffer = mpp_frame_get_buffer(frame);					
				if (buffer) {
					output->video_poc = mpp_frame_get_poc(frame);
					// find fb_id by frame prime_fd
					MppBufferInfo info;
					ret = mpp_buffer_info_get(buffer, &info);
					assert(!ret);
					for (i=0; i<MAX_FRAMES; i++) {
						if (mpi.frame_to_drm[i].prime_fd == info.fd) break;
					}
					assert(i!=MAX_FRAMES);

					ts = ats;

					output->video_cur_fb_id = mpi.frame_to_drm[i].fb_id;
					frame_stats_byfd[output->video_cur_fb_id] = frame_stats[output->video_poc];
					decoded_frame++;

					// clock_gettime(CLOCK_MONOTONIC, &fps_end);
					// uint64_t time_us=(fps_end.tv_sec - fps_start.tv_sec)*1000000ll + ((fps_end.tv_nsec - fps_start.tv_nsec)/1000ll) % 1000000ll;
					// if (time_us >= 1000000) {
					// 	uint64_t sum = 0;
					// 	for (int i = 0; i < decoded_frame; ++i) {
					// 		sum += latency_avg[i];
					// 		if (latency_avg[i] > max_latency) {
					// 			max_latency = latency_avg[i];
					// 		}
					// 		if (latency_avg[i] < min_latency) {
					// 			min_latency = latency_avg[i];
					// 		}
					// 	}
					// 	//printf("decoding latency=%.2f ms (%.2f, %.2f), framerate=%d fps\n", (sum / (decoded_frame))/1000.0, max_latency/1000.0, min_latency/1000.0, decoded_frame);	

					// 	fps_start = fps_end;
					// 	osd_vars.current_framerate = decoded_frame;
					// 	decoded_frame = 0;
					// 	max_latency = 0;
					// 	min_latency = 1844674407370955161;
					// }
					// struct timespec rtime = frame_stats[output->video_poc];
					// latency_avg[decoded_frame] = (fps_end.tv_sec - rtime.tv_sec)*1000000ll + ((fps_end.tv_nsec - rtime.tv_nsec)/1000ll) % 1000000ll;
				}
			}
			
			frm_eos = mpp_frame_get_eos(frame);
			mpp_frame_deinit(&frame);
			frame = NULL;
		} else assert(0);
	}
	printf("Frame thread done.\n");
}


int enable_dvr = 0;
char * dvr_file;

int read_rtp_stream(int port, MppPacket *packet, uint8_t* nal_buffer) {
	// Create socket
  	int socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in address;
	memset(&address, 0x00, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	bind(socketFd, (struct sockaddr*)&address, sizeof(struct sockaddr_in));

	if (fcntl(socketFd, F_SETFL, O_NONBLOCK) == -1) {
		printf("ERROR: Unable to set non-blocking mode\n");
		return 1;
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

		// send packet until it success
		while (!signal_flag && MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
				usleep(10000);
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
	ret = modeset_prepare(drm_fd, output);
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

	////////////////////////////////////////////// MAIN LOOP
	
	read_rtp_stream(listen_port, packet, nal_buffer);

	////////////////////////////////////////////// MPI CLEANUP

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