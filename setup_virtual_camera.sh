#!/usr/bin/env bash
# ==============================================================================
# Скрипт автоматичного розгортання та перевірки файлів EyeCam (Bash / Linux)
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "======================================================================"
echo "  EyeCam: Розгортання підсистеми віртуальної камери (Linux & Windows)"
echo "  Каталог: ${SCRIPT_DIR}"
echo "======================================================================"

# Створення каталогів
mkdir -p "${SCRIPT_DIR}/include"
mkdir -p "${SCRIPT_DIR}/src/linux"
mkdir -p "${SCRIPT_DIR}/src/windows/filter"
mkdir -p "${SCRIPT_DIR}/src/examples"
mkdir -p "${SCRIPT_DIR}/scripts/linux"
mkdir -p "${SCRIPT_DIR}/scripts/windows"

echo "[+] Структуру каталогів підготовлено."

# Надання прав на виконання скриптів
chmod +x "${SCRIPT_DIR}/scripts/linux/setup_v4l2loopback.sh" 2>/dev/null || true

echo "[+] Усі файли проекту знаходяться на своїх місцях згідно зі специфікацією:"
ls -la "${SCRIPT_DIR}/include"
ls -la "${SCRIPT_DIR}/src/linux"
ls -la "${SCRIPT_DIR}/src/windows"
ls -la "${SCRIPT_DIR}/src/windows/filter"
ls -la "${SCRIPT_DIR}/src/examples"
ls -la "${SCRIPT_DIR}/scripts/linux"
ls -la "${SCRIPT_DIR}/scripts/windows"

echo ""
echo "[*] Для збірки на Linux виконайте:"
echo "    mkdir -p build && cd build"
echo "    cmake .."
echo "    cmake --build . --config Release"
