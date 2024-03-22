
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
int osd_thread_signal = 0;

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

cairo_surface_t *fps_icon;
cairo_surface_t *lat_icon;
cairo_surface_t* net_icon;

int osd_vblank_count = 0;
struct timespec last_osd_refresh;

void modeset_paint_framebuffer(struct modeset_output *out) {
	osd_vblank_count++;
	unsigned int j,k,off;
	cairo_t* cr;
	cairo_surface_t *surface;

	struct modeset_buf *buf = &out->osd_bufs[out->osd_buf_curr];

	// TODO(gehee) This is super slow; re-enable with alpha.
	// for (j = 0; j < buf->height; ++j) {
	//     for (k = 0; k < buf->width; ++k) {
	//         off = buf->stride * j + k * 4;
	//         *(uint32_t*)&buf->map[off] = (0 << 24) | (0 << 16) | (0 << 8) | 0;
	//     }
	// }

	surface = cairo_image_surface_create_for_data(buf->map, CAIRO_FORMAT_ARGB32, buf->width, buf->height, buf->stride);
	cr = cairo_create (surface);

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

	int osd_x = buf->width - 300;
	if (osd_vars.enable_frame_counter == 1 ){
		osd_x = buf->width - 600;
		cairo_select_font_face (cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size (cr, 120);
		cairo_set_source_rgba(cr, 0, 0, 0, 1); // R, G, B, A
		cairo_rectangle(cr, osd_x, 0, 1000, 300); 
		cairo_fill(cr);
		char str[80];
		sprintf(str, "%d", osd_vblank_count);
		cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to(cr, osd_x + 20, 200);
		cairo_show_text(cr, str);
		out->osd_buf_curr = (out->osd_buf_curr+1) % OSD_BUF_COUNT;
		return;
	}

	struct timespec curr_time;
	clock_gettime(CLOCK_MONOTONIC, &curr_time);
	uint64_t time_us=(curr_time.tv_sec - last_osd_refresh.tv_sec)*1000000ll + ((curr_time.tv_nsec - last_osd_refresh.tv_nsec)/1000ll) % 1000000ll;
	if (time_us<=1000000) { // refresh every 1000ms
		return;
	}
	cairo_set_source_rgba(cr, 0, 0, 0, 0.4); // R, G, B, A
	cairo_rectangle(cr, osd_x, 0, 300, stats_height); 
	cairo_fill(cr);
	cairo_select_font_face (cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size (cr, 20);

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
	last_osd_refresh = curr_time;

	// Switch buffer at each draw call
	out->osd_buf_curr = (out->osd_buf_curr+1) % OSD_BUF_COUNT;
}

void init_icons() {
	fps_icon = surface_from_embedded_png(framerate_icon, framerate_icon_length);
	lat_icon = surface_from_embedded_png(latency_icon, latency_icon_length);
	net_icon = surface_from_embedded_png(bandwidth_icon, bandwidth_icon_length);
}


void *__OSD_THREAD__(void *param) {
	init_icons();
	struct modeset_output *out = param;
	while(!osd_thread_signal) {
		modeset_paint_framebuffer(out);
		usleep(1000000);
    }
	printf("OSD thread done.\n");
}