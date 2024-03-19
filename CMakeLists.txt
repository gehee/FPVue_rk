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

add_executable(${PROJECT_NAME} ${SOURCE_FILES})
target_link_libraries(${PROJECT_NAME} rockchip_mpp pthread drm m cairo)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(
    CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -Werror -fsanitize=undefined -fsanitize=address"
  )
  target_link_options(${PROJECT_NAME}
    BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
  )
endif()

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
