#include "android_vcam/usb_aoa_session.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace android_vcam {

#ifdef _WIN32
namespace {

#define LIBUSB_CALL WINAPI

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

#pragma pack(push, 1)
struct libusb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
};
#pragma pack(pop)

struct libusb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    uint8_t bRefresh;
    uint8_t bSynchAddress;
    const unsigned char* extra;
    int extra_length;
};

struct libusb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
    const libusb_endpoint_descriptor* endpoint;
    const unsigned char* extra;
    int extra_length;
};

struct libusb_interface {
    const libusb_interface_descriptor* altsetting;
    int num_altsetting;
};

struct libusb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t MaxPower;
    const libusb_interface* interface;
    const unsigned char* extra;
    int extra_length;
};

struct LibUsbApi {
    HMODULE dll = nullptr;
    int(LIBUSB_CALL* init)(libusb_context**) = nullptr;
    void(LIBUSB_CALL* exit)(libusb_context*) = nullptr;
    SSIZE_T(LIBUSB_CALL* get_device_list)(libusb_context*, libusb_device***) = nullptr;
    void(LIBUSB_CALL* free_device_list)(libusb_device**, int) = nullptr;
    int(LIBUSB_CALL* get_device_descriptor)(libusb_device*, libusb_device_descriptor*) = nullptr;
    int(LIBUSB_CALL* get_device_speed)(libusb_device*) = nullptr;
    int(LIBUSB_CALL* get_config_descriptor)(libusb_device*, uint8_t, libusb_config_descriptor**) = nullptr;
    void(LIBUSB_CALL* free_config_descriptor)(libusb_config_descriptor*) = nullptr;
    int(LIBUSB_CALL* open)(libusb_device*, libusb_device_handle**) = nullptr;
    void(LIBUSB_CALL* close)(libusb_device_handle*) = nullptr;
    int(LIBUSB_CALL* claim_interface)(libusb_device_handle*, int) = nullptr;
    int(LIBUSB_CALL* release_interface)(libusb_device_handle*, int) = nullptr;
    int(LIBUSB_CALL* clear_halt)(libusb_device_handle*, unsigned char) = nullptr;
    int(LIBUSB_CALL* control_transfer)(
        libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, unsigned int) = nullptr;
    int(LIBUSB_CALL* bulk_transfer)(
        libusb_device_handle*, unsigned char, unsigned char*, int, int*, unsigned int) = nullptr;
    const char*(LIBUSB_CALL* strerror)(int) = nullptr;
};

constexpr uint16_t kGoogleAoaVid = 0x18D1U;
constexpr uint8_t kEndpointTransferMask = 0x03U;
constexpr uint8_t kEndpointTransferBulk = 0x02U;
constexpr uint8_t kEndpointDirIn = 0x80U;

// AOA Control Requests (USB Vendor Request Type)
constexpr uint8_t kUsbReqVendorIn  = 0xC0U;  // LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR
constexpr uint8_t kUsbReqVendorOut = 0x40U;  // LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR
constexpr uint8_t kAoaReqGetProtocol = 51U;
constexpr uint8_t kAoaReqSendString  = 52U;
constexpr uint8_t kAoaReqStart       = 53U;

bool IsAoaAccessoryPid(uint16_t vid, uint16_t pid) {
    if (vid != kGoogleAoaVid) return false;
    return (pid == 0x2D00U || pid == 0x2D01U || pid == 0x2D04U || pid == 0x2D05U);
}

bool IsCandidateAndroidVid(uint16_t vid) {
    static const uint16_t kAndroidVids[] = {
        0x18D1U,  // Google
        0x04E8U,  // Samsung
        0x2717U,  // Xiaomi / Redmi / POCO
        0x22D9U,  // OPPO / Realme / OnePlus
        0x2A70U,  // OnePlus
        0x2D95U,  // vivo / iQOO
        0x0FCEU,  // Sony
        0x12D1U,  // Huawei / Honor
        0x22B8U,  // Motorola
        0x0BB4U,  // HTC
        0x2916U,  // Nothing
        0x19D2U,  // ZTE / RedMagic
        0x0E8DU,  // MediaTek
        0x05C6U,  // Qualcomm
        0x0B05U,  // ASUS
        0x17EFU,  // Lenovo
        0x1004U,  // LG
        0x04DDU,  // Sharp
        0x2AE5U,  // Fairphone
    };
    for (uint16_t v : kAndroidVids) {
        if (vid == v) return true;
    }
    return false;
}

UsbLinkSpeed ConvertLibUsbSpeed(int raw_speed, uint16_t bcd_usb) {
    if (raw_speed >= 1 && raw_speed <= 6) {
        return static_cast<UsbLinkSpeed>(raw_speed);
    }
    if (bcd_usb >= 0x0310) return UsbLinkSpeed::SuperPlus_10Gbps;
    if (bcd_usb >= 0x0300) return UsbLinkSpeed::Super_5Gbps;
    if (bcd_usb >= 0x0200) return UsbLinkSpeed::High_480Mbps;
    if (bcd_usb >= 0x0110) return UsbLinkSpeed::Full_12Mbps;
    return UsbLinkSpeed::Unknown;
}

bool LoadLibUsbDll(const std::filesystem::path& base_dir, LibUsbApi& api, std::string* out_error) {
    std::filesystem::path p1 = base_dir / L"runtime" / L"libusb-1.0.dll";
    std::filesystem::path p2 = base_dir / L"libusb-1.0.dll";
    std::error_code ec;
    std::filesystem::path target = std::filesystem::exists(p1, ec) ? p1 : p2;

    api.dll = LoadLibraryExW(
        target.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!api.dll) {
        if (out_error) {
            *out_error = "Missing runtime/libusb-1.0.dll required for USB AOA mode.";
        }
        return false;
    }

#define RESOLVE_FN(field, sym)                                                       \
    api.field = reinterpret_cast<decltype(api.field)>(GetProcAddress(api.dll, sym)); \
    if (!api.field) {                                                                \
        FreeLibrary(api.dll);                                                        \
        api.dll = nullptr;                                                           \
        if (out_error) *out_error = "Missing export " sym " in libusb-1.0.dll.";     \
        return false;                                                                \
    }

    RESOLVE_FN(init, "libusb_init");
    RESOLVE_FN(exit, "libusb_exit");
    RESOLVE_FN(get_device_list, "libusb_get_device_list");
    RESOLVE_FN(free_device_list, "libusb_free_device_list");
    RESOLVE_FN(get_device_descriptor, "libusb_get_device_descriptor");
    RESOLVE_FN(get_device_speed, "libusb_get_device_speed");
    RESOLVE_FN(get_config_descriptor, "libusb_get_config_descriptor");
    RESOLVE_FN(free_config_descriptor, "libusb_free_config_descriptor");
    RESOLVE_FN(open, "libusb_open");
    RESOLVE_FN(close, "libusb_close");
    RESOLVE_FN(claim_interface, "libusb_claim_interface");
    RESOLVE_FN(release_interface, "libusb_release_interface");
    RESOLVE_FN(clear_halt, "libusb_clear_halt");
    RESOLVE_FN(control_transfer, "libusb_control_transfer");
    RESOLVE_FN(bulk_transfer, "libusb_bulk_transfer");
    api.strerror = reinterpret_cast<decltype(api.strerror)>(GetProcAddress(api.dll, "libusb_strerror"));
#undef RESOLVE_FN

    return true;
}

bool SendAoaIdentityString(LibUsbApi& api, libusb_device_handle* h, uint16_t index, const char* str) {
    size_t len = std::strlen(str) + 1U;
    int rc = api.control_transfer(
        h,
        kUsbReqVendorOut,
        kAoaReqSendString,
        0,
        index,
        reinterpret_cast<unsigned char*>(const_cast<char*>(str)),
        static_cast<uint16_t>(len),
        1000U);
    return rc >= 0;
}

}  // namespace
#endif

struct UsbAoaSession::Impl {
#ifdef _WIN32
    LibUsbApi api{};
    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;
    int claimed_iface = -1;
    uint8_t bulk_in_ep = 0;
    uint8_t bulk_out_ep = 0;
#endif
};

UsbAoaSession::UsbAoaSession() : impl_(new Impl()) {}

UsbAoaSession::~UsbAoaSession() {
    StopAndClose();
    delete impl_;
}

bool UsbAoaSession::OpenAndNegotiateAoa(
    const UsbAoaSessionConfig& cfg,
    ConnectedAndroidInfo& out_device_info,
    std::string* out_error) {
    StopAndClose();
    audio_packets_.store(0, std::memory_order_relaxed);
    out_device_info = ConnectedAndroidInfo{};

#ifdef _WIN32
    if (!LoadLibUsbDll(cfg.base_dir, impl_->api, out_error)) {
        return false;
    }
    if (impl_->api.init(&impl_->ctx) != 0 || !impl_->ctx) {
        StopAndClose();
        if (out_error) *out_error = "libusb_init failed for USB AOA session.";
        return false;
    }

    auto try_open_aoa_accessory = [&](std::string* open_err) -> bool {
        libusb_device** list = nullptr;
        SSIZE_T cnt = impl_->api.get_device_list(impl_->ctx, &list);
        if (cnt <= 0 || !list) return false;

        bool opened = false;
        for (SSIZE_T i = 0; i < cnt; ++i) {
            libusb_device_descriptor desc{};
            if (impl_->api.get_device_descriptor(list[i], &desc) != 0) continue;
            if (!IsAoaAccessoryPid(desc.idVendor, desc.idProduct)) continue;

            libusb_device_handle* h = nullptr;
            int orc = impl_->api.open(list[i], &h);
            if (orc != 0 || !h) {
                if (open_err) {
                    *open_err = "libusb_open failed on AOA accessory 18D1:" +
                                std::to_string(desc.idProduct) + " (code " + std::to_string(orc) +
                                ") — click '🔧 安裝／修復驅動' once while phone is in AOA mode.";
                }
                continue;
            }

            // Interface 0 is always the Google AOA Accessory Bulk interface per AOA spec
            libusb_config_descriptor* cdesc = nullptr;
            uint8_t ep_in = 0, ep_out = 0;
            int aoa_if_num = 0;
            if (impl_->api.get_config_descriptor(list[i], 0, &cdesc) == 0 && cdesc) {
                if (cdesc->bNumInterfaces > 0 && cdesc->interface[0].num_altsetting > 0) {
                    const auto& alt = cdesc->interface[0].altsetting[0];
                    aoa_if_num = alt.bInterfaceNumber;
                    for (uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                        const auto& ep = alt.endpoint[e];
                        if ((ep.bmAttributes & kEndpointTransferMask) == kEndpointTransferBulk) {
                            if ((ep.bEndpointAddress & kEndpointDirIn) != 0) {
                                if (ep_in == 0) ep_in = ep.bEndpointAddress;
                            } else {
                                if (ep_out == 0) ep_out = ep.bEndpointAddress;
                            }
                        }
                    }
                }
                impl_->api.free_config_descriptor(cdesc);
            }

            if (ep_in == 0 || ep_out == 0) {
                impl_->api.close(h);
                continue;
            }

            int crc = impl_->api.claim_interface(h, aoa_if_num);
            if (crc != 0) {
                if (open_err) {
                    *open_err = "Failed to claim AOA USB interface (code " + std::to_string(crc) + ").";
                }
                impl_->api.close(h);
                continue;
            }

            impl_->handle = h;
            impl_->claimed_iface = aoa_if_num;
            impl_->bulk_in_ep = ep_in;
            impl_->bulk_out_ep = ep_out;

            int raw_spd = impl_->api.get_device_speed(list[i]);
            out_device_info.usb_speed = ConvertLibUsbSpeed(raw_spd, desc.bcdUSB);
            out_device_info.display_model = "Android USB AOA Accessory";
            opened = true;
            break;
        }

        impl_->api.free_device_list(list, 1);
        return opened;
    };

    std::string aoa_open_err;
    if (!try_open_aoa_accessory(&aoa_open_err)) {
        // Attempt AOA 51/52/53 switch on any attached standard Android USB device
        libusb_device** list = nullptr;
        SSIZE_T cnt = impl_->api.get_device_list(impl_->ctx, &list);
        bool switched_to_aoa = false;
        bool saw_android_candidate = false;
        int last_open_rc = 0;

        if (cnt > 0 && list) {
            for (SSIZE_T i = 0; i < cnt; ++i) {
                libusb_device_descriptor desc{};
                if (impl_->api.get_device_descriptor(list[i], &desc) != 0) continue;
                if (!IsCandidateAndroidVid(desc.idVendor)) continue;
                saw_android_candidate = true;

                libusb_device_handle* h = nullptr;
                int orc = impl_->api.open(list[i], &h);
                if (orc != 0 || !h) {
                    last_open_rc = orc;
                    continue;
                }

                uint8_t proto_buf[2] = {0, 0};
                int prc = impl_->api.control_transfer(
                    h, kUsbReqVendorIn, kAoaReqGetProtocol, 0, 0, proto_buf, 2, 1000U);
                uint16_t aoa_ver = static_cast<uint16_t>(proto_buf[0] | (proto_buf[1] << 8));
                if (prc == 2 && aoa_ver >= 1U) {
                    bool ok_str =
                        SendAoaIdentityString(impl_->api, h, 0, kAoaManufacturer) &&
                        SendAoaIdentityString(impl_->api, h, 1, kAoaModel) &&
                        SendAoaIdentityString(impl_->api, h, 2, kAoaDescription) &&
                        SendAoaIdentityString(impl_->api, h, 3, kAoaVersion) &&
                        SendAoaIdentityString(impl_->api, h, 4, kAoaUri) &&
                        SendAoaIdentityString(impl_->api, h, 5, kAoaSerial);
                    if (ok_str) {
                        int src = impl_->api.control_transfer(
                            h, kUsbReqVendorOut, kAoaReqStart, 0, 0, nullptr, 0, 1000U);
                        if (src >= 0) {
                            switched_to_aoa = true;
                        }
                    }
                }
                impl_->api.close(h);
                if (switched_to_aoa) break;
            }
            impl_->api.free_device_list(list, 1);
        }

        if (switched_to_aoa) {
            auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(5500);
            while (std::chrono::steady_clock::now() < wait_until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (try_open_aoa_accessory(&aoa_open_err)) {
                    break;
                }
            }
        } else if (saw_android_candidate && last_open_rc != 0) {
            StopAndClose();
            if (out_error) {
                *out_error = "libusb_open access denied on Android USB node (code " +
                             std::to_string(last_open_rc) +
                             ") — please click '🔧 安裝／修復驅動' once with phone plugged in.";
            }
            return false;
        }
    }

    if (!impl_->handle) {
        StopAndClose();
        if (out_error) {
            *out_error = aoa_open_err.empty()
                             ? "Waiting for Android phone over USB-C (AOA Companion Mode)..."
                             : aoa_open_err;
        }
        return false;
    }

    // Send 24-byte JCAO START_STREAM configuration frame to TJCap Companion APK
    auto start_frame = BuildTJCapAoaControlFrame(
        kAoaCmdStartStream,
        static_cast<uint16_t>(cfg.max_fps),
        cfg.max_size,
        cfg.video_bit_rate,
        cfg.enable_audio,
        cfg.lock_initial_orientation);
    int transferred = 0;
    {
        std::lock_guard<std::mutex> lk(write_mu_);
        impl_->api.bulk_transfer(
            impl_->handle,
            impl_->bulk_out_ep,
            start_frame.data(),
            static_cast<int>(start_frame.size()),
            &transferred,
            600U);
    }

    return true;
#else
    (void)cfg;
    if (out_error) *out_error = "UsbAoaSession is only supported on Windows.";
    return false;
#endif
}

bool UsbAoaSession::RunBulkStreamLoop(
    const std::atomic<bool>& stop_flag,
    const OnVideoPacketFn& on_video,
    const OnAudioPacketFn& on_audio,
    const OnAudioDisabledFn& on_audio_disabled,
    std::string* out_error) {
#ifdef _WIN32
    if (!impl_->handle) {
        if (out_error) *out_error = "AOA USB handle is not open.";
        return false;
    }

    TJCapAoaStreamParser parser(
        [&](const std::string& model, int sdk_int, uint32_t w, uint32_t h) {
            std::printf(
                "[AOA] TJCap Companion APK connected: %s (Android API %d, %ux%u)\n",
                model.c_str(),
                sdk_int,
                w,
                h);
            std::fflush(stdout);
        },
        on_video,
        [&](const AndroidPcmAudioPacket& apkt) {
            audio_packets_.fetch_add(1ULL, std::memory_order_relaxed);
            if (on_audio) {
                on_audio(apkt);
            }
        },
        [&](int sdk_int) {
            if (on_audio_disabled) {
                on_audio_disabled(sdk_int, false);
            }
        });

    std::vector<uint8_t> rbuf(131072);
    constexpr int kLibusbErrorTimeout = -7;

    while (!stop_flag.load(std::memory_order_relaxed)) {
        int transferred = 0;
        int rc = impl_->api.bulk_transfer(
            impl_->handle,
            impl_->bulk_in_ep,
            rbuf.data(),
            static_cast<int>(rbuf.size()),
            &transferred,
            250U);
        if (rc == 0 || (rc == kLibusbErrorTimeout && transferred > 0)) {
            if (transferred > 0) {
                std::string perr;
                if (!parser.FeedUsbBulkBytes(
                        std::span<const uint8_t>(rbuf.data(), static_cast<size_t>(transferred)), &perr)) {
                    if (out_error) *out_error = perr;
                    return false;
                }
            }
            continue;
        }
        if (rc == kLibusbErrorTimeout) {
            continue;
        }
        if (out_error) {
            *out_error = "AOA Bulk IN transfer interrupted (libusb code " + std::to_string(rc) + ").";
        }
        return false;
    }

    return true;
#else
    (void)stop_flag;
    (void)on_video;
    (void)on_audio;
    (void)on_audio_disabled;
    if (out_error) *out_error = "UsbAoaSession is only supported on Windows.";
    return false;
#endif
}

void UsbAoaSession::StopAndClose() {
#ifdef _WIN32
    if (impl_->handle) {
        auto stop_frame = BuildTJCapAoaControlFrame(kAoaCmdStopStream, 60, 1920, 16000000, false, true);
        int transferred = 0;
        {
            std::lock_guard<std::mutex> lk(write_mu_);
            impl_->api.bulk_transfer(
                impl_->handle,
                impl_->bulk_out_ep,
                stop_frame.data(),
                static_cast<int>(stop_frame.size()),
                &transferred,
                250U);
        }
        if (impl_->claimed_iface >= 0) {
            impl_->api.release_interface(impl_->handle, impl_->claimed_iface);
            impl_->claimed_iface = -1;
        }
        impl_->api.close(impl_->handle);
        impl_->handle = nullptr;
    }
    if (impl_->ctx) {
        impl_->api.exit(impl_->ctx);
        impl_->ctx = nullptr;
    }
    if (impl_->api.dll) {
        FreeLibrary(impl_->api.dll);
        impl_->api = LibUsbApi{};
    }
#endif
}

}  // namespace android_vcam
