#Requires -Version 5.1
$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "[Info] Requesting Administrator privileges..." -ForegroundColor Green
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    exit 0
}

Set-Location -LiteralPath $PSScriptRoot

Write-Host "=============================================================" -ForegroundColor Green
Write-Host "  JCast for Android 完整移除程式" -ForegroundColor Green
Write-Host "=============================================================" -ForegroundColor Green

foreach ($procName in @("JCast for Android", "android_vcam_gui", "android_vcam_host")) {
    $p = Get-Process -Name $procName -ErrorAction SilentlyContinue
    if ($p) {
        Stop-Process -Name $procName -Force -ErrorAction SilentlyContinue
    }
}
Start-Sleep -Milliseconds 400

if (Test-Path ".\android_vcam_driver_setup.exe") {
    & ".\android_vcam_driver_setup.exe" --uninstall
} elseif (Test-Path ".\android_vcam_dshow.dll") {
    & regsvr32.exe /u /s ".\android_vcam_dshow.dll"
}

Write-Host ""
Write-Host "[完成] 已停止內建 ADB 服務、移除 AOA 過濾層並反註冊 JCast for Android 虛擬擷取卡。" -ForegroundColor Green
Read-Host "按 Enter 鍵關閉視窗"
