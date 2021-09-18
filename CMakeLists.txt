cmake_minimum_required(VERSION 3.5)

set(CMAKE_VERBOSE_MAKEFILE ON)
project(v4l2-demo-01 LANGUAGES C)

set(DEPENDENTS_DIR "${PROJECT_SOURCE_DIR}/dependents")

add_library(avfilter STATIC IMPORTED)
set_target_properties(avfilter PROPERTIES
    IMPORTED_LOCATION "${DEPENDENTS_DIR}/lib/libavfilter.a")

add_library(swscale STATIC IMPORTED)
set_target_properties(swscale PROPERTIES
    IMPORTED_LOCATION "${DEPENDENTS_DIR}/lib/libswscale.a")

add_library(avformat STATIC IMPORTED)
set_target_properties(avformat PROPERTIES
    IMPORTED_LOCATION "${DEPENDENTS_DIR}/lib/libavformat.a")

add_library(avcodec STATIC IMPORTED)
set_target_properties(avcodec PROPERTIES
    IMPORTED_LOCATION "${DEPENDENTS_DIR}/lib/libavcodec.a")

add_library(avutil STATIC IMPORTED)
set_target_properties(avutil PROPERTIES
    IMPORTED_LOCATION "${DEPENDENTS_DIR}/lib/libavutil.a")

add_library(x264 STATIC IMPORTED)
set_target_properties(x264 PROPERTIES
    IMPORTED_LOCATION "${DEPENDENTS_DIR}/lib/libx264.a")

set(TARGET_DEPENDENCY_LDFLAGS)
list(APPEND TARGET_DEPENDENCY_LDFLAGS
    avfilter swscale avformat avcodec avutil
    x264 pthread m dl
)


include_directories("${DEPENDENTS_DIR}/include")


add_executable(v4l2-demo-01 main.c
    encode.c
    v4l2.c
)
target_link_libraries(v4l2-demo-01
    ${TARGET_DEPENDENCY_LDFLAGS}
)
