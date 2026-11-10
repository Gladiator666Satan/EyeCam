#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace EyeCam {

/**
 * @brief Підтримувані формати пікселів для віртуальної камери.
 */
enum class PixelFormat : uint32_t {
    RGB24 = 0,  // 24-бітний RGB (3 байти на піксель: R, G, B)
    BGR24 = 1,  // 24-бітний BGR (3 байти на піксель: B, G, R - стандарт DirectShow / Windows)
    YUY2  = 2,  // YUYV / YUY2 (4:2:2 піддискретизація, 2 байти на піксель: Y0, U0, Y1, V0)
    NV12  = 3   // Двопланарний формат YUV 4:2:0
};

/**
 * @brief Роздільна здатність кадру.
 */
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

/**
 * @brief Метадані та структура сирого відеокадру.
 */
struct VideoFrame {
    Resolution resolution;
    PixelFormat format = PixelFormat::YUY2;
    uint32_t stride = 0;           // Розмір рядка в байтах (pitch/stride)
    size_t dataSize = 0;           // Повний розмір буфера даних
    const uint8_t* data = nullptr; // Вказівник на сирі піксельні байти
    uint64_t timestampNs = 0;      // Часова мітка кадру в наносекундах
};

/**
 * @brief Обчислює необхідний розмір буфера в байтах для заданого формату та роздільної здатності.
 */
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

/**
 * @brief Обчислює stride (кількість байт у рядку).
 */
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

/**
 * @brief Заголовок спільної пам'яті (Shared Memory) для IPC між продюсером та Windows DirectShow фільтром.
 */
#pragma pack(push, 1)
struct SharedMemoryHeader {
    uint32_t magicNumber;       // 0x45594543 ("EYEC")
    uint32_t version;           // Версія протоколу (1)
    uint32_t width;             // Поточна ширина
    uint32_t height;            // Поточна висота
    uint32_t pixelFormat;       // PixelFormat
    uint32_t fps;               // Бажаний FPS (30 або 60)
    uint64_t frameIndex;        // Лічильник кадрів, що зростає
    uint64_t timestampNs;       // Часова мітка генерації
    uint32_t dataSize;          // Фактичний розмір кадру
    uint32_t isConnected;       // 1 - якщо продюсер активний, 0 - якщо зупинений
};
#pragma pack(pop)

constexpr uint32_t EYECAM_MAGIC = 0x45594543;
constexpr size_t MAX_FRAME_BUFFER_SIZE = 3840 * 2160 * 4; // До 4K RGBA/YUY2

} // namespace EyeCam
