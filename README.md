# FPVue_RK3566

This application decodes an RTP video stream using the [Rockchip MPP library](https://github.com/rockchip-linux/mpp) and displays it on screen.
It also displays a simple cairo based OSD that shows the bandwidth, decoding latency, and framerate of the decoded video.

Tested on RK3566 (Radxa Zero 3W).

# Compilation

Build on the RK3566 linux system directly.

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

- without OSD
```
./fpvue
```
- with OSD
```
./fpvue --osd
```

### Known issues

1. OSD does not load if the program is not started from the project root folder
2. OSD is flickering
3. Properly support full screen and scaling video