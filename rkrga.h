#ifndef RK_RGA_H
#define RK_RGA_H

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <rga.h>

#define RGA_ALIGN(x, a)   (((x)+(a)-1)&~((a)-1))
#define CODEC_ALIGN(x, a)   (((x)+(a)-1)&~((a)-1))

int rga_chk_dev_cap();

int rga_set_src_fmt();

int rga_set_dst_fmt();

int rga_open_dev_strm();

int rga_src_buf_map();

int rga_dst_buf_map() ;

int rga_set_img_rotation(unsigned degree);

int rga_init(int srcW, int srcH, int dstW, int dstH);

int rga_swscale(int srcFrmFd, int dstFrmFd) ;


#endif
