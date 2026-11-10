#!/usr/bin/env bash
# EyeCam Android Component Validator & Setup Script (Bash)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "==> Checking EyeCam Android components in: ${PROJECT_ROOT}"

DIRECTORIES=(
    "${PROJECT_ROOT}/src/main/java/com/eyecam/model"
    "${PROJECT_ROOT}/src/main/java/com/eyecam/service"
    "${PROJECT_ROOT}/src/main/java/com/eyecam/ui"
    "${PROJECT_ROOT}/src/main/java/com/eyecam/ui/components"
    "${PROJECT_ROOT}/src/main/java/com/eyecam/ui/theme"
    "${PROJECT_ROOT}/src/main/java/com/eyecam/viewmodel"
)

for dir in "${DIRECTORIES[@]}"; do
    if [ ! -d "$dir" ]; then
        mkdir -p "$dir"
        echo "  [CREATED] $dir"
    fi
done

FILES=(
    "src/main/AndroidManifest.xml"
    "src/main/java/com/eyecam/model/StreamState.kt"
    "src/main/java/com/eyecam/service/PowerLockManager.kt"
    "src/main/java/com/eyecam/service/ThermalManager.kt"
    "src/main/java/com/eyecam/service/CameraStreamService.kt"
    "src/main/java/com/eyecam/ui/theme/Color.kt"
    "src/main/java/com/eyecam/ui/theme/Theme.kt"
    "src/main/java/com/eyecam/viewmodel/StreamViewModel.kt"
    "src/main/java/com/eyecam/ui/components/CameraPreview.kt"
    "src/main/java/com/eyecam/ui/components/QuickToggleHud.kt"
    "src/main/java/com/eyecam/ui/components/DisplayProtectionOverlay.kt"
    "src/main/java/com/eyecam/ui/EyeCamScreen.kt"
    "src/main/java/com/eyecam/ui/MainActivity.kt"
    "build.gradle.kts"
    "settings.gradle.kts"
)

ALL_VALID=true
for file in "${FILES[@]}"; do
    FULL_PATH="${PROJECT_ROOT}/${file}"
    if [ -f "$FULL_PATH" ]; then
        LEN=$(wc -c < "$FULL_PATH")
        echo "  [OK] ${file} (${LEN} bytes)"
    else
        echo "  [MISSING] ${file}"
        ALL_VALID=false
    fi
done

if [ "$ALL_VALID" = true ]; then
    echo -e "\n==> All EyeCam Android files are present and verified!"
else
    echo -e "\n==> Some files are missing."
fi
