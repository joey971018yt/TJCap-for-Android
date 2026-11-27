@echo off
setlocal
cd /d "%~dp0\.."
echo [Build] Configuring CMake Visual Studio x64 Release build for JCast for Android...
cmake -S . -B build-msvc -A x64
if errorlevel 1 exit /b 1
cmake --build build-msvc --config Release --parallel
if errorlevel 1 exit /b 1
echo [Test] Running unit tests...
.\build-msvc\Release\android_vcam_tests.exe
if errorlevel 1 exit /b 1
echo [OK] Build & tests succeeded! Binaries are in build-msvc\Release\
