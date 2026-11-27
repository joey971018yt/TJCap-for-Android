cmake_minimum_required(VERSION 3.20)
project(android_usb_vcam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /wd4324 /utf-8 /EHsc /guard:cf /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS)
    add_link_options(/DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT /GUARD:CF)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    if(WIN32)
        add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
        add_link_options(-Wl,--dynamicbase -Wl,--high-entropy-va -Wl,--nxcompat)
    endif()
endif()

# Core cross-platform protocol, USB/ADB probe parser, image scaling, shared-memory IPC, updater, screenshot, and diagnostics library
add_library(android_vcam_core STATIC
    src/protocol/android_stream_protocol.cpp
    src/usb/usb_android_probe.cpp
    src/common/image_convert.cpp
    src/common/shared_ipc.cpp
    src/common/app_updater.cpp
    src/common/screenshot_util.cpp
    src/common/diagnostics.cpp
)
target_include_directories(android_vcam_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX AND NOT APPLE)
    target_link_libraries(android_vcam_core PUBLIC pthread rt)
endif()
if(WIN32)
    target_link_libraries(android_vcam_core PUBLIC setupapi cfgmgr32)
endif()

# Automated unit tests (buildable & runnable on both Linux and Windows)
add_executable(android_vcam_tests
    tests/test_protocol_and_pipeline.cpp
)
target_link_libraries(android_vcam_tests PRIVATE android_vcam_core)
if(WIN32)
    target_link_libraries(android_vcam_tests PRIVATE winhttp bcrypt shell32 ole32 user32 windowscodecs setupapi cfgmgr32)
endif()
if(MINGW)
    target_link_options(android_vcam_tests PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME protocol_and_pipeline_tests COMMAND android_vcam_tests)

# Windows-native targets: DirectShow Virtual Capture Card DLL, Host Daemon, Driver Setup, and GUI App
if(WIN32)
    # 1. DirectShow Video + Audio Virtual Capture Card DLL
    add_library(android_vcam_dshow SHARED
        src/dshow/dll_main.cpp
        src/dshow/video_capture_filter.cpp
        src/dshow/audio_capture_filter.cpp
        src/dshow/android_vcam_dshow.def
        src/common/binary_version.rc
    )
    target_include_directories(android_vcam_dshow PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dshow
    )
    target_link_libraries(android_vcam_dshow PRIVATE
        android_vcam_core
        strmiids
        ole32
        oleaut32
        uuid
        advapi32
        user32
        gdi32
        winmm
    )
    set_target_properties(android_vcam_dshow PROPERTIES
        PREFIX ""
        OUTPUT_NAME "android_vcam_dshow"
    )
    if(MINGW)
        target_link_options(android_vcam_dshow PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
            -Wl,--kill-at
        )
    endif()

    # 2. Android USB-C Capture, ADB Tunnel, Hardware H.264 Decoder & WASAPI Audio Router Host Executable
    add_executable(android_vcam_host
        src/host/host_main.cpp
        src/usb/adb_stream_session.cpp
        src/usb/usb_aoa_session.cpp
        src/decoder/mf_h264_decoder.cpp
        src/decoder/wasapi_renderer.cpp
        src/common/binary_version.rc
    )
    target_include_directories(android_vcam_host PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(android_vcam_host PRIVATE
        android_vcam_core
        ws2_32
        ole32
        oleaut32
        uuid
        mfplat
        mfuuid
        d3d11
        dxgi
        avrt
        winmm
        advapi32
        user32
    )
    if(MINGW)
        target_link_options(android_vcam_host PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    # 3. DirectShow Registration & Bundled ADB Service Refresh Tool
    add_executable(android_vcam_driver_setup
        src/driver_tool/driver_setup_main.cpp
        src/common/binary_version.rc
    )
    target_include_directories(android_vcam_driver_setup PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(android_vcam_driver_setup PRIVATE
        android_vcam_core
        setupapi
        cfgmgr32
        advapi32
        ole32
        shell32
        user32
    )
    if(MINGW)
        target_link_options(android_vcam_driver_setup PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    # 4. One-Click Green-Themed Graphical App (android_vcam_gui.exe / TJCap for Android.exe)
    set(ANDROID_VCAM_GUI_EXTRA_SRCS src/gui/android_vcam_gui.rc)
    if(MSVC)
        list(APPEND ANDROID_VCAM_GUI_EXTRA_SRCS src/gui/android_vcam_gui.manifest)
    endif()
    add_executable(android_vcam_gui WIN32
        src/gui/gui_main.cpp
        src/decoder/wasapi_renderer.cpp
        ${ANDROID_VCAM_GUI_EXTRA_SRCS}
    )
    target_include_directories(android_vcam_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(android_vcam_gui PRIVATE
        android_vcam_core
        setupapi
        cfgmgr32
        comctl32
        gdi32
        user32
        shell32
        advapi32
        ole32
        oleaut32
        uuid
        winmm
        winhttp
        bcrypt
        windowscodecs
    )
    if(MINGW)
        target_compile_definitions(android_vcam_gui PRIVATE ANDROID_VCAM_EMBED_RC_MANIFEST=1)
        target_link_options(android_vcam_gui PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()
