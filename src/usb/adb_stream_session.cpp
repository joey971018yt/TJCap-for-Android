#include "android_vcam/adb_stream_session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

namespace android_vcam {

namespace {

std::string TrimStr(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
        ++i;
    }
    if (i > 0) s.erase(0, i);
    return s;
}

#ifdef _WIN32
std::wstring Utf8ToWideLocal(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool RunAdbSyncCommand(
    const std::filesystem::path& adb_exe,
    const std::wstring& args,
    uint32_t timeout_ms,
    std::string& out_text,
    DWORD* out_exit = nullptr) {
    out_text.clear();
    if (out_exit) *out_exit = 1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rpipe = nullptr;
    HANDLE wpipe = nullptr;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return false;
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmd = L"\"" + adb_exe.wstring() + L"\" " + args;
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    std::wstring work_dir = adb_exe.parent_path().wstring();
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        adb_exe.c_str(),
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        work_dir.empty() ? nullptr : work_dir.c_str(),
        &si,
        &pi);
    CloseHandle(wpipe);

    if (!ok) {
        CloseHandle(rpipe);
        return false;
    }

    DWORD t0 = GetTickCount();
    char buf[1024];
    while (true) {
        DWORD avail = 0;
        while (PeekNamedPipe(rpipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD nr = 0;
            if (ReadFile(rpipe, buf, std::min<DWORD>(sizeof(buf), avail), &nr, nullptr) && nr > 0) {
                out_text.append(buf, nr);
            } else {
                break;
            }
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            DWORD nr = 0;
            while (ReadFile(rpipe, buf, sizeof(buf), &nr, nullptr) && nr > 0) {
                out_text.append(buf, nr);
            }
            break;
        }
        if (GetTickCount() - t0 >= timeout_ms) {
            TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 200);
            CloseHandle(rpipe);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return false;
        }
    }

    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    if (out_exit) *out_exit = ec;

    CloseHandle(rpipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

SOCKET ConnectLoopbackTcp(uint16_t port, uint32_t recv_timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    DWORD tv = recv_timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}
#endif

}  // namespace

struct AdbStreamSession::Impl {
#ifdef _WIN32
    bool wsa_started = false;
    std::filesystem::path adb_exe;
    std::string active_serial;
    uint16_t tunnel_port = 0;
    bool tunnel_active = false;
    int android_sdk = 0;

    HANDLE server_proc = nullptr;
    HANDLE server_log_pipe = nullptr;
    std::thread server_log_thread;
    std::mutex log_mu;
    std::string server_log_tail;

    SOCKET video_sock = INVALID_SOCKET;
    SOCKET audio_sock = INVALID_SOCKET;
    uint8_t initial_dummy_byte = 0;
    bool got_dummy_byte = false;

    void CloseSocketSafe(SOCKET& s) {
        if (s != INVALID_SOCKET) {
            shutdown(s, SD_BOTH);
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
#endif
};

AdbStreamSession::AdbStreamSession() : impl_(new Impl()) {}

AdbStreamSession::~AdbStreamSession() {
    StopAndCleanup();
    delete impl_;
}

bool AdbStreamSession::ConnectAndStart(
    const AdbSessionConfig& cfg,
    ConnectedAndroidInfo& out_device_info,
    std::string* out_error) {
    StopAndCleanup();
    audio_packets_.store(0, std::memory_order_relaxed);
    out_device_info = ConnectedAndroidInfo{};

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (out_error) *out_error = "WSAStartup failed.";
        return false;
    }
    impl_->wsa_started = true;

    std::error_code ec;
    if (!std::filesystem::exists(cfg.adb_exe_path, ec)) {
        if (out_error) *out_error = "Missing bundled runtime/adb.exe.";
        return false;
    }
    if (!std::filesystem::exists(cfg.server_jar_path, ec)) {
        if (out_error) *out_error = "Missing bundled runtime/jcast-server.jar.";
        return false;
    }
    impl_->adb_exe = cfg.adb_exe_path;

    // 1. Query `adb devices -l` (retry once after restarting server if version mismatch occurs)
    std::string dev_out;
    DWORD dev_ec = 0;
    if (!RunAdbSyncCommand(cfg.adb_exe_path, L"devices -l", 5000U, dev_out, &dev_ec) || dev_ec != 0) {
        if (dev_out.find("doesn't match") != std::string::npos) {
            std::string dummy;
            RunAdbSyncCommand(cfg.adb_exe_path, L"kill-server", 2500U, dummy, nullptr);
            RunAdbSyncCommand(cfg.adb_exe_path, L"devices -l", 5000U, dev_out, &dev_ec);
        }
    }
    if (dev_ec != 0) {
        if (out_error) {
            *out_error = "Failed to start ADB daemon (5037): " + TrimStr(dev_out);
        }
        return false;
    }

    auto entries = ParseAdbDevicesOutput(dev_out);
    const AdbDeviceEntry* chosen = nullptr;
    const AdbDeviceEntry* unauthorized_cand = nullptr;
    const AdbDeviceEntry* offline_cand = nullptr;

    for (const auto& e : entries) {
        if (!cfg.preferred_serial.empty() && e.serial.find(cfg.preferred_serial) == std::string::npos) {
            continue;
        }
        if (e.state == AdbDeviceState::Authorized) {
            // Prefer wired USB serial over wireless ip:port
            if (!chosen || (chosen->is_wireless && !e.is_wireless)) {
                chosen = &e;
            }
        } else if (e.state == AdbDeviceState::Unauthorized && !unauthorized_cand) {
            unauthorized_cand = &e;
        } else if ((e.state == AdbDeviceState::Offline || e.state == AdbDeviceState::NoPermissions) &&
                   !offline_cand) {
            offline_cand = &e;
        }
    }

    if (!chosen) {
        if (unauthorized_cand) {
            if (out_error) {
                *out_error = "Android device is unauthorized — please unlock phone and tap 'Allow USB debugging'.";
            }
            return false;
        }
        if (offline_cand) {
            if (out_error) {
                *out_error = (offline_cand->state == AdbDeviceState::NoPermissions)
                                 ? "No permissions for Android USB driver interface."
                                 : "Android ADB device is offline — please replug USB-C cable.";
            }
            return false;
        }

        // Check if physical Android USB hardware is plugged in without USB debugging enabled
        bool any_usb = false;
        bool adb_if = false;
        std::string hw_desc;
        ProbeConnectedAndroidLinkSpeed(nullptr, &any_usb, &adb_if, &hw_desc);
        if (any_usb && !adb_if) {
            if (out_error) {
                *out_error = "Detected Android USB device (" + (hw_desc.empty() ? "USB" : hw_desc) +
                             ") with no USB debugging enabled — enable Developer Options -> USB Debugging.";
            }
            return false;
        }

        if (out_error) {
            *out_error = "Waiting for Android phone/tablet over USB-C (ensure USB Debugging is ON)...";
        }
        return false;
    }

    impl_->active_serial = chosen->serial;
    std::wstring wserial = Utf8ToWideLocal(chosen->serial);

    // 2. Query Android SDK level, release, brand/model, and physical USB link speed
    std::string sdk_str, rel_str, brand_str, model_str;
    RunAdbSyncCommand(
        cfg.adb_exe_path, L"-s \"" + wserial + L"\" shell getprop ro.build.version.sdk", 2500U, sdk_str);
    RunAdbSyncCommand(
        cfg.adb_exe_path, L"-s \"" + wserial + L"\" shell getprop ro.build.version.release", 2500U, rel_str);
    RunAdbSyncCommand(
        cfg.adb_exe_path, L"-s \"" + wserial + L"\" shell getprop ro.product.manufacturer", 2500U, brand_str);
    RunAdbSyncCommand(
        cfg.adb_exe_path, L"-s \"" + wserial + L"\" shell getprop ro.product.model", 2500U, model_str);

    sdk_str = TrimStr(sdk_str);
    rel_str = TrimStr(rel_str);
    brand_str = TrimStr(brand_str);
    model_str = TrimStr(model_str);
    if (model_str.empty()) model_str = chosen->model;

    impl_->android_sdk = std::atoi(sdk_str.c_str());
    out_device_info.serial = chosen->serial;
    out_device_info.android_sdk = impl_->android_sdk;
    out_device_info.android_release = rel_str;
    out_device_info.display_model =
        (!brand_str.empty() && model_str.find(brand_str) == std::string::npos)
            ? (brand_str + " " + model_str)
            : (model_str.empty() ? std::string("Android Device") : model_str);
    out_device_info.usb_speed = ProbeConnectedAndroidLinkSpeed();

    // 3. Push bundled jcast-server.jar to /data/local/tmp/jcast-server.jar
    std::string push_out;
    DWORD push_ec = 0;
    std::wstring push_args =
        L"-s \"" + wserial + L"\" push \"" + cfg.server_jar_path.wstring() + L"\" /data/local/tmp/jcast-server.jar";
    if (!RunAdbSyncCommand(cfg.adb_exe_path, push_args, 8000U, push_out, &push_ec) || push_ec != 0) {
        if (out_error) {
            *out_error = "adb push jcast-server.jar failed: " + TrimStr(push_out);
        }
        return false;
    }

    // 4. Generate random 31-bit SCID and establish `adb forward`
    std::random_device rd;
    uint32_t scid = (rd() & 0x7FFFFFFFU);
    if (scid == 0) scid = 0x1A2B3C4DU;
    char scid_hex[16] = {0};
    std::snprintf(scid_hex, sizeof(scid_hex), "%08x", scid);

    uint16_t chosen_port = 0;
    std::string fwd_out;
    for (uint16_t candidate = 27183; candidate <= 27225; ++candidate) {
        DWORD fwd_ec = 0;
        std::wstring fwd_args =
            L"-s \"" + wserial + L"\" forward tcp:" + std::to_wstring(candidate) +
            L" localabstract:scrcpy_" + Utf8ToWideLocal(scid_hex);
        if (RunAdbSyncCommand(cfg.adb_exe_path, fwd_args, 3000U, fwd_out, &fwd_ec) && fwd_ec == 0) {
            chosen_port = candidate;
            break;
        }
    }
    if (chosen_port == 0) {
        if (out_error) {
            *out_error = "adb forward failed: " + TrimStr(fwd_out);
        }
        return false;
    }
    impl_->tunnel_port = chosen_port;
    impl_->tunnel_active = true;

    // 5. Spawn asynchronous `app_process` server on Android over ADB shell
    const bool request_audio = cfg.enable_audio;
    std::wstring shell_args =
        L"-s \"" + wserial +
        L"\" shell CLASSPATH=/data/local/tmp/jcast-server.jar app_process / com.genymobile.scrcpy.Server " +
        Utf8ToWideLocal(kBundledScrcpyServerVersion) +
        L" scid=" + Utf8ToWideLocal(scid_hex) +
        L" log_level=info tunnel_forward=true video=true audio=" + (request_audio ? L"true" : L"false") +
        L" control=false stay_awake=false power_off_on_close=false cleanup=true"
        L" video_codec=h264 audio_codec=raw"
        L" max_size=" + std::to_wstring(cfg.max_size) +
        L" max_fps=" + std::to_wstring(cfg.max_fps) +
        L" video_bit_rate=" + std::to_wstring(cfg.video_bit_rate) +
        (cfg.lock_initial_orientation ? L" capture_orientation=@" : L"") +
        L" send_device_meta=true send_stream_meta=true send_frame_meta=true send_dummy_byte=true";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rpipe = nullptr;
    HANDLE wpipe = nullptr;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) {
        StopAndCleanup();
        if (out_error) *out_error = "CreatePipe failed for app_process.";
        return false;
    }
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring full_cmd = L"\"" + cfg.adb_exe_path.wstring() + L"\" " + shell_args;
    std::vector<wchar_t> cmd_buf(full_cmd.begin(), full_cmd.end());
    cmd_buf.push_back(L'\0');

    std::wstring work_dir = cfg.adb_exe_path.parent_path().wstring();
    PROCESS_INFORMATION pi{};
    BOOL launched = CreateProcessW(
        cfg.adb_exe_path.c_str(),
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        work_dir.empty() ? nullptr : work_dir.c_str(),
        &si,
        &pi);
    CloseHandle(wpipe);

    if (!launched) {
        CloseHandle(rpipe);
        StopAndCleanup();
        if (out_error) *out_error = "Failed to launch app_process via adb shell.";
        return false;
    }
    CloseHandle(pi.hThread);
    impl_->server_proc = pi.hProcess;
    impl_->server_log_pipe = rpipe;

    impl_->server_log_thread = std::thread([this]() {
        char buf[512];
        DWORD nr = 0;
        while (impl_->server_log_pipe &&
               ReadFile(impl_->server_log_pipe, buf, sizeof(buf), &nr, nullptr) && nr > 0) {
            std::lock_guard<std::mutex> lk(impl_->log_mu);
            impl_->server_log_tail.append(buf, nr);
            if (impl_->server_log_tail.size() > 4096U) {
                impl_->server_log_tail.erase(0, impl_->server_log_tail.size() - 4096U);
            }
        }
    });

    // 6. Connect Video TCP socket and wait for the 1-byte dummy handshake (handles adb forward startup race!)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(6000);
    bool video_ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (WaitForSingleObject(impl_->server_proc, 0) == WAIT_OBJECT_0) {
            break;
        }
        SOCKET vs = ConnectLoopbackTcp(impl_->tunnel_port, 220U);
        if (vs != INVALID_SOCKET) {
            uint8_t dummy = 0;
            int n = recv(vs, reinterpret_cast<char*>(&dummy), 1, 0);
            if (n == 1) {
                impl_->video_sock = vs;
                impl_->initial_dummy_byte = dummy;
                impl_->got_dummy_byte = true;
                video_ready = true;
                break;
            }
            closesocket(vs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(55));
    }

    if (!video_ready) {
        std::string tail;
        {
            std::lock_guard<std::mutex> lk(impl_->log_mu);
            tail = TrimStr(impl_->server_log_tail);
        }
        StopAndCleanup();
        if (out_error) {
            *out_error = "app_process handshake timed out" + (tail.empty() ? std::string(".") : (": " + tail));
        }
        return false;
    }

    // Set video socket read timeout to 350ms for responsive stop_flag polling during VFR static screens
    {
        DWORD tv = 350U;
        setsockopt(impl_->video_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    }

    // 7. Immediately connect Audio TCP socket if audio=true (unblocks DesktopConnection.open() on Android!)
    if (request_audio) {
        SOCKET as = ConnectLoopbackTcp(impl_->tunnel_port, 350U);
        if (as == INVALID_SOCKET) {
            StopAndCleanup();
            if (out_error) *out_error = "Failed to connect secondary audio forward socket.";
            return false;
        }
        impl_->audio_sock = as;
    }

    return true;
#else
    (void)cfg;
    if (out_error) *out_error = "AdbStreamSession is only supported on Windows.";
    return false;
#endif
}

bool AdbStreamSession::RunStreamLoop(
    const std::atomic<bool>& stop_flag,
    const OnVideoPacketFn& on_video,
    const OnAudioPacketFn& on_audio,
    const OnAudioDisabledFn& on_audio_disabled,
    std::string* out_error) {
#ifdef _WIN32
    if (impl_->video_sock == INVALID_SOCKET) {
        if (out_error) *out_error = "Video socket is not connected.";
        return false;
    }

    std::atomic<bool> loop_done{false};
    std::thread audio_thr;

    if (impl_->audio_sock != INVALID_SOCKET) {
        audio_thr = std::thread([&]() {
            ScrcpyAudioStreamParser aparser(
                [&](bool is_err) {
                    if (on_audio_disabled) {
                        on_audio_disabled(impl_->android_sdk, is_err);
                    }
                },
                [&](const AndroidPcmAudioPacket& apkt) {
                    audio_packets_.fetch_add(1ULL, std::memory_order_relaxed);
                    if (on_audio) {
                        on_audio(apkt);
                    }
                });

            std::vector<uint8_t> abuf(16384);
            while (!stop_flag.load(std::memory_order_relaxed) &&
                   !loop_done.load(std::memory_order_relaxed)) {
                int nr = recv(
                    impl_->audio_sock,
                    reinterpret_cast<char*>(abuf.data()),
                    static_cast<int>(abuf.size()),
                    0);
                if (nr > 0) {
                    std::string perr;
                    if (!aparser.FeedBytes(std::span<const uint8_t>(abuf.data(), static_cast<size_t>(nr)), &perr)) {
                        break;
                    }
                    if (aparser.IsDisabledByDevice()) {
                        break;
                    }
                } else if (nr == 0) {
                    break;
                } else {
                    int werr = WSAGetLastError();
                    if (werr == WSAETIMEDOUT || werr == WSAEWOULDBLOCK) {
                        continue;
                    }
                    break;
                }
            }
        });
    }

    ScrcpyVideoStreamParser vparser(
        /*expect_dummy_byte=*/false,  // Already consumed during ConnectAndStart handshake!
        nullptr,
        on_video);

    std::vector<uint8_t> vbuf(131072);
    bool clean_stop = false;

    while (!stop_flag.load(std::memory_order_relaxed)) {
        int nr = recv(
            impl_->video_sock,
            reinterpret_cast<char*>(vbuf.data()),
            static_cast<int>(vbuf.size()),
            0);
        if (nr > 0) {
            std::string perr;
            if (!vparser.FeedBytes(std::span<const uint8_t>(vbuf.data(), static_cast<size_t>(nr)), &perr)) {
                if (out_error) *out_error = perr;
                break;
            }
        } else if (nr == 0) {
            if (out_error) *out_error = "Android USB-C stream socket closed (EOF / cable disconnected).";
            break;
        } else {
            int werr = WSAGetLastError();
            if (werr == WSAETIMEDOUT || werr == WSAEWOULDBLOCK) {
                continue;
            }
            if (out_error) {
                *out_error = "USB-C stream socket recv failed (WSA " + std::to_string(werr) + ").";
            }
            break;
        }
    }

    if (stop_flag.load(std::memory_order_relaxed)) {
        clean_stop = true;
    }

    loop_done.store(true, std::memory_order_release);
    impl_->CloseSocketSafe(impl_->audio_sock);
    impl_->CloseSocketSafe(impl_->video_sock);
    if (audio_thr.joinable()) {
        audio_thr.join();
    }
    return clean_stop;
#else
    (void)stop_flag;
    (void)on_video;
    (void)on_audio;
    (void)on_audio_disabled;
    if (out_error) *out_error = "AdbStreamSession is only supported on Windows.";
    return false;
#endif
}

void AdbStreamSession::StopAndCleanup() {
#ifdef _WIN32
    impl_->CloseSocketSafe(impl_->audio_sock);
    impl_->CloseSocketSafe(impl_->video_sock);
    impl_->got_dummy_byte = false;

    if (impl_->server_proc) {
        if (WaitForSingleObject(impl_->server_proc, 200) != WAIT_OBJECT_0) {
            TerminateProcess(impl_->server_proc, 0);
            WaitForSingleObject(impl_->server_proc, 300);
        }
        CloseHandle(impl_->server_proc);
        impl_->server_proc = nullptr;
    }

    if (impl_->server_log_pipe) {
        CloseHandle(impl_->server_log_pipe);
        impl_->server_log_pipe = nullptr;
    }
    if (impl_->server_log_thread.joinable()) {
        impl_->server_log_thread.join();
    }
    {
        std::lock_guard<std::mutex> lk(impl_->log_mu);
        impl_->server_log_tail.clear();
    }

    if (impl_->tunnel_active && !impl_->adb_exe.empty() && !impl_->active_serial.empty()) {
        std::string dummy;
        std::wstring rm_args =
            L"-s \"" + Utf8ToWideLocal(impl_->active_serial) +
            L"\" forward --remove tcp:" + std::to_wstring(impl_->tunnel_port);
        RunAdbSyncCommand(impl_->adb_exe, rm_args, 1500U, dummy, nullptr);
        impl_->tunnel_active = false;
        impl_->tunnel_port = 0;
    }
    impl_->active_serial.clear();

    if (impl_->wsa_started) {
        WSACleanup();
        impl_->wsa_started = false;
    }
#endif
}

}  // namespace android_vcam
