#include "android_vcam/usb_android_probe.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#ifndef FILE_DEVICE_USB
#define FILE_DEVICE_USB 0x00000022
#endif
#endif

namespace android_vcam {

namespace {

std::string TrimAscii(std::string_view sv) {
    size_t b = 0;
    while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) {
        ++b;
    }
    size_t e = sv.size();
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) {
        --e;
    }
    return std::string(sv.substr(b, e - b));
}

bool LooksLikeWirelessSerial(std::string_view serial) {
    if (serial.find("_adb-tls-") != std::string_view::npos) {
        return true;
    }
    // IPv4:port pattern e.g. 192.168.1.25:5555
    size_t colon = serial.find(':');
    if (colon != std::string_view::npos && serial.find('.') < colon) {
        return true;
    }
    return false;
}

#ifdef _WIN32
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring ToLowerWide(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

std::vector<std::wstring> ParseMultiSzWide(const BYTE* raw, DWORD byte_len) {
    std::vector<std::wstring> list;
    if (!raw || byte_len < sizeof(wchar_t)) return list;
    const auto* wptr = reinterpret_cast<const wchar_t*>(raw);
    size_t max_chars = byte_len / sizeof(wchar_t);
    size_t start = 0;
    while (start < max_chars && wptr[start] != L'\0') {
        size_t end = start;
        while (end < max_chars && wptr[end] != L'\0') ++end;
        list.emplace_back(wptr + start, end - start);
        start = end + 1;
    }
    return list;
}

// Dedicated Android smartphone/tablet Vendor IDs
bool IsDedicatedAndroidVid(const std::wstring& low_hwid) {
    static const wchar_t* kDedicatedVids[] = {
        L"vid_18d1",  // Google (Pixel / Nexus / AOSP ADB)
        L"vid_04e8",  // Samsung Galaxy / Tab
        L"vid_2717",  // Xiaomi / Redmi / POCO
        L"vid_22d9",  // OPPO / Realme / OnePlus
        L"vid_2a70",  // OnePlus
        L"vid_2d95",  // vivo / iQOO
        L"vid_0fce",  // Sony Xperia
        L"vid_12d1",  // Huawei / Honor
        L"vid_22b8",  // Motorola
        L"vid_0bb4",  // HTC
        L"vid_2916",  // Nothing
        L"vid_19d2",  // ZTE / Nubia / RedMagic
        L"vid_0e8d",  // MediaTek Android
        L"vid_05c6",  // Qualcomm Android
        L"vid_04dd",  // Sharp Aquos
        L"vid_2ae5",  // Fairphone
        L"vid_1004",  // LG Mobile
    };
    for (const wchar_t* v : kDedicatedVids) {
        if (low_hwid.find(v) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

// Multi-product PC+Phone OEMs (ASUS, Lenovo) — require MTP/PTP/ADB composite hint to avoid matching mice/keyboards
bool IsMultiProductAndroidVid(const std::wstring& low_hwid) {
    return low_hwid.find(L"vid_0b05") != std::wstring::npos ||
           low_hwid.find(L"vid_17ef") != std::wstring::npos;
}

// GUID_DEVINTERFACE_USB_HUB {f18a0e88-c30c-11d0-8815-00a0c906bed8}
static const GUID kGuidDevInterfaceUsbHub = {
    0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8}};

// Standard Win32 USB IOCTL structures (packed per usbioctl.h ABI)
constexpr DWORD kIoctlUsbGetNodeConnectionInfoEx =
    CTL_CODE(FILE_DEVICE_USB, 274, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kIoctlUsbGetNodeConnectionInfoExV2 =
    CTL_CODE(FILE_DEVICE_USB, 279, METHOD_BUFFERED, FILE_ANY_ACCESS);

#pragma pack(push, 1)
struct UsbDeviceDescriptorWire {
    UCHAR bLength;
    UCHAR bDescriptorType;
    USHORT bcdUSB;
    UCHAR bDeviceClass;
    UCHAR bDeviceSubClass;
    UCHAR bDeviceProtocol;
    UCHAR bMaxPacketSize0;
    USHORT idVendor;
    USHORT idProduct;
    USHORT bcdDevice;
    UCHAR iManufacturer;
    UCHAR iProduct;
    UCHAR iSerialNumber;
    UCHAR bNumConfigurations;
};

struct UsbNodeConnectionInfoExWire {
    ULONG ConnectionIndex;
    UsbDeviceDescriptorWire DeviceDescriptor;
    UCHAR CurrentConfigurationValue;
    UCHAR Speed;  // 0=Low, 1=Full, 2=High, 3=Super
    BOOLEAN DeviceIsHub;
    USHORT DeviceAddress;
    ULONG NumberOfOpenPipes;
    ULONG ConnectionStatus;
};

struct UsbNodeConnectionInfoExV2Wire {
    ULONG ConnectionIndex;
    ULONG Length;
    ULONG SupportedUsbProtocols;  // bit0=Usb110, bit1=Usb200, bit2=Usb300
    ULONG Flags;                  // bit0=OperatingAtSuperSpeedOrHigher, bit2=OperatingAtSuperSpeedPlusOrHigher
};
#pragma pack(pop)

UsbLinkSpeed QueryUsbPortSpeedFromHub(DEVINST usb_dev_inst, uint16_t* out_bcd_usb) {
    // Climb up to the USB root device node (skip &MI_xx child composite interfaces)
    DEVINST cur = usb_dev_inst;
    for (int depth = 0; depth < 4; ++depth) {
        wchar_t inst_id[MAX_PATH] = {0};
        if (CM_Get_Device_IDW(cur, inst_id, MAX_PATH, 0) != CR_SUCCESS) {
            break;
        }
        std::wstring low_id = ToLowerWide(inst_id);
        if (low_id.find(L"&mi_") != std::wstring::npos) {
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, cur, 0) == CR_SUCCESS) {
                cur = parent;
                continue;
            }
        }
        break;
    }

    // Read 1-based USB Hub port number (CM_DRP_ADDRESS)
    ULONG port_num = 0;
    ULONG prop_len = sizeof(port_num);
    ULONG reg_type = 0;
    if (CM_Get_DevNode_Registry_PropertyW(
            cur, CM_DRP_ADDRESS, &reg_type, &port_num, &prop_len, 0) != CR_SUCCESS ||
        port_num == 0) {
        return UsbLinkSpeed::Unknown;
    }

    DEVINST hub_inst = 0;
    if (CM_Get_Parent(&hub_inst, cur, 0) != CR_SUCCESS) {
        return UsbLinkSpeed::Unknown;
    }

    wchar_t hub_dev_id[MAX_PATH] = {0};
    if (CM_Get_Device_IDW(hub_inst, hub_dev_id, MAX_PATH, 0) != CR_SUCCESS) {
        return UsbLinkSpeed::Unknown;
    }

    // Locate the device interface path for hub_dev_id
    HDEVINFO hub_set = SetupDiGetClassDevsW(
        &kGuidDevInterfaceUsbHub, hub_dev_id, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (hub_set == INVALID_HANDLE_VALUE) {
        return UsbLinkSpeed::Unknown;
    }

    SP_DEVICE_INTERFACE_DATA if_data{};
    if_data.cbSize = sizeof(if_data);
    std::wstring hub_path;
    if (SetupDiEnumDeviceInterfaces(hub_set, nullptr, &kGuidDevInterfaceUsbHub, 0, &if_data)) {
        BYTE detail_buf[1024] = {0};
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buf);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(hub_set, &if_data, detail, sizeof(detail_buf), nullptr, nullptr)) {
            hub_path = detail->DevicePath;
        }
    }
    SetupDiDestroyDeviceInfoList(hub_set);

    if (hub_path.empty()) {
        return UsbLinkSpeed::Unknown;
    }

    HANDLE hHub = CreateFileW(
        hub_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hHub == INVALID_HANDLE_VALUE) {
        hHub = CreateFileW(
            hub_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (hHub == INVALID_HANDLE_VALUE) {
        return UsbLinkSpeed::Unknown;
    }

    UsbLinkSpeed detected = UsbLinkSpeed::Unknown;
    uint16_t bcd_usb = 0;

    UsbNodeConnectionInfoExWire info_ex{};
    info_ex.ConnectionIndex = port_num;
    DWORD bytes_ret = 0;
    if (DeviceIoControl(
            hHub,
            kIoctlUsbGetNodeConnectionInfoEx,
            &info_ex,
            sizeof(info_ex),
            &info_ex,
            sizeof(info_ex),
            &bytes_ret,
            nullptr)) {
        bcd_usb = info_ex.DeviceDescriptor.bcdUSB;
        if (out_bcd_usb) *out_bcd_usb = bcd_usb;
        switch (info_ex.Speed) {
            case 0:
                detected = UsbLinkSpeed::Low_1_5Mbps;
                break;
            case 1:
                detected = UsbLinkSpeed::Full_12Mbps;
                break;
            case 2:
                detected = UsbLinkSpeed::High_480Mbps;
                break;
            case 3:
                detected = UsbLinkSpeed::Super_5Gbps;
                break;
            default:
                break;
        }
    }

    UsbNodeConnectionInfoExV2Wire info_v2{};
    info_v2.ConnectionIndex = port_num;
    info_v2.Length = sizeof(info_v2);
    info_v2.SupportedUsbProtocols = 0x07;  // Usb110 | Usb200 | Usb300
    if (DeviceIoControl(
            hHub,
            kIoctlUsbGetNodeConnectionInfoExV2,
            &info_v2,
            sizeof(info_v2),
            &info_v2,
            sizeof(info_v2),
            &bytes_ret,
            nullptr)) {
        // Bit 2 = DeviceIsOperatingAtSuperSpeedPlusOrHigher (10Gbps / 20Gbps)
        // Bit 0 = DeviceIsOperatingAtSuperSpeedOrHigher (5Gbps+)
        if ((info_v2.Flags & 0x04U) != 0U) {
            detected = (bcd_usb >= 0x0322) ? UsbLinkSpeed::SuperPlus_20Gbps : UsbLinkSpeed::SuperPlus_10Gbps;
        } else if ((info_v2.Flags & 0x01U) != 0U) {
            detected = UsbLinkSpeed::Super_5Gbps;
        }
    }

    CloseHandle(hHub);
    return detected;
}

bool RunHiddenProcessCapture(
    const std::filesystem::path& exe_path,
    const std::wstring& args,
    uint32_t timeout_ms,
    std::string& out_stdout,
    DWORD* out_exit_code) {
    out_stdout.clear();
    if (out_exit_code) *out_exit_code = 1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        return false;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmd = L"\"" + exe_path.wstring() + L"\" " + args;
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    std::wstring work_dir = exe_path.parent_path().wstring();
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        exe_path.c_str(),
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        work_dir.empty() ? nullptr : work_dir.c_str(),
        &si,
        &pi);
    CloseHandle(write_pipe);

    if (!ok) {
        CloseHandle(read_pipe);
        return false;
    }

    DWORD start_tick = GetTickCount();
    char chunk[1024];
    while (true) {
        DWORD avail = 0;
        while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD nread = 0;
            if (ReadFile(read_pipe, chunk, std::min<DWORD>(sizeof(chunk), avail), &nread, nullptr) && nread > 0) {
                out_stdout.append(chunk, nread);
            } else {
                break;
            }
        }

        DWORD wait_rc = WaitForSingleObject(pi.hProcess, 25);
        if (wait_rc == WAIT_OBJECT_0) {
            // Drain remaining bytes
            DWORD nread = 0;
            while (ReadFile(read_pipe, chunk, sizeof(chunk), &nread, nullptr) && nread > 0) {
                out_stdout.append(chunk, nread);
            }
            break;
        }

        if (GetTickCount() - start_tick >= timeout_ms) {
            TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 200);
            CloseHandle(read_pipe);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return false;
        }
    }

    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    if (out_exit_code) *out_exit_code = ec;

    CloseHandle(read_pipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
#endif

}  // namespace

std::vector<AdbDeviceEntry> ParseAdbDevicesOutput(std::string_view raw_output) {
    std::vector<AdbDeviceEntry> entries;
    std::string text(raw_output);
    std::istringstream iss(text);
    std::string line;

    while (std::getline(iss, line)) {
        std::string trimmed = TrimAscii(line);
        if (trimmed.empty()) continue;
        if (trimmed.rfind("List of devices", 0) == 0) continue;
        if (trimmed[0] == '*') continue;  // "* daemon not running; starting now..."

        std::istringstream ls(trimmed);
        std::string serial;
        std::string state_tok;
        if (!(ls >> serial >> state_tok)) {
            continue;
        }

        AdbDeviceEntry dev;
        dev.serial = serial;
        dev.is_wireless = LooksLikeWirelessSerial(serial);

        if (state_tok == "device") {
            dev.state = AdbDeviceState::Authorized;
        } else if (state_tok == "unauthorized") {
            dev.state = AdbDeviceState::Unauthorized;
        } else if (state_tok == "offline" || state_tok == "connecting" || state_tok == "authorizing") {
            dev.state = AdbDeviceState::Offline;
        } else if (state_tok == "no") {
            // "no permissions ..."
            dev.state = AdbDeviceState::NoPermissions;
        } else {
            continue;
        }

        std::string kv;
        while (ls >> kv) {
            if (kv.rfind("model:", 0) == 0) {
                dev.model = kv.substr(6);
                std::replace(dev.model.begin(), dev.model.end(), '_', ' ');
            } else if (kv.rfind("product:", 0) == 0) {
                dev.product = kv.substr(8);
            } else if (kv.rfind("device:", 0) == 0) {
                dev.device_codename = kv.substr(7);
            }
        }

        entries.push_back(std::move(dev));
    }

    return entries;
}

std::filesystem::path ResolveBundledAdbPath(const std::filesystem::path& base_dir) {
    std::error_code ec;
    std::filesystem::path p1 = base_dir / L"runtime" / L"adb.exe";
    if (std::filesystem::exists(p1, ec)) return p1;
    std::filesystem::path p2 = base_dir / L"adb.exe";
    if (std::filesystem::exists(p2, ec)) return p2;
    return p1;
}

std::filesystem::path ResolveBundledServerJarPath(const std::filesystem::path& base_dir) {
    std::error_code ec;
    std::filesystem::path p1 = base_dir / L"runtime" / L"jcast-server.jar";
    if (std::filesystem::exists(p1, ec)) return p1;
    std::filesystem::path p2 = base_dir / L"jcast-server.jar";
    if (std::filesystem::exists(p2, ec)) return p2;
    return p1;
}

UsbLinkSpeed ProbeConnectedAndroidLinkSpeed(
    uint16_t* out_bcd_usb,
    bool* out_any_android_usb,
    bool* out_adb_iface_found,
    std::string* out_hw_desc) {
    if (out_bcd_usb) *out_bcd_usb = 0;
    if (out_any_android_usb) *out_any_android_usb = false;
    if (out_adb_iface_found) *out_adb_iface_found = false;
    if (out_hw_desc) out_hw_desc->clear();

#ifdef _WIN32
    HDEVINFO dev_info = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) {
        return UsbLinkSpeed::Unknown;
    }

    bool found_any_android = false;
    bool found_adb_iface = false;
    UsbLinkSpeed best_speed = UsbLinkSpeed::Unknown;
    uint16_t best_bcd = 0;
    std::string best_desc;

    SP_DEVINFO_DATA dev_data{};
    dev_data.cbSize = sizeof(dev_data);

    for (DWORD idx = 0; SetupDiEnumDeviceInfo(dev_info, idx, &dev_data); ++idx) {
        BYTE hwid_buf[2048] = {0};
        DWORD reg_type = 0, req_size = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_HARDWAREID, &reg_type, hwid_buf, sizeof(hwid_buf), &req_size)) {
            continue;
        }
        auto hwids = ParseMultiSzWide(hwid_buf, req_size);

        BYTE compat_buf[2048] = {0};
        DWORD compat_size = 0;
        std::vector<std::wstring> compats;
        if (SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_COMPATIBLEIDS, &reg_type, compat_buf, sizeof(compat_buf), &compat_size)) {
            compats = ParseMultiSzWide(compat_buf, compat_size);
        }

        bool is_apple = false;
        bool has_adb_compat = false;
        bool has_mtp_ptp_compat = false;
        bool has_hid_or_uvc_or_bt = false;

        for (const auto& c : compats) {
            std::wstring lc = ToLowerWide(c);
            if (lc.find(L"class_ff&subclass_42") != std::wstring::npos) {
                has_adb_compat = true;
            }
            if (lc.find(L"class_06") != std::wstring::npos || lc.find(L"ms_comp_mtp") != std::wstring::npos) {
                has_mtp_ptp_compat = true;
            }
            if (lc.find(L"class_03") != std::wstring::npos ||
                lc.find(L"class_0e") != std::wstring::npos ||
                lc.find(L"class_e0") != std::wstring::npos) {
                has_hid_or_uvc_or_bt = true;
            }
        }

        wchar_t friendly_buf[256] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(
                dev_info,
                &dev_data,
                SPDRP_FRIENDLYNAME,
                nullptr,
                reinterpret_cast<BYTE*>(friendly_buf),
                sizeof(friendly_buf) - sizeof(wchar_t),
                nullptr)) {
            SetupDiGetDeviceRegistryPropertyW(
                dev_info,
                &dev_data,
                SPDRP_DEVICEDESC,
                nullptr,
                reinterpret_cast<BYTE*>(friendly_buf),
                sizeof(friendly_buf) - sizeof(wchar_t),
                nullptr);
        }
        std::wstring friendly_w(friendly_buf);
        std::wstring low_friendly = ToLowerWide(friendly_w);
        if (low_friendly.find(L"adb interface") != std::wstring::npos ||
            low_friendly.find(L"android") != std::wstring::npos) {
            if (low_friendly.find(L"adb") != std::wstring::npos) {
                has_adb_compat = true;
            }
        }

        bool matched_this = has_adb_compat;
        for (const auto& id : hwids) {
            std::wstring lid = ToLowerWide(id);
            if (lid.find(L"vid_05ac") != std::wstring::npos) {
                is_apple = true;
                break;
            }
            if (IsDedicatedAndroidVid(lid) && !has_hid_or_uvc_or_bt) {
                matched_this = true;
            } else if (IsMultiProductAndroidVid(lid) && (has_adb_compat || has_mtp_ptp_compat)) {
                matched_this = true;
            }
        }

        if (is_apple || !matched_this) {
            continue;
        }

        found_any_android = true;
        if (has_adb_compat) {
            found_adb_iface = true;
        }

        uint16_t cur_bcd = 0;
        UsbLinkSpeed sp = QueryUsbPortSpeedFromHub(dev_data.DevInst, &cur_bcd);
        if (static_cast<int>(sp) > static_cast<int>(best_speed)) {
            best_speed = sp;
            best_bcd = cur_bcd;
        }
        if ((best_desc.empty() || has_adb_compat) && !friendly_w.empty()) {
            best_desc = WideToUtf8(friendly_w);
        }
    }

    SetupDiDestroyDeviceInfoList(dev_info);

    if (out_any_android_usb) *out_any_android_usb = found_any_android;
    if (out_adb_iface_found) *out_adb_iface_found = found_adb_iface;
    if (out_bcd_usb) *out_bcd_usb = best_bcd;
    if (out_hw_desc) *out_hw_desc = best_desc;
    return best_speed;
#else
    return UsbLinkSpeed::Unknown;
#endif
}

bool QueryConnectedAdbDevices(
    const std::filesystem::path& adb_exe_path,
    std::vector<AdbDeviceEntry>& out_devices,
    std::string* out_error,
    uint32_t timeout_ms) {
    out_devices.clear();
#ifdef _WIN32
    std::error_code ec;
    if (!std::filesystem::exists(adb_exe_path, ec)) {
        if (out_error) *out_error = "Missing bundled runtime/adb.exe.";
        return false;
    }

    std::string raw_out;
    DWORD exit_code = 0;
    if (!RunHiddenProcessCapture(adb_exe_path, L"devices -l", timeout_ms, raw_out, &exit_code)) {
        if (out_error) *out_error = "Failed or timed out running adb devices -l.";
        return false;
    }
    if (exit_code != 0) {
        if (out_error) *out_error = "adb devices returned error: " + TrimAscii(raw_out);
        return false;
    }
    out_devices = ParseAdbDevicesOutput(raw_out);
    return true;
#else
    (void)adb_exe_path;
    (void)timeout_ms;
    if (out_error) *out_error = "QueryConnectedAdbDevices is only active on Windows.";
    return false;
#endif
}

}  // namespace android_vcam
