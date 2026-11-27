#include "android_vcam/android_stream_protocol.h"

#include <algorithm>
#include <cstring>

namespace android_vcam {

namespace {

uint32_t ReadBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t ReadBe64(const uint8_t* p) {
    return (static_cast<uint64_t>(ReadBe32(p)) << 32) |
           static_cast<uint64_t>(ReadBe32(p + 4));
}

void WriteBe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>(v & 0xFFU));
}

void WriteBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFU));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFU));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>(v & 0xFFU));
}

void WriteBe64(std::vector<uint8_t>& out, uint64_t v) {
    WriteBe32(out, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFULL));
    WriteBe32(out, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
}

bool ContainsH264SpsNalu(std::span<const uint8_t> data) {
    for (size_t i = 0; i + 4 < data.size(); ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            size_t nal_idx = 0;
            if (data[i + 2] == 0x01) {
                nal_idx = i + 3;
            } else if (data[i + 2] == 0x00 && i + 4 < data.size() && data[i + 3] == 0x01) {
                nal_idx = i + 4;
            }
            if (nal_idx > 0 && nal_idx < data.size()) {
                uint8_t nal_type = data[nal_idx] & 0x1FU;
                if (nal_type == 7U) {
                    return true;
                }
            }
        }
    }
    return false;
}

void CompactReadBuffer(std::vector<uint8_t>& buf, size_t& read_pos) {
    if (read_pos == 0) return;
    if (read_pos >= buf.size()) {
        buf.clear();
        read_pos = 0;
    } else if (read_pos >= 65536U || read_pos * 2U >= buf.size()) {
        size_t rem = buf.size() - read_pos;
        std::memmove(buf.data(), buf.data() + read_pos, rem);
        buf.resize(rem);
        read_pos = 0;
    }
}

}  // namespace

ScrcpyVideoStreamParser::ScrcpyVideoStreamParser(
    bool expect_dummy_byte,
    OnDeviceMetaFn on_device_meta,
    OnVideoPacketFn on_video_packet)
    : on_device_meta_(std::move(on_device_meta)),
      on_video_packet_(std::move(on_video_packet)) {
    Reset(expect_dummy_byte);
}

void ScrcpyVideoStreamParser::Reset(bool expect_dummy_byte) {
    state_ = expect_dummy_byte ? State::WaitDummyByte : State::WaitDeviceMeta;
    buf_.clear();
    read_pos_ = 0;
    device_name_.clear();
    codec_id_ = 0;
    session_width_ = 0;
    session_height_ = 0;
    pending_pts_flags_ = 0;
    pending_payload_size_ = 0;
    latest_csd_annexb_.clear();
    csd_prepended_since_update_ = false;
    frames_parsed_ = 0;
}

bool ScrcpyVideoStreamParser::FeedBytes(std::span<const uint8_t> chunk, std::string* out_error) {
    if (state_ == State::Failed) {
        if (out_error) *out_error = "Video stream parser is in failed state.";
        return false;
    }
    if (chunk.empty()) {
        return true;
    }

    buf_.insert(buf_.end(), chunk.begin(), chunk.end());

    while (true) {
        const size_t avail = buf_.size() - read_pos_;
        const uint8_t* ptr = buf_.data() + read_pos_;

        if (state_ == State::WaitDummyByte) {
            if (avail < 1U) break;
            read_pos_ += 1U;
            state_ = State::WaitDeviceMeta;
            continue;
        }

        if (state_ == State::WaitDeviceMeta) {
            if (avail < kScrcpyDeviceNameFieldLen) break;
            size_t len = 0;
            while (len < kScrcpyDeviceNameFieldLen && ptr[len] != 0) {
                ++len;
            }
            device_name_.assign(reinterpret_cast<const char*>(ptr), len);
            read_pos_ += kScrcpyDeviceNameFieldLen;
            if (on_device_meta_) {
                on_device_meta_(device_name_);
            }
            state_ = State::WaitCodecHeader;
            continue;
        }

        if (state_ == State::WaitCodecHeader) {
            if (avail < 4U) break;
            codec_id_ = ReadBe32(ptr);
            read_pos_ += 4U;
            if (codec_id_ == kScrcpyStreamDisabledCode || codec_id_ == kScrcpyStreamErrorCode) {
                state_ = State::Failed;
                if (out_error) {
                    *out_error = "Android video encoder reported failure on device (codec header=" +
                                 std::to_string(codec_id_) + ").";
                }
                return false;
            }
            if (codec_id_ != kScrcpyCodecIdH264) {
                state_ = State::Failed;
                if (out_error) {
                    *out_error = "Unexpected video codec ID from server (expected H.264 0x68323634).";
                }
                return false;
            }
            state_ = State::WaitRecordHeader;
            continue;
        }

        if (state_ == State::WaitRecordHeader) {
            if (avail < 12U) break;
            uint64_t hdr64 = ReadBe64(ptr);
            uint32_t word2 = ReadBe32(ptr + 8);
            read_pos_ += 12U;

            if ((hdr64 & kScrcpyPacketFlagSession) != 0ULL) {
                uint32_t new_w = static_cast<uint32_t>(hdr64 & 0xFFFFFFFFULL);
                uint32_t new_h = word2;
                if (new_w > 0 && new_w <= 8192U && new_h > 0 && new_h <= 8192U) {
                    session_width_ = new_w;
                    session_height_ = new_h;
                }
                continue;
            }

            if (word2 > kScrcpyMaxPacketBytes) {
                state_ = State::Failed;
                if (out_error) {
                    *out_error = "Corrupt video packet length (" + std::to_string(word2) + " bytes).";
                }
                return false;
            }
            pending_pts_flags_ = hdr64;
            pending_payload_size_ = word2;
            state_ = State::WaitPacketPayload;
            continue;
        }

        if (state_ == State::WaitPacketPayload) {
            if (avail < pending_payload_size_) break;

            const bool is_cfg = (pending_pts_flags_ & kScrcpyPacketFlagConfig) != 0ULL;
            const bool is_key = (pending_pts_flags_ & kScrcpyPacketFlagKeyFrame) != 0ULL;
            const uint64_t pts_us = is_cfg ? 0ULL : (pending_pts_flags_ & kScrcpyPtsMask);

            std::span<const uint8_t> raw_payload(ptr, pending_payload_size_);
            read_pos_ += pending_payload_size_;
            state_ = State::WaitRecordHeader;

            if (is_cfg) {
                latest_csd_annexb_.assign(raw_payload.begin(), raw_payload.end());
                csd_prepended_since_update_ = false;
                continue;
            }

            AndroidEncodedVideoPacket pkt;
            pkt.pts_ns = pts_us * 1000ULL;
            pkt.width = session_width_;
            pkt.height = session_height_;
            pkt.is_config = false;
            pkt.is_keyframe = is_key;

            if (!latest_csd_annexb_.empty() &&
                (is_key || !csd_prepended_since_update_) &&
                !ContainsH264SpsNalu(raw_payload)) {
                pkt.annexb_data.reserve(latest_csd_annexb_.size() + raw_payload.size());
                pkt.annexb_data.insert(
                    pkt.annexb_data.end(), latest_csd_annexb_.begin(), latest_csd_annexb_.end());
                pkt.annexb_data.insert(
                    pkt.annexb_data.end(), raw_payload.begin(), raw_payload.end());
                csd_prepended_since_update_ = true;
            } else {
                pkt.annexb_data.assign(raw_payload.begin(), raw_payload.end());
            }

            ++frames_parsed_;
            if (on_video_packet_) {
                on_video_packet_(pkt);
            }
            continue;
        }

        break;
    }

    CompactReadBuffer(buf_, read_pos_);
    return true;
}

ScrcpyAudioStreamParser::ScrcpyAudioStreamParser(
    OnAudioDisabledFn on_disabled,
    OnAudioPacketFn on_audio_packet)
    : on_disabled_(std::move(on_disabled)),
      on_audio_packet_(std::move(on_audio_packet)) {
    Reset();
}

void ScrcpyAudioStreamParser::Reset() {
    state_ = State::WaitCodecHeader;
    buf_.clear();
    read_pos_ = 0;
    codec_id_ = 0;
    pending_pts_flags_ = 0;
    pending_payload_size_ = 0;
    packets_parsed_ = 0;
}

bool ScrcpyAudioStreamParser::FeedBytes(std::span<const uint8_t> chunk, std::string* out_error) {
    if (state_ == State::Disabled) {
        return true;
    }
    if (state_ == State::Failed) {
        if (out_error) *out_error = "Audio stream parser is in failed state.";
        return false;
    }
    if (chunk.empty()) {
        return true;
    }

    buf_.insert(buf_.end(), chunk.begin(), chunk.end());

    while (true) {
        const size_t avail = buf_.size() - read_pos_;
        const uint8_t* ptr = buf_.data() + read_pos_;

        if (state_ == State::WaitCodecHeader) {
            if (avail < 4U) break;
            codec_id_ = ReadBe32(ptr);
            read_pos_ += 4U;

            if (codec_id_ == kScrcpyStreamDisabledCode || codec_id_ == kScrcpyStreamErrorCode) {
                state_ = State::Disabled;
                buf_.clear();
                read_pos_ = 0;
                if (on_disabled_) {
                    on_disabled_(codec_id_ == kScrcpyStreamErrorCode);
                }
                return true;
            }
            if (codec_id_ != kScrcpyCodecIdRaw) {
                state_ = State::Failed;
                if (out_error) {
                    *out_error = "Unexpected audio codec ID (expected RAW PCM 0x00726177).";
                }
                return false;
            }
            state_ = State::WaitRecordHeader;
            continue;
        }

        if (state_ == State::WaitRecordHeader) {
            if (avail < 12U) break;
            uint64_t hdr64 = ReadBe64(ptr);
            uint32_t word2 = ReadBe32(ptr + 8);
            read_pos_ += 12U;

            if ((hdr64 & kScrcpyPacketFlagSession) != 0ULL) {
                continue;
            }
            if (word2 > kScrcpyMaxPacketBytes) {
                state_ = State::Failed;
                if (out_error) {
                    *out_error = "Corrupt audio packet length (" + std::to_string(word2) + " bytes).";
                }
                return false;
            }
            pending_pts_flags_ = hdr64;
            pending_payload_size_ = word2;
            state_ = State::WaitPacketPayload;
            continue;
        }

        if (state_ == State::WaitPacketPayload) {
            if (avail < pending_payload_size_) break;

            const bool is_cfg = (pending_pts_flags_ & kScrcpyPacketFlagConfig) != 0ULL;
            const uint64_t pts_us = is_cfg ? 0ULL : (pending_pts_flags_ & kScrcpyPtsMask);

            std::span<const uint8_t> raw_payload(ptr, pending_payload_size_);
            read_pos_ += pending_payload_size_;
            state_ = State::WaitRecordHeader;

            if (!is_cfg && !raw_payload.empty()) {
                size_t aligned_bytes = raw_payload.size() & ~static_cast<size_t>(3U);
                if (aligned_bytes > 0) {
                    AndroidPcmAudioPacket apkt;
                    apkt.pts_ns = pts_us * 1000ULL;
                    apkt.pcm_s16le.assign(raw_payload.begin(), raw_payload.begin() + aligned_bytes);
                    ++packets_parsed_;
                    if (on_audio_packet_) {
                        on_audio_packet_(apkt);
                    }
                }
            }
            continue;
        }

        break;
    }

    CompactReadBuffer(buf_, read_pos_);
    return true;
}

TJCapAoaStreamParser::TJCapAoaStreamParser(
    OnDeviceHelloFn on_hello,
    OnVideoPacketFn on_video,
    OnAudioPacketFn on_audio,
    OnAudioDisabledFn on_audio_disabled)
    : on_hello_(std::move(on_hello)),
      on_video_(std::move(on_video)),
      on_audio_(std::move(on_audio)),
      on_audio_disabled_(std::move(on_audio_disabled)) {
    Reset();
}

void TJCapAoaStreamParser::Reset() {
    state_ = State::WaitHeader;
    buf_.clear();
    read_pos_ = 0;
    pending_type_ = 0;
    pending_meta_ = 0;
    pending_len_ = 0;
    device_model_.clear();
    android_sdk_ = 0;
    session_width_ = 0;
    session_height_ = 0;
    latest_csd_annexb_.clear();
    csd_prepended_since_update_ = false;
    video_frames_ = 0;
    audio_packets_ = 0;
}

bool TJCapAoaStreamParser::FeedUsbBulkBytes(std::span<const uint8_t> chunk, std::string* out_error) {
    if (state_ == State::Failed) {
        if (out_error) *out_error = "AOA stream parser is in failed state.";
        return false;
    }
    if (chunk.empty()) {
        return true;
    }

    buf_.insert(buf_.end(), chunk.begin(), chunk.end());

    while (true) {
        const size_t avail = buf_.size() - read_pos_;
        const uint8_t* ptr = buf_.data() + read_pos_;

        if (state_ == State::WaitHeader) {
            if (avail < kTJCapAoaStreamHeaderBytes) break;

            uint32_t magic = ReadBe32(ptr);
            if (magic != kTJCapAoaMagic) {
                // Resynchronize scan up to 4KB in case of leftover stale USB FIFO byte
                size_t sync_offset = 1U;
                bool resynced = false;
                while (sync_offset + 4U <= avail && sync_offset < 4096U) {
                    if (ReadBe32(ptr + sync_offset) == kTJCapAoaMagic) {
                        read_pos_ += sync_offset;
                        resynced = true;
                        break;
                    }
                    ++sync_offset;
                }
                if (resynced) {
                    continue;
                }
                state_ = State::Failed;
                if (out_error) {
                    *out_error = "Invalid TJCap AOA packet magic (expected 'JCAO').";
                }
                return false;
            }

            pending_type_ = ptr[4];
            pending_meta_ = ReadBe64(ptr + 8);
            pending_len_ = ReadBe32(ptr + 16);
            read_pos_ += kTJCapAoaStreamHeaderBytes;

            if (pending_len_ > kScrcpyMaxPacketBytes) {
                state_ = State::Failed;
                if (out_error) {
                    *out_error = "AOA packet payload exceeds safety limit (" + std::to_string(pending_len_) + ").";
                }
                return false;
            }

            state_ = State::WaitPayload;
            continue;
        }

        if (state_ == State::WaitPayload) {
            if (avail < pending_len_) break;

            std::span<const uint8_t> payload(ptr, pending_len_);
            read_pos_ += pending_len_;
            state_ = State::WaitHeader;

            switch (pending_type_) {
                case kAoaPktHelloDeviceMeta: {
                    if (payload.size() >= 76U) {
                        size_t nlen = 0;
                        while (nlen < 64U && payload[nlen] != 0) ++nlen;
                        device_model_.assign(reinterpret_cast<const char*>(payload.data()), nlen);
                        android_sdk_ = static_cast<int>(ReadBe32(payload.data() + 64));
                        uint32_t w = ReadBe32(payload.data() + 68);
                        uint32_t h = ReadBe32(payload.data() + 72);
                        if (w > 0 && w <= 8192U && h > 0 && h <= 8192U) {
                            session_width_ = w;
                            session_height_ = h;
                        }
                        if (on_hello_) {
                            on_hello_(device_model_, android_sdk_, session_width_, session_height_);
                        }
                    }
                    break;
                }

                case kAoaPktVideoSizeChanged: {
                    if (payload.size() >= 8U) {
                        uint32_t w = ReadBe32(payload.data());
                        uint32_t h = ReadBe32(payload.data() + 4);
                        if (w > 0 && w <= 8192U && h > 0 && h <= 8192U) {
                            session_width_ = w;
                            session_height_ = h;
                        }
                    }
                    break;
                }

                case kAoaPktVideoConfigSpsPps: {
                    uint32_t w = static_cast<uint32_t>((pending_meta_ >> 32) & 0xFFFFFFFFULL);
                    uint32_t h = static_cast<uint32_t>(pending_meta_ & 0xFFFFFFFFULL);
                    if (w > 0 && w <= 8192U && h > 0 && h <= 8192U) {
                        session_width_ = w;
                        session_height_ = h;
                    }
                    latest_csd_annexb_.assign(payload.begin(), payload.end());
                    csd_prepended_since_update_ = false;
                    break;
                }

                case kAoaPktVideoH264Keyframe:
                case kAoaPktVideoH264Frame: {
                    if (payload.empty()) break;
                    bool is_key = (pending_type_ == kAoaPktVideoH264Keyframe);
                    AndroidEncodedVideoPacket vpkt;
                    vpkt.pts_ns = pending_meta_ * 1000ULL;
                    vpkt.width = session_width_;
                    vpkt.height = session_height_;
                    vpkt.is_config = false;
                    vpkt.is_keyframe = is_key;

                    if (!latest_csd_annexb_.empty() &&
                        (is_key || !csd_prepended_since_update_) &&
                        !ContainsH264SpsNalu(payload)) {
                        vpkt.annexb_data.reserve(latest_csd_annexb_.size() + payload.size());
                        vpkt.annexb_data.insert(
                            vpkt.annexb_data.end(), latest_csd_annexb_.begin(), latest_csd_annexb_.end());
                        vpkt.annexb_data.insert(
                            vpkt.annexb_data.end(), payload.begin(), payload.end());
                        csd_prepended_since_update_ = true;
                    } else {
                        vpkt.annexb_data.assign(payload.begin(), payload.end());
                    }

                    ++video_frames_;
                    if (on_video_) {
                        on_video_(vpkt);
                    }
                    break;
                }

                case kAoaPktAudioPcmS16LE: {
                    size_t aligned = payload.size() & ~static_cast<size_t>(3U);
                    if (aligned > 0) {
                        AndroidPcmAudioPacket apkt;
                        apkt.pts_ns = pending_meta_ * 1000ULL;
                        apkt.pcm_s16le.assign(payload.begin(), payload.begin() + aligned);
                        ++audio_packets_;
                        if (on_audio_) {
                            on_audio_(apkt);
                        }
                    }
                    break;
                }

                case kAoaPktAudioUnsupported: {
                    if (on_audio_disabled_) {
                        on_audio_disabled_(android_sdk_);
                    }
                    break;
                }

                case kAoaPktHeartbeat:
                default:
                    break;
            }
            continue;
        }

        break;
    }

    CompactReadBuffer(buf_, read_pos_);
    return true;
}

std::vector<uint8_t> BuildScrcpyVideoWireHeader(
    std::string_view device_name,
    bool include_dummy_byte,
    uint32_t codec_id) {
    std::vector<uint8_t> out;
    out.reserve((include_dummy_byte ? 1U : 0U) + kScrcpyDeviceNameFieldLen + 4U);
    if (include_dummy_byte) {
        out.push_back(0x00);
    }
    size_t copy_len = std::min(device_name.size(), kScrcpyDeviceNameFieldLen - 1U);
    for (size_t i = 0; i < copy_len; ++i) {
        out.push_back(static_cast<uint8_t>(device_name[i]));
    }
    while (out.size() < (include_dummy_byte ? 1U : 0U) + kScrcpyDeviceNameFieldLen) {
        out.push_back(0x00);
    }
    WriteBe32(out, codec_id);
    return out;
}

std::vector<uint8_t> BuildScrcpySessionMetaRecord(uint32_t width, uint32_t height, bool is_client_resize) {
    std::vector<uint8_t> out;
    out.reserve(12U);
    uint32_t flags = 0x80000000U | (is_client_resize ? 1U : 0U);
    WriteBe32(out, flags);
    WriteBe32(out, width);
    WriteBe32(out, height);
    return out;
}

std::vector<uint8_t> BuildScrcpyFramePacket(
    std::span<const uint8_t> payload,
    uint64_t pts_us,
    bool is_config,
    bool is_keyframe) {
    std::vector<uint8_t> out;
    out.reserve(12U + payload.size());
    uint64_t pts_flags = 0;
    if (is_config) {
        pts_flags = kScrcpyPacketFlagConfig;
    } else {
        pts_flags = (pts_us & kScrcpyPtsMask);
        if (is_keyframe) {
            pts_flags |= kScrcpyPacketFlagKeyFrame;
        }
    }
    WriteBe64(out, pts_flags);
    WriteBe32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> BuildTJCapAoaControlFrame(
    uint8_t cmd,
    uint16_t max_fps,
    uint32_t max_dim,
    uint32_t video_bitrate_bps,
    bool enable_audio,
    bool lock_orientation) {
    std::vector<uint8_t> out;
    out.reserve(kTJCapAoaCtrlFrameBytes);
    WriteBe32(out, kTJCapAoaMagic);
    out.push_back(kTJCapAoaVersion);
    out.push_back(cmd);
    WriteBe16(out, max_fps);
    WriteBe32(out, max_dim);
    WriteBe32(out, video_bitrate_bps);
    uint8_t flags = 0;
    if (enable_audio) flags |= kAoaCtrlFlagEnableAudio;
    if (lock_orientation) flags |= kAoaCtrlFlagLockOrientation;
    out.push_back(flags);
    while (out.size() < kTJCapAoaCtrlFrameBytes) {
        out.push_back(0);
    }
    return out;
}

std::vector<uint8_t> BuildTJCapAoaStreamPacket(
    uint8_t pkt_type,
    uint64_t pts_us_or_meta,
    std::span<const uint8_t> payload) {
    std::vector<uint8_t> out;
    out.reserve(kTJCapAoaStreamHeaderBytes + payload.size());
    WriteBe32(out, kTJCapAoaMagic);
    out.push_back(pkt_type);
    out.push_back(0);
    WriteBe16(out, 0);
    WriteBe64(out, pts_us_or_meta);
    WriteBe32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace android_vcam
