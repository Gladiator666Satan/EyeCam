#pragma once

#include <QQuickItem>
#include <QSGTextureProvider>
#include <QSGSimpleTextureNode>
#include <QMutex>

/**
 * @brief Кастомний елемент QQuickItem для високоефективного рендерингу відеокадрів
 * у дереві сцени Qt Quick Scene Graph (RHI Direct3D 11 / OpenGL / Vulkan).
 */
class VideoTextureItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit VideoTextureItem(QQuickItem *parent = nullptr);
    ~VideoTextureItem() override = default;

    bool isTextureProvider() const override { return true; }

public slots:
    void onNewFrame(const uint8_t *frameData, int width, int height, size_t size);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

private:
    QMutex m_frameMutex;
    QByteArray m_currentFrameData;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    bool m_hasNewFrame = false;
};
