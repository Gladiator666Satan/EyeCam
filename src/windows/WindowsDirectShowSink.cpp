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

    // Спроба відкриття Global об'єкта, у разі відсутності прав UAC - відкат до Local префікса
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
        ss << "Помилка виклику CreateFileMapping: " << GetLastError();
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
        ss << "Помилка виклику MapViewOfFile: " << GetLastError();
        m_lastError = ss.str();
        Close();
        return false;
    }

    m_hFrameEvent = CreateEventW(nullptr, FALSE, FALSE, SHM_EVENT_NAME_GLOBAL);
    if (m_hFrameEvent == nullptr) {
        m_hFrameEvent = CreateEventW(nullptr, FALSE, FALSE, SHM_EVENT_NAME_LOCAL);
    }

    if (m_hFrameEvent == nullptr) {
        std::ostringstream ss;
        ss << "Помилка створення FrameEvent: " << GetLastError();
        m_lastError = ss.str();
        Close();
        return false;
    }

    m_hMutex = CreateMutexW(nullptr, FALSE, SHM_MUTEX_NAME_GLOBAL);
    if (m_hMutex == nullptr) {
        m_hMutex = CreateMutexW(nullptr, FALSE, SHM_MUTEX_NAME_LOCAL);
    }

    if (m_hMutex == nullptr) {
        std::ostringstream ss;
        ss << "Помилка створення SHM Mutex: " << GetLastError();
        m_lastError = ss.str();
        Close();
        return false;
    }

    m_width = width;
    m_height = height;
    m_format = format;
    m_fps = fps;
    m_frameCount = 0;

    // Ініціалізація заголовка у спільній пам'яті
    DWORD waitResult = WaitForSingleObject(m_hMutex, 2000);
    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
        SharedMemoryHeader* header = reinterpret_cast<SharedMemoryHeader*>(m_pBuf);
        header->magic = SHM_MAGIC;
        header->version = SHM_PROTOCOL_VERSION;
        header->headerSize = sizeof(SharedMemoryHeader);
        header->totalSize = static_cast<uint32_t>(TOTAL_SHARED_MEMORY_SIZE);
        header->maxFrameSize = static_cast<uint32_t>(MAX_FRAME_BUFFER_SIZE);
        header->ringBufferOffset = static_cast<uint32_t>(sizeof(SharedMemoryHeader));
        header->activeSlot = 0;
        header->state = StreamState::RUNNING;

        FrameMetadata* meta0 = reinterpret_cast<FrameMetadata*>(
            reinterpret_cast<uint8_t*>(m_pBuf) + sizeof(SharedMemoryHeader)
        );
        meta0->width = width;
        meta0->height = height;
        meta0->fourcc = PixelFormatToFourCC(format);
        meta0->stride = static_cast<uint32_t>(m_frameSize / height);
        meta0->dataSize = static_cast<uint32_t>(m_frameSize);
        meta0->fps = fps;
        meta0->frameIndex = 0;
        meta0->timestampNs = 0;

        FrameMetadata* meta1 = reinterpret_cast<FrameMetadata*>(
            reinterpret_cast<uint8_t*>(m_pBuf) + sizeof(SharedMemoryHeader) + SINGLE_SLOT_SIZE
        );
        *meta1 = *meta0;

        ReleaseMutex(m_hMutex);
    } else {
        m_lastError = "Не вдалося захопити м'ютекс для ініціалізації заголовка SHM.";
        Close();
        return false;
    }

    return true;
#endif
}

void WindowsDirectShowSink::Close() {
#if defined(_WIN32)
    if (m_hMutex != nullptr && m_pBuf != nullptr) {
        DWORD waitResult = WaitForSingleObject(m_hMutex, 500);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
            SharedMemoryHeader* header = reinterpret_cast<SharedMemoryHeader*>(m_pBuf);
            header->state = StreamState::STOPPED;
            ReleaseMutex(m_hMutex);
        }
    }

    if (m_pBuf != nullptr) {
        UnmapViewOfFile(m_pBuf);
        m_pBuf = nullptr;
    }

    if (m_hMapFile != nullptr) {
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
    }

    if (m_hFrameEvent != nullptr) {
        CloseHandle(m_hFrameEvent);
        m_hFrameEvent = nullptr;
    }

    if (m_hMutex != nullptr) {
        CloseHandle(m_hMutex);
        m_hMutex = nullptr;
    }

    m_width = 0;
    m_height = 0;
    m_frameSize = 0;
    m_frameCount = 0;
#endif
}

bool WindowsDirectShowSink::WriteFrame(const uint8_t* data, size_t size) {
#if !defined(_WIN32)
    return false;
#else
    if (!IsOpen() || data == nullptr || size == 0) {
        m_lastError = "Спільна пам'ять не відкрита або вхідний буфер порожній.";
        return false;
    }

    if (size > MAX_FRAME_BUFFER_SIZE) {
        m_lastError = "Розмір кадру перевищує виділений розмір слоту пам'яті.";
        return false;
    }

    DWORD waitResult = WaitForSingleObject(m_hMutex, 50);
    if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
        m_lastError = "Таймаут очікування м'ютекса спільної пам'яті.";
        return false;
    }

    SharedMemoryHeader* header = reinterpret_cast<SharedMemoryHeader*>(m_pBuf);
    
    // Подвійна буферизація: пишемо у пасивний слот
    uint32_t writeSlot = 1 - header->activeSlot;
    size_t slotOffset = sizeof(SharedMemoryHeader) + (static_cast<size_t>(writeSlot) * SINGLE_SLOT_SIZE);
    
    FrameMetadata* meta = reinterpret_cast<FrameMetadata*>(
        reinterpret_cast<uint8_t*>(m_pBuf) + slotOffset
    );

    uint8_t* pFramePayload = reinterpret_cast<uint8_t*>(meta) + sizeof(FrameMetadata);

    meta->width = m_width;
    meta->height = m_height;
    meta->fourcc = PixelFormatToFourCC(m_format);
    meta->stride = static_cast<uint32_t>(m_frameSize / m_height);
    meta->dataSize = static_cast<uint32_t>(size);
    meta->fps = m_fps;
    meta->frameIndex = ++m_frameCount;
    
    auto now = std::chrono::steady_clock::now();
    meta->timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    std::memcpy(pFramePayload, data, size);

    // Атомарне перемикання активного слота для споживача (DirectShow фільтр)
    header->activeSlot = writeSlot;

    ReleaseMutex(m_hMutex);

    // Сповіщення споживача про готовність нового кадру
    SetEvent(m_hFrameEvent);

    return true;
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

bool WindowsDirectShowSink::SetFormat(uint32_t width, uint32_t height, PixelFormat format, uint32_t fps) {
    return Open(width, height, format, fps);
}

} // namespace EyeCam