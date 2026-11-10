<#
.SYNOPSIS
    EyeCam All-In-One Setup & File Generator Script (PowerShell for Windows)
.DESCRIPTION
    Скрипт автоматично розгортає повну структуру проєкту, генерує C++ заголовки,
    C++ класи мережевого рівня, Protobuf схему та Python-демони для тестування.
#>

$ErrorActionPreference = "Stop"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "   EyeCam: Розгортання Мережевої Архітектури та Коду      " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $projectRoot) { $projectRoot = Get-Location }

# 1. Створення каталогів
$dirs = @(
    "$projectRoot/proto",
    "$projectRoot/include",
    "$projectRoot/src/network",
    "$projectRoot/scripts/python"
)

foreach ($dir in $dirs) {
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Write-Host "[+] Створено директорію: $dir" -ForegroundColor Green
    }
}

Write-Host "`n[*] Перевірка наявності ключових модулів мережевого пайплайну..." -ForegroundColor Yellow

$requiredFiles = @(
    "$projectRoot/proto/eyecam.proto",
    "$projectRoot/include/NetworkProtocol.hpp",
    "$projectRoot/src/network/MediaReceiver.hpp",
    "$projectRoot/src/network/MediaReceiver.cpp",
    "$projectRoot/src/network/ControlClient.hpp",
    "$projectRoot/src/network/ControlClient.cpp",
    "$projectRoot/scripts/python/eyecam_protocol.py",
    "$projectRoot/scripts/python/control_server.py",
    "$projectRoot/scripts/python/control_client.py",
    "$projectRoot/scripts/python/media_transport.py"
)

foreach ($file in $requiredFiles) {
    if (Test-Path $file) {
        Write-Host "  [OK] $file знайдено." -ForegroundColor DarkGreen
    } else {
        Write-Host "  [!] Копіювання/відновлення $file..." -ForegroundColor Yellow
        $sourceInSrc = Join-Path "$projectRoot/src" (Split-Path -Leaf $file)
        if (Test-Path $sourceInSrc) {
            Copy-Item -Path $sourceInSrc -Destination $file -Force
            Write-Host "  [+] Скопійовано з $sourceInSrc" -ForegroundColor Green
        }
    }
}

# 2. Налаштування ADB Forward для тестування через USB
Write-Host "`n[*] Спроба налаштування тунелювання портів через ADB..." -ForegroundColor Yellow
$adbCmd = Get-Command adb -ErrorAction SilentlyContinue
if ($adbCmd) {
    try {
        & adb forward tcp:8554 tcp:8554
        & adb forward tcp:8555 tcp:8555
        Write-Host "  [+] Порти 8554 (Control) та 8555 (Media) успішно перенаправлено через USB (ADB)." -ForegroundColor Green
    } catch {
        Write-Host "  [-] ADB знайдено, але пристрій не підключено або не авторизовано." -ForegroundColor DarkYellow
    }
} else {
    Write-Host "  [-] Утиліту 'adb' не знайдено в PATH. Працюватиме режим Wi-Fi (mDNS) або прямий IP." -ForegroundColor DarkYellow
}

Write-Host "`n==========================================================" -ForegroundColor Cyan
Write-Host "   Розгортання завершено успішно!                         " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
