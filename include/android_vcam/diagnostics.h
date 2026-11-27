#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace android_vcam {

enum class DiagSeverity : uint8_t {
    Info = 0,
    Warning = 1,
    Error = 2,
    Critical = 3,
};

enum class DiagCode : uint16_t {
    None = 0,

    // E1xx: System Environment & Bundled Runtime Files
    E101_MissingCoreFiles       = 101,
    E102_AdbDaemonStartFailed   = 102,
    E103_MissingMediaFoundation = 103,
    E104_DshowNotRegistered     = 104,

    // E2xx: Android USB Driver, ADB Port & Tunnel Socket
    E201_AndroidUsbDriverIssue  = 201,
    E202_AdbVersionConflict     = 202,
    E203_TunnelForwardFailed    = 203,
    E204_DeviceBusyOrOffline    = 204,

    // E3xx: Android USB Debugging Authorization, Agent Push & USB-C Cable I/O
    E301_UsbDebugUnauthorized   = 301,
    E302_AgentStartFailed       = 302,
    E303_HandshakeNoFrames      = 303,
    E304_UsbTransferIoDrop      = 304,

    // E4xx: H.264 Video Decoder & GPU Hardware Acceleration
    E401_DecoderInitFailed      = 401,
    E402_GpuFallbackToSoftware  = 402,
    E403_HighDecodeLatency      = 403,

    // E5xx: Audio Routing, Shared Memory, Host Crash & Android < 11 Audio Notice
    E501_AudioEndpointLost      = 501,
    E502_SharedMemInitFailed    = 502,
    E503_HostUnexpectedExit     = 503,
    E504_DeviceAudioUnsupported = 504,
};

struct DiagCodeSpec {
    DiagCode code = DiagCode::None;
    DiagSeverity default_severity = DiagSeverity::Error;
    const char* code_str = "TJCAP-E000";
    const char* title_utf8 = "";
    const char* cause_utf8 = "";
    const char* fix_steps_utf8 = "";
    bool can_auto_repair_driver = false;
};

struct ParsedDiagEvent {
    DiagCode code = DiagCode::None;
    DiagSeverity severity = DiagSeverity::Error;
    std::string detail_utf8;
};

// Looks up Traditional Chinese root-cause & step-by-step fix specification for any DiagCode
const DiagCodeSpec& LookupDiagSpec(DiagCode code);

// Parses numeric code string such as "E301" or "TJCAP-E301" into DiagCode
DiagCode ParseDiagCodeToken(std::string_view token);

// Classifies raw ADB / USB / socket error messages into a structured DiagCode
DiagCode ClassifyUsbOrTransportError(std::string_view raw_err);

// Formats a structured host diagnostic stdout line: "[TJCAP-DIAG|E301|WARN] <detail>"
std::string FormatHostDiagLine(DiagCode code, DiagSeverity sev, std::string_view detail_utf8);

// Parses a line emitted by FormatHostDiagLine (returns true if line starts with "[TJCAP-DIAG|")
bool ParseHostDiagLine(std::string_view line_utf8, ParsedDiagEvent& out_event);

// Translates a Windows process exit code (e.g. 0xC0000005, 0xC0000135) into a clear Chinese explanation
std::string DescribeProcessExitCode(uint32_t exit_code);

// Privacy-safe redaction: masks Windows/Unix home directory usernames and Android USB serial numbers
std::string RedactSensitiveReportText(std::string_view raw_utf8);

// Builds a safe RFC 3986 percent-encoded GitHub New Issue URL pre-filled with version & error summary
std::string BuildGitHubNewIssueUrl(
    std::string_view owner,
    std::string_view repo,
    std::string_view app_version,
    DiagCode primary_code,
    std::string_view short_summary_utf8);

}  // namespace android_vcam
