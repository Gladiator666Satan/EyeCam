# ==============================================================================
# EyeCam Studio — PowerShell Script для перевірки та розгортання структури проєкту
# Запуск: powershell -ExecutionPolicy Bypass -File .\deploy_eyecam_desktop.ps1
# ==============================================================================

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
if (-not $ProjectRoot) { $ProjectRoot = (Get-Location).Path }

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  EyeCam Studio: Perevirka ta rozgortannya codovoi bazy   " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "Directory: $ProjectRoot" -ForegroundColor DarkGray

# 1. Spysok katalogiv
$Directories = @(
    "src/core",
    "src/pipeline",
    "src/ui/qml/components",
    "src/ui/qml/views"
)

foreach ($dir in $Directories) {
    $fullPath = Join-Path $ProjectRoot $dir
    if (-not (Test-Path $fullPath)) {
        New-Item -ItemType Directory -Path $fullPath -Force | Out-Null
        Write-Host " [+] Created folder: $dir" -ForegroundColor Green
    }
}

# 2. Spysok neobhidnykh fayliv
$RequiredFiles = @(
    "CMakeLists.txt",
    "ARCHITECTURE.md",
    "src/main.cpp",
    "src/core/AppState.hpp",
    "src/core/AppState.cpp",
    "src/core/TelemetryService.hpp",
    "src/core/TelemetryService.cpp",
    "src/core/DeviceDiscoveryService.hpp",
    "src/core/DeviceDiscoveryService.cpp",
    "src/pipeline/VideoPipeline.hpp",
    "src/pipeline/VideoPipeline.cpp",
    "src/pipeline/VirtualCameraSink.hpp",
    "src/pipeline/VirtualCameraSink.cpp",
    "src/pipeline/VideoTextureProvider.hpp",
    "src/pipeline/VideoTextureProvider.cpp",
    "src/ui/qml/Main.qml",
    "src/ui/qml/Theme.qml",
    "src/ui/qml/components/GlassCard.qml",
    "src/ui/qml/components/GlassSlider.qml",
    "src/ui/qml/components/TelemetryBadge.qml",
    "src/ui/qml/views/LeftSidebar.qml",
    "src/ui/qml/views/CenterViewport.qml",
    "src/ui/qml/views/RightInspector.qml"
)

Write-Host "`nPerevirka fayliv:" -ForegroundColor Yellow
$missing = 0
foreach ($f in $RequiredFiles) {
    $filePath = Join-Path $ProjectRoot $f
    if (Test-Path $filePath) {
        $size = (Get-Item $filePath).Length
        Write-Host " [OK] $f ($size bytes)" -ForegroundColor Gray
    } else {
        Write-Host " [MISSING] $f" -ForegroundColor Red
        $missing++
    }
}

if ($missing -eq 0) {
    Write-Host "`n[SUCCESS] Vsi 22 desktop-fayly prisutni v robochiy paptsi!" -ForegroundColor Green
} else {
    Write-Host "`n[WARNING] Kilkist vidsutnih fayliv: $missing" -ForegroundColor Red
}

Write-Host "==========================================================" -ForegroundColor Cyan
