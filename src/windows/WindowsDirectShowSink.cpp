#include "WindowsDirectShowSink.hpp"
#include "SharedMemoryProtocol.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstring>
#include <iostream>
#include <sstream>

namespace EyeCam {

WindowsDirectShowSink::WindowsDirectShowSink()
    : m_hMapFile(nullptr), m_pBuf(nullptr), m_hFrameEvent(nullptr), m_hMutex(nullptr),
      m_width(0), m_height(0), m_format(PixelFormat::YUY2), m_fps(30), 
      m_frameSize(0), m_frameCount(0) {
}

WindowsDirectShowSink::~WindowsDirectShowSink() {
    Close();
}

WindowsDirectShowSink::WindowsDirectShowSink(WindowsDirectShowSink&& other) noexcept
    : m_hMapFile(other.m_hMapFile),
      m_pBuf(other.m_pBuf),
      m_hFrameEvent(other.m_hFrameEvent),
      m_hMutex(other.m_hMutex),
      m_width(other.m_width),
      m_height(other.m_height),
      m_format(other.m_format),
      m_fps(other.m_fps),
      m_frameSize(other.m_frameSize),
      m_frameCount(other.m_frameCount),
      m_lastError(std::move(other.m_lastError)) {
    other.m_hMapFile = nullptr;
    other.m_pBuf = nullptr;
    other.m_hFrameEvent = nullptr;
    other.m_hMutex = nullptr;
}

WindowsDirectShowSink& WindowsDirectShowSink::operator=(WindowsDirectShowSink&& other) noexcept {
    if (this != &other) {
        Close();
        m_hMapFile = other.m_hMapFile;
        m_pBuf = other.m_pBuf;
        m_hFrameEvent = other.m_hFrameEvent;
        m_hMutex = other.m_hMutex;
        m_width = other.m_width;
        m_height = other.m_height;
        m_format = other.m_format;
        m_fps = other.m_fps;
        m_frameSize = other.m_frameSize;
        m_frameCount = other.m_frameCount;
        m_lastError = std::move(other.m_lastError);

        other.m_hMapFile = nullptr;
        other.m_pBuf = nullptr;
        other.m_hFrameEvent = nullptr;
        other.m_hMutex = nullptr;
    }
    return *this;
}

bool WindowsDirectShowSink::Open(uint32_t width, uint32_t height, PixelFormat format, uint32_t fps) {
#if !defined(_WIN32)
    m_lastError = "WindowsDirectShowSink підтримується виключно на платформі Windows.";
    return false;
#else
    Close();

    m_frameSize = CalculateFrameSize(width, height, format);
    if (m_frameSize == 0 || m_frameSize > MAX_FRAME_BUFFER_SIZE) {
        m_lastError = "Некоректний або занадто великий розмір буфера кадру.";
        return false;
    }

    // Спроба відкриття Global\\ об'єкта, у разі відсутності прав UAC - відкат до Local\\
    m_hMapFile = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(TOTAL_SHARED_MEMORY_SIZE),
        SHM_NAME_GLOBAL
    );

    if (m_hMapFile == nullptr) {
        m_hMapFile = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(TOTAL_SHARED_MEMORY_SIZE),
            SHM_NAME_LOCAL
        );
    }

    if (m_hMapFile == nullptr) {
        std::ostringstream ss;
        ss << "Помилка CreateFileMappingW (код: " << GetLastError() << ")";
        m_lastError = ss.str();
        return false;
    }

    m_pBuf = MapViewOfFile(
        m_hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        TOTAL_SHARED_MEMORY_SIZE
    );

    if (m_pBuf == nullptr) {
        std::ostringstream ss;
        ss << "Помилка MapViewOfFile (код: " << GetLastError() << ")";
        m_lastError = ss.str();
        Close();
        return false;
    }

    // Створення події синхронізації (Event)
    m_hFrameEvent = CreateEventW(nullptr, FALSE, FALSE, EVENT_FRAME_GLOBAL);
    if (m_hFrameEvent == nullptr) {
        m_hFrameEvent = CreateEventW(nullptr, FALSE, FALSE, EVENT_FRAME_LOCAL);
    }

    // Створення м'ютекса (Mutex)
    m_hMutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME_GLOBAL);
    if (m_hMutex == nullptr) {
        m_hMutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME_LOCAL);
    }

    // Ініціалізація заголовка спільної пам'яті
    if (m_hMutex) {
        WaitForSingleObject(m_hMutex, INFINITE);
    }

    auto* header = reinterpret_cast<SharedMemoryHeader*>(m_pBuf);
    header->magicNumber = EYECAM_MAGIC;
    header->version = 1;
    header->width = width;
    header->height = height;
    header->pixelFormat = static_cast<uint32_t>(format);
    header->fps = fps;
    header->frameIndex = 0;
    header->timestampNs = 0;
    header->dataSize = static_cast<uint32_t>(m_frameSize);
    header->isConnected = 1;

    if (m_hMutex) {
        ReleaseMutex(m_hMutex);
    }

    m_width = width;
    m_height = height;
    m_format = format;
    m_fps = fps;
    m_frameCount = 0;
    m_lastError.clear();

    return true;
#endif
}

void WindowsDirectShowSink::Close() {
#if defined(_WIN32)
    if (m_pBuf != nullptr) {
        if (m_hMutex != nullptr) {
            WaitForSingleObject(m_hMutex, 50);
            auto* header = reinterpret_cast<SharedMemoryHeader*>(m_pBuf);
            header->isConnected = 0;
            ReleaseMutex(m_hMutex);
        }
        UnmapViewOfFile(m_pBuf);
        m_pBuf = nullptr;
    }

    if (m_hMutex != nullptr) {
        CloseHandle(m_hMutex);
        m_hMutex = nullptr;
    }

    if (m_hFrameEvent != nullptr) {
        CloseHandle(m_hFrameEvent);
        m_hFrameEvent = nullptr;
    }

    if (m_hMapFile != nullptr) {
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
    }
#endif
    m_width = 0;
    m_height = 0;
    m_frameSize = 0;
    m_frameCount = 0;
}

bool WindowsDirectShowSink::IsOpen() const {
    return m_pBuf != nullptr && m_hMapFile != nullptr;
}

bool WindowsDirectShowSink::SetFormat(uint32_t width, uint32_t height, PixelFormat format, uint32_t fps) {
    if (!IsOpen()) {
        m_lastError = "Канал пам'яті не відкрито для зміни формату.";
        return false;
    }

#if defined(_WIN32)
    size_t newSize = CalculateFrameSize(width, height, format);
    if (newSize == 0 || newSize > MAX_FRAME_BUFFER_SIZE) {
        m_lastError = "Неприпустима геометрія або перевищено ліміт буфера.";
        return false;
    }

    if (m_hMutex) {
        WaitForSingleObject(m_hMutex, 200);
    }

    auto* header = reinterpret_cast<SharedMemoryHeader*>(m_pBuf);
    header->width = width;
    header->height = height;
    header->pixelFormat = static_cast<uint32_t>(format);
    header->fps = fps;
    header->dataSize = static_cast<uint32_t>(newSize);

    if (m_hMutex) {
        ReleaseMutex(m_hMutex);
    }

    m_width = width;
    m_height = height;
    m_format = format;
    m_fps = fps;
    m_frameSize = newSize;

    if (m_hFrameEvent) {
        SetEvent(m_hFrameEvent);
    }
    return true;
#else
    return false;
#endif
}

bool WindowsDirectShowSink::WriteFrame(const uint8_t* data, size_t size) {
    if (!IsOpen()) {
        m_lastError = "Спроба запису в неініціалізований буфер Windows DirectShow.";
        return false;
    }

    if (data == nullptr || size != m_frameSize) {
        std::ostringstream ss;
        ss << "Невідповідність розміру кадру: надано " << size 
           << " байт, очікується " << m_frameSize << " байт.";
        m_lastError = ss.str();
        return false;
    }

#if defined(_WIN32)
    if (m_hMutex) {
        WaitForSingleObject(m_hMutex, 50);
    }

    auto* header = reinterpret_cast<SharedMemoryHeader*>(m_pBuf);
    uint8_t* pixelBuffer = reinterpret_cast<uint8_t*>(m_pBuf) + sizeof(SharedMemoryHeader);

    // Швидке копіювання байтів у відображену спільну пам'ять
    std::memcpy(pixelBuffer, data, size);

    m_frameCount++;
    header->frameIndex = m_frameCount;
    header->timestampNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    if (m_hMutex) {
        ReleaseMutex(m_hMutex);
    }

    if (m_hFrameEvent) {
        SetEvent(m_hFrameEvent);
    }
    return true;
#else
    return false;
#endif
}

bool WindowsDirectShowSink::WriteFrame(const VideoFrame& frame) {
    if (frame.resolution.width != m_width || 
        frame.resolution.height != m_height || 
        frame.format != m_format) {
        if (!SetFormat(frame.resolution.width, frame.resolution.height, frame.format, m_fps)) {
            return false;
        }
    }

    return WriteFrame(frame.data, frame.dataSize);
}

} // namespace EyeCam
