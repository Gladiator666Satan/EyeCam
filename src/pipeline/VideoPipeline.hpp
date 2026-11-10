#pragma once

#include <QObject>
#include <QThread>
#include <QString>
#include <memory>
#include <atomic>

class AppState;
class TelemetryService;
class VirtualCameraSink;

/**
 * @brief Високопродуктивний апаратно-прискорений відеотракт EyeCam
 * 
 * Відповідає за:
 * 1. Апаратне декодування H.264/H.265 (Windows: D3D11VA / NVDEC, Linux: VA-API).
 * 2. Одночасну паралельну роздачу кадрів (Tee / Branching):
 *    - Гілка А: Локальний UI Viewport для миттєвого відображення без затримок.
 *    - Гілка Б: Драйвер віртуальної камери Stage 3 через VirtualCameraSink.
 */
class VideoPipeline : public QObject {
    Q_OBJECT

public:
    explicit VideoPipeline(AppState *state, TelemetryService *telemetry, QObject *parent = nullptr);
    ~VideoPipeline() override;

    bool startPipeline(const QString &sourceUri, int port);
    void stopPipeline();

    bool isRunning() const { return m_isRunning; }

signals:
    // Сигнал готовності нового кадру для локального рендерера
    void frameReady(const uint8_t *frameData, int width, int height, size_t size);
    void pipelineError(const QString &errorMsg);

private slots:
    void processWorkerLoop();

private:
    AppState *m_appState;
    TelemetryService *m_telemetry;
    std::unique_ptr<VirtualCameraSink> m_vcamSink;

    std::unique_ptr<QThread> m_workerThread;
    std::atomic<bool> m_isRunning{false};
    std::atomic<bool> m_shouldStop{false};

    QString m_sourceUri;
    int m_port = 8554;
    int m_videoWidth = 3840;
    int m_videoHeight = 2160;
    int m_targetFps = 60;

    void initializeHardwareDecoder();
    void dispatchDecodedFrame(const uint8_t *data, size_t size, uint64_t ptsNs);
};
