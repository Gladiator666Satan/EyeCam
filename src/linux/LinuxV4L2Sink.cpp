#include "LinuxV4L2Sink.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <cerrno>
#include <cstring>
#include <thread>
#endif

#include <iostream>
#include <sstream>

namespace EyeCam {

LinuxV4L2Sink::LinuxV4L2Sink()
    : m_fd(-1), m_width(0), m_height(0), m_format(PixelFormat::YUY2), 
      m_fps(30), m_frameSize(0), m_frameDurationNs(0) {
}

LinuxV4L2Sink::~LinuxV4L2Sink() {
    Close();
}

LinuxV4L2Sink::LinuxV4L2Sink(LinuxV4L2Sink&& other) noexcept
    : m_fd(other.m_fd),
      m_devicePath(std::move(other.m_devicePath)),
      m_width(other.m_width),
      m_height(other.m_height),
      m_format(other.m_format),
      m_fps(other.m_fps),
      m_frameSize(other.m_frameSize),
      m_lastError(std::move(other.m_lastError)),
      m_lastFrameTime(other.m_lastFrameTime),
      m_frameDurationNs(other.m_frameDurationNs) {
    other.m_fd = -1;
}

LinuxV4L2Sink& LinuxV4L2Sink::operator=(LinuxV4L2Sink&& other) noexcept {
    if (this != &other) {
        Close();
        m_fd = other.m_fd;
        m_devicePath = std::move(other.m_devicePath);
        m_width = other.m_width;
        m_height = other.m_height;
        m_format = other.m_format;
        m_fps = other.m_fps;
        m_frameSize = other.m_frameSize;
        m_lastError = std::move(other.m_lastError);
        m_lastFrameTime = other.m_lastFrameTime;
        m_frameDurationNs = other.m_frameDurationNs;

        other.m_fd = -1;
    }
    return *this;
}

bool LinuxV4L2Sink::Open(const std::string& devicePath, uint32_t width, uint32_t height, 
                         PixelFormat format, uint32_t fps) {
#if !defined(__linux__)
    m_lastError = "LinuxV4L2Sink підтримується виключно на платформі Linux.";
    return false;
#else
    Close();

    // Відкриття дескриптора пристрою V4L2 у режимі читання/запису (O_RDWR)
    m_fd = open(devicePath.c_str(), O_RDWR);
    if (m_fd < 0) {
        std::ostringstream ss;
        ss << "Не вдалося відкрити пристрій " << devicePath << ": " << strerror(errno);
        m_lastError = ss.str();
        return false;
    }

    // Перевірка можливостей пристрою (Capabilities)
    if (!QueryCapabilities(m_fd)) {
        Close();
        return false;
    }

    // Застосування формату та роздільної здатності
    if (!ApplyFormat(m_fd, width, height, format)) {
        Close();
        return false;
    }

    // Встановлення бажаної частоти кадрів (FPS)
    ApplyFrameRate(m_fd, fps);

    m_devicePath = devicePath;
    m_width = width;
    m_height = height;
    m_format = format;
    m_fps = fps;
    m_frameSize = CalculateFrameSize(width, height, format);
    m_frameDurationNs = std::chrono::nanoseconds(1000000000ULL / fps);
    m_lastFrameTime = std::chrono::steady_clock::now();

    m_lastError.clear();
    return true;
#endif
}

void LinuxV4L2Sink::Close() {
#if defined(__linux__)
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
#endif
    m_devicePath.clear();
    m_width = 0;
    m_height = 0;
    m_frameSize = 0;
}

bool LinuxV4L2Sink::IsOpen() const {
    return m_fd >= 0;
}

bool LinuxV4L2Sink::SetFormat(uint32_t width, uint32_t height, PixelFormat format, uint32_t fps) {
    if (!IsOpen()) {
        m_lastError = "Пристрій не відкритий для динамічної зміни формату.";
        return false;
    }

#if defined(__linux__)
    // Динамічна зміна формату через виклик VIDIOC_S_FMT без перезапуску пристрою
    if (!ApplyFormat(m_fd, width, height, format)) {
        return false;
    }

    if (fps != m_fps && fps > 0) {
        ApplyFrameRate(m_fd, fps);
        m_fps = fps;
        m_frameDurationNs = std::chrono::nanoseconds(1000000000ULL / fps);
    }

    m_width = width;
    m_height = height;
    m_format = format;
    m_frameSize = CalculateFrameSize(width, height, format);
    return true;
#else
    return false;
#endif
}

bool LinuxV4L2Sink::WriteFrame(const uint8_t* data, size_t size) {
    if (!IsOpen()) {
        m_lastError = "Спроба запису в закритий пристрій.";
        return false;
    }

    if (data == nullptr || size != m_frameSize) {
        std::ostringstream ss;
        ss << "Невідповідність розміру кадру: передано " << size 
           << " байт, очікується " << m_frameSize << " байт.";
        m_lastError = ss.str();
        return false;
    }

#if defined(__linux__)
    // Запис сирого кадру в дескриптор v4l2loopback
    ssize_t written = write(m_fd, data, size);
    if (written < 0) {
        std::ostringstream ss;
        ss << "Помилка запису в V4L2 пристрій: " << strerror(errno);
        m_lastError = ss.str();
        return false;
    }

    if (static_cast<size_t>(written) != size) {
        std::ostringstream ss;
        ss << "Записано неповний кадр: " << written << " з " << size << " байт.";
        m_lastError = ss.str();
        return false;
    }

    return true;
#else
    return false;
#endif
}

bool LinuxV4L2Sink::WriteFrame(const VideoFrame& frame) {
    // Якщо роздільна здатність або формат змінилися в ході стрімінгу — здійснюємо динамічну адаптацію
    if (frame.resolution.width != m_width || 
        frame.resolution.height != m_height || 
        frame.format != m_format) {
        if (!SetFormat(frame.resolution.width, frame.resolution.height, frame.format, m_fps)) {
            return false;
        }
    }

    return WriteFrame(frame.data, frame.dataSize);
}

bool LinuxV4L2Sink::WriteFramePaced(const uint8_t* data, size_t size) {
    if (!WriteFrame(data, size)) {
        return false;
    }

    // Регулювання таймінгу виводу кадрів для стабільних 30 або 60 FPS
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - m_lastFrameTime;
    if (elapsed < m_frameDurationNs) {
        std::this_thread::sleep_for(m_frameDurationNs - elapsed);
    }
    m_lastFrameTime = std::chrono::steady_clock::now();
    return true;
}

bool LinuxV4L2Sink::QueryCapabilities(int fd) {
#if defined(__linux__)
    struct v4l2_capability cap;
    std::memset(&cap, 0, sizeof(cap));

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        std::ostringstream ss;
        ss << "Помилка виклику VIDIOC_QUERYCAP: " << strerror(errno);
        m_lastError = ss.str();
        return false;
    }

    uint32_t capabilities = cap.capabilities;
    if (capabilities & V4L2_CAP_DEVICE_CAPS) {
        capabilities = cap.device_caps;
    }

    // Пристрій для sink-модуля обов'язково повинен підтримувати V4L2_CAP_VIDEO_OUTPUT
    if (!(capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
        m_lastError = "Пристрій не підтримує запис відео (відсутній прапорець V4L2_CAP_VIDEO_OUTPUT). Перевірте параметри v4l2loopback.";
        return false;
    }

    return true;
#else
    (void)fd;
    return false;
#endif
}

bool LinuxV4L2Sink::ApplyFormat(int fd, uint32_t width, uint32_t height, PixelFormat format) {
#if defined(__linux__)
    struct v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));

    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = ToV4L2PixelFormat(format);
    fmt.fmt.pix.sizeimage = CalculateFrameSize(width, height, format);
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = CalculateStride(width, format);
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        std::ostringstream ss;
        ss << "Помилка конфігурації формату VIDIOC_S_FMT (" << width << "x" << height 
           << "): " << strerror(errno);
        m_lastError = ss.str();
        return false;
    }

    // Перевірка, чи драйвер підтвердив запрошену роздільну здатність
    if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height) {
        std::ostringstream ss;
        ss << "Драйвер змінив геометрію: запитано " << width << "x" << height 
           << ", встановлено " << fmt.fmt.pix.width << "x" << fmt.fmt.pix.height;
        m_lastError = ss.str();
        // Зауваження: деякі версії ядра можуть підганяти розмір під кратність
    }

    return true;
#else
    (void)fd; (void)width; (void)height; (void)format;
    return false;
#endif
}

bool LinuxV4L2Sink::ApplyFrameRate(int fd, uint32_t fps) {
#if defined(__linux__)
    struct v4l2_streamparm parm;
    std::memset(&parm, 0, sizeof(parm));

    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = fps;

    // Встановлення FPS (деякі реалізації v4l2loopback можуть ігнорувати або повертати EINVAL, що не є фатальним)
    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        // Логуємо як попередження, але не зупиняємо роботу
        return false;
    }
    return true;
#else
    (void)fd; (void)fps;
    return false;
#endif
}

uint32_t LinuxV4L2Sink::ToV4L2PixelFormat(PixelFormat format) const {
#if defined(__linux__)
    switch (format) {
        case PixelFormat::RGB24: return V4L2_PIX_FMT_RGB24;
        case PixelFormat::BGR24: return V4L2_PIX_FMT_BGR24;
        case PixelFormat::YUY2:  return V4L2_PIX_FMT_YUYV;
        case PixelFormat::NV12:  return V4L2_PIX_FMT_NV12;
        default:                 return V4L2_PIX_FMT_YUYV;
    }
#else
    (void)format;
    return 0;
#endif
}

} // namespace EyeCam
