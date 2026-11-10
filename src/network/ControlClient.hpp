#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonObject>
#include "NetworkProtocol.hpp"

namespace EyeCam::Network {

/**
 * @brief Клієнт двостороннього контрольного каналу (TCP з TCP_NODELAY).
 * Забезпечує:
 * - Відправку команд керування камерою (ISO, Shutter, Focus, WB, модуль).
 * - Прецизійне вимірювання затримки (NTP Ping/Pong RTT).
 * - Автоматичний Keep-Alive.
 * - Терміновий запит IDR при втратах у Media-каналі.
 */
class ControlClient : public QObject {
    Q_OBJECT

public:
    explicit ControlClient(QObject *parent = nullptr);
    ~ControlClient() override;

    void connectToDevice(const QString &host = "127.0.0.1", quint16 port = 8554);
    void disconnectFromDevice();
    bool isConnected() const;

public slots:
    // Команди керування камерою
    void setCameraModule(const QString &moduleName);
    void setISO(bool autoIso, int isoValue = 100);
    void setShutter(bool autoExposure, qint64 exposureTimeNs = 20000000);
    void setFocus(const QString &mode, double distanceDiopters = 0.0);
    void setWhiteBalance(const QString &mode, int colorTempK = 5500);
    void requestKeyframe(const QString &reason = "packet_loss", uint32_t lastFrameId = 0);

signals:
    void sigConnected();
    void sigDisconnected();
    void sigLatencyMetricsUpdated(double rttMs, double jitterMs, double clockOffsetMs);
    void sigTelemetryReceived(const QJsonObject &telemetryState);
    void sigAckReceived(const QString &action, bool success, const QString &errorMessage);

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onReadyRead();
    void sendPing();

private:
    QTcpSocket *m_socket{nullptr};
    QTimer m_pingTimer;

    // Стан читання кадру
    QByteArray m_readBuffer;
    bool m_readingHeader{true};
    ControlFrameHeader m_currentHeader{};

    // Метрики
    double m_smoothedRttMs{0.0};
    double m_jitterMs{0.0};

    void sendControlFrame(ControlMessageType type, const QByteArray &payload);
    void sendJsonCommand(const QJsonObject &jsonObj);
    void handleIncomingFrame(ControlMessageType type, const QByteArray &payload, uint64_t rxTimeUs);
};

} // namespace EyeCam::Network
