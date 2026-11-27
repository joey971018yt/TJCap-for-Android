#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace android_vcam {

inline constexpr uint32_t kShmMagic = 0x4156434DU;  // 'AVCM'
inline constexpr uint32_t kShmVersion = 2U;

inline constexpr uint32_t kDefaultCanvasWidth = 1920U;
inline constexpr uint32_t kDefaultCanvasHeight = 1080U;

// Supports up to 3840x2560 or 2560x3840 (covers Galaxy S24/S25 Ultra 3120x1440, Pixel Pro 2992x1344, Galaxy Tab Ultra 2960x1848, 4K UHD)
inline constexpr uint32_t kMaxFrameDimension = 3840U;
inline constexpr size_t kMaxVideoPixels = 3840ULL * 2560ULL;
inline constexpr size_t kMaxNv12FrameBytes = kMaxVideoPixels * 3ULL / 2ULL;
inline constexpr size_t kVideoSlotCount = 3U;

// Aligns arbitrary device dimensions to 4-pixel width & 2-pixel height for DirectShow NV12/YUY2/RGB24 compatibility
inline void AlignAdaptiveDimensions(uint32_t in_w, uint32_t in_h, uint32_t& out_w, uint32_t& out_h) {
    if (in_w < 64 || in_h < 64) {
        out_w = kDefaultCanvasWidth;
        out_h = kDefaultCanvasHeight;
        return;
    }
    uint32_t w = std::min(in_w, kMaxFrameDimension);
    uint32_t h = std::min(in_h, kMaxFrameDimension);
    if (static_cast<uint64_t>(w) * h > kMaxVideoPixels) {
        double scale = 0.999;
        while (static_cast<uint64_t>(w * scale) * static_cast<uint64_t>(h * scale) > kMaxVideoPixels) {
            scale *= 0.95;
        }
        w = static_cast<uint32_t>(w * scale);
        h = static_cast<uint32_t>(h * scale);
    }
    out_w = std::max(64U, (w + 2U) & ~3U);
    out_h = std::max(64U, h & ~1U);
}

// Clamps source dimensions proportionally so neither width nor height exceeds max_long_edge, then 4x2-aligns
inline void ClampAdaptiveDimensionsToMax(
    uint32_t in_w,
    uint32_t in_h,
    uint32_t max_long_edge,
    uint32_t& out_w,
    uint32_t& out_h) {
    if (in_w < 64 || in_h < 64) {
        out_w = kDefaultCanvasWidth;
        out_h = kDefaultCanvasHeight;
        return;
    }
    uint32_t cap = std::clamp(max_long_edge, 320U, kMaxFrameDimension);
    uint32_t cur_max = std::max(in_w, in_h);
    uint32_t scaled_w = in_w;
    uint32_t scaled_h = in_h;
    if (cur_max > cap) {
        scaled_w = static_cast<uint32_t>((static_cast<uint64_t>(in_w) * cap + cur_max / 2U) / cur_max);
        scaled_h = static_cast<uint32_t>((static_cast<uint64_t>(in_h) * cap + cur_max / 2U) / cur_max);
    }
    AlignAdaptiveDimensions(scaled_w, scaled_h, out_w, out_h);
}

// 48000 Hz, 16-bit signed stereo (4 bytes per stereo sample frame)
inline constexpr uint32_t kAudioSampleRate = 48000U;
inline constexpr uint32_t kAudioChannels = 2U;
inline constexpr uint32_t kAudioBytesPerFrame = 4U;
inline constexpr uint64_t kAudioRingFrames = 65536ULL;  // Power of two (~1.365s buffer)
inline constexpr uint64_t kAudioRingMask = kAudioRingFrames - 1ULL;

inline constexpr const char* kDefaultShmName = "AndroidUsbVcamSharedMemV2";

// Bitflags for VideoSlotHeader::stream_flags
inline constexpr uint32_t kStreamFlagHwAccelerated = 1U << 0;
inline constexpr uint32_t kStreamFlagMockSource    = 1U << 1;
inline constexpr uint32_t kStreamFlagAdaptiveRes   = 1U << 2;
inline constexpr uint32_t kStreamFlagHighRefresh   = 1U << 3;
// Bits 8..11 encode static_cast<uint32_t>(UsbLinkSpeed) & 0xFU

enum class StreamHealthGrade : uint8_t {
    Idle = 0,           // Capture host not running
    WaitingStream = 1,  // Host running, waiting for Android frames
    Excellent = 2,      // Smooth low-latency streaming
    StaticVfr = 3,      // Normal Android SurfaceFlinger VFR power-saving on untouched static screen
    Warning = 4,        // High decode latency (>14ms) or bandwidth/frame drop bottleneck
};

struct FrameTelemetryMeta {
    uint32_t fps_x100 = 0;
    uint32_t decode_latency_us = 0;
    uint32_t pipeline_latency_us = 0;
    uint32_t stream_flags = 0;
    uint32_t dropped_frames = 0;
};

struct StreamHealthSnapshot {
    bool active = false;
    bool waiting_first_frame = false;
    StreamHealthGrade grade = StreamHealthGrade::Idle;

    double fps = 0.0;
    double bitrate_mbps = 0.0;
    double peak_bitrate_mbps = 0.0;
    double decode_latency_ms = 0.0;
    double pipeline_latency_ms = 0.0;

    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;

    uint64_t total_frames = 0;
    uint64_t total_video_bytes = 0;
    uint64_t total_audio_packets = 0;
    uint32_t dropped_frames = 0;
    uint64_t last_frame_age_ms = 0;

    bool hw_accelerated = false;
    bool mock_source = false;
    bool adaptive_res = false;
    bool high_refresh = false;
    uint8_t usb_speed_code = 0;
};

StreamHealthGrade EvaluateStreamHealthGrade(const StreamHealthSnapshot& s);

struct alignas(64) VideoSlotHeader {
    uint64_t frame_seq = 0;
    uint64_t pts_ns = 0;
    uint32_t frame_width = kDefaultCanvasWidth;
    uint32_t frame_height = kDefaultCanvasHeight;
    uint32_t source_width = kDefaultCanvasWidth;
    uint32_t source_height = kDefaultCanvasHeight;
    uint32_t bitrate_kbps = 0;
    uint32_t fps_x100 = 0;
    uint32_t decode_latency_us = 0;
    uint32_t pipeline_latency_us = 0;
    uint32_t stream_flags = 0;
    uint32_t dropped_frames = 0;
    uint64_t publish_mono_ms = 0;
};
static_assert(sizeof(VideoSlotHeader) == 64, "VideoSlotHeader must remain 64 bytes for IPC ABI compatibility");

struct alignas(64) SharedCaptureLayout {
    uint32_t magic = 0;
    uint32_t version = 0;

    std::atomic<uint64_t> producer_heartbeat_ms{0};
    std::atomic<uint32_t> producer_streaming{0};

    // Active output dimensions advertised to newly connecting DirectShow consumers (e.g. OBS Device Default)
    std::atomic<uint32_t> active_width{2560U};
    std::atomic<uint32_t> active_height{1440U};

    // Triple-buffered lock-free video state + real-time telemetry counters
    std::atomic<uint32_t> latest_video_slot{0};
    std::atomic<uint64_t> total_video_frames{0};
    std::atomic<uint64_t> total_video_bytes{0};
    std::atomic<uint64_t> total_audio_packets{0};
    std::atomic<uint32_t> live_bitrate_kbps{0};
    std::atomic<uint32_t> live_fps_x100{0};

    VideoSlotHeader slot_headers[kVideoSlotCount];
    alignas(64) uint8_t video_slots[kVideoSlotCount][kMaxNv12FrameBytes];

    // Lock-free SPMC (Single-Producer Multi-Consumer) 48kHz stereo LPCM ring buffer
    std::atomic<uint64_t> audio_write_frame_pos{0};
    alignas(64) int16_t audio_ring_samples[kAudioRingFrames * kAudioChannels];
};

uint64_t MonotonicMillisNow();

class SharedCaptureProducer {
public:
    SharedCaptureProducer() = default;
    ~SharedCaptureProducer();

    SharedCaptureProducer(const SharedCaptureProducer&) = delete;
    SharedCaptureProducer& operator=(const SharedCaptureProducer&) = delete;

    bool Open(const std::string& shm_name = kDefaultShmName);
    void Close();
    bool IsOpen() const { return layout_ != nullptr; }

    void SetAdvertisedDimensions(uint32_t width, uint32_t height);
    void AddEncodedVideoBytes(uint64_t byte_count);
    void SetAudioPacketCount(uint64_t packet_count);

    uint8_t* AcquireBackbufferSlot(uint32_t& out_slot_index);
    void PublishBackbufferSlot(
        uint32_t slot_index,
        uint32_t frame_width,
        uint32_t frame_height,
        uint64_t pts_ns,
        uint32_t src_width,
        uint32_t src_height,
        uint32_t bitrate_kbps = 0,
        const FrameTelemetryMeta& meta = {});

    void PublishNV12Frame(
        const uint8_t* nv12_data,
        uint32_t frame_width,
        uint32_t frame_height,
        uint64_t pts_ns,
        uint32_t src_width = 0,
        uint32_t src_height = 0,
        uint32_t bitrate_kbps = 0,
        const FrameTelemetryMeta& meta = {});

    void PushAudioPcmS16LE(const int16_t* stereo_samples, size_t stereo_frame_count);

    void TouchHeartbeat(bool active_streaming);

private:
    SharedCaptureLayout* layout_ = nullptr;
    uint32_t write_slot_cursor_ = 1;
    std::string shm_name_;
#ifdef _WIN32
    void* mapping_handle_ = nullptr;
#else
    int shm_fd_ = -1;
    bool owns_unlink_ = false;
#endif
};

class SharedCaptureConsumer {
public:
    SharedCaptureConsumer() = default;
    ~SharedCaptureConsumer();

    SharedCaptureConsumer(const SharedCaptureConsumer&) = delete;
    SharedCaptureConsumer& operator=(const SharedCaptureConsumer&) = delete;

    bool Open(const std::string& shm_name = kDefaultShmName);
    void Close();
    bool IsOpen() const { return layout_ != nullptr; }

    bool IsProducerAlive(uint64_t max_age_ms = 1500ULL) const;

    bool QueryAdvertisedDimensions(uint32_t& out_width, uint32_t& out_height) const;

    bool ReadLatestNV12Frame(
        uint8_t* out_nv12,
        size_t out_capacity,
        uint32_t& out_frame_w,
        uint32_t& out_frame_h,
        uint64_t& out_frame_seq,
        uint64_t& out_pts_ns,
        uint32_t* out_src_w = nullptr,
        uint32_t* out_src_h = nullptr);

    bool ReadLatestPreviewBGRA32(
        uint32_t box_w,
        uint32_t box_h,
        bool force_refresh,
        uint64_t& io_last_seq,
        std::vector<uint8_t>& out_bgra_topdown,
        uint32_t& out_fit_w,
        uint32_t& out_fit_h,
        uint32_t& out_src_w,
        uint32_t& out_src_h,
        bool* out_pixels_updated = nullptr,
        uint32_t* out_bitrate_kbps = nullptr);

    bool CaptureLatestFullResBGRA32(
        std::vector<uint8_t>& out_bgra_topdown,
        uint32_t& out_width,
        uint32_t& out_height);

    bool ReadStreamHealth(StreamHealthSnapshot& out_snapshot);

    size_t ReadAudioPcmS16LE(int16_t* out_stereo_samples, size_t requested_stereo_frames);

private:
    SharedCaptureLayout* layout_ = nullptr;
    uint64_t audio_read_cursor_ = 0;
    bool audio_cursor_initialized_ = false;

    uint64_t telem_prev_mono_ms_ = 0;
    uint64_t telem_prev_frames_ = 0;
    uint64_t telem_prev_bytes_ = 0;
    double telem_ema_fps_ = 0.0;
    double telem_ema_mbps_ = 0.0;
    double telem_peak_mbps_ = 0.0;
#ifdef _WIN32
    void* mapping_handle_ = nullptr;
#else
    int shm_fd_ = -1;
#endif
};

}  // namespace android_vcam
