#include "android_vcam/diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace android_vcam {

namespace {

const DiagCodeSpec kNoneSpec{
    DiagCode::None,
    DiagSeverity::Info,
    "JCAST-OK",
    "系統運作正常",
    "目前未偵測到任何硬體、ADB 通道、驅動或串流異常。",
    "無需額外操作。",
    false,
};

const DiagCodeSpec kSpecs[] = {
    {
        DiagCode::E101_MissingCoreFiles,
        DiagSeverity::Critical,
        "JCAST-E101",
        "缺少核心執行檔或內建 ADB／擷取元件",
        "程式資料夾內遺失必要元件（如 android_vcam_host.exe、android_vcam_dshow.dll、runtime\\adb.exe 或 runtime\\jcast-server.jar），常見於未完整解壓縮 Portable.zip 或遭防毒軟體誤隔離。",
        "1. 請確認已將整個 ZIP 壓縮包「完整解壓縮」至同一資料夾（勿直接在壓縮檔內雙擊執行）。\n"
        "2. 若防毒軟體攔截，請將 JCast for Android 資料夾加入信任排除清單，或改下載 Setup.exe 正式安裝版重新安裝。",
        false,
    },
    {
        DiagCode::E102_AdbDaemonStartFailed,
        DiagSeverity::Error,
        "JCAST-E102",
        "內建 ADB 通訊服務啟動失敗或 5037 連接埠衝突",
        "無法啟動內建 runtime\\adb.exe 背景服務，常見於電腦背景同時開啟了第三方手機助手／模擬器內附的舊版 ADB 佔用 TCP 5037 連接埠。",
        "1. 請先關閉背景執行中的手機助手、Android 模擬器或其他有線投屏工具。\n"
        "2. 點擊主視窗「🔧 安裝／修復驅動」重整服務後，再點擊「🚀 智慧一鍵啟動」。",
        true,
    },
    {
        DiagCode::E103_MissingMediaFoundation,
        DiagSeverity::Critical,
        "JCAST-E103",
        "系統缺少 Windows Media Foundation H.264 視訊解碼元件",
        "找不到 mfplat.dll 或 msmpeg2vdec.dll，常見於 Windows 10 / 11 N 或 KN 歐洲精簡版系統未安裝媒體功能套件。",
        "1. 開啟 Windows「設定 ➔ 系統 ➔ 選用功能 (Optional Features) ➔ 新增選用功能」。\n"
        "2. 搜尋並安裝「媒體功能套件 (Media Feature Pack)」，完成後重新開機即可正常硬體解碼。",
        false,
    },
    {
        DiagCode::E104_DshowNotRegistered,
        DiagSeverity::Warning,
        "JCAST-E104",
        "DirectShow 虛擬擷取卡尚未註冊或路徑已變更",
        "尚未完成首次虛擬鏡頭註冊，或解壓縮資料夾在註冊後曾被搬移至其他路徑。",
        "1. 點擊主視窗或本診斷視窗中的「🔧 安裝／修復驅動」按鈕，自動以系統管理員權限重新註冊虛擬擷取卡。\n"
        "2. 若 OBS Studio 已開啟，修復後重新啟動 OBS 即可選取 JCast for Android (Video) / (Audio)。",
        true,
    },
    {
        DiagCode::E201_AndroidUsbDriverIssue,
        DiagSeverity::Warning,
        "JCAST-E201",
        "已偵測到 Android USB 連線，但尚未開啟「USB 偵錯」或缺少 WinUSB 介面",
        "電腦已識別到 Android 手機 USB 連線，但手機尚未暴露 ADB 偵錯介面；最常見原因為手機尚未開啟「開發者選項 ➔ USB 偵錯」，或 USB 用途被設為純充電且停用了 ADB。",
        "1. 在 Android 手機開啟「設定 ➔ 關於手機 ➔ 連點 7 下『版本號碼』(小米為 OS 版本)」，退回「系統／更多設定 ➔ 開發者選項」開啟「USB 偵錯」。\n"
        "2. 下拉手機通知列確認 USB 用途為「檔案傳輸 (MTP)」或「MIDI」，若是舊款三星／特定品牌亦可至 Windows Update 檢查選用驅動更新。",
        false,
    },
    {
        DiagCode::E202_AdbVersionConflict,
        DiagSeverity::Warning,
        "JCAST-E202",
        "偵測到系統內有其他舊版 ADB 伺服器版本衝突",
        "其他軟體啟動的舊版 adb.exe 與內建高速版 ADB 版本不一致 (adb server version doesn't match)，導致連線重設。",
        "1. 請關閉其他投屏軟體、Android Studio 或手機模擬器，避免背景重複搶奪 ADB 伺服器。\n"
        "2. 重新點擊「🚀 智慧一鍵啟動」，JCast 將自動重啟乾淨的內建 ADB 通道。",
        false,
    },
    {
        DiagCode::E203_TunnelForwardFailed,
        DiagSeverity::Error,
        "JCAST-E203",
        "建立本機 USB-C 高速轉發通道 (adb forward) 失敗",
        "無法建立 127.0.0.1 本機 Loopback 轉發連接埠，可能因本機安全軟體攔截迴路連線，或手機在握手瞬間斷開。",
        "1. 請確認防毒／防火牆未封鎖 JCast for Android 與 runtime\\adb.exe 的本機 127.0.0.1 連線。\n"
        "2. 重新拔插 USB-C 傳輸線後再試一次。",
        false,
    },
    {
        DiagCode::E204_DeviceBusyOrOffline,
        DiagSeverity::Error,
        "JCAST-E204",
        "Android 裝置處於離線 (offline) 或忙碌狀態",
        "ADB 偵測到裝置狀態為 offline，通常因 USB-C 線材接觸不良、USB 集線器供電不穩，或手機剛切換 USB 模式尚未就緒。",
        "1. 請拔下手機端 USB-C 傳輸線，等待 2 秒後重新插回電腦主機板原生 USB 接口。\n"
        "2. 若仍顯示 offline，請在手機「開發者選項」將「USB 偵錯」關閉再重新打開一次。",
        false,
    },
    {
        DiagCode::E301_UsbDebugUnauthorized,
        DiagSeverity::Warning,
        "JCAST-E301",
        "手機尚未點擊「允許 USB 偵錯」授權視窗 (unauthorized)",
        "電腦已連通手機 ADB 介面，但手機螢幕正在等待您點擊 RSA 金鑰信任授權。",
        "1. 請解鎖手機螢幕，在彈出的「允許 USB 偵錯嗎？」視窗勾選「一律允許透過這台電腦進行偵錯」，並點擊「允許／確定」。\n"
        "2. 若手機沒跳出授權視窗，請在「開發者選項」點擊「撤銷 USB 偵錯授權」，再重新拔插 USB-C 線即可重新彈出！",
        false,
    },
    {
        DiagCode::E302_AgentStartFailed,
        DiagSeverity::Error,
        "JCAST-E302",
        "啟動手機端免安裝硬體編碼微服務 (app_process) 失敗",
        "無法推送 jcast-server.jar 至 /data/local/tmp 或執行 app_process 被品牌系統攔截（如 OPPO / Realme / 小米未開啟對應偵錯子選項）。",
        "1. OPPO / OnePlus / Realme 用戶：請在開發者選項額外關閉「權限監控 (Disable permission monitoring)」。\n"
        "2. 確認手機內部儲存空間未完全滿載，並重新插拔 USB-C 線再試。",
        false,
    },
    {
        DiagCode::E303_HandshakeNoFrames,
        DiagSeverity::Warning,
        "JCAST-E303",
        "USB-C 影音通道已建立但逾時未收到畫面封包",
        "已連通手機端編碼器但超過 8 秒未收到 H.264 影格；通常因手機處於休眠黑屏（SurfaceFlinger 暫停出幀）或當前 App 啟用 FLAG_SECURE 防錄影保護。",
        "1. 請按電源鍵點亮並解鎖 Android 手機螢幕，隨意滑動一下桌面即可立即觸發硬體編碼出幀。\n"
        "2. 注意：銀行 App 或 Netflix DRM 版權內容受 Android 系統級 FLAG_SECURE 保護時會呈黑畫面，一般遊戲與應用程式皆可正常擷取。",
        false,
    },
    {
        DiagCode::E304_UsbTransferIoDrop,
        DiagSeverity::Error,
        "JCAST-E304",
        "USB-C 傳輸途中發生連線中斷或線材瞬斷",
        "串流期間視訊 Socket 意外中斷；除正常拔線外，常見於 USB-C 接頭鬆動、機殼前置 USB 孔壓降過大，或僅充電劣質線材干擾。",
        "1. 檢查手機與電腦兩端 USB-C 接頭是否緊密，優先改插「電腦主機板後方原生 USB 3.0 / Type-C 接口」。\n"
        "2. 若使用較長的 USB 2.0 線材偶發瞬斷，可將最大解析度上限調為「1080p (1920p)」降低匯流排負載。",
        false,
    },
    {
        DiagCode::E401_DecoderInitFailed,
        DiagSeverity::Critical,
        "JCAST-E401",
        "H.264 視訊解碼器初始化失敗",
        "顯示卡 D3D11VA 硬體解碼與系統軟體解碼器皆無法建立，可能因顯示卡驅動異常或缺少系統解碼器。",
        "1. 請嘗試取消勾選主視窗的「啟用 GPU 硬體超低延遲解碼 (D3D11VA)」改用 CPU 軟解測試。\n"
        "2. 更新 NVIDIA / AMD / Intel 顯示卡驅動程式；若為 Windows N 版請安裝 Media Feature Pack。",
        false,
    },
    {
        DiagCode::E402_GpuFallbackToSoftware,
        DiagSeverity::Warning,
        "JCAST-E402",
        "GPU 硬體解碼異常，已自動無縫降級為 CPU 軟體解碼",
        "顯示卡 D3D11VA 發生驅動重置 (TDR) 或不支援目前畫面組態；系統已自動切換至 CPU 軟體解碼以確保直播不中斷。",
        "1. 目前串流已自動恢復運作，您可繼續安心直播。\n"
        "2. 若 CPU 軟解在 2K+ / 120fps 下延遲超過 14ms，建議稍後更新顯示卡驅動，或將解析度上限選為 1080p。",
        false,
    },
    {
        DiagCode::E403_HighDecodeLatency,
        DiagSeverity::Warning,
        "JCAST-E403",
        "解碼管線延遲偏高（超過 60fps 16.7ms 預算）",
        "單張影格解碼與縮放耗時過長，可能導致高幀率直播掉幀；常見於未開啟 D3D11VA 硬體解碼卻執行 2K+ 超高解析度。",
        "1. 請確認已勾選「啟用 GPU 硬體超低延遲解碼 (D3D11VA)」。\n"
        "2. 若電腦無獨立／內建硬解加速能力，建議將最大解析度改選為「1080p (1920p)」或「720p (1280p)」。",
        false,
    },
    {
        DiagCode::E501_AudioEndpointLost,
        DiagSeverity::Warning,
        "JCAST-E501",
        "電腦端音訊監聽裝置已斷開或失效",
        "直播途中拔除了耳機／USB 音效卡或藍牙音訊中斷 (AUDCLNT_E_DEVICE_INVALIDATED)。註：OBS 內的「JCast for Android (Audio)」不受影響仍正常收音！",
        "1. 若僅需 OBS 直播收音，可忽略此提示，OBS 音軌完全正常。\n"
        "2. 若需恢復電腦耳機同步監聽，請重新接上耳機後停止並重新啟動一次投影。",
        false,
    },
    {
        DiagCode::E502_SharedMemInitFailed,
        DiagSeverity::Critical,
        "JCAST-E502",
        "無法建立跨行程高畫質共享記憶體 (AndroidUsbVcamSharedMemV2)",
        "系統可用虛擬記憶體（分頁檔 Commit Charge）不足，或遭舊版高權限行程鎖定同名區段。",
        "1. 關閉佔用大量記憶體的閒置程式，確認 Windows 虛擬記憶體（分頁檔）未被手動停用。\n"
        "2. 重新啟動 JCast for Android 與 OBS Studio。",
        false,
    },
    {
        DiagCode::E503_HostUnexpectedExit,
        DiagSeverity::Critical,
        "JCAST-E503",
        "背景擷取核心程序 (android_vcam_host.exe) 異常終止",
        "擷取引擎在非使用者手動停止的情況下提前結束並回傳非零錯誤碼。",
        "1. 請檢視下方「程序結束代碼說明」與近期錯誤紀錄。\n"
        "2. 點擊「📋 複製診斷報告」或「🐞 開啟 GitHub 回報問題」將完整脫敏診斷單提供給開發者快速排查。",
        false,
    },
    {
        DiagCode::E504_DeviceAudioUnsupported,
        DiagSeverity::Warning,
        "JCAST-E504",
        "此手機系統版本不支援免 Root 內錄音訊，已自動維持純畫面鏡像",
        "Android 原生免 Root 系統內錄音訊需要 Android 11 (API 30) 以上版本；目前裝置已自動停用音訊軌並保持低延遲畫面傳輸。",
        "1. 畫面擷取完全正常，您可繼續使用視訊直播。\n"
        "2. 若手機為 Android 8～10 舊機型需同步收音，可搭配 3.5mm 音源線接入電腦 Line-In，或升級至 Android 11+ 裝置。",
        false,
    },
};

std::string ToLowerCopy(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

const char* SeverityTag(DiagSeverity sev) {
    switch (sev) {
        case DiagSeverity::Info:
            return "INFO";
        case DiagSeverity::Warning:
            return "WARN";
        case DiagSeverity::Critical:
            return "CRIT";
        case DiagSeverity::Error:
        default:
            return "ERR";
    }
}

DiagSeverity ParseSeverityTag(std::string_view sv) {
    if (sv == "INFO") return DiagSeverity::Info;
    if (sv == "WARN") return DiagSeverity::Warning;
    if (sv == "CRIT") return DiagSeverity::Critical;
    return DiagSeverity::Error;
}

std::string UrlEncodeRfc3986(std::string_view input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char c : input) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

const DiagCodeSpec& LookupDiagSpec(DiagCode code) {
    for (const auto& s : kSpecs) {
        if (s.code == code) {
            return s;
        }
    }
    return kNoneSpec;
}

DiagCode ParseDiagCodeToken(std::string_view token) {
    if (token.rfind("JCAST-", 0) == 0) {
        token.remove_prefix(6);
    }
    if (token.size() >= 4 && (token[0] == 'E' || token[0] == 'e')) {
        int num = std::atoi(std::string(token.substr(1)).c_str());
        for (const auto& s : kSpecs) {
            if (static_cast<int>(s.code) == num) {
                return s.code;
            }
        }
    }
    return DiagCode::None;
}

DiagCode ClassifyUsbOrTransportError(std::string_view raw_err) {
    std::string low = ToLowerCopy(raw_err);
    if ((low.find("adb.exe") != std::string::npos || low.find("jcast-server.jar") != std::string::npos) &&
        (low.find("missing") != std::string::npos || low.find("not found") != std::string::npos)) {
        return DiagCode::E101_MissingCoreFiles;
    }
    if (low.find("server version") != std::string::npos && low.find("doesn't match") != std::string::npos) {
        return DiagCode::E202_AdbVersionConflict;
    }
    if (low.find("cannot bind") != std::string::npos || low.find("5037") != std::string::npos ||
        low.find("failed to start daemon") != std::string::npos) {
        return DiagCode::E102_AdbDaemonStartFailed;
    }
    if (low.find("unauthorized") != std::string::npos || low.find("allow usb debugging") != std::string::npos) {
        return DiagCode::E301_UsbDebugUnauthorized;
    }
    if (low.find("offline") != std::string::npos || low.find("device busy") != std::string::npos) {
        return DiagCode::E204_DeviceBusyOrOffline;
    }
    if (low.find("no usb debugging") != std::string::npos || low.find("usb driver") != std::string::npos ||
        low.find("no permissions") != std::string::npos) {
        return DiagCode::E201_AndroidUsbDriverIssue;
    }
    if (low.find("forward") != std::string::npos && low.find("fail") != std::string::npos) {
        return DiagCode::E203_TunnelForwardFailed;
    }
    if (low.find("app_process") != std::string::npos || low.find("push") != std::string::npos ||
        low.find("permission monitoring") != std::string::npos || low.find("securityexception") != std::string::npos) {
        return DiagCode::E302_AgentStartFailed;
    }
    if (low.find("socket closed") != std::string::npos || low.find("connection reset") != std::string::npos ||
        low.find("recv failed") != std::string::npos || low.find("eof") != std::string::npos ||
        low.find("disconnected") != std::string::npos) {
        return DiagCode::E304_UsbTransferIoDrop;
    }
    return DiagCode::None;
}

std::string FormatHostDiagLine(DiagCode code, DiagSeverity sev, std::string_view detail_utf8) {
    const auto& spec = LookupDiagSpec(code);
    const char* short_code = spec.code_str;
    if (std::strncmp(short_code, "JCAST-", 6) == 0) {
        short_code += 6;
    }
    std::string out = "[JCAST-DIAG|";
    out += short_code;
    out += "|";
    out += SeverityTag(sev);
    out += "] ";
    out.append(detail_utf8.data(), detail_utf8.size());
    return out;
}

bool ParseHostDiagLine(std::string_view line_utf8, ParsedDiagEvent& out_event) {
    constexpr std::string_view kPrefix = "[JCAST-DIAG|";
    if (line_utf8.rfind(kPrefix, 0) != 0) {
        return false;
    }
    size_t close_br = line_utf8.find(']', kPrefix.size());
    if (close_br == std::string_view::npos) {
        return false;
    }
    std::string_view header = line_utf8.substr(kPrefix.size(), close_br - kPrefix.size());
    size_t pipe_pos = header.find('|');
    if (pipe_pos == std::string_view::npos) {
        return false;
    }
    std::string_view code_tok = header.substr(0, pipe_pos);
    std::string_view sev_tok = header.substr(pipe_pos + 1);

    DiagCode code = ParseDiagCodeToken(code_tok);
    if (code == DiagCode::None) {
        return false;
    }

    size_t msg_start = close_br + 1;
    while (msg_start < line_utf8.size() && line_utf8[msg_start] == ' ') {
        ++msg_start;
    }

    out_event.code = code;
    out_event.severity = ParseSeverityTag(sev_tok);
    out_event.detail_utf8.assign(line_utf8.substr(msg_start));
    return true;
}

std::string DescribeProcessExitCode(uint32_t exit_code) {
    char hex_buf[32] = {0};
    std::snprintf(hex_buf, sizeof(hex_buf), "0x%08X (%u)", exit_code, exit_code);
    std::string s = hex_buf;

    switch (exit_code) {
        case 0U:
            return s + " — 正常結束";
        case 1U:
            return s + " — 初始化錯誤（請查看上方對應 JCAST-E 錯誤碼，如缺少檔案、ADB 通道或共享記憶體失敗）";
        case 0xC0000135U:
            return s + " — STATUS_DLL_NOT_FOUND（系統缺少必要系統 DLL 或未完整解壓縮程式資料夾）";
        case 0xC000007BU:
            return s + " — STATUS_INVALID_IMAGE_FORMAT（32/64 位元 DLL 混用或檔案損毀）";
        case 0xC0000005U:
            return s + " — STATUS_ACCESS_VIOLATION（記憶體存取違規，可能為顯示卡驅動或外部元件異常）";
        case 0xC0000409U:
            return s + " — STATUS_STACK_BUFFER_OVERRUN / FAST_FAIL（安全防護攔截異常狀態）";
        default:
            return s + " — 非預期結束代碼";
    }
}

std::string RedactSensitiveReportText(std::string_view raw_utf8) {
    std::string s(raw_utf8);

    // 1. Mask Windows/macOS/Linux user profile directory names: Users\<name>\ or /home/<name>/
    size_t search_pos = 0;
    while (search_pos < s.size()) {
        std::string low = ToLowerCopy(s);
        size_t u_pos = low.find("users", search_pos);
        if (u_pos == std::string::npos) {
            break;
        }
        size_t sep1 = u_pos + 5;
        if (sep1 < s.size() && (s[sep1] == '\\' || s[sep1] == '/')) {
            size_t name_start = sep1 + 1;
            size_t name_end = name_start;
            while (name_end < s.size() && s[name_end] != '\\' && s[name_end] != '/' &&
                   s[name_end] != '\r' && s[name_end] != '\n' && s[name_end] != '"') {
                ++name_end;
            }
            if (name_end > name_start && name_end < s.size() && (s[name_end] == '\\' || s[name_end] == '/')) {
                std::string token = s.substr(name_start, name_end - name_start);
                if (token != "<USER>" && token != "Public" && token != "Default") {
                    s.replace(name_start, name_end - name_start, "<USER>");
                    search_pos = name_start + 6;
                    continue;
                }
            }
        }
        search_pos = u_pos + 5;
    }

    search_pos = 0;
    while (search_pos < s.size()) {
        size_t h_pos = s.find("/home/", search_pos);
        if (h_pos == std::string::npos) break;
        size_t name_start = h_pos + 6;
        size_t name_end = name_start;
        while (name_end < s.size() && s[name_end] != '/' && s[name_end] != '\r' && s[name_end] != '\n') {
            ++name_end;
        }
        if (name_end > name_start && name_end < s.size() && s[name_end] == '/') {
            std::string token = s.substr(name_start, name_end - name_start);
            if (token != "<USER>") {
                s.replace(name_start, name_end - name_start, "<USER>");
                search_pos = name_start + 6;
                continue;
            }
        }
        search_pos = h_pos + 6;
    }

    // 2. Mask explicit "Serial=<value>" or "-s <serial>" Android device serials while keeping first 3 chars
    const char* kMarkers[] = {"Serial=", "serial=", "-s "};
    for (const char* marker : kMarkers) {
        size_t mlen = std::strlen(marker);
        search_pos = 0;
        while (search_pos < s.size()) {
            size_t spos = s.find(marker, search_pos);
            if (spos == std::string::npos) break;
            size_t val_start = spos + mlen;
            while (val_start < s.size() && s[val_start] == ' ') ++val_start;
            size_t val_end = val_start;
            while (val_end < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[val_end])) ||
                    s[val_end] == '-' || s[val_end] == '_' || s[val_end] == ':' || s[val_end] == '.')) {
                ++val_end;
            }
            size_t len = val_end - val_start;
            std::string val = s.substr(val_start, len);
            if (len > 3 && val != "N/A" && val.find("REDACTED") == std::string::npos) {
                std::string masked = val.substr(0, 3) + "****-REDACTED";
                s.replace(val_start, len, masked);
                search_pos = val_start + masked.size();
            } else {
                search_pos = val_end;
            }
        }
    }

    return s;
}

std::string BuildGitHubNewIssueUrl(
    std::string_view owner,
    std::string_view repo,
    std::string_view app_version,
    DiagCode primary_code,
    std::string_view short_summary_utf8) {
    const auto& spec = LookupDiagSpec(primary_code);
    std::string title = "[診斷回報] JCast for Android v" + std::string(app_version);
    if (primary_code != DiagCode::None) {
        title += " — ";
        title += spec.code_str;
        title += " ";
        title += spec.title_utf8;
    }

    std::string body =
        "### 問題簡述\n"
        "（請在此補充您遇到的操作情境，例如 Android 手機品牌型號、Android 版本、使用的 USB-C 線材）\n\n"
        "### 自動偵測摘要\n";
    body += "- 版本：`v" + std::string(app_version) + "`\n";
    if (primary_code != DiagCode::None) {
        body += std::string("- 主要錯誤碼：`") + spec.code_str + "` (" + spec.title_utf8 + ")\n";
    }
    if (!short_summary_utf8.empty()) {
        body += "- 狀態摘要：" + RedactSensitiveReportText(short_summary_utf8) + "\n";
    }
    body +=
        "\n### 完整系統診斷報告（已自動隱私脫敏）\n"
        "> 💡 **診斷報告已自動複製到您的剪貼簿，請直接在下方空白處按 `Ctrl + V` 貼上：**\n\n"
        "```text\n"
        "（請在此按 Ctrl+V 貼上剪貼簿中的診斷報告）\n"
        "```\n";

    std::string url = "https://github.com/";
    url.append(owner.data(), owner.size());
    url.push_back('/');
    url.append(repo.data(), repo.size());
    url += "/issues/new?title=" + UrlEncodeRfc3986(title) + "&body=" + UrlEncodeRfc3986(body);
    return url;
}

}  // namespace android_vcam
