#include "android_vcam/wasapi_renderer.h"
#include "android_vcam/diagnostics.h"
#include "android_vcam/shared_ipc.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
#endif

namespace android_vcam {

#ifdef _WIN32
namespace {

// PKEY_Device_FriendlyName {A45C254E-DF1C-4EFD-8020-67D146A850E0}, 14
static const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace
#endif

WasapiAudioRouter::WasapiAudioRouter() = default;

WasapiAudioRouter::~WasapiAudioRouter() {
    Stop();
}

std::vector<AudioEndpointInfo> WasapiAudioRouter::EnumerateRenderEndpoints() {
    std::vector<AudioEndpointInfo> result;
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return result;
    }

    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(collection->Item(i, &dev)) || !dev) {
                continue;
            }
            LPWSTR dev_id = nullptr;
            dev->GetId(&dev_id);

            IPropertyStore* props = nullptr;
            std::string friendly;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                    friendly = WideToUtf8(pv.pwszVal);
                }
                PropVariantClear(&pv);
                props->Release();
            }

            if (dev_id && !friendly.empty()) {
                result.push_back({dev_id, friendly});
            }
            if (dev_id) {
                CoTaskMemFree(dev_id);
            }
            dev->Release();
        }
        collection->Release();
    }
    enumerator->Release();
#endif
    return result;
}

bool WasapiAudioRouter::StartAutoRoute(const std::string& shm_name, const std::string& target_name_substring) {
    Stop();
#ifdef _WIN32
    const std::string needle = ToLowerAscii(target_name_substring);

    // "__default__" / "default": follow the current Windows default playback device and
    // automatically re-route when it changes (e.g. plugging in headphones / Bluetooth).
    if (needle == "__default__" || needle == "default") {
        active_endpoint_name_ = "Windows 預設播放裝置";
        stop_requested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        worker_ = std::thread(&WasapiAudioRouter::WorkerLoop, this, shm_name, std::wstring());
        return true;
    }

    auto endpoints = EnumerateRenderEndpoints();
    if (endpoints.empty()) {
        return false;
    }

    const AudioEndpointInfo* chosen = nullptr;
    if (!needle.empty()) {
        for (const auto& ep : endpoints) {
            if (ToLowerAscii(ep.friendly_name_utf8).find(needle) != std::string::npos) {
                chosen = &ep;
                break;
            }
        }
    } else {
        // Auto-detect VB-Audio Virtual Cable ("CABLE Input") or Virtual Audio Cable
        for (const auto& ep : endpoints) {
            std::string low = ToLowerAscii(ep.friendly_name_utf8);
            if (low.find("cable input") != std::string::npos ||
                low.find("vb-audio") != std::string::npos) {
                chosen = &ep;
                break;
            }
        }
    }

    if (!chosen) {
        return false;
    }

    active_endpoint_name_ = chosen->friendly_name_utf8;
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&WasapiAudioRouter::WorkerLoop, this, shm_name, chosen->id);
    return true;
#else
    (void)shm_name;
    (void)target_name_substring;
    return false;
#endif
}

void WasapiAudioRouter::Stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
    active_endpoint_name_.clear();
}

#ifdef _WIN32
namespace {

std::wstring QueryDefaultRenderId(IMMDeviceEnumerator* enumerator) {
    std::wstring out;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) && dev) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            out = id;
            CoTaskMemFree(id);
        }
        dev->Release();
    }
    return out;
}

std::string QueryFriendlyName(IMMDevice* dev) {
    std::string friendly;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
            friendly = WideToUtf8(pv.pwszVal);
        }
        PropVariantClear(&pv);
        props->Release();
    }
    return friendly;
}

}  // namespace
#endif

void WasapiAudioRouter::WorkerLoop(std::string shm_name, std::wstring device_id) {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const bool follow_default = device_id.empty();
    IMMDeviceEnumerator* enumerator = nullptr;
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!event_handle ||
        FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator))) ||
        !enumerator) {
        if (event_handle) CloseHandle(event_handle);
        running_.store(false, std::memory_order_release);
        CoUninitialize();
        return;
    }

    SharedCaptureConsumer consumer;
    consumer.Open(shm_name);

    auto sleep_interruptible = [&](int ms) {
        for (int t = 0; t < ms && !stop_requested_.load(std::memory_order_relaxed); t += 50) {
            Sleep(50);
        }
    };

    // Outer loop: (re)open the endpoint. Handles headphone plug/unplug, Bluetooth reconnects,
    // default-device switches and AUDCLNT_E_DEVICE_INVALIDATED without stopping the router.
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        IMMDevice* device = nullptr;
        IAudioClient* audio_client = nullptr;
        IAudioRenderClient* render_client = nullptr;
        std::wstring current_id = follow_default ? QueryDefaultRenderId(enumerator) : device_id;

        auto release_device = [&]() {
            if (audio_client) audio_client->Stop();
            if (render_client) render_client->Release();
            if (audio_client) audio_client->Release();
            if (device) device->Release();
            render_client = nullptr;
            audio_client = nullptr;
            device = nullptr;
        };

        if (current_id.empty() ||
            FAILED(enumerator->GetDevice(current_id.c_str(), &device)) || !device ||
            FAILED(device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client))) ||
            !audio_client) {
            release_device();
            sleep_interruptible(500);
            continue;
        }

        std::string friendly = QueryFriendlyName(device);
        if (!friendly.empty()) {
            active_endpoint_name_ = friendly;
        }

        // Request 48000 Hz 16-bit Stereo PCM with Windows automatic mixer SRC/format conversion if needed
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = 48000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 4;
        wfx.nAvgBytesPerSec = 48000 * 4;

        DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                             AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        // 20ms buffer (200,000 100ns units)
        UINT32 buffer_frames = 0;
        if (FAILED(audio_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED, stream_flags, 200000, 0, &wfx, nullptr)) ||
            FAILED(audio_client->SetEventHandle(event_handle)) ||
            FAILED(audio_client->GetService(
                __uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client))) ||
            !render_client ||
            FAILED(audio_client->GetBufferSize(&buffer_frames)) ||
            FAILED(audio_client->Start())) {
            release_device();
            sleep_interruptible(500);
            continue;
        }

        std::cout << "[Audio] 電腦端監聽輸出 -> \"" << active_endpoint_name_ << "\"\n";

        ULONGLONG last_default_check = GetTickCount64();
        while (!stop_requested_.load(std::memory_order_relaxed)) {
            DWORD wait_rc = WaitForSingleObject(event_handle, 50);
            if (stop_requested_.load(std::memory_order_relaxed)) {
                break;
            }
            if (wait_rc != WAIT_OBJECT_0 && wait_rc != WAIT_TIMEOUT) {
                break;
            }

            if (follow_default) {
                ULONGLONG now = GetTickCount64();
                if (now - last_default_check >= 500ULL) {
                    last_default_check = now;
                    std::wstring def_id = QueryDefaultRenderId(enumerator);
                    if (!def_id.empty() && def_id != current_id) {
                        break;  // Windows default output changed (e.g. headphones plugged in) -> re-route
                    }
                }
            }

            if (!consumer.IsOpen()) {
                consumer.Open(shm_name);
            }

            UINT32 padding = 0;
            if (FAILED(audio_client->GetCurrentPadding(&padding))) {
                std::cout << FormatHostDiagLine(
                                 DiagCode::E501_AudioEndpointLost,
                                 DiagSeverity::Warning,
                                 "電腦端監聽音訊裝置 \"" + active_endpoint_name_ +
                                     "\" 已中斷或拔除，正在自動重新連線（OBS 內的 TJCap for Android (Audio) 仍正常運作）。")
                          << "\n";
                break;
            }
            UINT32 avail_frames = (buffer_frames > padding) ? (buffer_frames - padding) : 0;
            if (avail_frames == 0) {
                continue;
            }

            BYTE* data = nullptr;
            if (SUCCEEDED(render_client->GetBuffer(avail_frames, &data)) && data) {
                consumer.ReadAudioPcmS16LE(reinterpret_cast<int16_t*>(data), avail_frames);
                render_client->ReleaseBuffer(avail_frames, 0);
            }
        }

        release_device();
        if (!stop_requested_.load(std::memory_order_relaxed)) {
            sleep_interruptible(300);
        }
    }

    enumerator->Release();
    CloseHandle(event_handle);
    running_.store(false, std::memory_order_release);
    CoUninitialize();
#else
    (void)shm_name;
    (void)device_id;
#endif
}

}  // namespace android_vcam
