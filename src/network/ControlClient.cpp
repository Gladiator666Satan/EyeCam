#include "ControlClient.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

namespace EyeCam::Network {

ControlClient::ControlClient(QObject *parent)
    : QObject(parent), m_socket(new QTcpSocket(this)) {
    
    connect(m_socket, &QTcpSocket::connected, this, &ControlClient::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &ControlClient::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &ControlClient::onReadyRead);

    connect(&m_pingTimer, &QTimer::timeout, this, &ControlClient::sendPing);
}

ControlClient::~ControlClient() {
    disconnectFromDevice();
}

void ControlClient::connectToDevice(const QString &host, quint16 port) {
    disconnectFromDevice();

    qInfo() << "[ControlClient] Підключення до камери:" << host << ":" << port;
    m_socket->connectToHost(host, port);
}

void ControlClient::disconnectFromDevice() {
    m_pingTimer.stop();
    if (m_socket && (m_socket->isOpen() || m_socket->state() != QAbstractSocket::UnconnectedState)) {
        m_socket->disconnectFromHost();
    }
    m_readBuffer.clear();
    m_readingHeader = true;
}

bool ControlClient::isConnected() const {
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

void ControlClient::onSocketConnected() {
    // Вмикаємо TCP_NODELAY (LowDelayOption) для негайної передачі пакетів без буферизації Нейгла
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

    qInfo() << "[ControlClient] Успішно підключено до контрольного каналу камери!";
    m_pingTimer.start(500); // Відправка пінгу двічі на секунду
    emit sigConnected();
}

void ControlClient::onSocketDisconnected() {
    m_pingTimer.stop();
    qWarning() << "[ControlClient] З'єднання з камерою втрачено.";
    emit sigDisconnected();
}

void ControlClient::sendPing() {
    if (!isConnected()) return;

    uint64_t t1 = getMonotonicTimeUs();
    QJsonObject obj;
    obj["t1_us"] = static_cast<qint64>(t1);

    QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    sendControlFrame(ControlMessageType::FramePing, payload);
}

void ControlClient::sendControlFrame(ControlMessageType type, const QByteArray &payload) {
    if (!isConnected()) return;

    ControlFrameHeader header;
    header.msg_type = static_cast<uint16_t>(type);
    header.payload_len = static_cast<uint32_t>(payload.size());
    header.serializeToNet();

    QByteArray packet;
    packet.resize(sizeof(ControlFrameHeader) + payload.size());
    std::memcpy(packet.data(), &header, sizeof(ControlFrameHeader));
    std::memcpy(packet.data() + sizeof(ControlFrameHeader), payload.constData(), payload.size());

    m_socket->write(packet);
    m_socket->flush();
}

void ControlClient::sendJsonCommand(const QJsonObject &jsonObj) {
    QByteArray payload = QJsonDocument(jsonObj).toJson(QJsonDocument::Compact);
    sendControlFrame(ControlMessageType::JsonCommand, payload);
}

void ControlClient::setCameraModule(const QString &moduleName) {
    QJsonObject obj;
    obj["action"] = "SetCameraModule";
    obj["module"] = moduleName;
    sendJsonCommand(obj);
}

void ControlClient::setISO(bool autoIso, int isoValue) {
    QJsonObject obj;
    obj["action"] = "SetISO";
    obj["auto"] = autoIso;
    obj["iso"] = isoValue;
    sendJsonCommand(obj);
}

void ControlClient::setShutter(bool autoExposure, qint64 exposureTimeNs) {
    QJsonObject obj;
    obj["action"] = "SetShutter";
    obj["auto"] = autoExposure;
    obj["exposure_ns"] = exposureTimeNs;
    sendJsonCommand(obj);
}

void ControlClient::setFocus(const QString &mode, double distanceDiopters) {
    QJsonObject obj;
    obj["action"] = "SetFocus";
    obj["mode"] = mode;
    obj["distance_diopters"] = distanceDiopters;
    sendJsonCommand(obj);
}

void ControlClient::setWhiteBalance(const QString &mode, int colorTempK) {
    QJsonObject obj;
    obj["action"] = "SetWhiteBalance";
    obj["mode"] = mode;
    obj["kelvin"] = colorTempK;
    sendJsonCommand(obj);
}

void ControlClient::requestKeyframe(const QString &reason, uint32_t lastFrameId) {
    QJsonObject obj;
    obj["reason"] = reason;
    obj["last_received_frame_id"] = static_cast<qint64>(lastFrameId);

    QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    sendControlFrame(ControlMessageType::KeyframeRequest, payload);
}

void ControlClient::onReadyRead() {
    m_readBuffer.append(m_socket->readAll());
    uint64_t rxTimeUs = getMonotonicTimeUs();

    while (true) {
        if (m_readingHeader) {
            if (m_readBuffer.size() < static_cast<qsizetype>(sizeof(ControlFrameHeader))) {
                break; // Чекаємо повного заголовка
            }

            std::memcpy(&m_currentHeader, m_readBuffer.constData(), sizeof(ControlFrameHeader));
            if (!m_currentHeader.isValid()) {
                qCritical() << "[ControlClient] Невірний магічний байт у контрольному кадрі! Розриваємо зв'язок.";
                disconnectFromDevice();
                return;
            }
            m_currentHeader.deserializeFromNet();

            m_readBuffer.remove(0, sizeof(ControlFrameHeader));
            m_readingHeader = false;
        }

        if (!m_readingHeader) {
            if (m_readBuffer.size() < static_cast<qsizetype>(m_currentHeader.payload_len)) {
                break; // Чекаємо повного тіла повідомлення
            }

            QByteArray payload = m_readBuffer.left(m_currentHeader.payload_len);
            m_readBuffer.remove(0, m_currentHeader.payload_len);
            m_readingHeader = true;

            handleIncomingFrame(static_cast<ControlMessageType>(m_currentHeader.msg_type), payload, rxTimeUs);
        }
    }
}

void ControlClient::handleIncomingFrame(ControlMessageType type, const QByteArray &payload, uint64_t rxTimeUs) {
    if (type == ControlMessageType::FramePong) {
        auto doc = QJsonDocument::fromJson(payload);
        if (doc.isObject()) {
            auto obj = doc.object();
            uint64_t t1 = obj["t1_us"].toVariant().toULongLong();
            uint64_t t2 = obj["t2_us"].toVariant().toULongLong();
            uint64_t t3 = obj["t3_us"].toVariant().toULongLong();
            uint64_t t4 = rxTimeUs;

            auto [rttMs, offsetMs] = calculateNtpMetrics(t1, t2, t3, t4);

            if (m_smoothedRttMs == 0.0) {
                m_smoothedRttMs = rttMs;
                m_jitterMs = 0.0;
            } else {
                double diff = std::abs(rttMs - m_smoothedRttMs);
                m_jitterMs += (diff - m_jitterMs) * 0.125;
                m_smoothedRttMs += (rttMs - m_smoothedRttMs) * 0.125;
            }

            emit sigLatencyMetricsUpdated(rttMs, m_jitterMs, offsetMs);
        }
    } else if (type == ControlMessageType::JsonCommand) {
        auto doc = QJsonDocument::fromJson(payload);
        if (doc.isObject()) {
            auto obj = doc.object();
            QString msgType = obj["type"].toString();

            if (msgType == "ack") {
                emit sigAckReceived(obj["action"].toString(), obj["success"].toBool(), obj["error"].toString());
            } else if (msgType == "device_state") {
                emit sigTelemetryReceived(obj["state"].toObject());
            }
        }
    }
}

} // namespace EyeCam::Network
