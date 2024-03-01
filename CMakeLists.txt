cmake_minimum_required(VERSION 2.8)

set(CMAKE_C_COMPILER "/usr/bin/gcc")

project(fpvue)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES drm.c
				osd.c
				rtp_frame.c
				main.c)

include_directories("/usr/include/libdrm" "/usr/local/include/rockchip" "/usr/include/cairo")

add_executable(fpvue ${SOURCE_FILES})
target_link_libraries(fpvue rockchip_mpp pthread drm cairo)
