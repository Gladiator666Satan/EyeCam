#include "VideoTextureProvider.hpp"
#include <QQuickWindow>
#include <QSGTexture>
#include <QImage>

VideoTextureItem::VideoTextureItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

void VideoTextureItem::onNewFrame(const uint8_t *frameData, int width, int height, size_t size) {
    {
        QMutexLocker locker(&m_frameMutex);
        if (m_currentFrameData.size() != static_cast<qsizetype>(size)) {
            m_currentFrameData.resize(static_cast<qsizetype>(size));
        }
        std::memcpy(m_currentFrameData.data(), frameData, size);
        m_frameWidth = width;
        m_frameHeight = height;
        m_hasNewFrame = true;
    }

    // Запит оновлення сцени Qt Quick
    update();
}

QSGNode *VideoTextureItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
    }

    QQuickWindow *win = window();
    if (!win) return node;

    QByteArray frameCopy;
    int w = 0, h = 0;
    bool hasFrame = false;

    {
        QMutexLocker locker(&m_frameMutex);
        if (m_hasNewFrame && m_frameWidth > 0 && m_frameHeight > 0) {
            frameCopy = m_currentFrameData;
            w = m_frameWidth;
            h = m_frameHeight;
            m_hasNewFrame = false;
            hasFrame = true;
        }
    }

    if (hasFrame) {
        // У повному апаратному пайплайні текстура передається як D3D11 / OpenGL Shared Resource
        // Тут виконується безпечне перетворення буфера у текстуру Qt RHI
        QImage img(reinterpret_cast<const uchar *>(frameCopy.constData()), w, h, QImage::Format_Grayscale8);
        QSGTexture *texture = win->createTextureFromImage(img, QQuickWindow::TextureIsOpaque);
        node->setTexture(texture);
        node->setOwnsTexture(true);
    }

    node->setRect(boundingRect());
    node->setFiltering(QSGTexture::Linear);
    return node;
}
