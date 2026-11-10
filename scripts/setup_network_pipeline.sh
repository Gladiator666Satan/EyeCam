#!/usr/bin/env bash
# ==============================================================================
# EyeCam All-In-One Setup & File Generator Script (Bash for Linux)
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "=========================================================="
echo "   EyeCam: Розгортання Мережевого Рівня (Linux)          "
echo "=========================================================="

mkdir -p "$PROJECT_ROOT/proto"
mkdir -p "$PROJECT_ROOT/include"
mkdir -p "$PROJECT_ROOT/src/network"
mkdir -p "$PROJECT_ROOT/scripts/python"

echo "[*] Перевірка цілісності модулів..."
REQUIRED_FILES=(
    "$PROJECT_ROOT/proto/eyecam.proto"
    "$PROJECT_ROOT/include/NetworkProtocol.hpp"
    "$PROJECT_ROOT/src/network/MediaReceiver.hpp"
    "$PROJECT_ROOT/src/network/MediaReceiver.cpp"
    "$PROJECT_ROOT/src/network/ControlClient.hpp"
    "$PROJECT_ROOT/src/network/ControlClient.cpp"
    "$PROJECT_ROOT/scripts/python/eyecam_protocol.py"
    "$PROJECT_ROOT/scripts/python/control_server.py"
    "$PROJECT_ROOT/scripts/python/control_client.py"
    "$PROJECT_ROOT/scripts/python/media_transport.py"
)

for file in "${REQUIRED_FILES[@]}"; do
    if [ -f "$file" ]; then
        echo "  [OK] $file знайдено."
    else
        echo "  [!] Відновлення $file..."
        cp -f "$PROJECT_ROOT/src/$(basename "$file")" "$file" 2>/dev/null || true
    fi
done

# ADB Forwarding
if command -v adb >/dev/null 2>&1; then
    echo "[*] Налаштування прокидання портів ADB через USB..."
    adb forward tcp:8554 tcp:8554 2>/dev/null || true
    adb forward tcp:8555 tcp:8555 2>/dev/null || true
    echo "  [+] Порти 8554 та 8555 перенаправлено."
else
    echo "  [-] ADB не знайдено в PATH."
fi

echo "=========================================================="
echo "   Розгортання завершено!                                "
echo "=========================================================="
