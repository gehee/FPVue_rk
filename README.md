# FPVue_rk

WFB-ng client (Video Decoder) for Rockchip platform powered by the [Rockchip MPP library](https://github.com/rockchip-linux/mpp).
It also displays a simple cairo based OSD that shows the bandwidth, decoding latency, and framerate of the decoded video.

Tested on RK3566 (Radxa Zero 3W) and RK3588s (Orange Pi 5).

# Compilation

Build on the Rockchip linux system directly.

## Install dependencies
- rockchip_mpp
```
git clone https://github.com/rockchip-linux/mpp.git
cmake CMakeLists.txt
make
sudo make install
```
- drm, cairo 
```
sudo apt install libdrm-dev libcairo-dev
```

## Build Instructions
```
cmake CMakeLists.txt
make
```

### Run

```
./fpvue --osd
```

### Known issues

1. Crashes when video feed resolution is higher than the screen resolution.