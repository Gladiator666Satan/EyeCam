#pragma once

#include "CommonTypes.hpp"

namespace EyeCam {

// Імена системних об'єктів пам'яті Windows
// Використовуємо префікс Global\ для підтримки взаємодії між різними сесіями (UAC / сервіси),
// з резервним відкриттям Local\ у разі обмежень прав.
constexpr const wchar_t* SHM_NAME_GLOBAL         = L"Global\\EyeCam_VirtualCamera_Shm";
constexpr const wchar_t* SHM_NAME_LOCAL          = L"Local\\EyeCam_VirtualCamera_Shm";
constexpr const wchar_t* EVENT_FRAME_GLOBAL      = L"Global\\EyeCam_VirtualCamera_Event";
constexpr const wchar_t* EVENT_FRAME_LOCAL       = L"Local\\EyeCam_VirtualCamera_Event";
constexpr const wchar_t* MUTEX_NAME_GLOBAL       = L"Global\\EyeCam_VirtualCamera_Mutex";
constexpr const wchar_t* MUTEX_NAME_LOCAL        = L"Local\\EyeCam_VirtualCamera_Mutex";

// Магічні константи протоколів
constexpr uint32_t MAGIC_ECAM_TRIPLE             = 0x4543414D; // 'ECAM' (Triple Buffering з VirtualCameraSink)
constexpr uint32_t MAGIC_EYECAM_DIRECT           = 0x45594543; // 'EYEC' (Direct Frame з WindowsDirectShowSink)

// FourCC коди форматів
constexpr uint32_t FOURCC_NV12                   = 0x3231564E; // 'NV12'
constexpr uint32_t FOURCC_YUY2                   = 0x32595559; // 'YUY2'
constexpr uint32_t FOURCC_RGB24                  = 0x34324252; // 'RB24'

/**
 * @brief Заголовок кільцевого трипл-буфера спільної пам'яті (Zero-Lock IPC)
 */
#pragma pack(push, 1)
struct TripleBufferShmHeader {
    uint32_t magic;           // MAGIC_ECAM_TRIPLE
    uint32_t version;         // Версія (1)
    uint32_t width;           // Ширина (наприклад, 1920, 3840)
    uint32_t height;          // Висота (наприклад, 1080, 2160)
    uint32_t fourcc;          // FOURCC_NV12, FOURCC_YUY2, тощо
    uint32_t frameRate;       // 30 або 60 FPS
    uint32_t activeBufferIdx; // Індекс активного буфера (0, 1 або 2)
    uint64_t sequenceNumber;  // Монотонний лічильник кадрів
    uint64_t timestampNs;     // PTS у наносекундах
    uint32_t isConnected;     // 1 - активний, 0 - відключений
};
#pragma pack(pop)

// Максимальний буфер спільної пам'яті (до 4K YUV/RGB потрійної буферизації)
constexpr size_t TOTAL_SHARED_MEMORY_SIZE = sizeof(TripleBufferShmHeader) + (MAX_FRAME_BUFFER_SIZE * 3);

} // namespace EyeCam
