# FPVue_rk

> [!IMPORTANT]
> August 2024: Unfortunately, this repository is now in an unmaintained state.
> I don't have the time to maitain it like I would like to and there was not enough interest from external developers to contribute to it.
> Feel free to fork it and take over but for now it will stay as is.

WFB-ng client (Video Decoder) for Rockchip platform powered by the [Rockchip MPP library](https://github.com/rockchip-linux/mpp).
It also displays a simple cairo based OSD that shows the bandwidth, decoding latency, and framerate of the decoded video, and wfb-ng link statistics.

Tested on RK3566 (Radxa Zero 3W) and RK3588s (Orange Pi 5).

# Compilation

Build on the Rockchip linux system directly.

## Install dependencies

- rockchip_mpp

```
git clone https://github.com/rockchip-linux/mpp.git
cmake -B build
sudo cmake --build build --target install
```

- drm, cairo

```
sudo apt install libdrm-dev libcairo-dev
```

- gstreamer

```
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev
```

## Build Instructions

Build and run application in production environment:

```
cmake -B build
sudo cmake --build build --target install
build/fpvue
```

Build and run application for debugging purposes:

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
build/fpvue --osd
```

### Usage

Show command line options:
```
fpvue --help
```

### Known issues

1. Video is cropped when the fpv feed resolution is bigger than the screen mode.
1. Crashes when video feed resolution is higher than the screen resolution.
