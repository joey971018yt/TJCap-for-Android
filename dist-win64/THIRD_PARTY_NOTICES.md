# 第三方開源元件授權聲明 (Third-Party Open Source Notices)

**JCast for Android** 隨附使用以下獨立預編譯之第三方開源元件（皆透過獨立行程、動態連結程式庫或作業系統驅動層隔離呼叫，未與主程式靜態連結）：

---

## 1. Android SDK Platform-Tools — ADB (`runtime/adb.exe`, `runtime/AdbWinApi.dll`, `runtime/AdbWinUsbApi.dll`)
- **官方網站 / 原始碼**：https://android.googlesource.com/platform/packages/modules/adb
- **授權條款**：Apache License, Version 2.0 (`Apache-2.0`)
- **使用方式**：作為獨立執行檔於「免裝手機 App 直連模式」建立本機 USB 偵錯轉發通道。

---

## 2. scrcpy-server (`runtime/jcast-server.jar`, upstream `scrcpy-server v5.0.1`)
- **官方網站 / 原始碼**：https://github.com/Genymobile/scrcpy
- **版權聲明**：Copyright (C) 2018 Genymobile / Romain Vimont
- **授權條款**：Apache License, Version 2.0 (`Apache-2.0`)
- **使用方式**：於「免裝手機 App 直連模式」透過 `app_process` 在手機端執行硬體 H.264 螢幕編碼與系統內錄音訊串流。

---

## 3. libusb (`runtime/libusb-1.0.dll`)
- **官方網站 / 原始碼**：https://github.com/libusb/libusb
- **授權條款**：GNU Lesser General Public License v2.1 (`LGPL-2.1`)
- **使用方式**：於「免開 USB 偵錯備援模式 (USB AOA)」透過 `LoadLibraryExW` 動態載入，使用者可隨時自行替換相容版本之 `libusb-1.0.dll`。

---

## 4. libusb-win32 (`drivers/libusb0.sys`, `drivers/libusb0.dll`)
- **官方網站 / 原始碼**：https://github.com/mcuee/libusb-win32
- **數位簽章**：GlobalSign GCC R45 EV CodeSigning CA / Microsoft WHQL 相容簽章
- **授權條款**：
  - `libusb0.sys`（獨立核心態過濾驅動程式）：GNU General Public License v3.0 (`GPL-3.0`) — 見隨附檔案 `drivers/COPYING_GPL.txt`
  - `libusb0.dll`（使用者態介面程式庫）：GNU Lesser General Public License v2.1 (`LGPL-2.1`) — 見隨附檔案 `drivers/COPYING_LGPL.txt`
