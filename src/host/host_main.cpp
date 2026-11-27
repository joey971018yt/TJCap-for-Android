#include "android_vcam/adb_stream_session.h"
#include "android_vcam/diagnostics.h"
#include "android_vcam/image_convert.h"
#include "android_vcam/mf_h264_decoder.h"
#include "android_vcam/shared_ipc.h"
#include "android_vcam/usb_android_probe.h"
#include "android_vcam/usb_aoa_session.h"
#include "android_vcam/wasapi_renderer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

using namespace android_vcam;

std::atomic<bool> g_stop_flag{false};

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        g_stop_flag.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

std::filesystem::path HostExeDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2 - 1);
    return std::filesystem::path(buf).parent_path();
}
#else
void SignalHandler(int) {
    g_stop_flag.store(true, std::memory_order_release);
}

std::filesystem::path HostExeDir() {
    return std::filesystem::current_path();
}
#endif

enum class ResolutionMode {
    NativeAdaptive,
    Fixed1440p,
    Fixed1080p,
    Fixed720p,
};

enum class ConnectionMode {
    AdbZeroApk,       // Default: USB Debugging (ADB + app_process, zero APK)
    UsbAoaCompanion,  // Fallback: No USB Debugging (TJCap Companion APK + MediaProjection + USB AOA)
    SmartAuto,        // Auto: Prefer ADB if USB Debugging active, else switch to USB AOA Companion
};

struct HostOptions {
    bool mock_mode = false;
    bool lock_orientation = true;
    bool prefer_hw_decode = true;
    bool enable_audio_route = true;
    bool list_audio_only = false;
    ConnectionMode conn_mode = ConnectionMode::AdbZeroApk;
    ResolutionMode res_mode = ResolutionMode::NativeAdaptive;
    uint32_t explicit_max_dim = 0;
    uint32_t max_fps = 60U;
    uint32_t explicit_bitrate_mbps = 0;
    std::string preferred_serial;
    std::string audio_out_match;
    std::string stop_event_name;
};

void PrintUsage() {
    std::cout
        << "TJCap for Android — Core Capture Host (android_vcam_host.exe)\n"
        << "Usage: android_vcam_host.exe [options]\n\n"
        << "Options:\n"
        << "  --conn <adb|aoa|auto>  Connection transport mode (default: adb):\n"
        << "                           adb  : Zero-APK USB Debugging direct mode\n"
        << "                           aoa  : No-USB-Debugging TJCap Companion APK + USB AOA mode\n"
        << "                           auto : Prefer ADB, automatically fall back to USB AOA\n"
        << "  --res <mode>           Resolution & aspect mode (native / native1080 / 1440p / 1080p / 720p)\n"
        << "  --max-dim <px>         Lock max transmission long-edge (960 / 1280 / 1920 / 2560 / 3840)\n"
        << "  --fps <60|90|120>      Target maximum frame rate (default: 60)\n"
        << "  --orient <lock|auto>   Orientation policy: 'lock' (default) or 'auto'\n"
        << "  --bitrate-mbps <mbps>  Explicit H.264 target bitrate in Mbps\n"
        << "  --mock                 Run self-test pipeline (color bars + 48kHz stereo tone)\n"
        << "  --serial <id>          Connect to a specific Android device serial substring\n"
        << "  --audio-out <name>     Route 48kHz audio via WASAPI to endpoint matching <name>\n"
        << "  --no-audio-route       Disable automatic WASAPI virtual cable audio routing\n"
        << "  --list-audio           List active Windows audio playback endpoints and exit\n"
        << "  --sw-decode            Force CPU H.264 decoding instead of D3D11VA hardware acceleration\n"
        << "  --stop-event <name>    Win32 named event for graceful GUI stop signaling\n"
        << "  --help                 Show this help message\n";
}

bool ParseArgs(int argc, char** argv, HostOptions& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return false;
        } else if (arg == "--mock") {
            opts.mock_mode = true;
        } else if (arg == "--conn" && i + 1 < argc) {
            std::string c = argv[++i];
            if (c == "aoa" || c == "companion") {
                opts.conn_mode = ConnectionMode::UsbAoaCompanion;
            } else if (c == "auto") {
                opts.conn_mode = ConnectionMode::SmartAuto;
            } else {
                opts.conn_mode = ConnectionMode::AdbZeroApk;
            }
        } else if (arg == "--res" && i + 1 < argc) {
            std::string r = argv[++i];
            if (r == "native1080") {
                opts.res_mode = ResolutionMode::NativeAdaptive;
                if (opts.explicit_max_dim == 0) opts.explicit_max_dim = 1920U;
            } else if (r == "1440p" || r == "2k") {
                opts.res_mode = ResolutionMode::Fixed1440p;
            } else if (r == "1080p" || r == "fhd") {
                opts.res_mode = ResolutionMode::Fixed1080p;
            } else if (r == "720p" || r == "hd") {
                opts.res_mode = ResolutionMode::Fixed720p;
            } else {
                opts.res_mode = ResolutionMode::NativeAdaptive;
            }
        } else if (arg == "--max-dim" && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            if (v >= 320 && v <= 3840) opts.explicit_max_dim = static_cast<uint32_t>(v);
        } else if (arg == "--fps" && i + 1 < argc) {
            int f = std::atoi(argv[++i]);
            if (f >= 30 && f <= 144) opts.max_fps = static_cast<uint32_t>(f);
        } else if (arg == "--orient" && i + 1 < argc) {
            std::string o = argv[++i];
            opts.lock_orientation = (o != "auto" && o != "unlocked");
        } else if (arg == "--bitrate-mbps" && i + 1 < argc) {
            int b = std::atoi(argv[++i]);
            if (b >= 2 && b <= 80) opts.explicit_bitrate_mbps = static_cast<uint32_t>(b);
        } else if ((arg == "--serial" || arg == "--udid") && i + 1 < argc) {
            opts.preferred_serial = argv[++i];
        } else if (arg == "--audio-out" && i + 1 < argc) {
            opts.audio_out_match = argv[++i];
        } else if (arg == "--no-audio-route") {
            opts.enable_audio_route = false;
        } else if (arg == "--list-audio") {
            opts.list_audio_only = true;
        } else if (arg == "--sw-decode") {
            opts.prefer_hw_decode = false;
        } else if (arg == "--stop-event" && i + 1 < argc) {
            opts.stop_event_name = argv[++i];
        }
    }
    return true;
}

int RunMockPipeline(SharedCaptureProducer& producer, const HostOptions& opts) {
    uint32_t mock_w = 2560U, mock_h = 1440U;
    if (opts.res_mode == ResolutionMode::Fixed720p || (opts.explicit_max_dim > 0 && opts.explicit_max_dim <= 1280U)) {
        mock_w = 1280U;
        mock_h = 720U;
    } else if (opts.res_mode == ResolutionMode::Fixed1080p ||
               (opts.explicit_max_dim > 0 && opts.explicit_max_dim <= 1920U)) {
        mock_w = 1920U;
        mock_h = 1080U;
    }
    producer.SetAdvertisedDimensions(mock_w, mock_h);

    std::cout << "[Host] Starting --mock self-test generator (" << mock_w << "x" << mock_h
              << "@60fps NV12 + 48kHz Stereo LPCM)...\n"
              << "[Host] Open OBS / Discord / Zoom and select 'TJCap for Android (Video)'!\n";

    constexpr uint64_t kFrameIntervalNs = 16666667ULL;
    constexpr size_t kAudioFramesPerVideoFrame = 800;
    std::vector<int16_t> pcm_chunk(kAudioFramesPerVideoFrame * 2);

    uint64_t frame_idx = 0;
    double phase_l = 0.0;
    double phase_r = 0.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr uint64_t kMockSynthBytesPerFrame = 37500ULL;
    uint32_t ema_pipe_us = 450U;

    auto next_tick = std::chrono::steady_clock::now();

    while (!g_stop_flag.load(std::memory_order_relaxed)) {
        auto t0 = std::chrono::steady_clock::now();
        uint32_t slot = 0;
        uint8_t* dst_nv12 = producer.AcquireBackbufferSlot(slot);
        if (dst_nv12) {
            GenerateAnimatedColorBarsNV12(dst_nv12, mock_w, mock_h, frame_idx);
            auto t1 = std::chrono::steady_clock::now();
            uint32_t cur_us = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
            ema_pipe_us = (frame_idx == 0) ? cur_us : ((ema_pipe_us * 3U + cur_us) / 4U);

            producer.AddEncodedVideoBytes(kMockSynthBytesPerFrame);
            FrameTelemetryMeta meta{};
            meta.fps_x100 = 6000U;
            meta.decode_latency_us = ema_pipe_us / 2U;
            meta.pipeline_latency_us = std::max(100U, ema_pipe_us);
            meta.stream_flags = kStreamFlagHwAccelerated | kStreamFlagMockSource | kStreamFlagAdaptiveRes;
            meta.dropped_frames = 0U;

            producer.PublishBackbufferSlot(
                slot, mock_w, mock_h, frame_idx * kFrameIntervalNs, mock_w, mock_h, 18000U, meta);
        }

        for (size_t i = 0; i < kAudioFramesPerVideoFrame; ++i) {
            pcm_chunk[i * 2] = static_cast<int16_t>(std::sin(phase_l) * 4000.0);
            pcm_chunk[i * 2 + 1] = static_cast<int16_t>(std::sin(phase_r) * 4000.0);
            phase_l += kTwoPi * 440.0 / 48000.0;
            phase_r += kTwoPi * 554.365 / 48000.0;
            if (phase_l > kTwoPi) phase_l -= kTwoPi;
            if (phase_r > kTwoPi) phase_r -= kTwoPi;
        }
        producer.PushAudioPcmS16LE(pcm_chunk.data(), kAudioFramesPerVideoFrame);
        producer.SetAudioPacketCount(frame_idx + 1ULL);

        ++frame_idx;
        if (frame_idx % 300 == 0) {
            std::cout << "[Mock] Published " << frame_idx << " frames (" << mock_w << "x" << mock_h
                      << "@60fps, Latency: " << (ema_pipe_us / 1000.0) << " ms, 48kHz Stereo)\n";
        }

        next_tick += std::chrono::nanoseconds(kFrameIntervalNs);
        std::this_thread::sleep_until(next_tick);
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

#ifdef _WIN32
    SetDllDirectoryW(L"");
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    timeBeginPeriod(1);
#else
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
#endif

    HostOptions opts;
    if (!ParseArgs(argc, argv, opts)) {
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_SUCCESS;
    }

#ifdef _WIN32
    struct ScopedStopEventWatcher {
        HANDLE stop_evt = nullptr;
        HANDLE exit_evt = nullptr;
        std::thread thr;
        ~ScopedStopEventWatcher() {
            if (exit_evt) SetEvent(exit_evt);
            if (thr.joinable()) thr.join();
            if (exit_evt) CloseHandle(exit_evt);
            if (stop_evt) CloseHandle(stop_evt);
        }
    } stop_watcher;

    if (!opts.stop_event_name.empty()) {
        std::wstring wname(opts.stop_event_name.begin(), opts.stop_event_name.end());
        stop_watcher.stop_evt = OpenEventW(SYNCHRONIZE, FALSE, wname.c_str());
        if (stop_watcher.stop_evt) {
            stop_watcher.exit_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            stop_watcher.thr = std::thread([hStop = stop_watcher.stop_evt, hExit = stop_watcher.exit_evt]() {
                HANDLE handles[2] = {hStop, hExit};
                DWORD rc = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (rc == WAIT_OBJECT_0) {
                    g_stop_flag.store(true, std::memory_order_release);
                }
            });
        }
    }
#endif

    if (opts.list_audio_only) {
        auto endpoints = WasapiAudioRouter::EnumerateRenderEndpoints();
        std::cout << "Active Windows Audio Render Endpoints (" << endpoints.size() << "):\n";
        for (const auto& ep : endpoints) {
            std::cout << "  - " << ep.friendly_name_utf8 << "\n";
        }
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_SUCCESS;
    }

    SharedCaptureProducer producer;
    if (!producer.Open(kDefaultShmName)) {
        std::cerr << FormatHostDiagLine(
                         DiagCode::E502_SharedMemInitFailed,
                         DiagSeverity::Critical,
                         "無法建立跨行程共享記憶體 (" + std::string(kDefaultShmName) + ")。")
                  << "\n";
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_FAILURE;
    }

    if (opts.res_mode == ResolutionMode::Fixed720p || (opts.explicit_max_dim > 0 && opts.explicit_max_dim <= 1280U)) {
        producer.SetAdvertisedDimensions(1280, 720);
    } else if (opts.res_mode == ResolutionMode::Fixed1080p ||
               (opts.explicit_max_dim > 0 && opts.explicit_max_dim <= 1920U)) {
        producer.SetAdvertisedDimensions(1920, 1080);
    } else {
        producer.SetAdvertisedDimensions(2560, 1440);
    }

    WasapiAudioRouter audio_router;
    if (opts.enable_audio_route) {
        if (audio_router.StartAutoRoute(kDefaultShmName, opts.audio_out_match)) {
            std::cout << "[Audio] Low-latency WASAPI router active -> \""
                      << audio_router.ActiveEndpointName() << "\"\n";
        } else if (!opts.audio_out_match.empty()) {
            std::cerr << "[Audio] Warning: No playback endpoint matched \"" << opts.audio_out_match << "\".\n";
        } else {
            std::cout << "[Audio] DirectShow 'TJCap for Android (Audio)' active (48kHz LPCM).\n";
        }
    }

    if (opts.mock_mode) {
        int rc = RunMockPipeline(producer, opts);
        audio_router.Stop();
        producer.Close();
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return rc;
    }

    std::filesystem::path base_dir = HostExeDir();
    std::filesystem::path adb_exe = ResolveBundledAdbPath(base_dir);
    std::filesystem::path server_jar = ResolveBundledServerJarPath(base_dir);

    const char* conn_label = "ADB Zero-APK Direct";
    if (opts.conn_mode == ConnectionMode::UsbAoaCompanion) conn_label = "USB AOA Companion APK (No USB Debugging)";
    else if (opts.conn_mode == ConnectionMode::SmartAuto) conn_label = "Smart Auto (ADB -> USB AOA Fallback)";

    std::cout << "[Host] Waiting for Android device over USB-C (Transport: " << conn_label
              << ", Target FPS: " << opts.max_fps << ")...\n";

    DiagCode last_open_diag = DiagCode::None;

    while (!g_stop_flag.load(std::memory_order_relaxed)) {
        producer.TouchHeartbeat(false);

        bool any_usb = false;
        bool adb_iface = false;
        UsbLinkSpeed pre_speed = ProbeConnectedAndroidLinkSpeed(nullptr, &any_usb, &adb_iface, nullptr);

        uint32_t locked_max_dim =
            (opts.explicit_max_dim > 0)
                ? opts.explicit_max_dim
                : MaxSelectableDimForUsbSpeed(pre_speed);
        if (opts.res_mode == ResolutionMode::Fixed1440p) {
            locked_max_dim = std::min<uint32_t>(2560U, locked_max_dim);
        } else if (opts.res_mode == ResolutionMode::Fixed1080p) {
            locked_max_dim = std::min<uint32_t>(1920U, locked_max_dim);
        } else if (opts.res_mode == ResolutionMode::Fixed720p) {
            locked_max_dim = 1280U;
        }

        const uint32_t target_bps =
            (opts.explicit_bitrate_mbps > 0)
                ? (opts.explicit_bitrate_mbps * 1000000U)
                : RecommendedBitrateBps(locked_max_dim, opts.max_fps, pre_speed);

        bool use_aoa_this_attempt = (opts.conn_mode == ConnectionMode::UsbAoaCompanion);
        if (opts.conn_mode == ConnectionMode::SmartAuto && any_usb && !adb_iface) {
            use_aoa_this_attempt = true;
        }

        AdbStreamSession adb_session;
        UsbAoaSession aoa_session;
        ConnectedAndroidInfo dev_info;
        std::string conn_err;
        bool connected = false;

        if (!use_aoa_this_attempt) {
            AdbSessionConfig acfg;
            acfg.adb_exe_path = adb_exe;
            acfg.server_jar_path = server_jar;
            acfg.preferred_serial = opts.preferred_serial;
            acfg.max_size = locked_max_dim;
            acfg.max_fps = opts.max_fps;
            acfg.video_bit_rate = target_bps;
            acfg.lock_initial_orientation = opts.lock_orientation;
            acfg.enable_audio = true;
            connected = adb_session.ConnectAndStart(acfg, dev_info, &conn_err);
        } else {
            UsbAoaSessionConfig ucfg;
            ucfg.base_dir = base_dir;
            ucfg.max_size = locked_max_dim;
            ucfg.max_fps = opts.max_fps;
            ucfg.video_bit_rate = target_bps;
            ucfg.lock_initial_orientation = opts.lock_orientation;
            ucfg.enable_audio = true;
            connected = aoa_session.OpenAndNegotiateAoa(ucfg, dev_info, &conn_err);
        }

        if (!connected) {
            static auto last_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            auto now = std::chrono::steady_clock::now();
            DiagCode cur_diag = ClassifyUsbOrTransportError(conn_err);
            if (cur_diag != DiagCode::None && cur_diag != last_open_diag) {
                last_open_diag = cur_diag;
                DiagSeverity sev =
                    (cur_diag == DiagCode::E301_UsbDebugUnauthorized ||
                     cur_diag == DiagCode::E201_AndroidUsbDriverIssue)
                        ? DiagSeverity::Warning
                        : DiagSeverity::Error;
                std::cout << FormatHostDiagLine(cur_diag, sev, conn_err) << "\n";
                last_log = now;
            } else if (now - last_log >= std::chrono::seconds(5)) {
                std::cout << (use_aoa_this_attempt ? "[AOA] " : "[ADB] ") << conn_err << "\n";
                last_log = now;
            }

            for (int i = 0; i < 15 && !g_stop_flag.load(std::memory_order_relaxed); ++i) {
                producer.TouchHeartbeat(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        last_open_diag = DiagCode::None;

        const UsbLinkSpeed conn_speed =
            (dev_info.usb_speed != UsbLinkSpeed::Unknown) ? dev_info.usb_speed : pre_speed;

        std::cout << (use_aoa_this_attempt ? "[USB-AOA] Connected: " : "[USB-ADB] Connected: ")
                  << dev_info.display_model
                  << " | Link: " << UsbLinkSpeedName(conn_speed)
                  << " | Locked Max Edge: " << locked_max_dim << "px"
                  << " | Target: " << (target_bps / 1000000U) << " Mbps @ " << opts.max_fps << "fps\n";
        if (use_aoa_this_attempt) {
            std::cout << "[AOA] 請在手機彈出的「TJCap Companion」點擊『🚀 授權螢幕錄製並開始 USB-C 直播』！\n";
        }

        MfH264Decoder decoder;
        std::string dec_err;
        if (!decoder.Initialize(opts.prefer_hw_decode, &dec_err)) {
            if (opts.prefer_hw_decode && decoder.Initialize(false, &dec_err)) {
                std::cout << FormatHostDiagLine(
                                 DiagCode::E402_GpuFallbackToSoftware,
                                 DiagSeverity::Warning,
                                 "GPU D3D11VA 硬體解碼初始化失敗，已自動切換為 CPU 軟體解碼模式。")
                          << "\n";
            } else {
                std::cerr << FormatHostDiagLine(DiagCode::E401_DecoderInitFailed, DiagSeverity::Critical, dec_err)
                          << "\n";
                adb_session.StopAndCleanup();
                aoa_session.StopAndClose();
                audio_router.Stop();
                producer.Close();
#ifdef _WIN32
                timeEndPeriod(1);
#endif
                return EXIT_FAILURE;
            }
        }
        std::cout << "[Decoder] Windows Media Foundation H.264 MFT initialized ("
                  << (decoder.IsHardwareAccelerated() ? "D3D11VA Hardware GPU" : "CPU Software Fallback")
                  << ")\n";

        std::atomic<uint32_t> session_flags{
            (decoder.IsHardwareAccelerated() ? kStreamFlagHwAccelerated : 0U) |
            ((opts.res_mode == ResolutionMode::NativeAdaptive) ? kStreamFlagAdaptiveRes : 0U) |
            ((opts.max_fps >= 90U) ? kStreamFlagHighRefresh : 0U) |
            ((static_cast<uint32_t>(conn_speed) & 0xFU) << 8)};

        std::atomic<bool> got_any_frame{false};
        std::atomic<bool> session_done{false};
        const double no_frame_warn_sec = use_aoa_this_attempt ? 16.0 : 8.5;
        std::thread session_watchdog([&]() {
            auto start_tp = std::chrono::steady_clock::now();
            bool warned_no_frames = false;
            while (!session_done.load(std::memory_order_acquire) &&
                   !g_stop_flag.load(std::memory_order_relaxed)) {
                producer.TouchHeartbeat(got_any_frame.load(std::memory_order_acquire));
                if (!warned_no_frames && !got_any_frame.load(std::memory_order_acquire)) {
                    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_tp).count();
                    if (sec >= no_frame_warn_sec) {
                        warned_no_frames = true;
                        std::cout << FormatHostDiagLine(
                                         DiagCode::E303_HandshakeNoFrames,
                                         DiagSeverity::Warning,
                                         use_aoa_this_attempt
                                             ? "AOA 配件通道已連通但尚未收到畫面：請確認手機已安裝『TJCap Companion APK』並點擊『開始螢幕錄製』。"
                                             : "USB-C 影音通道已建立但超過 8 秒未收到畫面封包：請確認手機螢幕已點亮解鎖並滑動一下畫面。")
                                  << "\n";
                    }
                }
                for (int k = 0; k < 5 && !session_done.load(std::memory_order_acquire) &&
                                !g_stop_flag.load(std::memory_order_relaxed);
                     ++k) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });

        uint64_t console_frames = 0;
        uint64_t console_video_bytes = 0;
        auto console_timer = std::chrono::steady_clock::now();

        uint64_t fast_frames = 0;
        uint64_t fast_video_bytes = 0;
        auto fast_timer = console_timer;

        uint32_t latest_kbps = 0;
        uint32_t latest_fps_x100 = opts.max_fps * 100U;
        uint32_t ema_dec_us = 0;
        uint32_t ema_pipe_us = 0;
        uint32_t decode_drops = 0;
        uint32_t consecutive_dec_failures = 0;
        bool warned_high_latency = false;
        bool latency_seeded = false;
        uint32_t last_src_w = 0, last_src_h = 0;

        auto query_audio_pkt_count = [&]() -> uint64_t {
            return use_aoa_this_attempt ? aoa_session.AudioPacketsReceived()
                                        : adb_session.AudioPacketsReceived();
        };

        auto on_video_cb = [&](const AndroidEncodedVideoPacket& vpkt) {
            auto pkt_t0 = std::chrono::steady_clock::now();
            producer.AddEncodedVideoBytes(vpkt.annexb_data.size());
            console_video_bytes += vpkt.annexb_data.size();
            fast_video_bytes += vpkt.annexb_data.size();

            bool emitted_frame = false;
            bool dec_ok = decoder.DecodeAnnexB(
                vpkt.annexb_data,
                vpkt.pts_ns,
                vpkt.width,
                vpkt.height,
                [&](const DecodedNV12View& view) {
                    emitted_frame = true;
                    got_any_frame.store(true, std::memory_order_release);
                    consecutive_dec_failures = 0;

                    auto dec_t1 = std::chrono::steady_clock::now();
                    uint32_t cur_dec_us = static_cast<uint32_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(dec_t1 - pkt_t0).count());

                    uint32_t out_w = 0, out_h = 0;
                    if (opts.res_mode == ResolutionMode::NativeAdaptive) {
                        ClampAdaptiveDimensionsToMax(view.width, view.height, locked_max_dim, out_w, out_h);
                    } else if (opts.res_mode == ResolutionMode::Fixed1440p) {
                        out_w = 2560U;
                        out_h = 1440U;
                    } else if (opts.res_mode == ResolutionMode::Fixed720p) {
                        out_w = 1280U;
                        out_h = 720U;
                    } else {
                        out_w = 1920U;
                        out_h = 1080U;
                    }

                    if (view.width != last_src_w || view.height != last_src_h) {
                        last_src_w = view.width;
                        last_src_h = view.height;
                        uint32_t g = std::gcd(last_src_w, last_src_h);
                        double aspect = (last_src_h > 0)
                                            ? (static_cast<double>(last_src_w) / static_cast<double>(last_src_h))
                                            : 1.777;
                        std::printf(
                            "[Video] Android native resolution: %ux%u (ratio %.2f:1, %u:%u) -> Output %ux%u\n",
                            last_src_w,
                            last_src_h,
                            aspect,
                            g ? (last_src_w / g) : last_src_w,
                            g ? (last_src_h / g) : last_src_h,
                            out_w,
                            out_h);
                        std::fflush(stdout);
                    }

                    ++console_frames;
                    ++fast_frames;
                    auto now = std::chrono::steady_clock::now();
                    double fast_dt = std::chrono::duration<double>(now - fast_timer).count();
                    if (fast_dt >= 0.45) {
                        double inst_fps = static_cast<double>(fast_frames) / fast_dt;
                        double inst_mbps = (static_cast<double>(fast_video_bytes) * 8.0) / (fast_dt * 1e6);
                        latest_fps_x100 = static_cast<uint32_t>(inst_fps * 100.0 + 0.5);
                        latest_kbps = static_cast<uint32_t>(inst_mbps * 1000.0 + 0.5);
                        fast_frames = 0;
                        fast_video_bytes = 0;
                        fast_timer = now;
                    }

                    uint32_t slot = 0;
                    uint8_t* dst_nv12 = producer.AcquireBackbufferSlot(slot);
                    if (dst_nv12) {
                        ScaleAndLetterboxNV12(
                            view.y_plane,
                            view.y_stride,
                            view.uv_plane,
                            view.uv_stride,
                            view.width,
                            view.height,
                            dst_nv12,
                            out_w,
                            out_h);
                        auto pipe_t1 = std::chrono::steady_clock::now();
                        uint32_t cur_pipe_us = static_cast<uint32_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(pipe_t1 - pkt_t0).count());

                        if (!latency_seeded) {
                            ema_dec_us = cur_dec_us;
                            ema_pipe_us = cur_pipe_us;
                            latency_seeded = true;
                        } else {
                            ema_dec_us = (ema_dec_us * 3U + cur_dec_us) / 4U;
                            ema_pipe_us = (ema_pipe_us * 3U + cur_pipe_us) / 4U;
                        }

                        if (!warned_high_latency && console_frames > 90 && ema_pipe_us > 17500U) {
                            warned_high_latency = true;
                            char lbuf[160] = {0};
                            std::snprintf(
                                lbuf,
                                sizeof(lbuf),
                                "解碼管線平均耗時 %.1f ms 超過 60fps 預算 (16.7 ms)，建議確認開啟 GPU D3D11VA 硬解或調降解析度上限。",
                                ema_pipe_us / 1000.0);
                            std::cout << FormatHostDiagLine(
                                             DiagCode::E403_HighDecodeLatency, DiagSeverity::Warning, lbuf)
                                      << "\n";
                        }

                        FrameTelemetryMeta meta{};
                        meta.fps_x100 = latest_fps_x100;
                        meta.decode_latency_us = ema_dec_us;
                        meta.pipeline_latency_us = ema_pipe_us;
                        meta.stream_flags = session_flags.load(std::memory_order_relaxed);
                        meta.dropped_frames = decode_drops;

                        producer.PublishBackbufferSlot(
                            slot, out_w, out_h, view.pts_ns, view.width, view.height, latest_kbps, meta);
                    }

                    double elapsed = std::chrono::duration<double>(now - console_timer).count();
                    if (elapsed >= 4.0) {
                        double fps = static_cast<double>(console_frames) / elapsed;
                        double mbps = (static_cast<double>(console_video_bytes) * 8.0) / (elapsed * 1e6);
                        std::printf(
                            "[Stream] %.1f fps | Bitrate: %.1f Mbps | Decode: %.2f ms (Total %.2f ms) | Output: %ux%u (Source %ux%u) | Audio pkts: %llu\n",
                            fps,
                            mbps,
                            ema_dec_us / 1000.0,
                            ema_pipe_us / 1000.0,
                            out_w,
                            out_h,
                            last_src_w,
                            last_src_h,
                            static_cast<unsigned long long>(query_audio_pkt_count()));
                        std::fflush(stdout);
                        console_frames = 0;
                        console_video_bytes = 0;
                        console_timer = now;
                    }
                });

            if (!dec_ok) {
                ++decode_drops;
                ++consecutive_dec_failures;
                if (decoder.IsHardwareAccelerated() && consecutive_dec_failures >= 15U) {
                    decoder.Shutdown();
                    std::string fb_err;
                    if (decoder.Initialize(false, &fb_err)) {
                        consecutive_dec_failures = 0;
                        uint32_t prev_f = session_flags.load(std::memory_order_relaxed);
                        session_flags.store(prev_f & ~kStreamFlagHwAccelerated, std::memory_order_release);
                        std::cout << FormatHostDiagLine(
                                         DiagCode::E402_GpuFallbackToSoftware,
                                         DiagSeverity::Warning,
                                         "偵測到顯示卡 D3D11VA 硬體解碼連續異常，已自動熱切換至 CPU 軟體解碼維持直播不中斷！")
                                  << "\n";
                    }
                }
            } else if (emitted_frame) {
                consecutive_dec_failures = 0;
            }
        };

        auto on_audio_cb = [&](const AndroidPcmAudioPacket& apkt) {
            size_t stereo_frames = apkt.pcm_s16le.size() / kAudioBytesPerFrame;
            if (stereo_frames > 0) {
                producer.PushAudioPcmS16LE(
                    reinterpret_cast<const int16_t*>(apkt.pcm_s16le.data()), stereo_frames);
                producer.SetAudioPacketCount(query_audio_pkt_count());
            }
        };

        auto on_audio_disabled_cb = [&](int android_sdk, bool /*is_err*/) {
            char msg[192] = {0};
            std::snprintf(
                msg,
                sizeof(msg),
                "手機端未提供系統內錄音訊 (API %d)，已自動維持純畫面流暢鏡像。",
                android_sdk);
            std::cout << FormatHostDiagLine(
                             DiagCode::E504_DeviceAudioUnsupported, DiagSeverity::Warning, msg)
                      << "\n";
        };

        std::string loop_err;
        if (!use_aoa_this_attempt) {
            adb_session.RunStreamLoop(g_stop_flag, on_video_cb, on_audio_cb, on_audio_disabled_cb, &loop_err);
        } else {
            aoa_session.RunBulkStreamLoop(g_stop_flag, on_video_cb, on_audio_cb, on_audio_disabled_cb, &loop_err);
        }

        session_done.store(true, std::memory_order_release);
        if (session_watchdog.joinable()) {
            session_watchdog.join();
        }

        decoder.Shutdown();
        adb_session.StopAndCleanup();
        aoa_session.StopAndClose();

        if (!g_stop_flag.load(std::memory_order_relaxed)) {
            DiagCode drop_code = ClassifyUsbOrTransportError(loop_err);
            if (drop_code != DiagCode::None) {
                std::cout << FormatHostDiagLine(
                                 drop_code,
                                 DiagSeverity::Warning,
                                 "USB-C 裝置連線中斷 (" + loop_err + ")，正在自動等待重連...")
                          << "\n";
            } else {
                std::cout << "[USB-C] Device disconnected (" << loop_err << "). Waiting for reconnect...\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
        }
    }

    std::cout << "[Host] Shutting down cleanly.\n";
    audio_router.Stop();
    producer.Close();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return EXIT_SUCCESS;
}
