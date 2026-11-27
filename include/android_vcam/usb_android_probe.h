#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace android_vcam {

enum class UsbLinkSpeed : int {
    Unknown = 0,
    Low_1_5Mbps = 1,       // USB 1.0 Low-Speed (1.5 Mbps)
    Full_12Mbps = 2,       // USB 1.1 Full-Speed (12 Mbps)
    High_480Mbps = 3,      // USB 2.0 High-Speed (480 Mbps)
    Super_5Gbps = 4,       // USB 3.0 / 3.2 Gen 1 SuperSpeed (5 Gbps)
    SuperPlus_10Gbps = 5,  // USB 3.1 / 3.2 Gen 2 SuperSpeed+ (10 Gbps)
    SuperPlus_20Gbps = 6,  // USB 3.2 Gen 2x2 SuperSpeed+ (20 Gbps)
};

inline const char* UsbLinkSpeedName(UsbLinkSpeed speed) {
    switch (speed) {
        case UsbLinkSpeed::Low_1_5Mbps:
            return "USB 1.0 Low-Speed (1.5 Mbps)";
        case UsbLinkSpeed::Full_12Mbps:
            return "USB 1.1 Full-Speed (12 Mbps)";
        case UsbLinkSpeed::High_480Mbps:
            return "USB 2.0 High-Speed (480 Mbps)";
        case UsbLinkSpeed::Super_5Gbps:
            return "USB 3.0 SuperSpeed (5 Gbps)";
        case UsbLinkSpeed::SuperPlus_10Gbps:
            return "USB 3.2 Gen 2 SuperSpeed+ (10 Gbps)";
        case UsbLinkSpeed::SuperPlus_20Gbps:
            return "USB 3.2 Gen 2x2 SuperSpeed+ (20 Gbps)";
        default:
            return "Unknown Speed";
    }
}

// Returns the maximum selectable resolution long-edge cap allowed for a given USB link speed tier
inline constexpr uint32_t MaxSelectableDimForUsbSpeed(UsbLinkSpeed speed) {
    switch (speed) {
        case UsbLinkSpeed::Super_5Gbps:
        case UsbLinkSpeed::SuperPlus_10Gbps:
        case UsbLinkSpeed::SuperPlus_20Gbps:
            return 3840U;  // USB 3.x (5-20 Gbps): unlock full 2.8K / 4K unrestricted native tier
        case UsbLinkSpeed::High_480Mbps:
            return 2560U;  // USB 2.0 (480 Mbps): cap selectable max at 2K (2560p)
        case UsbLinkSpeed::Low_1_5Mbps:
        case UsbLinkSpeed::Full_12Mbps:
            return 1280U;  // USB 1.1 (12 Mbps): cap selectable max at 720p (1280p)
        default:
            return 2560U;  // Unplugged / unknown: conservative 2K cap until USB 3.x is detected
    }
}

// Computes optimal target H.264 encoder bitrate (bps) locked for the session given resolution, FPS, and USB speed
inline constexpr uint32_t RecommendedBitrateBps(uint32_t max_dim, uint32_t max_fps, UsbLinkSpeed speed) {
    const bool is_usb3 = (speed == UsbLinkSpeed::Super_5Gbps ||
                          speed == UsbLinkSpeed::SuperPlus_10Gbps ||
                          speed == UsbLinkSpeed::SuperPlus_20Gbps);
    uint32_t base_mbps = 16U;
    if (max_dim >= 3000U) {
        base_mbps = is_usb3 ? 32U : 22U;
    } else if (max_dim >= 2400U) {
        base_mbps = is_usb3 ? 24U : 18U;
    } else if (max_dim >= 1800U) {
        base_mbps = is_usb3 ? 18U : 14U;
    } else {
        base_mbps = 8U;
    }
    if (max_fps >= 90U) {
        base_mbps = (base_mbps * 135U) / 100U;
        if (!is_usb3 && base_mbps > 26U) base_mbps = 26U;
    }
    if (speed == UsbLinkSpeed::Full_12Mbps || speed == UsbLinkSpeed::Low_1_5Mbps) {
        base_mbps = 4U;
    }
    return base_mbps * 1000000U;
}

enum class AdbDeviceState {
    None,
    Authorized,    // "device" -> ready to stream!
    Unauthorized,  // "unauthorized" -> user needs to tap "Allow USB debugging" on phone screen
    Offline,       // "offline" -> cable glitch or ADB handshake pending
    NoPermissions, // driver / WinUSB access issue
};

struct AdbDeviceEntry {
    std::string serial;
    AdbDeviceState state = AdbDeviceState::None;
    std::string model;
    std::string product;
    std::string device_codename;
    bool is_wireless = false;  // true if serial looks like ip:port or _adb-tls-connect
};

// Pure C++20 parser for `adb devices -l` output (unit-tested on Linux & Windows)
std::vector<AdbDeviceEntry> ParseAdbDevicesOutput(std::string_view raw_output);

// Resolves bundled runtime/adb.exe and runtime/jcast-server.jar relative to base_dir
std::filesystem::path ResolveBundledAdbPath(const std::filesystem::path& base_dir);
std::filesystem::path ResolveBundledServerJarPath(const std::filesystem::path& base_dir);

struct AndroidUsbProbeSummary {
    bool usb_android_present = false;
    bool usb_adb_interface_present = false;  // Class 0xFF SubClass 0x42 Protocol 0x01 exposed
    UsbLinkSpeed usb_speed = UsbLinkSpeed::Unknown;
    uint16_t bcd_usb = 0;
    std::string usb_hardware_desc;

    std::vector<AdbDeviceEntry> adb_devices;
};

// Non-intrusively probes Windows SetupAPI + USB Hub IOCTLs for connected Android USB hardware & link speed
UsbLinkSpeed ProbeConnectedAndroidLinkSpeed(
    uint16_t* out_bcd_usb = nullptr,
    bool* out_any_android_usb = nullptr,
    bool* out_adb_iface_found = nullptr,
    std::string* out_hw_desc = nullptr);

// Runs a fast hidden `adb devices -l` query using bundled runtime/adb.exe (only starts daemon if requested or USB present)
bool QueryConnectedAdbDevices(
    const std::filesystem::path& adb_exe_path,
    std::vector<AdbDeviceEntry>& out_devices,
    std::string* out_error = nullptr,
    uint32_t timeout_ms = 2500U);

}  // namespace android_vcam
