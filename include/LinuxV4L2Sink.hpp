#pragma once

#include "CommonTypes.hpp"
#include <string>
#include <memory>
#include <chrono>

namespace EyeCam {

/**
 * @brief Модуль виводу відео у віртуальний пристрій v4l2loopback (/dev/videoX) для Linux.
 * 
 * Клас інкапсулює роботу з драйвером Video4Linux2:
 * - Відкриття та налаштування дескриптора пристрою;
 * - Перевірка можливостей (capabilities) через VIDIOC_QUERYCAP;
 * - Динамічна зміна роздільної здатності (Dynamic Resizing) та формату пікселів через VIDIOC_S_FMT;
 * - Конфігурація частоти кадрів (30/60 FPS) через VIDIOC_S_PARM;
 * - Прямий запис сирих буферів YUY2, RGB24, BGR24 у віртуальну ноду.
 */
class LinuxV4L2Sink {
public:
    LinuxV4L2Sink();
    ~LinuxV4L2Sink();

    // Заборона копіювання для запобігання дублюванню файлових дескрипторів
    LinuxV4L2Sink(const LinuxV4L2Sink&) = delete;
    LinuxV4L2Sink& operator=(const LinuxV4L2Sink&) = delete;

    // Дозвіл переміщення
    LinuxV4L2Sink(LinuxV4L2Sink&& other) noexcept;
    LinuxV4L2Sink& operator=(LinuxV4L2Sink&& other) noexcept;

    /**
     * @brief Відкриває пристрій v4l2loopback та ініціалізує початковий відеорежим.
     * @param devicePath Шлях до пристрою, наприклад "/dev/video0" або "/dev/video10"
     * @param width Ширина відео в пікселях (наприклад, 1920, 1280)
     * @param height Висота відео в пікселях (наприклад, 1080, 720)
     * @param format Формат пікселів (RGB24, BGR24, YUY2)
     * @param fps Частота кадрів (за замовчуванням 30 або 60)
     * @return true у разі успіху, false якщо виникла помилка
     */
    bool Open(const std::string& devicePath, uint32_t width, uint32_t height, 
              PixelFormat format = PixelFormat::YUY2, uint32_t fps = 30);

    /**
     * @brief Закриває пристрій та звільняє дескриптор.
     */
    void Close();

    /**
     * @brief Перевіряє, чи пристрій наразі відкритий.
     */
    bool IsOpen() const;

    /**
     * @brief Динамічно змінює роздільну здатність, формат або частоту кадрів на льоту.
     * @param width Нова ширина
     * @param height Нова висота
     * @param format Новий формат пікселів
     * @param fps Нова частота кадрів
     * @return true якщо переналаштування успішне
     */
    bool SetFormat(uint32_t width, uint32_t height, PixelFormat format, uint32_t fps = 30);

    /**
     * @brief Записує сирий буфер кадру у віртуальну камеру.
     * @param data Вказівник на сирі пікселі
     * @param size Розмір буфера в байтах (повинен відповідати CalculateFrameSize)
     * @return true якщо кадр успішно передано драйверу
     */
    bool WriteFrame(const uint8_t* data, size_t size);

    /**
     * @brief Перевантаження для запису структури VideoFrame.
     */
    bool WriteFrame(const VideoFrame& frame);

    /**
     * @brief Записує кадр із дотриманням таймінгу FPS (Pacing).
     * Якщо виклик здійснюється занадто швидко, метод очікує потрібний інтервал часу.
     */
    bool WriteFramePaced(const uint8_t* data, size_t size);

    // Гетери стану
    std::string GetDevicePath() const { return m_devicePath; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    PixelFormat GetFormat() const { return m_format; }
    uint32_t GetFps() const { return m_fps; }
    size_t GetExpectedFrameSize() const { return m_frameSize; }
    std::string GetLastError() const { return m_lastError; }

private:
    /**
     * @brief Запитує capabilities пристрою через VIDIOC_QUERYCAP.
     */
    bool QueryCapabilities(int fd);

    /**
     * @brief Застосовує параметри формату до пристрою через VIDIOC_S_FMT.
     */
    bool ApplyFormat(int fd, uint32_t width, uint32_t height, PixelFormat format);

    /**
     * @brief Встановлює частоту кадрів через VIDIOC_S_PARM.
     */
    bool ApplyFrameRate(int fd, uint32_t fps);

    /**
     * @brief Конвертує PixelFormat в ідентифікатор V4L2 fourcc.
     */
    uint32_t ToV4L2PixelFormat(PixelFormat format) const;

    int m_fd = -1;                          // Файловий дескриптор /dev/videoX
    std::string m_devicePath;               // Шлях до пристрою
    uint32_t m_width = 0;                   // Поточна ширина
    uint32_t m_height = 0;                  // Поточна висота
    PixelFormat m_format = PixelFormat::YUY2; // Поточний формат
    uint32_t m_fps = 30;                    // Поточний FPS
    size_t m_frameSize = 0;                 // Очікуваний розмір кадру в байтах
    std::string m_lastError;                // Опис останньої помилки

    // Для контролю темпу виводу кадрів (Pacing)
    std::chrono::steady_clock::time_point m_lastFrameTime;
    std::chrono::nanoseconds m_frameDurationNs{0};
};

} // namespace EyeCam
