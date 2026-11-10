# EyeCam Android Component Validator & Setup Script (PowerShell)
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Resolve-Path "$ScriptDir\.."

Write-Host "==> Checking EyeCam Android components in: $ProjectRoot" -ForegroundColor Cyan

$Directories = @(
    "$ProjectRoot\src\main\java\com\eyecam\model",
    "$ProjectRoot\src\main\java\com\eyecam\service",
    "$ProjectRoot\src\main\java\com\eyecam\ui",
    "$ProjectRoot\src\main\java\com\eyecam\ui\components",
    "$ProjectRoot\src\main\java\com\eyecam\ui\theme",
    "$ProjectRoot\src\main\java\com\eyecam\viewmodel"
)

foreach ($dir in $Directories) {
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
        Write-Host "  [CREATED] $dir" -ForegroundColor Green
    }
}

$Files = @(
    "src/main/AndroidManifest.xml",
    "src/main/java/com/eyecam/model/StreamState.kt",
    "src/main/java/com/eyecam/service/PowerLockManager.kt",
    "src/main/java/com/eyecam/service/ThermalManager.kt",
    "src/main/java/com/eyecam/service/CameraStreamService.kt",
    "src/main/java/com/eyecam/ui/theme/Color.kt",
    "src/main/java/com/eyecam/ui/theme/Theme.kt",
    "src/main/java/com/eyecam/viewmodel/StreamViewModel.kt",
    "src/main/java/com/eyecam/ui/components/CameraPreview.kt",
    "src/main/java/com/eyecam/ui/components/QuickToggleHud.kt",
    "src/main/java/com/eyecam/ui/components/DisplayProtectionOverlay.kt",
    "src/main/java/com/eyecam/ui/EyeCamScreen.kt",
    "src/main/java/com/eyecam/ui/MainActivity.kt",
    "build.gradle.kts",
    "settings.gradle.kts"
)

$allValid = $true
foreach ($file in $Files) {
    $fullPath = Join-Path $ProjectRoot $file
    if (Test-Path $fullPath) {
        $len = (Get-Item $fullPath).Length
        Write-Host "  [OK] $file ($len bytes)" -ForegroundColor Green
    } else {
        Write-Host "  [MISSING] $file" -ForegroundColor Red
        $allValid = $false
    }
}

if ($allValid) {
    Write-Host "`n==> All EyeCam Android files are present and verified!" -ForegroundColor Cyan
} else {
    Write-Host "`n==> Some files are missing." -ForegroundColor Yellow
}
