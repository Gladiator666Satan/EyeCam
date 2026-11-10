#pragma once

#include "CommonTypes.hpp"
#include <string>
#include <memory>
#include <chrono>

namespace EyeCam {

/**
 * @brief C++ клас-продюсер для віртуальної камери Windows.
 * 
 * Працює у зв'язці з DirectShow Capture Filter (EyeCamFilter.dll), який зареєстрований
 * у системі як фізичний вебкамерний пристрій (CLSID_VideoInputDeviceCategory).
 * 
 * Продюсер транслює сирі кадри YUY2/RGB через високошвидкісну спільну пам'ять (Shared Memory IPC)
 * з мінімальною латентністю та блокуванням подій (Event-driven).
 * Підтримує динамічну зміну роздільної здатності (Dynamic Resizing) та 30/60 FPS.
 */
class WindowsDirectShowSink {
public:
    WindowsDirectShowSink();
    ~WindowsDirectShowSink();

    // Заборона копіювання
    WindowsDirectShowSink(const WindowsDirectShowSink&) = delete;
    WindowsDirectShowSink& operator=(const WindowsDirectShowSink&) = delete;

    // Дозвіл переміщення
    WindowsDirectShowSink(WindowsDirectShowSink&& other) noexcept;
    WindowsDirectShowSink& operator=(WindowsDirectShowSink&& other) noexcept;

    /**
     * @brief Ініціалізує канал спільної пам'яті та синхронізації з DirectShow фільтром.
     * @param width Ширина відео (наприклад, 1920, 1280)
     * @param height Висота відео (наприклад, 1080, 720)
     * @param format Формат пікселів (YUY2, RGB24, BGR24)
     * @param fps Бажана частота кадрів (30 або 60)
     * @return true у разі успішної ініціалізації
     */
    bool Open(uint32_t width, uint32_t height, 
              PixelFormat format = PixelFormat::YUY2, uint32_t fps = 30);

    /**
     * @brief Закриває ресурси спільної пам'яті та об'єкти синхронізації.
     */
    void Close();

    /**
     * @brief Перевіряє, чи канал передачі активний.
     */
    bool IsOpen() const;

    /**
     * @brief Динамічно змінює роздільну здатність, формат або FPS на льоту.
     * DirectShow фільтр зчитує оновлений заголовок та адаптує медіа-потік для Zoom/Teams/Discord/OBS.
     */
    bool SetFormat(uint32_t width, uint32_t height, PixelFormat format, uint32_t fps = 30);

    /**
     * @brief Записує сирий буфер кадру у спільну пам'ять віртуальної камери.
     * @param data Вказівник на сирі пікселі
     * @param size Розмір буфера в байтах
     * @return true якщо кадр успішно записано та відправлено сигнал сповіщення
     */
    bool WriteFrame(const uint8_t* data, size_t size);

    /**
     * @brief Записує кадр із передачею структури VideoFrame.
     */
    bool WriteFrame(const VideoFrame& frame);

    // Гетери стану
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    PixelFormat GetFormat() const { return m_format; }
    uint32_t GetFps() const { return m_fps; }
    size_t GetExpectedFrameSize() const { return m_frameSize; }
    uint64_t GetFrameCount() const { return m_frameCount; }
    std::string GetLastError() const { return m_lastError; }

private:
    void* m_hMapFile = nullptr;             // HANDLE файлової проекції пам'яті (CreateFileMappingW)
    void* m_pBuf = nullptr;                 // Вказівник на відображену пам'ять (MapViewOfFile)
    void* m_hFrameEvent = nullptr;          // HANDLE події нового кадру (CreateEventW)
    void* m_hMutex = nullptr;               // HANDLE м'ютекса для атомарного запису (CreateMutexW)

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    PixelFormat m_format = PixelFormat::YUY2;
    uint32_t m_fps = 30;
    size_t m_frameSize = 0;
    uint64_t m_frameCount = 0;
    std::string m_lastError;
};

} // namespace EyeCam
