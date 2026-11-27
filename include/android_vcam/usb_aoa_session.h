#pragma once

#include "android_vcam/adb_stream_session.h"
#include "android_vcam/android_stream_protocol.h"
#include "android_vcam/usb_android_probe.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace android_vcam {

// Android Open Accessory (AOA v1/v2) USB identity matching companion-apk/app/src/main/res/xml/accessory_filter.xml
inline constexpr const char* kAoaManufacturer = "TJCap Project";
inline constexpr const char* kAoaModel        = "TJCap for Android";
inline constexpr const char* kAoaDescription  = "TJCap USB-C Virtual Capture Card";
inline constexpr const char* kAoaVersion      = "1.0";
inline constexpr const char* kAoaUri          = "https://github.com/joey971018yt/TJCap-for-Android";
inline constexpr const char* kAoaSerial       = "TJCAP-AOA-001";

struct UsbAoaSessionConfig {
    std::filesystem::path base_dir;
    uint32_t max_size = 2560U;
    uint32_t max_fps = 60U;
    uint32_t video_bit_rate = 20000000U;
    bool lock_initial_orientation = true;
    bool enable_audio = true;
};

class UsbAoaSession {
public:
    using OnVideoPacketFn = std::function<void(const AndroidEncodedVideoPacket&)>;
    using OnAudioPacketFn = std::function<void(const AndroidPcmAudioPacket&)>;
    using OnAudioDisabledFn = std::function<void(int android_sdk, bool is_error)>;

    UsbAoaSession();
    ~UsbAoaSession();

    UsbAoaSession(const UsbAoaSession&) = delete;
    UsbAoaSession& operator=(const UsbAoaSession&) = delete;

    // Loads libusb-1.0.dll, switches any attached Android phone into Google AOA Accessory Mode
    // (0x18D1:0x2D00 / 0x2D01) via control requests 51/52/53 if needed, claims Bulk endpoints,
    // and sends the 24-byte JCAO START_STREAM configuration frame to TJCap Companion APK.
    bool OpenAndNegotiateAoa(
        const UsbAoaSessionConfig& cfg,
        ConnectedAndroidInfo& out_device_info,
        std::string* out_error = nullptr);

    // Runs Bulk IN streaming loop decoding JCAO multiplexed video + 48kHz PCM audio packets.
    bool RunBulkStreamLoop(
        const std::atomic<bool>& stop_flag,
        const OnVideoPacketFn& on_video,
        const OnAudioPacketFn& on_audio,
        const OnAudioDisabledFn& on_audio_disabled,
        std::string* out_error = nullptr);

    // Sends CMD_STOP_STREAM and releases USB AOA interface cleanly.
    void StopAndClose();

    uint64_t AudioPacketsReceived() const { return audio_packets_.load(std::memory_order_relaxed); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::mutex write_mu_;
    std::atomic<uint64_t> audio_packets_{0};
};

}  // namespace android_vcam
