<p align="center">
  <img src="assets/app_icon.png" width="112" alt="TJCap for Android App Icon" />
</p>

<h1 align="center">TJCap for Android v1.0.0</h1>

<p align="center">
  <b>Windows 10 / 11 專用 — Android 手機／平板 USB-C 雙模免 Root 2K+ 60/120fps 虛擬擷取卡 App</b><br/>
  免實體擷取卡 · 免 Root · 預設免裝手機 App 直連（USB 偵錯）＋ 內附免開 USB 偵錯備援（TJCap Companion APK + USB AOA）<br/>
  USB 速率智慧辨識 · 四欄式即時健康度儀表板 · 獨立全螢幕預覽 (`F11`) · 原畫質無損截圖 (`F12`) · 智慧故障診斷與自動更新
</p>

---

## 一、下載與安裝（零外部依賴，開箱即用！）

請直接前往本專案右側的 **[Releases（發布頁面）](../../releases/latest)** 下載：

1. **📦 正式安裝版 App（最推薦）：`TJCap-for-Android-Setup-v1.0.0.exe`**
   - 雙擊執行安裝精靈，自動在**桌面**與**開始選單**建立帶有翠綠專屬圖示的 **「TJCap for Android」** App，並自動註冊 DirectShow 視訊＋音訊虛擬擷取卡（獨立 CLSID，可與 `JCast for iOS` 並存同時使用！）。
   - 內建 **GitHub Releases 自動更新功能**：日後有新版本時，App 內點擊 **`⬆️ 升級`** 即可自動驗證 `SHA-256` 並一鍵升級！
2. **📁 免安裝綠色便攜版 App：`TJCap-for-Android-Portable-v1.0.0.zip`**
   - 完整解壓縮後直接雙擊資料夾內的 **`TJCap for Android.exe`** 即可使用（同樣支援原地一鍵自動更新！）。
3. **📲 手機端免開 USB 偵錯備援包：`TJCap-Companion-v1.0.0.apk`** *(已同步內附於電腦端程式 `companion\` 資料夾中)*
   - **一般用戶無需安裝！** 僅當您的手遊反外掛禁止開啟「開發者選項／USB 偵錯」時，才需點擊控制台 **`📲 取得 APK`** 將此 APK 傳至手機安裝並切換為「🛡️ 免開 USB 偵錯備援 (AOA)」模式。

---

## 二、兩種免 Root USB-C 連線模式自由切換

| 比較項目 | ⚡ 模式一：免裝手機 App 直連（預設推薦） | 🛡️ 模式二：免開 USB 偵錯備援（Companion + AOA） |
| :--- | :--- | :--- |
| **需要 Root 嗎？** | ❌ **完全免 Root** | ❌ **完全免 Root** |
| **需要安裝手機 APK 嗎？** | ❌ **免裝任何 APK**（插線即用） | ✅ 安裝隨附輕量 `TJCap-Companion-v1.0.0.apk` (129 KB) |
| **需要開啟「USB 偵錯」嗎？** | ✅ 需開啟一次「開發者選項 ➔ USB 偵錯」 | ❌ **完全免開開發者選項與 USB 偵錯！** |
| **最高畫質與幀率** | 最高 **2K+/4K (`3840p`) · `60 / 120 FPS`** | 最高 **2K+/4K (`3840p`) · `60 / 120 FPS`** |
| **系統內錄立體聲音訊** | ✅ Android 11+ 全系統原生內錄 (`48kHz`) | ✅ Android 10+ `AudioPlaybackCapture` (`48kHz`) |
| **最適合情境** | 日常直播、一般手遊（小米免開安全設定、免每次點錄製授權） | **嚴格偵測並封鎖「USB 偵錯／開發者選項」的特定手遊**或公司管控機 |

---

## 三、30 秒快速開始教學（OBS Studio / Discord / Zoom / 外接大螢幕）

### 🔹 方法 A（預設推薦）：免裝手機 App 高速直連
1. 在 Android 手機開啟 **「設定 ➔ 關於手機 ➔ 連點 7 下『版本號碼』(小米為 OS 版本)」**，返回 **「系統／更多設定 ➔ 開發者選項」** 開啟 **「USB 偵錯」**。
2. 用 USB-C 傳輸線連接電腦，在手機彈出的視窗勾選 **「一律允許」** 並點 **「允許」**。
3. 開啟電腦上的 **「TJCap for Android」**，確認自動辨識出的解析度與 `60 / 120 FPS` 選項後，點擊 **`🚀 智慧一鍵啟動`**！

### 🔹 方法 B（備援選項）：免開 USB 偵錯模式（Companion APK + USB AOA）
1. 點擊主視窗右上方的 **`📲 取得 APK`** 按鈕，將 `TJCap-Companion-v1.0.0.apk` 傳到手機安裝（手機可完全關閉開發者選項與 USB 偵錯）。
2. 將主視窗第一列「USB-C 擷取連線模式」切換為 **`🛡️ 免開 USB 偵錯備援（TJCap Companion APK + USB AOA）`**，點擊 **`🚀 智慧一鍵啟動`**。
3. 電腦會自動將手機切換為高速 USB AOA 配件並喚醒手機上的 `TJCap Companion`，點擊手機畫面上的 **`🚀 授權螢幕錄製並開始 USB-C 直播`** 即可！

### 🔹 連接 OBS Studio 直播／錄影（或免開 OBS 直接全螢幕預覽）
- **OBS Studio**：新增 **「視訊擷取裝置」** ➔ 選擇 **`TJCap for Android (Video)`**（保持「裝置預設值」自動抓滿版原生比例、零黑邊！）；往下勾選 **「使用自訂音訊裝置」** ➔ 選擇 **`TJCap for Android (Audio)`**。
- **直接當電腦外接大螢幕**：點擊控制台右下角 **`🖥 獨立大視窗`**（按 **`F11`** 切換無邊框全螢幕、**`Tab`/`F3`** 顯示即時健康度 HUD、**`F12`** 一鍵擷取 1:1 原畫質無損 PNG 截圖並自動複製剪貼簿！）。

---

## 四、USB 接口速率自動辨識與最大解析度分級

| 偵測到的 USB 接口／線材速率 | 開放之最高可選解析度上限 | 可選項目說明 |
| :--- | :--- | :--- |
| **⚡ USB 3.x (`5 / 10 / 20 Gbps`)** | **最高 `3840p` 無上限** | 解鎖 `🌟 原生極限無上限 (3840p)`（完整釋放 Galaxy S25 Ultra `3120×1440`、Pixel Pro、旗艦平板原生畫質），並保留 `2560p` / `1920p` / `1280p` 及固定 16:9 畫布 |
| **🔌 USB 2.0 (`480 Mbps`)**<br/>*(多數中階手機或一般 USB-C 充電線)* | **最高 `2K (2560p)`** | 自動隱藏超負荷 `3840p`，提供 `💎 2K 高畫質上限 (2560p)`、`✨ 1080p 穩定推薦 (1920p)`、`⚡ 720p (1280p)` 與固定 16:9 畫布 |
| **⚠️ USB 1.1 (`1.5 / 12 Mbps`)** | **最高 `720p (1280p)`** | 僅開放 `⚡ 720p (1280p)`、`📱 540p (960p)` 與固定 `1280×720` 畫布，避免低速集線器斷流 |

---

## 五、從原始碼編譯 (Building from Source)

- **Windows 10/11 原生編譯 (Visual Studio 2022 MSVC x64)**：執行 `scripts\build_windows.bat`
- **TJCap Companion Android APK 編譯 (Android SDK Build-Tools 35)**：執行 `companion-apk/build_apk.sh`
- **第三方開源授權聲明**：請參閱 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
