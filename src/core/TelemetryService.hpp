#pragma once

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>

class AppState;

/**
 * @brief Сервіс розрахунку та згладжування метрик телеметрії реального часу
 * (RTT, FPS, Бітрейт, Втрата пакетів) з низькими накладними витратами.
 */
class TelemetryService : public QObject {
    Q_OBJECT

public:
    explicit TelemetryService(AppState *state, QObject *parent = nullptr);

    // Виклики з високошвидкісного потоку обробки кадрів
    void registerReceivedPacket(size_t bytes);
    void registerDecodedFrame();
    void recordRttSample(double rttMs);
    void recordPacketLossSample(double lossPercent);

public slots:
    void startMonitoring();
    void stopMonitoring();

private slots:
    void onMetricsTick();

private:
    AppState *m_appState;
    QTimer m_tickTimer;
    QElapsedTimer m_elapsedTimer;

    // Атомарні лічильники для потокобезпечного збору без м'ютексів
    std::atomic<uint64_t> m_bytesReceived{0};
    std::atomic<uint32_t> m_framesDecoded{0};
    std::atomic<double> m_latestRttMs{8.5};
    std::atomic<double> m_latestPacketLoss{0.0};

    // Згладжені значення (Exponential Moving Average)
    double m_smoothedFps = 60.0;
    double m_smoothedBitrateMbps = 28.5;
    double m_smoothedRttMs = 9.2;
    double m_smoothedPacketLoss = 0.01;

    qint64 m_lastTickTimestamp = 0;
};
