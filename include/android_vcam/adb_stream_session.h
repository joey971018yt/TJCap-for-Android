#pragma once

#include "android_vcam/android_stream_protocol.h"
#include "android_vcam/usb_android_probe.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace android_vcam {

struct AdbSessionConfig {
    std::filesystem::path adb_exe_path;
    std::filesystem::path server_jar_path;
    std::string preferred_serial;

    uint32_t max_size = 2560U;        // Locked session max long-edge
    uint32_t max_fps = 60U;           // 60 or 120
    uint32_t video_bit_rate = 20000000U;
    bool lock_initial_orientation = true;  // true => capture_orientation=@ (locked canvas, zero OBS flicker)
    bool enable_audio = true;
};

struct ConnectedAndroidInfo {
    std::string serial;
    std::string display_model;
    int android_sdk = 0;
    std::string android_release;
    UsbLinkSpeed usb_speed = UsbLinkSpeed::Unknown;
};

class AdbStreamSession {
public:
    using OnVideoPacketFn = std::function<void(const AndroidEncodedVideoPacket&)>;
    using OnAudioPacketFn = std::function<void(const AndroidPcmAudioPacket&)>;
    using OnAudioDisabledFn = std::function<void(int android_sdk, bool is_error)>;

    AdbStreamSession();
    ~AdbStreamSession();

    AdbStreamSession(const AdbStreamSession&) = delete;
    AdbStreamSession& operator=(const AdbStreamSession&) = delete;

    // Discovers an authorized USB Android device, queries SDK/model + USB link speed,
    // pushes tjcap-server.jar to /data/local/tmp/, establishes `adb forward`, and launches app_process.
    bool ConnectAndStart(
        const AdbSessionConfig& cfg,
        ConnectedAndroidInfo& out_device_info,
        std::string* out_error = nullptr);

    // Runs dual-socket (Video H.264 + Audio 48kHz Stereo s16le PCM) streaming until stop_flag or disconnect.
    bool RunStreamLoop(
        const std::atomic<bool>& stop_flag,
        const OnVideoPacketFn& on_video,
        const OnAudioPacketFn& on_audio,
        const OnAudioDisabledFn& on_audio_disabled,
        std::string* out_error = nullptr);

    // Closes sockets, terminates remote server process, and removes `adb forward` rule cleanly.
    void StopAndCleanup();

    uint64_t AudioPacketsReceived() const { return audio_packets_.load(std::memory_order_relaxed); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::atomic<uint64_t> audio_packets_{0};
};

}  // namespace android_vcam
