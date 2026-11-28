# 貢獻指南 (Contributing to TJCap for Android)

感謝你對 **TJCap for Android** 的興趣！歡迎任何形式的貢獻，包括回報 Bug、功能建議、文件改進與程式碼 Pull Request。

## 開發環境需求

- **Windows 建置**：Visual Studio 2022（C++20 桌面開發工具組）、CMake 3.22+、Inno Setup 6（選用，用於打包安裝檔）
- **Linux 交叉編譯**：`mingw-w64` (`x86_64-w64-mingw32-g++`)、CMake 3.22+
- **Companion APK**：JDK 17+、Android SDK Build-Tools 35

## 本機建置與測試

### Windows (MSVC x64)
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Linux (單元測試與 MinGW x64 交叉編譯)
```bash
# 執行核心單元測試
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j
ctest --test-dir build-linux --output-on-failure

# 交叉編譯 Windows x64 執行檔
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw -j
```

## 提交 Pull Request 流程

1. Fork 本專案並從 `main` 建立功能分支（例如 `feat/your-feature` 或 `fix/issue-123`）。
2. 確認所有單元測試通過，且未引入無關檔案或敏感憑證。
3. 使用清晰的 commit 訊息（建議採用 Conventional Commits，例如 `fix(audio): ...`、`feat(ui): ...`）。
4. 開啟 Pull Request 並填寫變更說明與測試方式。

## 授權聲明

提交至此專案的貢獻將依本專案之 [Apache License 2.0](LICENSE) 釋出。
