[Setup]
AppId={{9F85B3C1-6D2A-4E94-AB52-3E7D9E81F299}
AppName=TJCap for Android
AppVersion=1.0.0
AppVerName=TJCap for Android v1.0.0
AppPublisher=TJCap Project
AppPublisherURL=https://github.com/joey971018yt/TJCap-for-Android
DefaultDirName={autopf}\TJCap for Android
DefaultGroupName=TJCap for Android
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupIconFile=..\assets\app_icon.ico
UninstallDisplayIcon={app}\TJCap for Android.exe
UninstallDisplayName=TJCap for Android
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
OutputDir=..\build-installer
OutputBaseFilename=TJCap-for-Android-Setup-v1.0.0
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "在桌面建立「TJCap for Android」App 捷徑 (&D)"; GroupDescription: "附加捷徑："

[Files]
Source: "..\release-msvc-win64\TJCap for Android.exe"; DestDir: "{app}"; Flags: ignoreversion
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
Name: "{group}\TJCap for Android"; Filename: "{app}\TJCap for Android.exe"; IconFilename: "{app}\TJCap for Android.exe"; Comment: "啟動 TJCap for Android 智慧擷取控制台"
Name: "{group}\開啟 TJCap Companion APK 資料夾（免開 USB 偵錯備援）"; Filename: "{app}\companion"
Name: "{group}\解除安裝 TJCap for Android"; Filename: "{uninstallexe}"
Name: "{autodesktop}\TJCap for Android"; Filename: "{app}\TJCap for Android.exe"; IconFilename: "{app}\TJCap for Android.exe"; Tasks: desktopicon; Comment: "啟動 TJCap for Android 智慧擷取控制台"

[Run]
Filename: "{app}\android_vcam_driver_setup.exe"; Parameters: "--install"; StatusMsg: "正在自動註冊 TJCap for Android 虛擬擷取卡、ADB 與 AOA 配件驅動服務..."; Flags: runhidden waituntilterminated
Filename: "{app}\TJCap for Android.exe"; Description: "立即啟動「TJCap for Android」"; Flags: nowait postinstall skipifsilent shellexec

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/F /IM ""TJCap for Android.exe"" /IM android_vcam_gui.exe /IM android_vcam_host.exe"; Flags: runhidden; RunOnceId: "StopRunningProcesses"
Filename: "{app}\android_vcam_driver_setup.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterDriversAndDshow"
