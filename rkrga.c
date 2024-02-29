#include <linux/videodev2.h>
#include <errno.h>
#include "rkrga.h"

#define RK_RGA_DEV	"/dev/v4l/by-path/platform-ff680000.rga-video-index0"

#define rga_log(fmt,...)\
            do {\
                printf(fmt,##__VA_ARGS__);\
            } while(0)


int rga_mFd;
int rga_mSrcW;
int rga_mSrcH;
int rga_mDstW;
int rga_mDstH;
int rga_mSrcLen;

int rga_chk_dev_cap() {
    struct v4l2_capability cap = {0};
    int ret = 0;

    ret = ioctl(rga_mFd, VIDIOC_QUERYCAP, &cap);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_QUERYCAP.\n");
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M)) {
        rga_log("dev don't support VIDEO_M2M.\n");
        return -2;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        rga_log("dev don't support V4L2_CAP_STREAMING.\n");
        return -3;
    }

    return 0;
}

int rga_set_src_fmt() {
    struct v4l2_format fmt;
    int ret = 0;

    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width  = RGA_ALIGN(rga_mSrcW, 16);
    fmt.fmt.pix.height = RGA_ALIGN(rga_mSrcH, 16);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    ret = ioctl(rga_mFd, VIDIOC_S_FMT, &fmt);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIO_S_FMT.\n");
        return -1;
    }

    return 0;
}

int rga_set_dst_fmt() {
    struct v4l2_format fmt;
    int ret = 0;

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width  = RGA_ALIGN(rga_mDstW, 16);
    fmt.fmt.pix.height = RGA_ALIGN(rga_mDstH, 16);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    ret = ioctl(rga_mFd, VIDIOC_S_FMT, &fmt);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_S_FMT.\n");
        return -1;
    }

    return 0;
}

int rga_open_dev_strm() {
    enum v4l2_buf_type type;
    int ret = 0;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ret = ioctl(rga_mFd, VIDIOC_STREAMON, &type);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_STREAMON for %d %s.\n",
                errno, strerror(errno));
        return -1;
    }

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = ioctl(rga_mFd, VIDIOC_STREAMON, &type);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_STREAMON for %d %s.\n",
                errno, strerror(errno));
        return -2;
    }

    return 0;
}

int rga_src_buf_map() {
    struct v4l2_buffer buf;
    struct v4l2_requestbuffers reqbuf;
    int nb_src_bufs;
    int ret = 0;
    int i = 0;

    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = 5;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    reqbuf.memory = V4L2_MEMORY_DMABUF;

    ret = ioctl(rga_mFd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_REQBUFS for %d %s.\n", errno, strerror(errno));
        return -1;
    }

    nb_src_bufs = reqbuf.count;
    rga_log("get dma buffer nb:%d.\n", nb_src_bufs);
    for (i = 0; i < nb_src_bufs; i++) {
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.index = i;

        ret = ioctl(rga_mFd, VIDIOC_QUERYBUF, &buf);
        if (ret != 0) {
            rga_log("failed to ioctl VIDIOC_QUERYBUF.\n");
            return -2;
        }

    }
    rga_mSrcLen = buf.length;

    return 0;
}

int rga_dst_buf_map() {
    struct v4l2_buffer buf;
    struct v4l2_requestbuffers reqbuf;
    int ret = 0;
    int nb_dst_bufs;
    int i = 0;

    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = 5;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_DMABUF;

    ret = ioctl(rga_mFd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_REQBUFS for %d %s.\n", errno, strerror(errno));
        return -1;
    }

    nb_dst_bufs = reqbuf.count;
    rga_log("get dma buffer nb:%d.\n", nb_dst_bufs);
    for (i = 0; i < nb_dst_bufs; i++) {
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.index = i;

        ret = ioctl(rga_mFd, VIDIOC_QUERYBUF, &buf);
        if (ret != 0) {
            rga_log("failed to ioctl VIDIOC_QUERYBUF.\n");
            return -2;
        }

    }

    return 0;
}

int rga_set_img_rotation(unsigned degree) {
    struct v4l2_control ctrl = {0};
    int ret = 0;

    ctrl.id = V4L2_CID_ROTATE;
    ctrl.value = degree;
    ret = ioctl(rga_mFd, VIDIOC_S_CTRL, &ctrl);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_S_CTRL.\n");
        return -1;
    }
    return 0;
}

int rga_init(int srcW, int srcH, int dstW, int dstH) {
    int ret = 0;

    rga_mSrcW = srcW;
    rga_mSrcH = srcH;
    rga_mDstW = dstW;
    rga_mDstH = dstH;

    rga_mFd = open(RK_RGA_DEV, O_RDWR);
    if (rga_mFd < 0) {
        rga_log("failed to open rga dev %s.\n", RK_RGA_DEV);
        return -1;
    }

    ret = rga_chk_dev_cap();
    if (ret < 0) {
        rga_log("failed to exec chk_dev_cap %d.\n", ret);
        return -2;
    }

    ret = rga_set_src_fmt();
    if (ret < 0) {
        rga_log("failed to exec set_src_fmt %d.\n", ret);
        return -3;
    }

    ret = rga_set_dst_fmt();
    if (ret < 0) {
        rga_log("failed to exec set_dst_fmt %d.\n", ret);
        return -4;
    }

    ret = rga_set_img_rotation(90);
    if (ret < 0) {
        rga_log("failed to exec set_img_rotation %d.\n", ret);
        return -5;
    }

    ret = rga_src_buf_map();
    if (ret < 0) {
        rga_log("failed to exec src_buf_map %d.\n", ret);
        return -6;
    }

    ret = rga_dst_buf_map();
    if (ret < 0) {
        rga_log("failed to exec dst_buf_map %d.\n", ret);
        return -7;
    }

    ret = rga_open_dev_strm();
    if (ret < 0) {
        rga_log("failed to exec open_dev_strm %d.\n", ret);
        return -8;
    }

    return 0;
}

int rga_swscale(int srcFrmFd, int dstFrmFd) {
    struct v4l2_buffer buf = {0};
    int ret = 0;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.bytesused = rga_mSrcLen;
    buf.index = 0;
    buf.m.fd = srcFrmFd;
    ret = ioctl(rga_mFd, VIDIOC_QBUF, &buf);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_QBUF for %d %s.\n",
                errno, strerror(errno));
        return -1;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = 0;
    buf.m.fd = dstFrmFd;
    ret = ioctl(rga_mFd, VIDIOC_QBUF, &buf);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_QBUF for %d %s.\n",
                errno, strerror(errno));
        return -2;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_DMABUF;
    ret = ioctl(rga_mFd, VIDIOC_DQBUF, &buf);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_DQBUF for %d %s.\n",
                errno, strerror(errno));
        return -3;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_DMABUF;
    ret = ioctl(rga_mFd, VIDIOC_DQBUF, &buf);
    if (ret != 0) {
        rga_log("failed to ioctl VIDIOC_DQBUF for %d %s.\n",
                errno, strerror(errno));
        return -4;
    }

    return 0;
}