cmake_minimum_required(VERSION 3.16)
project(ebridge_host CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ebridge_host STATIC src/host.cpp src/dsu_server.cpp)
target_include_directories(ebridge_host
    PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../common)
find_package(Threads REQUIRED)
target_link_libraries(ebridge_host PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(ebridge_host PUBLIC ws2_32)
endif()

# Video: FFmpeg's encoders (libavcodec + libswscale) and, on Windows, Media
# Foundation, which needs nothing beyond Windows itself.
option(EBRIDGE_VIDEO "Enable video streaming" ON)
if(EBRIDGE_VIDEO AND WIN32)
    target_sources(ebridge_host PRIVATE src/video_encoder.cpp src/video_encoder_mf.cpp src/nv12.cpp)
    target_compile_definitions(ebridge_host PRIVATE EBRIDGE_HAVE_VIDEO)
    target_link_libraries(ebridge_host PRIVATE mfplat mfuuid ole32 oleaut32)
    message(STATUS "ebridge: video through Media Foundation")
endif()
if(EBRIDGE_VIDEO)
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(EBRIDGE_FFMPEG IMPORTED_TARGET libavcodec libavutil libswscale)
    endif()
    if(EBRIDGE_FFMPEG_FOUND)
        target_sources(ebridge_host PRIVATE src/video_encoder.cpp)
        target_compile_definitions(ebridge_host PRIVATE EBRIDGE_HAVE_FFMPEG EBRIDGE_HAVE_VIDEO)
        target_link_libraries(ebridge_host PRIVATE PkgConfig::EBRIDGE_FFMPEG)
        message(STATUS "ebridge: video enabled (FFmpeg ${EBRIDGE_FFMPEG_libavcodec_VERSION})")
    elseif(NOT WIN32)
        message(WARNING "ebridge: FFmpeg not found, building without video")
    endif()
endif()

# Audio needs libopus.
option(EBRIDGE_AUDIO "Enable audio streaming (needs libopus)" ON)
if(EBRIDGE_AUDIO)
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(EBRIDGE_OPUS IMPORTED_TARGET opus)
    endif()
    if(EBRIDGE_OPUS_FOUND)
        target_compile_definitions(ebridge_host PRIVATE EBRIDGE_HAVE_OPUS)
        target_link_libraries(ebridge_host PRIVATE PkgConfig::EBRIDGE_OPUS)
        message(STATUS "ebridge: audio enabled (Opus ${EBRIDGE_OPUS_VERSION})")
    else()
        message(WARNING "ebridge: libopus not found, building without audio")
    endif()
endif()

# Stand-alone test host: shows a test pattern that follows the left stick,
# prints input, rumbles on A.
add_executable(ebridge_host_cli tools/ebridge_host_cli.cpp)
target_link_libraries(ebridge_host_cli PRIVATE ebridge_host)

# External host: plays Eglantine on the Switch without modifying the emulator
# (captures its window, feeds the controls through Eglantine's DSU support).
if(WIN32)
    add_executable(eglantine_bridge tools/eglantine_bridge.cpp tools/window_capture.cpp
                                    tools/audio_capture.cpp)
    target_link_libraries(eglantine_bridge PRIVATE ebridge_host d3d11 dxgi ole32)
endif()
