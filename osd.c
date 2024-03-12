
#include "osd.h"

#include "drm.h"
#include <cairo.h>
#include "mavlink.h"
#include "icons/icons.h"

#define BILLION 1000000000L
#define WFB_LINK_LOST 1
#define WFB_LINK_JAMMED 2

#define PATH_MAX	4096

struct osd_vars osd_vars;

int modeset_perform_modeset_osd(int fd, struct modeset_output *output_list)
{
	int ret, flags;
	struct drm_object *plane = &output_list->osd_plane;
	struct modeset_buf *buf = &output_list->osd_bufs[output_list->osd_buf_switch ^ 1];

	ret = modeset_atomic_prepare_commit(fd, output_list, output_list->osd_request, plane, buf->fb, buf->width, buf->height, 2 /* zpos*/);
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed for osd plane %d: %m\n", plane->id);
		return ret;
	}

	/* perform test-only atomic commit */
	flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, output_list->osd_request, flags, NULL);
	if (ret < 0) {
		fprintf(stderr, "test-only atomic commit failed for osd plane %d: %m\n", plane->id);
		return ret;
	}

	/* initial modeset on all outputs */
	flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, output_list->osd_request, flags, NULL);
	if (ret < 0)
		fprintf(stderr, "modeset atomic commit failed for osd plane %d: %m\n", plane->id);

	return ret;
}

void modeset_draw_osd(int fd, struct drm_object *plane, struct modeset_output *out, 
	cairo_surface_t* fps_icon, cairo_surface_t* lat_icon, cairo_surface_t* net_icon) {
    
	// struct timespec draw_start, draw_end;
	// clock_gettime(CLOCK_MONOTONIC, &draw_start);

	struct modeset_buf *buf;
	unsigned int j,k,off,random;
	char time_left[5];
	cairo_t* cr;
	cairo_surface_t *surface;
	buf = &out->osd_bufs[out->osd_buf_switch ^ 1];
	for (j = 0; j < buf->height; ++j) {
	    for (k = 0; k < buf->width; ++k) {
	        off = buf->stride * j + k * 4;
	        *(uint32_t*)&buf->map[off] = (0 << 24) | (0 << 16) | (0 << 8) | 0;
	    }
	}

	int osd_x = buf->width - 300;
	surface = cairo_image_surface_create_for_data(buf->map, CAIRO_FORMAT_ARGB32, buf->width, buf->height, buf->stride);
	cr = cairo_create (surface);

	cairo_select_font_face (cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size (cr, 20);

	// stats height
	int stats_top_margin = 5;
	int stats_row_height = 33;
	int stats_height = 30;
	int row_count = 0;
	if (osd_vars.enable_video) {
		stats_height+=stats_row_height*3;
	} 
	if (osd_vars.enable_wfbng) {
		stats_height+=stats_row_height;
	} 

	cairo_set_source_rgba(cr, 0, 0, 0, 0.4); // R, G, B, A
	cairo_rectangle(cr, osd_x, 0, 300, stats_height); 
	cairo_fill(cr);
	

	if (osd_vars.enable_video) {
		row_count++;
		cairo_set_source_surface (cr, fps_icon, osd_x+22, stats_top_margin+stats_row_height-19);
		cairo_paint(cr);
		cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to (cr,osd_x+60, stats_top_margin+stats_row_height);
		char str[80];
		sprintf(str, "%d fps | %dx%d", osd_vars.current_framerate, osd_vars.video_width, osd_vars.video_height);
		cairo_show_text (cr, str);

		row_count++;
		cairo_set_source_surface (cr, lat_icon, osd_x+22, stats_top_margin+row_count*stats_row_height-19);
		cairo_paint (cr);
		cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to (cr,osd_x+60, stats_top_margin+stats_row_height*2);
		sprintf(str, "%.2f ms (%.2f, %.2f)", osd_vars.latency_avg/1000.0, osd_vars.latency_min/1000.0, osd_vars.latency_max/1000.0);
		cairo_show_text (cr, str);
		
		// Video Link Elements
		double avg_bw = 0;
		int avg_cnt = 0;
		for (int i = osd_vars.bw_curr; i<(osd_vars.bw_curr+10); ++i) {
			int h = osd_vars.bw_stats[i%10]/10000 * 1.2;
			if (h<0) {
				h = 0;
			}
			if (osd_vars.bw_stats[i%10]>0) {
				avg_bw += osd_vars.bw_stats[i%10];
				avg_cnt++;
			}
		}
		avg_bw = avg_bw / avg_cnt / 100;
		if (avg_bw < 1000) {
			sprintf(str, "%.2f KB/s", avg_bw );
		} else {
			sprintf(str, "%.2f MB/s", avg_bw / 1000 );
		}
		row_count++;
		cairo_set_source_surface (cr, net_icon, osd_x+22, stats_top_margin+row_count*stats_row_height-19);
		cairo_paint (cr);
		cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to (cr, osd_x+60, stats_top_margin+stats_row_height*row_count);
		cairo_show_text (cr, str);
	}

	 // WFB-ng Elements
	if (osd_vars.enable_wfbng) {
		char msg[16];
		memset(msg, 0x00, sizeof(msg));
		sprintf(msg, "WFB %3d F%d L%d", osd_vars.wfb_rssi, osd_vars.wfb_fec_fixed, osd_vars.wfb_errors);
		// //TODO (gehee) Only getting WFB_LINK_LOST when testing.
		// if (osd_vars.wfb_flags & WFB_LINK_LOST) {
		// 		sprintf(msg, "%s (LOST)", msg);
		// } else if (osd_vars.wfb_flags & WFB_LINK_JAMMED) {
		// 		sprintf(msg, "%s (JAMMED)", msg);
		// }
		row_count++;
		cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to(cr, osd_x+25, stats_top_margin+stats_row_height*row_count);
		cairo_show_text(cr, msg);
	}

	// Commit fb change.
	int ret;
	drmModeAtomicReq *req = out->osd_request;
	drmModeAtomicSetCursor(req, 0);
	int fb = out->osd_bufs[out->osd_buf_switch ^ 1].fb;
  	ret = set_drm_object_property(req, plane, "FB_ID", fb);
	assert(ret>0);
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);

	// clock_gettime(CLOCK_MONOTONIC, &draw_end);
	// uint64_t time_us=(draw_end.tv_sec - draw_start.tv_sec)*1000000ll + ((draw_end.tv_nsec - draw_start.tv_nsec)/1000ll) % 1000000ll;
	// printf("osd drawn in %.2f ms\n", time_us/1000.0);			

	// Switch buffer at each draw call
	out->osd_buf_switch ^= 1;
}

int osd_thread_signal;

typedef struct png_closure
{
	unsigned char * iter;
	unsigned int bytes_left;
} png_closure_t;

cairo_status_t on_read_png_stream(png_closure_t * closure, unsigned char * data, unsigned int length)
{
	if(length > closure->bytes_left) return CAIRO_STATUS_READ_ERROR;
	
	memcpy(data, closure->iter, length);
	closure->iter += length;
	closure->bytes_left -= length;
	return CAIRO_STATUS_SUCCESS;
}

cairo_surface_t * surface_from_embedded_png(const unsigned char * png, size_t length)
{
	int rc = -1;
	png_closure_t closure[1] = {{
		.iter = (unsigned char *)png,
		.bytes_left = length,
	}};
	return cairo_image_surface_create_from_png_stream(
		(cairo_read_func_t)on_read_png_stream,
		closure);
}

void *__OSD_THREAD__(void *param) {
	osd_thread_params *p = param;
	cairo_surface_t *fps_icon = surface_from_embedded_png(framerate_icon, framerate_icon_length);
	cairo_surface_t *lat_icon = surface_from_embedded_png(latency_icon, latency_icon_length);
	cairo_surface_t* net_icon = surface_from_embedded_png(bandwidth_icon, bandwidth_icon_length);
	modeset_perform_modeset_osd(p->fd, p->output_list);
	while(!osd_thread_signal) {
		modeset_draw_osd(p->fd, &p->output_list->osd_plane, p->output_list, fps_icon, lat_icon, net_icon);
		usleep(1000000);
    }
	// TODO(gehee) This code is never reached.
	printf("OSD thread done.\n");
}