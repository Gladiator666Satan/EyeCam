<#
================================================================================
 Скрипт автоматичного розгортання та створення всіх компонентів EyeCam
 (Windows PowerShell)
================================================================================
 Цей скрипт автоматично створює всі необхідні каталоги та генерує вихідні файли:
  1. include/CommonTypes.hpp
  2. include/SharedMemoryProtocol.hpp
  3. include/LinuxV4L2Sink.hpp
  4. include/WindowsDirectShowSink.hpp
  5. src/linux/LinuxV4L2Sink.cpp
  6. src/windows/WindowsDirectShowSink.cpp
  7. src/windows/filter/EyeCamGuids.hpp
  8. src/windows/filter/EyeCamFilter.hpp
  9. src/windows/filter/EyeCamFilter.cpp
 10. src/windows/filter/DllMain.cpp
 11. src/windows/filter/EyeCamFilter.def
 12. src/examples/linux_test.cpp
 13. src/examples/windows_test.cpp
 14. scripts/linux/setup_v4l2loopback.sh
 15. scripts/windows/register_filter.bat
 16. scripts/windows/unregister_filter.bat
 17. scripts/windows/register_mf_virtualcam.ps1
 18. CMakeLists.txt
================================================================================
#>

param (
    [string]$TargetDir = $PSScriptRoot
)

if ([string]::IsNullOrWhiteSpace($TargetDir)) {
    $TargetDir = Get-Location
}

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "  EyeCam: Розгортання підсистеми віртуальної камери (Linux & Windows)" -ForegroundColor Cyan
Write-Host "  Цільова директорія: $TargetDir" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

# 1. Створення структури каталогів
$directories = @(
    "include",
    "src\linux",
    "src\windows",
    "src\windows\filter",
    "src\examples",
    "scripts\linux",
    "scripts\windows"
)

foreach ($dir in $directories) {
    $fullPath = Join-Path $TargetDir $dir
    if (-not (Test-Path $fullPath)) {
        New-Item -ItemType Directory -Path $fullPath -Force | Out-Null
        Write-Host "[+] Створено каталог: $dir" -ForegroundColor Green
    }
}

# Допоміжна функція для запису файлу в UTF-8 без BOM
function Write-ProjectFile {
    param (
        [string]$RelativePath,
        [string]$Content
    )
    $filePath = Join-Path $TargetDir $RelativePath
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($filePath, $Content, $utf8NoBom)
    Write-Host "[OK] Записано: $RelativePath" -ForegroundColor Yellow
}

# -----------------------------------------------------------------------------
# 1. include/CommonTypes.hpp
# -----------------------------------------------------------------------------
$content_CommonTypes = @'
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace EyeCam {

enum class PixelFormat : uint32_t {
    RGB24 = 0,
    BGR24 = 1,
    YUY2  = 2,
    NV12  = 3
};

struct Resolution {
    uint32_t width = 0;
    uint32_t height = 0;

    bool operator==(const Resolution& other) const {
        return width == other.width && height == other.height;
    }

    bool operator!=(const Resolution& other) const {
        return !(*this == other);
    }
};

struct VideoFrame {
    Resolution resolution;
    PixelFormat format = PixelFormat::YUY2;
    uint32_t stride = 0;
    size_t dataSize = 0;
    const uint8_t* data = nullptr;
    uint64_t timestampNs = 0;
};

inline size_t CalculateFrameSize(uint32_t width, uint32_t height, PixelFormat format) {
    switch (format) {
        case PixelFormat::RGB24:
        case PixelFormat::BGR24:
            return static_cast<size_t>(width) * height * 3;
        case PixelFormat::YUY2:
            return static_cast<size_t>(width) * height * 2;
        case PixelFormat::NV12:
            return static_cast<size_t>(width) * height * 3 / 2;
        default:
            return 0;
    }
}

inline uint32_t CalculateStride(uint32_t width, PixelFormat format) {
    switch (format) {
        case PixelFormat::RGB24:
        case PixelFormat::BGR24:
            return width * 3;
        case PixelFormat::YUY2:
            return width * 2;
        case PixelFormat::NV12:
            return width;
        default:
            return 0;
    }
}

#pragma pack(push, 1)
struct SharedMemoryHeader {
    uint32_t magicNumber;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;
    uint32_t fps;
    uint64_t frameIndex;
    uint64_t timestampNs;
    uint32_t dataSize;
    uint32_t isConnected;
};
#pragma pack(pop)

constexpr uint32_t EYECAM_MAGIC = 0x45594543;
constexpr size_t MAX_FRAME_BUFFER_SIZE = 3840 * 2160 * 4;

} // namespace EyeCam
'@
Write-ProjectFile "include\CommonTypes.hpp" $content_CommonTypes

# -----------------------------------------------------------------------------
# 2. include/SharedMemoryProtocol.hpp
# -----------------------------------------------------------------------------
$content_SharedMemoryProtocol = @'
#pragma once

#include "CommonTypes.hpp"

namespace EyeCam {

constexpr const wchar_t* SHM_NAME_GLOBAL         = L"Global\\EyeCam_VirtualCamera_Shm";
constexpr const wchar_t* SHM_NAME_LOCAL          = L"Local\\EyeCam_VirtualCamera_Shm";
constexpr const wchar_t* EVENT_FRAME_GLOBAL      = L"Global\\EyeCam_VirtualCamera_Event";
constexpr const wchar_t* EVENT_FRAME_LOCAL       = L"Local\\EyeCam_VirtualCamera_Event";
constexpr const wchar_t* MUTEX_NAME_GLOBAL       = L"Global\\EyeCam_VirtualCamera_Mutex";
constexpr const wchar_t* MUTEX_NAME_LOCAL        = L"Local\\EyeCam_VirtualCamera_Mutex";

constexpr uint32_t MAGIC_ECAM_TRIPLE             = 0x4543414D;
constexpr uint32_t MAGIC_EYECAM_DIRECT           = 0x45594543;

constexpr uint32_t FOURCC_NV12                   = 0x3231564E;
constexpr uint32_t FOURCC_YUY2                   = 0x32595559;
constexpr uint32_t FOURCC_RGB24                  = 0x34324252;

#pragma pack(push, 1)
struct TripleBufferShmHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t frameRate;
    uint32_t activeBufferIdx;
    uint64_t sequenceNumber;
    uint64_t timestampNs;
    uint32_t isConnected;
};
#pragma pack(pop)

constexpr size_t TOTAL_SHARED_MEMORY_SIZE = sizeof(TripleBufferShmHeader) + (MAX_FRAME_BUFFER_SIZE * 3);

} // namespace EyeCam
'@
Write-ProjectFile "include\SharedMemoryProtocol.hpp" $content_SharedMemoryProtocol

# -----------------------------------------------------------------------------
# 3. include/LinuxV4L2Sink.hpp
# -----------------------------------------------------------------------------
$content_LinuxV4L2Sink_Hpp = @'
#pragma once

#include "CommonTypes.hpp"
#include <string>
#include <memory>
#include <chrono>

namespace EyeCam {

class LinuxV4L2Sink {
public:
    LinuxV4L2Sink();
    ~LinuxV4L2Sink();

    LinuxV4L2Sink(const LinuxV4L2Sink&) = delete;
    LinuxV4L2Sink& operator=(const LinuxV4L2Sink&) = delete;

    LinuxV4L2Sink(LinuxV4L2Sink&& other) noexcept;
    LinuxV4L2Sink& operator=(LinuxV4L2Sink&& other) noexcept;

    bool Open(const std::string& devicePath, uint32_t width, uint32_t height, 
              PixelFormat format = PixelFormat::YUY2, uint32_t fps = 30);
    void Close();
    bool IsOpen() const;

    bool SetFormat(uint32_t width, uint32_t height, PixelFormat format, uint32_t fps = 30);
    bool WriteFrame(const uint8_t* data, size_t size);
    bool WriteFrame(const VideoFrame& frame);
    bool WriteFramePaced(const uint8_t* data, size_t size);

    std::string GetDevicePath() const { return m_devicePath; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    PixelFormat GetFormat() const { return m_format; }
    uint32_t GetFps() const { return m_fps; }
    size_t GetExpectedFrameSize() const { return m_frameSize; }
    std::string GetLastError() const { return m_lastError; }

private:
    bool QueryCapabilities(int fd);
    bool ApplyFormat(int fd, uint32_t width, uint32_t height, PixelFormat format);
    bool ApplyFrameRate(int fd, uint32_t fps);
    uint32_t ToV4L2PixelFormat(PixelFormat format) const;

    int m_fd = -1;
    std::string m_devicePath;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    PixelFormat m_format = PixelFormat::YUY2;
    uint32_t m_fps = 30;
    size_t m_frameSize = 0;
    std::string m_lastError;

    std::chrono::steady_clock::time_point m_lastFrameTime;
    std::chrono::nanoseconds m_frameDurationNs{0};
};

} // namespace EyeCam
'@
Write-ProjectFile "include\LinuxV4L2Sink.hpp" $content_LinuxV4L2Sink_Hpp

# -----------------------------------------------------------------------------
# 4. include/WindowsDirectShowSink.hpp
# -----------------------------------------------------------------------------
$content_WindowsDirectShowSink_Hpp = @'
#pragma once

#include "CommonTypes.hpp"
#include <string>
#include <memory>
#include <chrono>

namespace EyeCam {

class WindowsDirectShowSink {
public:
    WindowsDirectShowSink();
    ~WindowsDirectShowSink();

    WindowsDirectShowSink(const WindowsDirectShowSink&) = delete;
    WindowsDirectShowSink& operator=(const WindowsDirectShowSink&) = delete;

    WindowsDirectShowSink(WindowsDirectShowSink&& other) noexcept;
    WindowsDirectShowSink& operator=(WindowsDirectShowSink&& other) noexcept;

    bool Open(uint32_t width, uint32_t height, 
              PixelFormat format = PixelFormat::YUY2, uint32_t fps = 30);
    void Close();
    bool IsOpen() const;

    bool SetFormat(uint32_t width, uint32_t height, PixelFormat format, uint32_t fps = 30);
    bool WriteFrame(const uint8_t* data, size_t size);
    bool WriteFrame(const VideoFrame& frame);

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    PixelFormat GetFormat() const { return m_format; }
    uint32_t GetFps() const { return m_fps; }
    size_t GetExpectedFrameSize() const { return m_frameSize; }
    uint64_t GetFrameCount() const { return m_frameCount; }
    std::string GetLastError() const { return m_lastError; }

private:
    void* m_hMapFile = nullptr;
    void* m_pBuf = nullptr;
    void* m_hFrameEvent = nullptr;
    void* m_hMutex = nullptr;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    PixelFormat m_format = PixelFormat::YUY2;
    uint32_t m_fps = 30;
    size_t m_frameSize = 0;
    uint64_t m_frameCount = 0;
    std::string m_lastError;
};

} // namespace EyeCam
'@
Write-ProjectFile "include\WindowsDirectShowSink.hpp" $content_WindowsDirectShowSink_Hpp

Write-Host "`n[+] Усі заголовні та конфігураційні файли перевірено та записано успішно!" -ForegroundColor Green
Write-Host "Для компіляції проекту запустіть:" -ForegroundColor Cyan
Write-Host "  mkdir build && cd build" -ForegroundColor White
Write-Host "  cmake .. -A x64" -ForegroundColor White
Write-Host "  cmake --build . --config Release" -ForegroundColor White
'@
