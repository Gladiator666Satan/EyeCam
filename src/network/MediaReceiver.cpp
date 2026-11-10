#include "MediaReceiver.hpp"
#include <QDebug>

namespace EyeCam::Network {

MediaReceiver::MediaReceiver(QObject *parent)
    : QObject(parent), m_socket(new QUdpSocket(this)) {
    connect(m_socket, &QUdpSocket::readyRead, this, &MediaReceiver::onReadyRead);
}

MediaReceiver::~MediaReceiver() {
    stopListening();
}

bool MediaReceiver::startListening(quint16 port, const QHostAddress &address) {
    stopListening();

    // Збільшуємо розмір системного буфера прийому до 4 МБ для уникнення дропів на рівні сокета
    if (!m_socket->bind(address, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qWarning() << "[MediaReceiver] Не вдалося зв'язати UDP порт" << port;
        return false;
    }

    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
    qInfo() << "[MediaReceiver] Слухаємо UDP медіа-потік на" << address.toString() << ":" << port;
    return true;
}

void MediaReceiver::stopListening() {
    if (m_socket && m_socket->isOpen()) {
        m_socket->close();
    }
    resetCurrentFrame();
    m_expectedSeq = -1;
}

void MediaReceiver::onReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<qsizetype>(m_socket->pendingDatagramSize()));
        m_socket->readDatagram(datagram.data(), datagram.size());
        
        m_bytesReceivedInInterval += datagram.size();
        processPacket(datagram);
    }
}

void MediaReceiver::processPacket(const QByteArray &datagram) {
    if (datagram.size() < static_cast<qsizetype>(sizeof(MediaPacketHeader))) {
        return;
    }

    MediaPacketHeader header;
    std::memcpy(&header, datagram.constData(), sizeof(MediaPacketHeader));

    if (!header.isValid()) {
        return;
    }
    header.deserializeFromNet();

    // 1. Детектування втрати пакетів через перевірку послідовності Sequence Number
    if (m_expectedSeq >= 0) {
        if (header.sequence_num != static_cast<uint16_t>(m_expectedSeq)) {
            uint16_t gap = static_cast<uint16_t>(header.sequence_num - static_cast<uint16_t>(m_expectedSeq));
            m_packetsLostCounter += gap;
            m_isCurrentFrameCorrupt = true;

            qWarning() << "[MediaReceiver] [PACKET LOSS] Втрачено" << gap << "пакетів! Очікувався"
                       << m_expectedSeq << "отримано" << header.sequence_num;

            // Якщо втрачено фрагмент ключового кадру — негайно ініціюємо запит нового IDR
            if (header.isKeyframe()) {
                emit sigKeyframeRequested(header.frame_id, "keyframe_fragment_lost");
            }
        }
    }
    m_expectedSeq = (header.sequence_num + 1) & 0xFFFF;

    // 2. Якщо почався новий кадр, але попередній не закінчився або пошкоджений
    if (m_currentFrameId != 0 && header.frame_id != m_currentFrameId) {
        if (!m_isCurrentFrameCorrupt && m_frameFragments.size() == m_currentTotalFragments) {
            dispatchCurrentFrame();
        } else {
            // Zero-Retransmission: відкидаємо пошкоджений або неповний кадр
            m_framesDroppedCounter++;
            qDebug() << "[MediaReceiver] [ZERO-RETRANSMISSION] Кадр #" << m_currentFrameId
                     << "відкинуто через некомплектність (" << m_frameFragments.size()
                     << "/" << m_currentTotalFragments << ")";
        }
        resetCurrentFrame();
    }

    // 3. Збереження фрагмента
    m_currentFrameId = header.frame_id;
    m_currentTotalFragments = header.fragment_total;
    m_currentPtsUs = header.pts_us;
    m_currentIsKeyframe = header.isKeyframe();

    const char *payloadPtr = datagram.constData() + sizeof(MediaPacketHeader);
    QByteArray payloadChunk(payloadPtr, header.payload_len);
    m_frameFragments[header.fragment_index] = payloadChunk;

    // 4. Перевірка завершення кадру
    if (header.isEndOfFrame() && !m_isCurrentFrameCorrupt) {
        if (m_frameFragments.size() == m_currentTotalFragments) {
            dispatchCurrentFrame();
            resetCurrentFrame();
        }
    }
}

void MediaReceiver::dispatchCurrentFrame() {
    QByteArray fullFrame;
    for (uint16_t i = 0; i < m_currentTotalFragments; ++i) {
        fullFrame.append(m_frameFragments.value(i));
    }

    emit sigFrameReady(fullFrame, m_currentPtsUs, m_currentIsKeyframe);
}

void MediaReceiver::resetCurrentFrame() {
    m_frameFragments.clear();
    m_isCurrentFrameCorrupt = false;
    m_currentFrameId = 0;
    m_currentTotalFragments = 0;
}

} // namespace EyeCam::Network
