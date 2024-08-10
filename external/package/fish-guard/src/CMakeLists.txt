cmake_minimum_required(VERSION 3.1)

project(fish-guard)

file(GLOB SRCS "*.cc")

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/../../host/aarch64-buildroot-linux-gnu/sysroot/usr/include/gstreamer-1.0
  ${CMAKE_CURRENT_SOURCE_DIR}/../../host/aarch64-buildroot-linux-gnu/sysroot/usr/include/glib-2.0
  ${CMAKE_CURRENT_SOURCE_DIR}/../../host/aarch64-buildroot-linux-gnu/sysroot/usr/lib/glib-2.0/include
)

add_executable(fish-guard ${SRCS})

target_link_libraries(fish-guard peer pthread gstreamer-1.0 gobject-2.0 INIReader wpa_client)

install(TARGETS fish-guard DESTINATION bin)
