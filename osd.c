
#include "osd.h"

#include "drm.h"
#include <cairo.h>

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
	int fps, uint64_t latency_avg, uint64_t latency_min, uint64_t latency_max, long long bw_stats[10], int bw_curr, 
	cairo_surface_t* fps_icon, cairo_surface_t* lat_icon, cairo_surface_t* net_icon)
{
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

	int osd_x = buf->width - 320;

	surface = cairo_image_surface_create_for_data (buf->map, CAIRO_FORMAT_ARGB32, buf->width, buf->height,buf->stride);
	cr = cairo_create (surface);
	cairo_select_font_face (cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size (cr,20);

	cairo_set_source_rgba(cr, 0, 0, 0, 0.3); // R, G, B, A
	cairo_rectangle(cr, osd_x, 0, 400, 115); 
	cairo_fill(cr);


	cairo_set_source_surface (cr, fps_icon, osd_x+30, 17);
	cairo_paint (cr);
	cairo_set_source_surface (cr, lat_icon, osd_x+30, 44);
	cairo_paint (cr);
	cairo_set_source_surface (cr, net_icon, osd_x+30, 71);
	cairo_paint (cr);

	cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
	cairo_move_to (cr,osd_x+60,35);
	char str[80];
	sprintf(str, "%d fps", fps);
	cairo_show_text (cr, str);
	
	
	cairo_move_to (cr,osd_x+60,62);
	sprintf(str, "%.2f ms (%.2f, %.2f)", latency_avg/1000.0, latency_min/1000.0, latency_max/1000.0);
	cairo_show_text (cr, str);
	

	double avg_bw = 0;
	int avg_cnt = 0;
	for (int i = bw_curr; i<(bw_curr+10); ++i) {
		int h = bw_stats[i%10]/10000 * 1.2;
		if (h<0) {
			h = 0;
		}
		if (bw_stats[i%10]>0) {
			avg_bw += bw_stats[i%10];
			avg_cnt++;
		}
	}
	avg_bw = avg_bw / avg_cnt / 100;
	if (avg_bw < 1000) {
		sprintf(str, "%.2f KB/s", avg_bw );
	} else {
		sprintf(str, "%.2f MB/s", avg_bw / 1000 );
	}
	cairo_move_to (cr, osd_x+60,89);
	cairo_show_text (cr, str);

	// Commit fb change.
	int ret;
	drmModeAtomicReq *req = out->osd_request;
	drmModeAtomicSetCursor(req, 0);
	int fb = out->osd_bufs[out->osd_buf_switch ^ 1].fb;
  	ret = set_drm_object_property(req, plane, "FB_ID", fb);
	assert(ret>0);
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);

	// Switch buffer at each draw call
	out->osd_buf_switch ^= 1;
}