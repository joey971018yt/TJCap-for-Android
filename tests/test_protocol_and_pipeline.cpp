#include "android_vcam/android_stream_protocol.h"
#include "android_vcam/app_updater.h"
#include "android_vcam/diagnostics.h"
#include "android_vcam/image_convert.h"
#include "android_vcam/screenshot_util.h"
#include "android_vcam/shared_ipc.h"
#include "android_vcam/usb_android_probe.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace android_vcam;

#define TEST_CHECK(expr)                                                                         \
    do {                                                                                         \
        if (!(expr)) {                                                                           \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> " << #expr << "\n";    \
            std::exit(1);                                                                        \
        }                                                                                        \
    } while (false)

namespace {

void TestScrcpyVideoStreamParserFragmented() {
    std::cout << "[Test 1] Scrcpy v5.0.1 video wire parser (fragmented chunks, SPS/PPS prepending, dynamic session meta)...\n";

    std::string got_dev_name;
    std::vector<AndroidEncodedVideoPacket> out_pkts;

    ScrcpyVideoStreamParser parser(
        /*expect_dummy_byte=*/true,
        [&](const std::string& name) { got_dev_name = name; },
        [&](const AndroidEncodedVideoPacket& pkt) { out_pkts.push_back(pkt); });

    std::vector<uint8_t> stream = BuildScrcpyVideoWireHeader("Samsung SM-S9280", true, kScrcpyCodecIdH264);

    auto meta1 = BuildScrcpySessionMetaRecord(3120, 1440, false);
    stream.insert(stream.end(), meta1.begin(), meta1.end());

    const std::vector<uint8_t> sps_pps = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x33,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0x80};
    auto cfg_pkt = BuildScrcpyFramePacket(sps_pps, 0, /*is_config=*/true, /*is_keyframe=*/false);
    stream.insert(stream.end(), cfg_pkt.begin(), cfg_pkt.end());

    const std::vector<uint8_t> idr_nalu = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xAA};
    auto idr_pkt = BuildScrcpyFramePacket(idr_nalu, 16667ULL, /*is_config=*/false, /*is_keyframe=*/true);
    stream.insert(stream.end(), idr_pkt.begin(), idr_pkt.end());

    const std::vector<uint8_t> p_nalu = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x12, 0x34};
    auto p_pkt = BuildScrcpyFramePacket(p_nalu, 33333ULL, /*is_config=*/false, /*is_keyframe=*/false);
    stream.insert(stream.end(), p_pkt.begin(), p_pkt.end());

    auto meta2 = BuildScrcpySessionMetaRecord(1440, 3120, false);
    stream.insert(stream.end(), meta2.begin(), meta2.end());
    auto p_pkt2 = BuildScrcpyFramePacket(p_nalu, 50000ULL, /*is_config=*/false, /*is_keyframe=*/false);
    stream.insert(stream.end(), p_pkt2.begin(), p_pkt2.end());

    for (size_t i = 0; i < stream.size(); i += 3) {
        size_t n = std::min<size_t>(3, stream.size() - i);
        std::string err;
        bool ok = parser.FeedBytes(std::span<const uint8_t>(stream.data() + i, n), &err);
        TEST_CHECK(ok && err.empty());
    }

    TEST_CHECK(got_dev_name == "Samsung SM-S9280");
    TEST_CHECK(out_pkts.size() == 3U);

    TEST_CHECK(out_pkts[0].is_keyframe);
    TEST_CHECK(out_pkts[0].width == 3120U && out_pkts[0].height == 1440U);
    TEST_CHECK(out_pkts[0].pts_ns == 16667000ULL);
    TEST_CHECK(out_pkts[0].annexb_data.size() == sps_pps.size() + idr_nalu.size());
    TEST_CHECK(std::memcmp(out_pkts[0].annexb_data.data(), sps_pps.data(), sps_pps.size()) == 0);

    TEST_CHECK(!out_pkts[1].is_keyframe);
    TEST_CHECK(out_pkts[1].annexb_data == p_nalu);

    TEST_CHECK(out_pkts[2].width == 1440U && out_pkts[2].height == 3120U);
    TEST_CHECK(out_pkts[2].pts_ns == 50000000ULL);
}

void TestScrcpyAudioStreamParser() {
    std::cout << "[Test 2] Scrcpy v5.0.1 RAW PCM audio parser & Android < 11 graceful disable...\n";

    std::vector<AndroidPcmAudioPacket> audio_out;
    bool disabled_called = false;

    ScrcpyAudioStreamParser aparser(
        [&](bool /*err*/) { disabled_called = true; },
        [&](const AndroidPcmAudioPacket& pkt) { audio_out.push_back(pkt); });

    std::vector<uint8_t> wire = {0x00, 0x72, 0x61, 0x77};
    std::vector<uint8_t> pcm_bytes(800 * 4, 0x5A);
    auto apkt = BuildScrcpyFramePacket(pcm_bytes, 20000ULL, false, true);
    wire.insert(wire.end(), apkt.begin(), apkt.end());

    std::string err;
    TEST_CHECK(aparser.FeedBytes(wire, &err));
    TEST_CHECK(!disabled_called);
    TEST_CHECK(audio_out.size() == 1U);
    TEST_CHECK(audio_out[0].pcm_s16le.size() == pcm_bytes.size());
    TEST_CHECK(audio_out[0].pts_ns == 20000000ULL);

    aparser.Reset();
    const uint8_t disable_hdr[4] = {0x00, 0x00, 0x00, 0x00};
    TEST_CHECK(aparser.FeedBytes(disable_hdr, &err));
    TEST_CHECK(disabled_called);
    TEST_CHECK(aparser.IsDisabledByDevice());
}

void TestJCastAoaCompanionWireParser() {
    std::cout << "[Test 3] JCast Companion APK + USB AOA ('JCAO') multiplexed AV wire parser...\n";

    auto ctrl = BuildJCastAoaControlFrame(kAoaCmdStartStream, 120, 2560, 24000000, true, true);
    TEST_CHECK(ctrl.size() == kJCastAoaCtrlFrameBytes);
    TEST_CHECK(ctrl[0] == 'J' && ctrl[1] == 'C' && ctrl[2] == 'A' && ctrl[3] == 'O');

    std::string hello_model;
    int hello_sdk = 0;
    std::vector<AndroidEncodedVideoPacket> vpkts;
    std::vector<AndroidPcmAudioPacket> apkts;

    JCastAoaStreamParser aoa_parser(
        [&](const std::string& m, int sdk, uint32_t w, uint32_t h) {
            hello_model = m;
            hello_sdk = sdk;
            TEST_CHECK(w == 2400U && h == 1080U);
        },
        [&](const AndroidEncodedVideoPacket& vp) { vpkts.push_back(vp); },
        [&](const AndroidPcmAudioPacket& ap) { apkts.push_back(ap); });

    std::vector<uint8_t> bulk_stream;

    // 1. Hello packet (76B payload)
    std::vector<uint8_t> hello_body(76, 0);
    const char* mstr = "ASUS ROG Phone 9";
    std::memcpy(hello_body.data(), mstr, std::strlen(mstr));
    hello_body[67] = 35;  // SDK 35
    hello_body[70] = (2400 >> 8) & 0xFF;
    hello_body[71] = 2400 & 0xFF;
    hello_body[74] = (1080 >> 8) & 0xFF;
    hello_body[75] = 1080 & 0xFF;
    auto p_hello = BuildJCastAoaStreamPacket(kAoaPktHelloDeviceMeta, 0, hello_body);
    bulk_stream.insert(bulk_stream.end(), p_hello.begin(), p_hello.end());

    // 2. Heartbeat packet
    auto p_hb = BuildJCastAoaStreamPacket(kAoaPktHeartbeat, 0, {});
    bulk_stream.insert(bulk_stream.end(), p_hb.begin(), p_hb.end());

    // 3. SPS/PPS Config + Keyframe + Audio PCM
    const std::vector<uint8_t> sps_pps = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x28};
    uint64_t wh_meta = (2400ULL << 32) | 1080ULL;
    auto p_csd = BuildJCastAoaStreamPacket(kAoaPktVideoConfigSpsPps, wh_meta, sps_pps);
    bulk_stream.insert(bulk_stream.end(), p_csd.begin(), p_csd.end());

    const std::vector<uint8_t> idr = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB};
    auto p_idr = BuildJCastAoaStreamPacket(kAoaPktVideoH264Keyframe, 8333ULL, idr);
    bulk_stream.insert(bulk_stream.end(), p_idr.begin(), p_idr.end());

    const std::vector<uint8_t> pcm(1024 * 4, 0x11);
    auto p_aud = BuildJCastAoaStreamPacket(kAoaPktAudioPcmS16LE, 10000ULL, pcm);
    bulk_stream.insert(bulk_stream.end(), p_aud.begin(), p_aud.end());

    for (size_t i = 0; i < bulk_stream.size(); i += 7) {
        size_t n = std::min<size_t>(7, bulk_stream.size() - i);
        std::string err;
        TEST_CHECK(aoa_parser.FeedUsbBulkBytes(std::span<const uint8_t>(bulk_stream.data() + i, n), &err));
    }

    TEST_CHECK(hello_model == "ASUS ROG Phone 9" && hello_sdk == 35);
    TEST_CHECK(vpkts.size() == 1U && vpkts[0].is_keyframe);
    TEST_CHECK(vpkts[0].width == 2400U && vpkts[0].height == 1080U);
    TEST_CHECK(vpkts[0].annexb_data.size() == sps_pps.size() + idr.size());
    TEST_CHECK(apkts.size() == 1U && apkts[0].pcm_s16le.size() == pcm.size());
}

void TestAdbDevicesParserAndUsbSpeedGating() {
    std::cout << "[Test 4] ADB devices output parser & USB link speed resolution/bitrate gating...\n";

    const char* raw_adb =
        "* daemon not running; starting now at tcp:5037\n"
        "* daemon started successfully\n"
        "List of devices attached\n"
        "192.168.1.88:5555      device product:husky model:Pixel_8_Pro device:husky transport_id:4\n"
        "R5CW90ABCDE            device usb:1-2 product:e3qzhx model:SM_S9280 device:e3q transport_id:2\n"
        "ABCD123456             unauthorized usb:1-3 transport_id:3\n"
        "OFF998877              offline transport_id:5\n";

    auto list = ParseAdbDevicesOutput(raw_adb);
    TEST_CHECK(list.size() == 4U);
    TEST_CHECK(list[0].is_wireless && list[0].state == AdbDeviceState::Authorized && list[0].model == "Pixel 8 Pro");
    TEST_CHECK(!list[1].is_wireless && list[1].state == AdbDeviceState::Authorized && list[1].model == "SM S9280");
    TEST_CHECK(list[2].state == AdbDeviceState::Unauthorized);
    TEST_CHECK(list[3].state == AdbDeviceState::Offline);

    TEST_CHECK(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::SuperPlus_10Gbps) == 3840U);
    TEST_CHECK(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::Super_5Gbps) == 3840U);
    TEST_CHECK(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::High_480Mbps) == 2560U);
    TEST_CHECK(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::Full_12Mbps) == 1280U);

    TEST_CHECK(RecommendedBitrateBps(3840, 60, UsbLinkSpeed::SuperPlus_10Gbps) == 32000000U);
    TEST_CHECK(RecommendedBitrateBps(2560, 60, UsbLinkSpeed::High_480Mbps) == 18000000U);
}

void TestSharedMemoryIpcAndImagePipeline() {
    std::cout << "[Test 5] Lock-free SharedCapture IPC (NV12 + 48kHz PCM + Preview BGRA32 + Health Telemetry)...\n";

    const std::string test_shm = "JCastAndroidUnitTestShmV2";
    SharedCaptureProducer prod;
    SharedCaptureConsumer cons;
    TEST_CHECK(prod.Open(test_shm));
    TEST_CHECK(cons.Open(test_shm));

    uint32_t out_w = 0, out_h = 0;
    ClampAdaptiveDimensionsToMax(3120, 1440, 2560, out_w, out_h);
    TEST_CHECK(out_w == 2560U && (out_h % 2U) == 0U);

    uint32_t slot = 0;
    uint8_t* dst = prod.AcquireBackbufferSlot(slot);
    TEST_CHECK(dst != nullptr);
    GenerateAnimatedColorBarsNV12(dst, 1280, 720, 5);

    FrameTelemetryMeta meta{};
    meta.fps_x100 = 6000U;
    meta.decode_latency_us = 1800U;
    meta.pipeline_latency_us = 2600U;
    meta.stream_flags = kStreamFlagHwAccelerated | kStreamFlagAdaptiveRes;
    prod.AddEncodedVideoBytes(36000);
    prod.PublishBackbufferSlot(slot, 1280, 720, 16666667ULL, 2400, 1080, 18000U, meta);

    std::vector<int16_t> samples(960 * 2, 1234);
    prod.PushAudioPcmS16LE(samples.data(), 960);
    prod.SetAudioPacketCount(1);

    std::vector<uint8_t> preview_bgra;
    uint64_t seq = 0;
    uint32_t fit_w = 0, fit_h = 0, src_w = 0, src_h = 0;
    bool updated = false;
    TEST_CHECK(cons.ReadLatestPreviewBGRA32(
        640, 360, false, seq, preview_bgra, fit_w, fit_h, src_w, src_h, &updated));
    TEST_CHECK(updated && fit_w == 640U && fit_h == 360U && src_w == 1280U && src_h == 720U);

    std::vector<uint8_t> full_bgra;
    uint32_t fw = 0, fh = 0;
    TEST_CHECK(cons.CaptureLatestFullResBGRA32(full_bgra, fw, fh));
    TEST_CHECK(fw == 1280U && fh == 720U && full_bgra.size() == 1280ULL * 720ULL * 4ULL);

    std::vector<int16_t> read_pcm(960 * 2, 0);
    size_t got_frames = cons.ReadAudioPcmS16LE(read_pcm.data(), 960);
    TEST_CHECK(got_frames == 480U && read_pcm[0] == 1234);
    prod.PushAudioPcmS16LE(samples.data(), 960);
    got_frames = cons.ReadAudioPcmS16LE(read_pcm.data(), 960);
    TEST_CHECK(got_frames == 960U && read_pcm[0] == 1234);

    StreamHealthSnapshot snap;
    TEST_CHECK(cons.ReadStreamHealth(snap));
    TEST_CHECK(snap.active && snap.hw_accelerated && snap.frame_width == 1280U && snap.source_width == 2400U);

    cons.Close();
    prod.Close();
}

void TestUpdaterAndDiagnosticsRedaction() {
    std::cout << "[Test 6] GitHub Release JSON parser, diagnostics error classification & privacy redaction...\n";

    TEST_CHECK(IsRemoteVersionNewer("1.0.0", "v1.1.0"));
    TEST_CHECK(!IsRemoteVersionNewer("1.0.0", "v1.0.0"));

    const char* sample_json = R"({
        "tag_name": "v1.1.0",
        "name": "JCast for Android v1.1.0",
        "html_url": "https://github.com/joey971018yt/android-usb-vcam/releases/tag/v1.1.0",
        "published_at": "2026-11-28T00:00:00Z",
        "draft": false,
        "prerelease": false,
        "body": "Bug fixes",
        "assets": [
            {
                "name": "JCast-for-Android-Setup-v1.1.0.exe",
                "browser_download_url": "https://github.com/joey971018yt/android-usb-vcam/releases/download/v1.1.0/JCast-for-Android-Setup-v1.1.0.exe",
                "size": 12345678,
                "digest": "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            },
            {
                "name": "JCast-for-Android-Portable-v1.1.0.zip",
                "browser_download_url": "https://github.com/joey971018yt/android-usb-vcam/releases/download/v1.1.0/JCast-for-Android-Portable-v1.1.0.zip",
                "size": 9876543,
                "digest": "sha256:fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
            }
        ]
    })";

    GitHubReleaseInfo rel;
    std::string jerr;
    TEST_CHECK(ParseGitHubReleaseJson(sample_json, rel, &jerr));
    TEST_CHECK(rel.setup_exe.name == "JCast-for-Android-Setup-v1.1.0.exe");
    TEST_CHECK(rel.portable_zip.name == "JCast-for-Android-Portable-v1.1.0.zip");
    TEST_CHECK(rel.setup_exe.sha256_hex.size() == 64U);

    TEST_CHECK(ClassifyUsbOrTransportError("Android device is unauthorized") == DiagCode::E301_UsbDebugUnauthorized);
    TEST_CHECK(ClassifyUsbOrTransportError("adb server version (41) doesn't match") == DiagCode::E202_AdbVersionConflict);

    std::string line = FormatHostDiagLine(DiagCode::E301_UsbDebugUnauthorized, DiagSeverity::Warning, "請點擊允許");
    ParsedDiagEvent pev;
    TEST_CHECK(ParseHostDiagLine(line, pev));
    TEST_CHECK(pev.code == DiagCode::E301_UsbDebugUnauthorized);

    std::string sensitive = "Path C:\\Users\\JoeyLin\\Desktop\\JCast and adb -s R5CW90887766 push ok";
    std::string redacted = RedactSensitiveReportText(sensitive);
    TEST_CHECK(redacted.find("JoeyLin") == std::string::npos);
    TEST_CHECK(redacted.find("C:\\Users\\<USER>\\Desktop") != std::string::npos);
    TEST_CHECK(redacted.find("R5CW90887766") == std::string::npos);
    TEST_CHECK(redacted.find("R5C****-REDACTED") != std::string::npos);

    std::string issue_url = BuildGitHubNewIssueUrl(
        "joey971018yt", "android-usb-vcam", "1.0.0", DiagCode::E301_UsbDebugUnauthorized, sensitive);
    TEST_CHECK(issue_url.find("https://github.com/joey971018yt/android-usb-vcam/issues/new?") == 0U);
    TEST_CHECK(issue_url.find("JoeyLin") == std::string::npos);
}

}  // namespace

int main() {
    TestScrcpyVideoStreamParserFragmented();
    TestScrcpyAudioStreamParser();
    TestJCastAoaCompanionWireParser();
    TestAdbDevicesParserAndUsbSpeedGating();
    TestSharedMemoryIpcAndImagePipeline();
    TestUpdaterAndDiagnosticsRedaction();
    std::cout << "\nAll JCast for Android (ADB + Companion AOA) protocol, IPC, USB gating, updater & diagnostics unit tests PASSED!\n";
    return 0;
}
