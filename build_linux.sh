#!/usr/bin/env bash
# ==============================================================================
# NexusCam / EyeCam - Master Linux Build Script
# Compiles the Linux V4L2 Sink Driver, Test Utilities, and Desktop Studio
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_linux"
BUILD_TYPE="${1:-Release}"

echo "======================================================================"
echo " NexusCam: Building Desktop Engine & V4L2 Virtual Camera for Linux"
echo " Build Type : ${BUILD_TYPE}"
echo " Source Dir : ${SCRIPT_DIR}"
echo " Output Dir : ${BUILD_DIR}"
echo "======================================================================"

# 1. Check prerequisites
echo "[1/4] Checking build dependencies..."
command -v cmake >/dev/null 2>&1 || { echo >&2 "[-] Error: cmake is required. Install: sudo apt install cmake"; exit 1; }
command -v pkg-config >/dev/null 2>&1 || { echo >&2 "[-] Error: pkg-config is required. Install: sudo apt install pkg-config"; exit 1; }

# Check compiler (prefer clang++, fallback to g++)
if command -v clang++ >/dev/null 2>&1; then
    export CXX=clang++
    export CC=clang
elif command -v g++ >/dev/null 2>&1; then
    export CXX=g++
    export CC=gcc
else
    echo >&2 "[-] Error: No C++20 compiler found (clang++ or g++ required)."
    exit 1
fi
echo "[+] Using compiler: $(${CXX} --version | head -n1)"

# Check optional v4l2loopback
if lsmod | grep -q "v4l2loopback"; then
    echo "[+] v4l2loopback kernel module is currently loaded."
else
    echo "[!] Note: v4l2loopback module not loaded. Run 'sudo ./scripts/linux/setup_v4l2loopback.sh' before streaming."
fi

# 2. Configure CMake
echo "[2/4] Configuring CMake project..."
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${SCRIPT_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_EYECAM_APP=ON

# 3. Build artifacts
echo "[3/4] Compiling targets..."
NPROC=$(nproc || echo 4)
cmake --build . --config "${BUILD_TYPE}" --parallel "${NPROC}"

# 4. Summary and validation
echo "[4/4] Build complete! Artifacts located in: ${BUILD_DIR}/bin"
ls -la "${BUILD_DIR}/bin" || true

echo "======================================================================"
echo " Next Steps:"
echo " 1. Setup V4L2 loopback device: sudo ./scripts/linux/setup_v4l2loopback.sh"
echo " 2. Run test pattern stream:   ${BUILD_DIR}/bin/linux_test"
if [ -f "${BUILD_DIR}/bin/EyeCam" ]; then
    echo " 3. Launch Desktop Studio UI:  ${BUILD_DIR}/bin/EyeCam"
fi
echo "======================================================================"
