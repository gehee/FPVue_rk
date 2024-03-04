
#include "osd.h"

#include "drm.h"
#include <cairo.h>
#include "mavlink.h"

#define BILLION 1000000000L
#define WFB_LINK_LOST 1
#define WFB_LINK_JAMMED 2

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

extern uint32_t frames_received;
uint32_t stats_rx_bytes = 0;
struct timespec last_timestamp = {0, 0};
float rx_rate = 0;
int hours = 0 , minutes = 0 , seconds = 0 , milliseconds = 0;

double getTimeInterval(struct timespec* timestamp, struct timespec* last_meansure_timestamp) {
  return (timestamp->tv_sec - last_meansure_timestamp->tv_sec) +
       (timestamp->tv_nsec - last_meansure_timestamp->tv_nsec) / 1000000000.;
}


void modeset_draw_osd(int fd, struct drm_object *plane, struct modeset_output *out, cairo_surface_t* fps_icon, cairo_surface_t* lat_icon, cairo_surface_t* net_icon) {
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

	// Video Decoder Elements
	cairo_set_source_rgba(cr, 0, 0, 0, 0.3); // R, G, B, A
	cairo_rectangle(cr, osd_x, 0, 400, 140); 
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
	sprintf(str, "%d fps", osd_vars.current_framerate);
	cairo_show_text (cr, str);
	
	
	cairo_move_to (cr,osd_x+60,62);
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
	cairo_move_to (cr, osd_x+60,89);
	cairo_show_text (cr, str);

	// WFB-ng Elements
    char msg[16];
    memset(msg, 0x00, sizeof(msg));
	sprintf(msg, "WFB %3d F%d L%d", osd_vars.wfb_rssi, osd_vars.wfb_fec_fixed, osd_vars.wfb_errors);
	if (osd_vars.wfb_flags & WFB_LINK_LOST) {
		sprintf(msg, "%s (LOST)", msg);
	} else if (osd_vars.wfb_flags & WFB_LINK_JAMMED) {
		sprintf(msg, "%s (JAMMED)", msg);
	}
	cairo_move_to(cr, osd_x+30, 120);
    cairo_show_text(cr, msg);

	// Mavlink elements
	uint32_t x_center = buf->width / 2;
	if (osd_vars.osd_mode > 0){
		// Artificial Horizon
		int32_t offset_pitch = osd_vars.telemetry_pitch * 4;
		int32_t offset_roll = osd_vars.telemetry_roll * 4;
		int32_t y_pos_left = ((int32_t)buf->height / 2 - 2 + offset_pitch + offset_roll);
		int32_t y_pos_right = ((int32_t)buf->height / 2 - 2 + offset_pitch - offset_roll);

		for (int i = 0; i < 4; i++) {
		if (y_pos_left > 0 && y_pos_left < buf->height &&
			y_pos_right > 0 && y_pos_right < buf->height) {
			//fbg_line(cr, x_center - 180, y_pos_left + i, x_center + 180, y_pos_right + i, 255, 255, 255);
		}
		}

		// Vertical Speedometer
		int32_t offset_vspeed = osd_vars.telemetry_vspeed * 5;
		int32_t y_pos_vspeed = ((int32_t)buf->height / 2 - offset_vspeed);
		for (int i = 0; i < 8; i++) {
		if (y_pos_vspeed > 0 && y_pos_vspeed < buf->height) {
			//fbg_line(cr, x_center + 242 + i, buf->height / 2, x_center + 242 + i, y_pos_vspeed, 255, 255, 255);
		}
		}

		for (int i = 0; i < 25; i++) {
			uint32_t width = (i == 12) ? 10 : 0;

			//   fbg_line(cr, x_center - 240 - width,
			//     buf->height / 2 - 120 + i * 10, x_center - 220,
			//     buf->height / 2 - 120 + i * 10, 255, 255, 255);
			//   fbg_line(cr, x_center - 240 - width,
			//     buf->height / 2 - 120 + i * 10 + 1, x_center - 220,
			//     buf->height / 2 - 120 + i * 10 + 1, 255, 255, 255);

			//   fbg_line(cr, x_center + 220, buf->height / 2 - 120 + i * 10,
			//     x_center + 240 + width, buf->height / 2 - 120 + i * 10, 255, 255, 255);
			//   fbg_line(cr, x_center + 220, buf->height / 2 - 120 + i * 10 + 1,
			//     x_center + 240 + width, buf->height / 2 - 120 + i * 10 + 1, 255, 255, 255);
		}

		// OSD telemetry
		sprintf(msg, "ALT:%.00fM", osd_vars.telemetry_altitude);
		cairo_move_to(cr, x_center + (20) + 260, buf->height / 2 - 8);
		cairo_show_text(cr, msg);
		sprintf(msg, "SPD:%.00fKM/H", osd_vars.telemetry_gspeed);
		cairo_move_to(cr, x_center - (16 * 3) - 360, buf->height / 2 - 8);
		cairo_show_text(cr, msg);
		sprintf(msg, "VSPD:%.00fM/S", osd_vars.telemetry_vspeed);
		cairo_move_to(cr, x_center + (20) + 260, buf->height / 2 + 22);
		cairo_show_text(cr, msg);
	}

    sprintf(msg, "BAT:%.02fV", osd_vars.telemetry_battery / 1000);
    cairo_move_to(cr, 40, buf->height - 30);
    cairo_show_text(cr, msg);
    sprintf(msg, "CONS:%.00fmAh", osd_vars.telemetry_current_consumed);
    cairo_move_to(cr, 40, buf->height - 60);
    cairo_show_text(cr, msg);
    sprintf(msg, "CUR:%.02fA", osd_vars.telemetry_current / 100);
    cairo_move_to(cr, 40, buf->height - 90);
    cairo_show_text(cr, msg);
    sprintf(msg, "THR:%.00f%%", osd_vars.telemetry_throttle);
    cairo_move_to(cr, 40, buf->height - 120);
    cairo_show_text(cr, msg);
    
	if (osd_vars.osd_mode > 0){
		sprintf(msg, "SATS:%.00f", osd_vars.telemetry_sats);
		cairo_move_to(cr,buf->width - 140, buf->height - 30);
		cairo_show_text(cr, msg);
		sprintf(msg, "HDG:%.00f", osd_vars.telemetry_hdg);
		cairo_move_to(cr,buf->width - 140, buf->height - 120);
		cairo_show_text(cr, msg);
		sprintf(osd_vars.c1, "%.00f", osd_vars.telemetry_lat);

		if (osd_vars.telemetry_lat < 10000000) {
		insertString(osd_vars.c1, "LAT:0.", 0);
		}

		if (osd_vars.telemetry_lat > 9999999) {
		if (numOfChars(osd_vars.c1) == 8) {
			insertString(osd_vars.c1, ".", 1);
		} else {
			insertString(osd_vars.c1, ".", 2);
		}
		insertString(osd_vars.c1, "LAT:", 0);
		}
		cairo_move_to(cr, buf->width - 240, buf->height - 90);
		cairo_show_text(cr,  osd_vars.c1);

		sprintf(osd_vars.c2, "%.00f", osd_vars.telemetry_lon);
		if (osd_vars.telemetry_lon < 10000000) {
		insertString(osd_vars.c2, "LON:0.", 0);
		}
		if (osd_vars.telemetry_lon > 9999999) {
		if (numOfChars(osd_vars.c2) == 8) {
			insertString(osd_vars.c2, ".", 1);
		} else {
			insertString(osd_vars.c2, ".", 2);
		}
		insertString(osd_vars.c2, "LON:", 0);
		}
		cairo_move_to(cr, buf->width - 240, buf->height - 60);
		cairo_show_text(cr,  osd_vars.c2);
		//sprintf(msg, "PITCH:%.00f", telemetry_pitch);
		//cairo_move_to(cr, msg, x_center + 440, buf->height - 140);
		//sprintf(msg, "ROLL:%.00f", telemetry_roll);
		//cairo_move_to(cr, msg, x_center + 440, buf->height - 170);
		sprintf(msg, "DIST:%.03fM", osd_vars.telemetry_distance);
		cairo_move_to(cr, x_center - 350, buf->height - 30);
		cairo_show_text(cr, msg);
	}
    
    sprintf(msg, "RSSI:%.00f", osd_vars.telemetry_rssi);
    cairo_move_to(cr, x_center - 50, buf->height - 30);
    cairo_show_text(cr,  msg);

    // Print rate stats
    struct timespec current_timestamp;
    if (!clock_gettime(CLOCK_MONOTONIC_COARSE, &current_timestamp)) {
      double interval = getTimeInterval(&current_timestamp, &last_timestamp);
      if (interval > 1) {
        last_timestamp = current_timestamp;
        rx_rate = ((float)stats_rx_bytes+(((float)stats_rx_bytes*25)/100)) / 1024.0f * 8;
        stats_rx_bytes = 0;
      }
    }

    uint64_t diff;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    sleep(1);
    clock_gettime(CLOCK_MONOTONIC, &end);
    diff = BILLION * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;

    char hud_frames_rx[32];
    if (osd_vars.osd_mode > 0){
    	cairo_move_to(cr, x_center - strlen(hud_frames_rx) / 2 * 16, 40);
    	cairo_show_text(cr, hud_frames_rx);
    } else {
    	cairo_move_to(cr, buf->width - 300, buf->height - 60);
    	cairo_show_text(cr, hud_frames_rx);
    sprintf(msg, "TIME:%.2d:%.2d", minutes,seconds);
    cairo_move_to(cr, buf->width - 300, buf->height - 90);
    cairo_show_text(cr, msg);
    if (osd_vars.telemetry_arm > 1700){
      seconds = seconds + diff/1000000000;
    }
    if(seconds > 59){
       seconds = 0;
       ++minutes;  
    }
    if(minutes > 59){
       seconds = 0;
       minutes = 0;
    }
    }
    float percent = rx_rate / (1024 * 10);
    if (percent > 1) {
      percent = 1;
    }

    uint32_t width = (strlen(hud_frames_rx) * 16) * percent;
    if (osd_vars.osd_mode > 0){
		cairo_set_source_rgba(cr, 255, 255, 255, 0.8); // R, G, B, A
		cairo_rectangle(cr,  x_center - strlen(hud_frames_rx) / 2 * 16, 64, width, 8); 
    } else {
		cairo_set_source_rgba(cr, 255, 255, 255, 0.8); // R, G, B, A
		cairo_rectangle(cr,  buf->width - 300, buf->height - 36, width, 8);
    }

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