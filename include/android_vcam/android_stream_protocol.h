#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace android_vcam {

// Exact scrcpy-server v5.0.1 wire constants
inline constexpr const char* kBundledScrcpyServerVersion = "5.0.1";
inline constexpr size_t kScrcpyDeviceNameFieldLen = 64U;
inline constexpr uint32_t kScrcpyCodecIdH264 = 0x68323634U;  // 'h264'
inline constexpr uint32_t kScrcpyCodecIdRaw  = 0x00726177U;  // '\0raw'
inline constexpr uint32_t kScrcpyStreamDisabledCode = 0x00000000U;
inline constexpr uint32_t kScrcpyStreamErrorCode    = 0x00000001U;

inline constexpr uint64_t kScrcpyPacketFlagSession  = 1ULL << 63;
inline constexpr uint64_t kScrcpyPacketFlagConfig   = 1ULL << 62;
inline constexpr uint64_t kScrcpyPacketFlagKeyFrame = 1ULL << 61;
inline constexpr uint64_t kScrcpyPtsMask            = (1ULL << 61) - 1ULL;

inline constexpr size_t kScrcpyMaxPacketBytes = 16U * 1024U * 1024U;  // 16 MB sanity guard

// Exact JCast Companion APK (USB AOA v2) multiplexed wire constants ('JCAO')
inline constexpr uint32_t kJCastAoaMagic = 0x4A43414FU;  // 'JCAO'
inline constexpr uint8_t kJCastAoaVersion = 1U;
inline constexpr size_t kJCastAoaCtrlFrameBytes = 24U;
inline constexpr size_t kJCastAoaStreamHeaderBytes = 20U;

inline constexpr uint8_t kAoaCmdStartStream = 0x01U;
inline constexpr uint8_t kAoaCmdStopStream  = 0x02U;
inline constexpr uint8_t kAoaCmdPing        = 0x03U;

inline constexpr uint8_t kAoaCtrlFlagEnableAudio     = 1U << 0;
inline constexpr uint8_t kAoaCtrlFlagLockOrientation = 1U << 1;

inline constexpr uint8_t kAoaPktHelloDeviceMeta   = 0x10U;
inline constexpr uint8_t kAoaPktVideoConfigSpsPps = 0x20U;
inline constexpr uint8_t kAoaPktVideoH264Keyframe = 0x21U;
inline constexpr uint8_t kAoaPktVideoH264Frame    = 0x22U;
inline constexpr uint8_t kAoaPktVideoSizeChanged  = 0x23U;
inline constexpr uint8_t kAoaPktAudioPcmS16LE     = 0x30U;
inline constexpr uint8_t kAoaPktAudioUnsupported  = 0x31U;
inline constexpr uint8_t kAoaPktHeartbeat         = 0x40U;

struct AndroidEncodedVideoPacket {
    std::vector<uint8_t> annexb_data;
    uint64_t pts_ns = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool is_config = false;
    bool is_keyframe = false;
};

struct AndroidPcmAudioPacket {
    std::vector<uint8_t> pcm_s16le;  // 48000 Hz, 2ch stereo, 16-bit LE
    uint64_t pts_ns = 0;
};

// Incremental byte-stream parser for the scrcpy v5.0.1 Video TCP socket
class ScrcpyVideoStreamParser {
public:
    using OnDeviceMetaFn = std::function<void(const std::string& device_name)>;
    using OnVideoPacketFn = std::function<void(const AndroidEncodedVideoPacket&)>;

    explicit ScrcpyVideoStreamParser(
        bool expect_dummy_byte = true,
        OnDeviceMetaFn on_device_meta = {},
        OnVideoPacketFn on_video_packet = {});

    void Reset(bool expect_dummy_byte = true);

    bool FeedBytes(std::span<const uint8_t> chunk, std::string* out_error = nullptr);

    const std::string& DeviceName() const { return device_name_; }
    uint32_t CodecId() const { return codec_id_; }
    uint32_t SessionWidth() const { return session_width_; }
    uint32_t SessionHeight() const { return session_height_; }
    uint64_t FramesParsed() const { return frames_parsed_; }

private:
    enum class State {
        WaitDummyByte,
        WaitDeviceMeta,
        WaitCodecHeader,
        WaitRecordHeader,
        WaitPacketPayload,
        Failed,
    };

    State state_ = State::WaitDummyByte;
    OnDeviceMetaFn on_device_meta_;
    OnVideoPacketFn on_video_packet_;

    std::vector<uint8_t> buf_;
    size_t read_pos_ = 0;

    std::string device_name_;
    uint32_t codec_id_ = 0;
    uint32_t session_width_ = 0;
    uint32_t session_height_ = 0;

    uint64_t pending_pts_flags_ = 0;
    uint32_t pending_payload_size_ = 0;

    std::vector<uint8_t> latest_csd_annexb_;
    bool csd_prepended_since_update_ = false;
    uint64_t frames_parsed_ = 0;
};

// Incremental byte-stream parser for the scrcpy v5.0.1 Audio TCP socket
class ScrcpyAudioStreamParser {
public:
    using OnAudioDisabledFn = std::function<void(bool is_error)>;
    using OnAudioPacketFn = std::function<void(const AndroidPcmAudioPacket&)>;

    explicit ScrcpyAudioStreamParser(
        OnAudioDisabledFn on_disabled = {},
        OnAudioPacketFn on_audio_packet = {});

    void Reset();

    bool FeedBytes(std::span<const uint8_t> chunk, std::string* out_error = nullptr);

    bool IsDisabledByDevice() const { return state_ == State::Disabled; }
    uint32_t CodecId() const { return codec_id_; }
    uint64_t PacketsParsed() const { return packets_parsed_; }

private:
    enum class State {
        WaitCodecHeader,
        WaitRecordHeader,
        WaitPacketPayload,
        Disabled,
        Failed,
    };

    State state_ = State::WaitCodecHeader;
    OnAudioDisabledFn on_disabled_;
    OnAudioPacketFn on_audio_packet_;

    std::vector<uint8_t> buf_;
    size_t read_pos_ = 0;

    uint32_t codec_id_ = 0;
    uint64_t pending_pts_flags_ = 0;
    uint32_t pending_payload_size_ = 0;
    uint64_t packets_parsed_ = 0;
};

// Incremental multiplexed byte-stream parser for JCast Companion APK over USB AOA Bulk IN ('JCAO')
class JCastAoaStreamParser {
public:
    using OnDeviceHelloFn = std::function<void(const std::string& model, int sdk_int, uint32_t w, uint32_t h)>;
    using OnVideoPacketFn = std::function<void(const AndroidEncodedVideoPacket&)>;
    using OnAudioPacketFn = std::function<void(const AndroidPcmAudioPacket&)>;
    using OnAudioDisabledFn = std::function<void(int sdk_int)>;

    JCastAoaStreamParser(
        OnDeviceHelloFn on_hello = {},
        OnVideoPacketFn on_video = {},
        OnAudioPacketFn on_audio = {},
        OnAudioDisabledFn on_audio_disabled = {});

    void Reset();

    bool FeedUsbBulkBytes(std::span<const uint8_t> chunk, std::string* out_error = nullptr);

    const std::string& DeviceModel() const { return device_model_; }
    int AndroidSdk() const { return android_sdk_; }
    uint32_t SessionWidth() const { return session_width_; }
    uint32_t SessionHeight() const { return session_height_; }
    uint64_t VideoFramesParsed() const { return video_frames_; }
    uint64_t AudioPacketsParsed() const { return audio_packets_; }

private:
    enum class State {
        WaitHeader,
        WaitPayload,
        Failed,
    };

    State state_ = State::WaitHeader;
    OnDeviceHelloFn on_hello_;
    OnVideoPacketFn on_video_;
    OnAudioPacketFn on_audio_;
    OnAudioDisabledFn on_audio_disabled_;

    std::vector<uint8_t> buf_;
    size_t read_pos_ = 0;

    uint8_t pending_type_ = 0;
    uint64_t pending_meta_ = 0;
    uint32_t pending_len_ = 0;

    std::string device_model_;
    int android_sdk_ = 0;
    uint32_t session_width_ = 0;
    uint32_t session_height_ = 0;
    std::vector<uint8_t> latest_csd_annexb_;
    bool csd_prepended_since_update_ = false;

    uint64_t video_frames_ = 0;
    uint64_t audio_packets_ = 0;
};

// Wire builders for ADB scrcpy & USB AOA Companion protocol unit tests and session control
std::vector<uint8_t> BuildScrcpyVideoWireHeader(
    std::string_view device_name,
    bool include_dummy_byte = true,
    uint32_t codec_id = kScrcpyCodecIdH264);
std::vector<uint8_t> BuildScrcpySessionMetaRecord(uint32_t width, uint32_t height, bool is_client_resize = false);
std::vector<uint8_t> BuildScrcpyFramePacket(
    std::span<const uint8_t> payload,
    uint64_t pts_us,
    bool is_config,
    bool is_keyframe);

std::vector<uint8_t> BuildJCastAoaControlFrame(
    uint8_t cmd,
    uint16_t max_fps,
    uint32_t max_dim,
    uint32_t video_bitrate_bps,
    bool enable_audio,
    bool lock_orientation);

std::vector<uint8_t> BuildJCastAoaStreamPacket(
    uint8_t pkt_type,
    uint64_t pts_us_or_meta,
    std::span<const uint8_t> payload);

}  // namespace android_vcam
