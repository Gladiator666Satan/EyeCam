#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QByteArray>
#include <QMap>
#include <QHostAddress>
#include "NetworkProtocol.hpp"

namespace EyeCam::Network {

/**
 * @brief Високопродуктивний асинхронний UDP приймач медіа-потоку.
 * Реалізує:
 * - Перевірку цілісності пакетів за 24-байтним заголовком MediaPacketHeader.
 * - Детектування пропуску Sequence Number.
 * - Політику нульової ретрансмісії (Zero-Retransmission Policy).
 * - Відправку сигналу на запит термінового IDR кадру при втраті даних.
 */
class MediaReceiver : public QObject {
    Q_OBJECT

public:
    explicit MediaReceiver(QObject *parent = nullptr);
    ~MediaReceiver() override;

    bool startListening(quint16 port = 8555, const QHostAddress &address = QHostAddress::AnyIPv4);
    void stopListening();

signals:
    // Сигнал готовності повного зібраного кадру до декодування
    void sigFrameReady(const QByteArray &frameData, uint64_t ptsUs, bool isKeyframe);
    // Сигнал необхідності відправки IDR запиту через Control Plane
    void sigKeyframeRequested(uint32_t lastFrameId, const QString &reason);
    // Метрики медіа-каналу
    void sigMediaMetricsUpdated(uint32_t bitrateKbps, uint32_t packetsLost, uint32_t framesDropped);

private slots:
    void onReadyRead();

private:
    QUdpSocket *m_socket{nullptr};
    
    int32_t m_expectedSeq{-1};
    uint32_t m_currentFrameId{0};
    uint16_t m_currentTotalFragments{0};
    uint64_t m_currentPtsUs{0};
    bool m_currentIsKeyframe{false};
    bool m_isCurrentFrameCorrupt{false};

    // Буфер фрагментів поточного кадру: index -> payload
    QMap<uint16_t, QByteArray> m_frameFragments;

    // Статистика
    uint32_t m_packetsLostCounter{0};
    uint32_t m_framesDroppedCounter{0};
    uint64_t m_bytesReceivedInInterval{0};

    void processPacket(const QByteArray &datagram);
    void dispatchCurrentFrame();
    void resetCurrentFrame();
};

} // namespace EyeCam::Network
