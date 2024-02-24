# FPVue_RK3566

This application decodes an RTP video stream using the [Rockchip MPP library](https://github.com/rockchip-linux/mpp) and displays it on screen.
It also displays a simple cairo based OSD that shows the bandwidth, decoding latency, and framerate of the decoded video.

Tested on RK3566 (Radxa Zero 3W).

# Compilation

Build on the RK3566 linux system directly.

## Dependencies
- rockchip_mpp
- pthread
- drm
- cairo

## Command
```
cmake CMakeLists.txt
make
```