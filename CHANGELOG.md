# TJCap for Android — 版本更新日誌 (Changelog)

## `v1.0.0` (2026-11-27) — 初版正式發布
* **🟢 雙模免 Root Android USB-C 有線擷取引擎**：
  * **模式一（預設推薦 — 免裝手機 App 直連）**：內建免外部設定之獨立 `ADB` 與硬體編碼微服務 (`tjcap-server.jar`)，免 Root、免手動安裝 APK、小米/Redmi 免開安全設定，支援最高 **2K+/4K (`3840p`) @ `60 / 120 FPS` H.264** 畫面與 **Android 11+ 全系統 `48kHz` 立體聲內錄音訊**。
  * **模式二（免開 USB 偵錯備援 — `TJCap Companion APK` + `USB AOA` 配件直連）**：隨附專屬簽章 **`TJCap-Companion-v1.0.0.apk`**（完整相容 Android 8～16），專為禁止開啟「開發者選項／USB 偵錯」的手遊或管控設備設計，透過 `MediaProjection` + `USB Open Accessory` Bulk 通道高速傳輸影音。
  * **智慧雙模自動切換**：支援優先走 ADB 直連，偵測到未開 USB 偵錯時自動切換至 USB AOA 配件模式。
* **🎥 Windows 10 / 11 原生雙路 DirectShow 虛擬擷取卡**：
  * 註冊獨立 CLSID 之 **`TJCap for Android (Video)`** 與 **`TJCap for Android (Audio)`**（可與 `JCast for iOS` 在同一台電腦並存同時運作！），零黑邊自適應輸出至 OBS Studio、Discord、Zoom、Teams。
* **⚡ USB 接口速率自動辨識與鎖定畫布機制**：
  * 透過 Windows SetupAPI + USB Hub IOCTL 即時辨識 `USB 3.x (5/10/20 Gbps)`、`USB 2.0 (480 Mbps)` 與 `USB 1.1`，分級解鎖 `3840p` / `2560p` / `1280p` 上限；開始串流後全程鎖定畫布不閃爍。
* **🎬 翠綠主題控制台 × 低負載即時預覽 × 獨立無邊框全螢幕視窗 (`F11`) × 原畫質無損截圖 (`F12`)**：
  * 內建 `D3D11VA` GPU 硬體解碼與異常零中斷自動 CPU 軟解自癒 (`TJCAP-E402`)。
  * 內建四欄式串流健康度儀表板（即時 `FPS` 折線圖、`Mbps` 碼率、微秒級延遲 `ms`、健康度分級）與 **`🩺 診斷報錯中心`（含自動隱私脫敏與一鍵 GitHub Issue 回報）**。
  * 內建 GitHub Releases `SHA-256` 自動驗證與一鍵升級（支援 `Setup.exe` 安裝版與 `Portable.zip` 便攜版）。
