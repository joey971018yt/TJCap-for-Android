#pragma once

#ifdef _WIN32
#include <guiddef.h>
#endif

namespace android_vcam {

// {9F85B3C1-6D2A-4E94-AB52-3E7D9E81F201}
// JCast for Android (Video) DirectShow Capture Filter CLSID (distinct from JCast for iOS)
#ifdef _WIN32
static const GUID CLSID_AndroidUsbVideoCapture = {
    0x9f85b3c1, 0x6d2a, 0x4e94, {0xab, 0x52, 0x3e, 0x7d, 0x9e, 0x81, 0xf2, 0x01}};

// {9F85B3C2-6D2A-4E94-AB52-3E7D9E81F202}
// JCast for Android (Audio) DirectShow Capture Filter CLSID
static const GUID CLSID_AndroidUsbAudioCapture = {
    0x9f85b3c2, 0x6d2a, 0x4e94, {0xab, 0x52, 0x3e, 0x7d, 0x9e, 0x81, 0xf2, 0x02}};
#endif

inline constexpr const wchar_t* kVideoDeviceFriendlyName = L"JCast for Android (Video)";
inline constexpr const wchar_t* kAudioDeviceFriendlyName = L"JCast for Android (Audio)";

}  // namespace android_vcam
