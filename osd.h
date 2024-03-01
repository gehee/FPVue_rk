#ifndef OSD_H
#define OSD_H

#include "drm.h"
#include <cairo.h>

int modeset_perform_modeset_osd(int fd, struct modeset_output *output_list);

void modeset_draw_osd(int fd, struct drm_object *plane, struct modeset_output *out, 
	int fps, uint64_t latency_avg, uint64_t latency_min, uint64_t latency_max, long long bw_stats[10], int bw_curr, 
	cairo_surface_t* fps_icon, cairo_surface_t* lat_icon, cairo_surface_t* net_icon);

#endif
