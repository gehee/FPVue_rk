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
#include <rk_mpi.h>

#include "drm.c"
#include "rtp_frame.h"

#define READ_BUF_SIZE (1024*1024) // SZ_1M https://github.com/rockchip-linux/mpp/blob/ed377c99a733e2cdbcc457a6aa3f0fcd438a9dff/osal/inc/mpp_common.h#L179
#define MAX_FRAMES 24		// min 16 and 20+ recommended (mpp/readme.txt)

#define PATH_MAX	4096

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

enum supported_eotf_type {
	TRADITIONAL_GAMMA_SDR = 0,
	TRADITIONAL_GAMMA_HDR,
	SMPTE_ST2084,
	HLG,
	FUTURE_EOTF
};

enum drm_hdmi_output_type {
	DRM_HDMI_OUTPUT_DEFAULT_RGB,
	DRM_HDMI_OUTPUT_YCBCR444,
	DRM_HDMI_OUTPUT_YCBCR422,
	DRM_HDMI_OUTPUT_YCBCR420,
	DRM_HDMI_OUTPUT_YCBCR_HQ,
	DRM_HDMI_OUTPUT_YCBCR_LQ,
	DRM_HDMI_OUTPUT_INVALID,
};

struct modeset_output *output_list;
int frm_eos = 0;
int drm_fd = 0;
pthread_mutex_t video_mutex;
pthread_cond_t video_cond;

// OSD Vars
int current_framerate = 0;
uint64_t current_latency = 0;
int bw_curr=0;
long long bw_stats[10];

// Headers
int read_rtp_stream(int port, MppPacket *packet, uint8_t* nal_buffer);


// frame_thread
//
// - allocate DRM buffers and DRM FB based on frame size
// - pick frame in blocking mode and output to screen overlay

void *frame_thread(void *param)
{
	int ret;
	int i;	
	MppFrame  frame  = NULL;
	int frid = 0;

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

				output_list->video_frm_width = mpp_frame_get_width(frame);
				output_list->video_frm_height = mpp_frame_get_height(frame);
				RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
				RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
				MppFrameFormat fmt = mpp_frame_get_fmt(frame);
				assert((fmt == MPP_FMT_YUV420SP) || (fmt == MPP_FMT_YUV420SP_10BIT));

				printf("frame info changed %d(%d)x%d(%d)\n", output_list->video_frm_width, hor_stride, output_list->video_frm_height, ver_stride);
			
				output_list->video_fb_x = 0;
				output_list->video_fb_y = 0;
				output_list->video_fb_width = output_list->video_crtc_width;
				output_list->video_fb_height = output_list->video_crtc_height;		

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
					offsets[1] = hor_stride * ver_stride;
					pitches[1] = hor_stride;
					ret = drmModeAddFB2(drm_fd, output_list->video_frm_width, output_list->video_frm_height, DRM_FORMAT_NV12, handles, pitches, offsets, &mpi.frame_to_drm[i].fb_id, 0);
					assert(!ret);
				}

				// register external frame group
				ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
				ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

				drmModeAtomicSetCursor(output_list->video_request, 0);
				ret = modeset_atomic_prepare_commit(drm_fd, output_list, output_list->video_request, &output_list->plane_video, 
					mpi.frame_to_drm[0].fb_id, output_list->video_frm_width, output_list->video_frm_height, -1 /*zpos*/);
				assert(ret >= 0);
				ret = drmModeAtomicCommit(drm_fd, output_list->video_request, DRM_MODE_ATOMIC_NONBLOCK, NULL);
				assert(!ret);

			} else {
				// regular frame received
				if (!mpi.first_frame_ts.tv_sec) {
					ts = ats;
					mpi.first_frame_ts = ats;
				}

				MppBuffer buffer = mpp_frame_get_buffer(frame);					
				if (buffer) {
					output_list->video_poc = mpp_frame_get_poc(frame);
					// find fb_id by frame prime_fd
					MppBufferInfo info;
					ret = mpp_buffer_info_get(buffer, &info);
					assert(!ret);
					for (i=0; i<MAX_FRAMES; i++) {
						if (mpi.frame_to_drm[i].prime_fd == info.fd) break;
					}
					assert(i!=MAX_FRAMES);

					ts = ats;
					frid++;
					
					// send DRM FB to display thread
					ret = pthread_mutex_lock(&video_mutex);
					assert(!ret);
					if (output_list->video_fb_id) output_list->video_skipped_frames++;
					output_list->video_fb_id = mpi.frame_to_drm[i].fb_id;
					ret = pthread_cond_signal(&video_cond);
					assert(!ret);
					ret = pthread_mutex_unlock(&video_mutex);
					assert(!ret);
					
				} else printf("FRAME no buff\n");
			}
			
			frm_eos = mpp_frame_get_eos(frame);
			mpp_frame_deinit(&frame);
			frame = NULL;
		} else assert(0);
	}
	return NULL;
}

// display_thread

void *display_thread(void *param)
{
	int ret;	
	int frame_counter = 0;
    struct timespec fps_start, fps_end;
	clock_gettime(CLOCK_MONOTONIC, &fps_start);

	while (!frm_eos) {
		int fb_id;
		
		ret = pthread_mutex_lock(&video_mutex);
		assert(!ret);
		while (output_list->video_fb_id==0) {
			pthread_cond_wait(&video_cond, &video_mutex);
			assert(!ret);
			if (output_list->video_fb_id == 0 && frm_eos) {
				ret = pthread_mutex_unlock(&video_mutex);
				assert(!ret);
				goto end;
			}
		}
		struct timespec ts, ats;
		clock_gettime(CLOCK_MONOTONIC, &ats);
		fb_id = output_list->video_fb_id;
		if (output_list->video_skipped_frames) 
			printf("DISPLAY skipped %d\n", output_list->video_skipped_frames);
		output_list->video_fb_id=0;
		output_list->video_skipped_frames=0;
		ret = pthread_mutex_unlock(&video_mutex);
		assert(!ret);

		if (param==NULL) {
			// show DRM FB in plane
			drmModeAtomicSetCursor(output_list->video_request, 0);
			ret = set_drm_object_property(output_list->video_request, &output_list->plane_video, "FB_ID", fb_id);
			assert(ret>0);
			ret = drmModeAtomicCommit(drm_fd, output_list->video_request, DRM_MODE_ATOMIC_NONBLOCK, NULL);
			frame_counter++;

			clock_gettime(CLOCK_MONOTONIC, &fps_end);
			uint64_t time_us=(fps_end.tv_sec - fps_start.tv_sec)*1000000ll + ((fps_end.tv_nsec - fps_start.tv_nsec)/1000ll) % 1000000ll;
			if (time_us >= 1000000) {
				current_framerate = frame_counter;
				frame_counter = 0;
				fps_start = fps_end;
			}
			
			struct timespec rtime = frame_stats[output_list->video_poc];
		    current_latency = (fps_end.tv_sec - rtime.tv_sec)*1000000ll + ((fps_end.tv_nsec - rtime.tv_nsec)/1000ll) % 1000000ll;
		}
	}
end:	
	printf("Display thread done.\n");
}

// signal

int signal_flag = 0;

void sig_handler(int signum)
{
	printf("Received signal %d\n", signum);
	signal_flag++;
}

// osd_thread

char * sc_file_get_executable_dir(void) {
	// <https://stackoverflow.com/a/1024937/1987178>
    char buf[PATH_MAX + 1]; // +1 for the null byte
    ssize_t len = readlink("/proc/self/exe", buf, PATH_MAX);
    if (len == -1) {
        perror("readlink");
        return NULL;
    }
	int i;
	int end = len;	
	for (i = 0; i < len; i++) {
		if (buf[i] == '/') {
			end = i;
		}
	}
	
    buf[end] = '\0';
    return strdup(buf);
}

void *osd_thread(void *param) {
	// Load icons from local folder.
	// TODO(geehe) embed into source file.
	char * icon_dir = sc_file_get_executable_dir();
	char * icon_path = (char *) malloc(1 + strlen(icon_dir)+ strlen("/icons/framerate.png") );
    strcpy(icon_path, icon_dir);
    strcat(icon_path, "/icons/framerate.png");
	cairo_surface_t *fps_icon = cairo_image_surface_create_from_png(icon_path);
    strcpy(icon_path, icon_dir);
    strcat(icon_path, "/icons/latency.png");
	cairo_surface_t *lat_icon = cairo_image_surface_create_from_png(icon_path);
    strcpy(icon_path, icon_dir);
    strcat(icon_path, "/icons/network.png");
	cairo_surface_t* net_icon = cairo_image_surface_create_from_png(icon_path);

	modeset_perform_modeset_osd(drm_fd, output_list);
	do {
		modeset_draw_osd(drm_fd, &output_list->plane_osd, output_list, current_framerate, current_latency, bw_stats, bw_curr, fps_icon, lat_icon, net_icon);
		usleep(1000000);
    } while (!signal_flag);
	modeset_cleanup(drm_fd, output_list);
}

// main

int main(int argc, char **argv)
{
	int ret;	
	int i, j;
	int enable_osd = 0;

	// Loop through each command-line argument
    for (int i = 1; i < argc; i++) {
        // Check if the argument is a flag
        if (argv[i][0] == '-') {
            // Process the flag
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                // Display help message
                printf("Usage: %s [--osd]\n", argv[0]);
                return 0;
            } else if (strcmp(argv[i], "--osd") == 0) {
                enable_osd = 1;
            } else {
                // Handle other flags or flag arguments
                printf("Unknown flag: %s\n", argv[i]);
                return 1; // Return an error code
            }
        } else {
            printf("Unknown flag: %s\n", argv[i]);
            return 1; // Return an error code
        }
    }
		
	MppCodingType mpp_type = MPP_VIDEO_CodingHEVC; //(MppCodingType)atoi(argv[1]);
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

	output_list = (struct modeset_output *)malloc(sizeof(struct modeset_output));

	output_list->video_request = drmModeAtomicAlloc();
	assert(output_list->video_request);
 	// SETUP DRM
	ret = modeset_prepare(drm_fd, output_list);
	assert(!ret);
	
	////////////////////////////////////////////// MPI SETUP
	MppPacket packet;

	uint8_t* nal_buffer = malloc(1024 * 1024);
	assert(nal_buffer);
	ret = mpp_packet_init(&packet, nal_buffer, READ_BUF_SIZE);
	assert(!ret);

	ret = mpp_create(&mpi.ctx, &mpi.mpi);
	assert(!ret);

	// decoder split mode (multi-data-input) need to be set before init
	int param = 1;
	// ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &param);
	// assert(!ret);

	// mpp_env_set_u32("mpi_debug", 0x1);
	// mpp_env_set_u32("mpp_buffer_debug", 0xf);
	// mpp_env_set_u32("h265d_debug", 0xfff);

	ret = mpp_init(mpi.ctx, MPP_CTX_DEC, mpp_type);
	assert(!ret);

	// blocked/wait read of frame in thread
	param = MPP_POLL_BLOCK;
	ret = mpi.mpi->control(mpi.ctx, MPP_SET_OUTPUT_BLOCK, &param);
	assert(!ret);

 	//////////////////// THREADS SETUP
	
	ret = pthread_mutex_init(&video_mutex, NULL);
	assert(!ret);
	ret = pthread_cond_init(&video_cond, NULL);
	assert(!ret);

	pthread_t tid_frame, tid_display, tid_osd;
	ret = pthread_create(&tid_frame, NULL, frame_thread, NULL);
	assert(!ret);
	ret = pthread_create(&tid_display, NULL, display_thread, argc==4?argv[3]:NULL);
	assert(!ret);
	if (enable_osd) {
		ret = pthread_create(&tid_osd, NULL, osd_thread, NULL);
		assert(!ret);
	}

	////////////////////////////////////////////// MAIN LOOP
	
	read_rtp_stream(5600, packet, nal_buffer);

	////////////////////////////////////////////// MPI CLEANUP

	ret = pthread_join(tid_frame, NULL);
	assert(!ret);
	
	ret = pthread_mutex_lock(&video_mutex);
	assert(!ret);	
	ret = pthread_cond_signal(&video_cond);
	assert(!ret);	
	ret = pthread_mutex_unlock(&video_mutex);
	assert(!ret);	

	ret = pthread_join(tid_display, NULL);
	assert(!ret);	
	
	ret = pthread_cond_destroy(&video_cond);
	assert(!ret);
	ret = pthread_mutex_destroy(&video_mutex);
	assert(!ret);

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
	drmModeSetCrtc(drm_fd,
			       output_list->saved_crtc->crtc_id,
			       output_list->saved_crtc->buffer_id,
			       output_list->saved_crtc->x,
			       output_list->saved_crtc->y,
			       &output_list->connector.id,
			       1,
			       &output_list->saved_crtc->mode);
	drmModeFreeCrtc(output_list->saved_crtc);

	modeset_cleanup(drm_fd, output_list);
	drmModeAtomicFree(output_list->video_request);
	drmModeAtomicFree(output_list->osd_request);
	close(drm_fd);

	return 0;
}



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

	printf("listening on socket 5600\n");

	uint8_t* rx_buffer = malloc(1024 * 1024);
    
	int nalStart = 0;
	int poc = 0;
	int ret = 0;
	struct timespec recv_ts;
	long long bytesReceived = 0; 
	uint8_t* nal;

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
			bw_curr = (bw_curr + 1) % 10;
			bw_stats[bw_curr] = bytesReceived;
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
	};
	printf("PACKET EOS\n");
	mpp_packet_set_eos(packet);
	mpp_packet_set_pos(packet, nal_buffer);
	mpp_packet_set_length(packet, 0);
	while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
		usleep(10000);
	}
}