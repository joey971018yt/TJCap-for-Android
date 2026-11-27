#include "android_vcam/dshow_guids.h"
#include "android_vcam/usb_android_probe.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <shellapi.h>
#endif

namespace {

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

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elev{};
    DWORD ret_len = 0;
    bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &ret_len) && elev.TokenIsElevated != 0;
    CloseHandle(token);
    return ok;
}

std::filesystem::path ExeDirectory() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2 - 1);
    return std::filesystem::path(buf).parent_path();
}

std::vector<std::wstring> ParseMultiSz(const BYTE* raw, DWORD byte_len) {
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

std::vector<BYTE> SerializeMultiSz(const std::vector<std::wstring>& items) {
    std::vector<wchar_t> wbuf;
    for (const auto& s : items) {
        if (!s.empty()) {
            wbuf.insert(wbuf.end(), s.begin(), s.end());
            wbuf.push_back(L'\0');
        }
    }
    wbuf.push_back(L'\0');
    if (wbuf.size() == 1) wbuf.push_back(L'\0');
    const auto* bptr = reinterpret_cast<const BYTE*>(wbuf.data());
    return std::vector<BYTE>(bptr, bptr + wbuf.size() * sizeof(wchar_t));
}

bool RestartUsbDeviceNode(HDEVINFO dev_info, SP_DEVINFO_DATA& dev_data) {
    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = DICS_PROPCHANGE;
    params.Scope = DICS_FLAG_GLOBAL;
    params.HwProfile = 0;
    if (!SetupDiSetClassInstallParamsW(dev_info, &dev_data, &params.ClassInstallHeader, sizeof(params))) {
        return false;
    }
    return SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, dev_info, &dev_data) != FALSE;
}

bool InstallLibUsb0KernelService(const std::filesystem::path& driver_dir) {
    std::filesystem::path src_sys = driver_dir / L"libusb0.sys";
    std::filesystem::path src_dll = driver_dir / L"libusb0.dll";

    wchar_t sys_root[MAX_PATH] = {0};
    GetSystemDirectoryW(sys_root, MAX_PATH);
    std::filesystem::path sys32_dir(sys_root);
    std::filesystem::path dst_sys = sys32_dir / L"drivers" / L"libusb0.sys";
    std::filesystem::path dst_dll = sys32_dir / L"libusb0.dll";

    std::error_code ec;
    if (std::filesystem::exists(src_sys, ec)) {
        std::filesystem::copy_file(src_sys, dst_sys, std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (std::filesystem::exists(src_dll, ec)) {
        std::filesystem::copy_file(src_dll, dst_dll, std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (!std::filesystem::exists(dst_sys, ec)) {
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, L"libusb0", SERVICE_ALL_ACCESS);
    if (!svc) {
        svc = CreateServiceW(
            scm,
            L"libusb0",
            L"libusb-win32 - Kernel Driver",
            SERVICE_ALL_ACCESS,
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            L"System32\\drivers\\libusb0.sys",
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (!svc) {
            CloseServiceHandle(scm);
            return false;
        }
        std::cout << "[AOA Driver] Registered WHQL-signed 'libusb0' kernel filter service.\n";
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool IsAndroidParentUsbNode(const std::wstring& low_hwid) {
    if (low_hwid.find(L"&mi_") != std::wstring::npos) return false;
    static const wchar_t* kAndroidParentVids[] = {
        L"vid_18d1", L"vid_04e8", L"vid_2717", L"vid_22d9", L"vid_2a70",
        L"vid_2d95", L"vid_0fce", L"vid_12d1", L"vid_22b8", L"vid_0bb4",
        L"vid_2916", L"vid_19d2", L"vid_0e8d", L"vid_05c6", L"vid_1004",
        L"vid_04dd", L"vid_2ae5",
    };
    for (const wchar_t* v : kAndroidParentVids) {
        if (low_hwid.find(v) != std::wstring::npos) return true;
    }
    return false;
}

int ConfigureAndroidAoaUpperFilters(bool attach_filter) {
    HDEVINFO dev_info = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) return 0;

    int matched = 0;
    SP_DEVINFO_DATA dev_data{};
    dev_data.cbSize = sizeof(dev_data);

    for (DWORD idx = 0; SetupDiEnumDeviceInfo(dev_info, idx, &dev_data); ++idx) {
        BYTE hwid_buf[2048] = {0};
        DWORD reg_type = 0, req_size = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_HARDWAREID, &reg_type, hwid_buf, sizeof(hwid_buf), &req_size)) {
            continue;
        }
        auto hwids = ParseMultiSz(hwid_buf, req_size);
        bool is_target = false;
        for (const auto& id : hwids) {
            if (IsAndroidParentUsbNode(ToLowerWide(id))) {
                is_target = true;
                break;
            }
        }
        if (!is_target) continue;
        ++matched;

        BYTE filt_buf[1024] = {0};
        DWORD filt_size = 0;
        std::vector<std::wstring> filters;
        if (SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_UPPERFILTERS, &reg_type, filt_buf, sizeof(filt_buf), &filt_size)) {
            filters = ParseMultiSz(filt_buf, filt_size);
        }

        bool has_libusb0 = false;
        for (const auto& f : filters) {
            if (ToLowerWide(f) == L"libusb0") {
                has_libusb0 = true;
                break;
            }
        }

        if (attach_filter && !has_libusb0) {
            filters.push_back(L"libusb0");
            auto raw_msz = SerializeMultiSz(filters);
            if (SetupDiSetDeviceRegistryPropertyW(
                    dev_info, &dev_data, SPDRP_UPPERFILTERS, raw_msz.data(), static_cast<DWORD>(raw_msz.size()))) {
                std::cout << "[AOA Driver] Attached non-intrusive UpperFilter=libusb0 to Android USB node.\n";
                RestartUsbDeviceNode(dev_info, dev_data);
            }
        } else if (!attach_filter && has_libusb0) {
            filters.erase(
                std::remove_if(
                    filters.begin(),
                    filters.end(),
                    [](const std::wstring& f) { return ToLowerWide(f) == L"libusb0"; }),
                filters.end());
            BOOL ok = filters.empty()
                          ? SetupDiSetDeviceRegistryPropertyW(dev_info, &dev_data, SPDRP_UPPERFILTERS, nullptr, 0)
                          : SetupDiSetDeviceRegistryPropertyW(
                                dev_info,
                                &dev_data,
                                SPDRP_UPPERFILTERS,
                                SerializeMultiSz(filters).data(),
                                static_cast<DWORD>(SerializeMultiSz(filters).size()));
            if (ok) {
                RestartUsbDeviceNode(dev_info, dev_data);
            }
        }
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return matched;
}

void UnblockRuntimeFiles(const std::filesystem::path& base_dir) {
    const wchar_t* kRelFiles[] = {
        L"TJCap for Android.exe",
        L"android_vcam_gui.exe",
        L"android_vcam_dshow.dll",
        L"android_vcam_host.exe",
        L"android_vcam_driver_setup.exe",
        L"android_vcam_tests.exe",
        L"runtime\\adb.exe",
        L"runtime\\AdbWinApi.dll",
        L"runtime\\AdbWinUsbApi.dll",
        L"runtime\\tjcap-server.jar",
        L"runtime\\libusb-1.0.dll",
        L"drivers\\libusb0.sys",
        L"drivers\\libusb0.dll",
        L"companion\\TJCap-Companion-v1.0.0.apk",
    };
    for (const wchar_t* rel : kRelFiles) {
        std::wstring ads = (base_dir / rel).wstring() + L":Zone.Identifier";
        DeleteFileW(ads.c_str());
    }
}

void RunQuietAdbAction(const std::filesystem::path& adb_exe, const std::wstring& arg) {
    std::error_code ec;
    if (!std::filesystem::exists(adb_exe, ec)) return;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring cmd = L"\"" + adb_exe.wstring() + L"\" " + arg;
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    std::wstring work_dir = adb_exe.parent_path().wstring();
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(
            adb_exe.c_str(),
            cmd_buf.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            work_dir.empty() ? nullptr : work_dir.c_str(),
            &si,
            &pi)) {
        WaitForSingleObject(pi.hProcess, 3500U);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

bool ManageDirectShowDll(const std::filesystem::path& dll_path, bool register_server) {
    if (!std::filesystem::exists(dll_path)) {
        std::cerr << "[DirectShow] DLL not found at: " << WideToUtf8(dll_path.wstring()) << "\n";
        return false;
    }
    HMODULE mod = LoadLibraryExW(
        dll_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) {
        std::cerr << "[DirectShow] LoadLibraryExW failed for android_vcam_dshow.dll (error "
                  << GetLastError() << ").\n";
        return false;
    }
    using DllRegFn = HRESULT(STDAPICALLTYPE*)();
    const char* proc_name = register_server ? "DllRegisterServer" : "DllUnregisterServer";
    auto fn = reinterpret_cast<DllRegFn>(GetProcAddress(mod, proc_name));
    if (!fn) {
        FreeLibrary(mod);
        return false;
    }
    HRESULT hr = fn();
    FreeLibrary(mod);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] " << proc_name << " failed with HRESULT 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }
    std::cout << "[DirectShow] " << (register_server ? "Registered" : "Unregistered")
              << " 'TJCap for Android (Video)' & 'TJCap for Android (Audio)' ("
              << WideToUtf8(dll_path.filename().wstring()) << ").\n";
    return true;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetDllDirectoryW(L"");
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetConsoleOutputCP(CP_UTF8);

    bool do_install = true;
    bool do_uninstall = false;
    bool status_only = false;
    std::filesystem::path base_dir = ExeDirectory();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--uninstall" || arg == "/u") {
            do_uninstall = true;
            do_install = false;
        } else if (arg == "--install" || arg == "/i") {
            do_install = true;
            do_uninstall = false;
        } else if (arg == "--status") {
            status_only = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "TJCap for Android — Virtual Capture Card, ADB & USB AOA Driver Setup\n"
                << "Usage:\n"
                << "  android_vcam_driver_setup.exe --install     Register DirectShow DLL, ADB & AOA UpperFilter\n"
                << "  android_vcam_driver_setup.exe --uninstall   Remove AOA UpperFilter, stop ADB & unregister DLL\n"
                << "  android_vcam_driver_setup.exe --status      Inspect Android USB speed & ADB/AOA status\n";
            return 0;
        }
    }

    if (status_only) {
        bool any_usb = false;
        bool adb_iface = false;
        std::string desc;
        auto sp = android_vcam::ProbeConnectedAndroidLinkSpeed(nullptr, &any_usb, &adb_iface, &desc);
        if (!any_usb) {
            std::cout << "[Status] No Android USB device currently plugged in.\n";
        } else {
            std::cout << "[Status] Android USB Device: " << (desc.empty() ? "Detected" : desc)
                      << " | Speed: " << android_vcam::UsbLinkSpeedName(sp)
                      << " | ADB Interface: " << (adb_iface ? "YES" : "NO (AOA Companion Mode available)") << "\n";
        }
        return 0;
    }

    if (!IsProcessElevated()) {
        std::cerr << "[Error] Administrator privileges are required.\n";
        return 1;
    }

    std::filesystem::path dshow_dll = base_dir / L"android_vcam_dshow.dll";
    std::filesystem::path drivers_dir = base_dir / L"drivers";
    std::filesystem::path adb_exe = android_vcam::ResolveBundledAdbPath(base_dir);

    if (do_install) {
        std::cout << "=== Installing TJCap for Android (ADB + USB AOA Dual-Mode) ===\n";
        UnblockRuntimeFiles(base_dir);
        bool dshow_ok = ManageDirectShowDll(dshow_dll, true);
        if (InstallLibUsb0KernelService(drivers_dir)) {
            ConfigureAndroidAoaUpperFilters(true);
        }
        RunQuietAdbAction(adb_exe, L"kill-server");
        RunQuietAdbAction(adb_exe, L"start-server");
        std::cout << "[Setup] DirectShow, ADB & USB AOA filter services ready.\n";
        return dshow_ok ? 0 : 1;
    }

    if (do_uninstall) {
        std::cout << "=== Uninstalling TJCap for Android ===\n";
        ConfigureAndroidAoaUpperFilters(false);
        RunQuietAdbAction(adb_exe, L"kill-server");
        ManageDirectShowDll(dshow_dll, false);
        return 0;
    }
    return 0;
#else
    (void)argc;
    (void)argv;
    std::cerr << "android_vcam_driver_setup is a Windows SetupAPI tool.\n";
    return 1;
#endif
}
