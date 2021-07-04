cmake_minimum_required(VERSION 3.5)

project(vaapi_test LANGUAGES C)

add_definitions(-DHAVE_VA_DRM -DMAKE_MAIN
    )

add_executable(vaapi_test
    ../h264encoder.c
    ../va_display.c
    ../va_display_drm.c
    ../va_display.h
    ../loadsurface.h
    ../loadsurface_yuv.h
    ../va_h264.h
    )

target_link_libraries(vaapi_test
    m
    va
    va-drm
    pthread
    )
