#include "VirtualCameraSink.hpp"
#include <QDebug>
#include <cstring>

#if defined(EYECAM_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

// Структура заголовка кільцевого трипл-буфера в пам'яті
struct VirtualCameraSink::ShmHeader {
    uint32_t magic;          // 'ECAM'
    uint32_t version;        // Версія протоколу
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;         // NV12 або RGBA
    uint32_t frameRate;
    uint32_t activeBufferIdx;// 0, 1 або 2
    uint64_t sequenceNumber;
    uint64_t timestampNs;
};

VirtualCameraSink::VirtualCameraSink(QObject *parent) : QObject(parent) {}

VirtualCameraSink::~VirtualCameraSink() {
    shutdown();
}

bool VirtualCameraSink::initialize(int width, int height, int fps) {
    m_width = width;
    m_height = height;
    m_fps = fps;

    // Розрахунок розміру NV12 кадру: Width * Height * 1.5
    m_frameSize = static_cast<size_t>(width * height * 1.5);
    size_t totalShmSize = sizeof(ShmHeader) + (m_frameSize * 3); // Потрійна буферизація

    qInfo() << "[VirtualCameraSink] Створення IPC каналу:" << width << "x" << height 
            << "@" << fps << "fps. Розмір буфера:" << totalShmSize / (1024 * 1024) << "MB";

#if defined(EYECAM_PLATFORM_WINDOWS)
    // Windows Shared Memory через FileMapping
    LPCWSTR shmName = L"Global\\EyeCam_VirtualCamera_Shm";
    LPCWSTR eventName = L"Global\\EyeCam_VirtualCamera_Event";

    m_shmHandle = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(totalShmSize),
        shmName
    );

    if (!m_shmHandle) {
        qWarning() << "[VirtualCameraSink] Помилка CreateFileMappingW:" << GetLastError();
        return false;
    }

    m_shmMemory = MapViewOfFile(m_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, totalShmSize);
    if (!m_shmMemory) {
        qWarning() << "[VirtualCameraSink] Помилка MapViewOfFile:" << GetLastError();
        CloseHandle(m_shmHandle);
        m_shmHandle = nullptr;
        return false;
    }

    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, eventName);

#else
    // Linux Shared Memory через shm_open / mmap
    int fd = shm_open("/eyecam_vcam_shm", O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        qWarning() << "[VirtualCameraSink] Помилка shm_open";
        return false;
    }
    if (ftruncate(fd, totalShmSize) != 0) {
        close(fd);
        return false;
    }

    m_shmMemory = mmap(nullptr, totalShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m_shmMemory == MAP_FAILED) {
        m_shmMemory = nullptr;
        return false;
    }
#endif

    // Ініціалізація метаданих у заголовку
    m_shmHeader = static_cast<ShmHeader *>(m_shmMemory);
    m_shmHeader->magic = 0x4543414D; // 'ECAM'
    m_shmHeader->version = 1;
    m_shmHeader->width = width;
    m_shmHeader->height = height;
    m_shmHeader->fourcc = 0x3231564E; // 'NV12'
    m_shmHeader->frameRate = fps;
    m_shmHeader->activeBufferIdx = 0;
    m_shmHeader->sequenceNumber = 0;
    m_shmHeader->timestampNs = 0;

    m_initialized = true;
    return true;
}

void VirtualCameraSink::shutdown() {
    if (!m_initialized) return;

#if defined(EYECAM_PLATFORM_WINDOWS)
    if (m_shmMemory) {
        UnmapViewOfFile(m_shmMemory);
        m_shmMemory = nullptr;
    }
    if (m_shmHandle) {
        CloseHandle(m_shmHandle);
        m_shmHandle = nullptr;
    }
    if (m_frameEvent) {
        CloseHandle(m_frameEvent);
        m_frameEvent = nullptr;
    }
#else
    if (m_shmMemory) {
        size_t totalShmSize = sizeof(ShmHeader) + (m_frameSize * 3);
        munmap(m_shmMemory, totalShmSize);
        shm_unlink("/eyecam_vcam_shm");
        m_shmMemory = nullptr;
    }
#endif

    m_initialized = false;
    qInfo() << "[VirtualCameraSink] IPC міст вимкнено.";
}

void VirtualCameraSink::writeFrame(const uint8_t *data, size_t size, uint64_t timestampNs) {
    if (!m_initialized || !m_shmHeader || !data || size == 0) return;

    // Вибір наступного буфера для потрійної буферизації без блокування споживача
    uint32_t nextBuffer = (m_shmHeader->activeBufferIdx + 1) % 3;
    uint8_t *bufferBase = reinterpret_cast<uint8_t *>(m_shmHeader + 1);
    uint8_t *targetDest = bufferBase + (nextBuffer * m_frameSize);

    // Копіювання розпакованого кадру
    std::memcpy(targetDest, data, std::min(size, m_frameSize));

    // Оновлення індексу та таймстемпу кадру
    m_shmHeader->timestampNs = timestampNs;
    m_shmHeader->sequenceNumber++;
    m_shmHeader->activeBufferIdx = nextBuffer;

#if defined(EYECAM_PLATFORM_WINDOWS)
    if (m_frameEvent) {
        // Сповіщення драйвера віртуальної камери про надходження нового кадру
        SetEvent(static_cast<HANDLE>(m_frameEvent));
    }
#endif
}
