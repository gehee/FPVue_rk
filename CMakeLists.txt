cmake_minimum_required(VERSION 2.8)

set(CMAKE_C_COMPILER "/usr/bin/gcc")

project(fpvue)

set(CMAKE_CXX_STANDARD 11)

add_compile_options("-Wno-address-of-packed-member")

set(SOURCE_FILES drm.c
				osd.c
				rtp.c
				mavlink.c
				main.c)

include_directories("/usr/include/libdrm" "/usr/include/cairo")

add_executable(fpvue ${SOURCE_FILES})
target_link_libraries(fpvue rockchip_mpp pthread drm m cairo)
