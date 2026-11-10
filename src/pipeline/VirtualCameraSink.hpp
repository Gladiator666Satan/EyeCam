#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include <cstdint>

/**
 * @brief Міжпроцесний міст (IPC) для передачі декодованих кадрів у драйвер віртуальної камери Stage 3
 * 
 * Використовує Shared Memory (Windows: CreateFileMapping / Linux: shm_open)
 * з потрійною буферизацією (Triple Buffering) та безблокувальною синхронізацією
 * для гарантії субмілісекундної передачі (Zero IPC Latency).
 */
class VirtualCameraSink : public QObject {
    Q_OBJECT

public:
    explicit VirtualCameraSink(QObject *parent = nullptr);
    ~VirtualCameraSink() override;

    bool initialize(int width, int height, int fps);
    void shutdown();

    /**
     * @brief Запис розпакованого кадру у спільну пам'ять для драйвера віртуальної веб-камери
     * @param data Вказівник на NV12 або RGBA дані кадру
     * @param size Розмір даних у байтах
     * @param timestampNs Часова мітка презентації кадру (PTS)
     */
    void writeFrame(const uint8_t *data, size_t size, uint64_t timestampNs);

    bool isActive() const { return m_initialized; }

private:
    bool m_initialized = false;
    int m_width = 3840;
    int m_height = 2160;
    int m_fps = 60;
    size_t m_frameSize = 0;

    struct ShmHeader;
    ShmHeader *m_shmHeader = nullptr;
    void *m_shmHandle = nullptr;
    void *m_shmMemory = nullptr;
    void *m_frameEvent = nullptr; // Windows Event або POSIX sem_t
};
