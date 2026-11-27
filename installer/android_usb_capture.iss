[Setup]
AppId={{9F85B3C1-6D2A-4E94-AB52-3E7D9E81F299}
AppName=JCast for Android
AppVersion=1.0.0
AppVerName=JCast for Android v1.0.0
AppPublisher=JCast Project
AppPublisherURL=https://github.com/joey971018yt/android-usb-vcam
DefaultDirName={autopf}\JCast for Android
DefaultGroupName=JCast for Android
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupIconFile=..\assets\app_icon.ico
UninstallDisplayIcon={app}\JCast for Android.exe
UninstallDisplayName=JCast for Android
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
OutputDir=..\build-installer
OutputBaseFilename=JCast-for-Android-Setup-v1.0.0
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "在桌面建立「JCast for Android」App 捷徑 (&D)"; GroupDescription: "附加捷徑："

[Files]
Source: "..\release-msvc-win64\JCast for Android.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\android_vcam_gui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\android_vcam_host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\android_vcam_driver_setup.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\android_vcam_dshow.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\runtime\*"; DestDir: "{app}\runtime"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\release-msvc-win64\drivers\*"; DestDir: "{app}\drivers"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\release-msvc-win64\companion\*"; DestDir: "{app}\companion"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\JCast for Android"; Filename: "{app}\JCast for Android.exe"; IconFilename: "{app}\JCast for Android.exe"; Comment: "啟動 JCast for Android 智慧擷取控制台"
Name: "{group}\開啟 JCast Companion APK 資料夾（免開 USB 偵錯備援）"; Filename: "{app}\companion"
Name: "{group}\解除安裝 JCast for Android"; Filename: "{uninstallexe}"
Name: "{autodesktop}\JCast for Android"; Filename: "{app}\JCast for Android.exe"; IconFilename: "{app}\JCast for Android.exe"; Tasks: desktopicon; Comment: "啟動 JCast for Android 智慧擷取控制台"

[Run]
Filename: "{app}\android_vcam_driver_setup.exe"; Parameters: "--install"; StatusMsg: "正在自動註冊 JCast for Android 虛擬擷取卡、ADB 與 AOA 配件驅動服務..."; Flags: runhidden waituntilterminated
Filename: "{app}\JCast for Android.exe"; Description: "立即啟動「JCast for Android」"; Flags: nowait postinstall skipifsilent shellexec

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/F /IM ""JCast for Android.exe"" /IM android_vcam_gui.exe /IM android_vcam_host.exe"; Flags: runhidden; RunOnceId: "StopRunningProcesses"
Filename: "{app}\android_vcam_driver_setup.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterDriversAndDshow"
