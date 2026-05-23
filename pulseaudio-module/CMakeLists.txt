cmake_minimum_required(VERSION 3.10)
project(pulseaudio_android C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")

include_directories(pulseaudio-module)
include_directories(../pulseaudio/build-arm64)
include_directories(../pulseaudio/src)
include_directories(../root-arm64/include)

link_directories(../root-arm64/lib/pulseaudio)
link_directories(../root-arm64/lib)

add_library(module-aaudio-sink SHARED
        module-aaudio-sink.c)

set_target_properties(module-aaudio-sink PROPERTIES
        OUTPUT_NAME "module-aaudio-sink"
        PREFIX ""
        SUFFIX ".so")

target_link_libraries(module-aaudio-sink
        pulsecore-13.0
        pulsecommon-13.0
        pulse
        aaudio)
