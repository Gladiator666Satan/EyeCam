#include "AppState.hpp"
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>

AppState::AppState(QObject *parent) : QObject(parent) {
    // Ініціалізація стандартних значень
#if defined(EYECAM_PLATFORM_WINDOWS)
    m_activeHwDecoderName = "D3D11VA / NVDEC (Win)";
#elif defined(EYECAM_PLATFORM_LINUX)
    m_activeHwDecoderName = "VA-API / NVDEC (Linux)";
#else
    m_activeHwDecoderName = "Апаратне декодування";
#endif
}

void AppState::setIsConnected(bool val) {
    if (m_isConnected != val) {
        m_isConnected = val;
        emit isConnectedChanged();
    }
}

void AppState::setActiveIp(const QString &ip) {
    if (m_activeIp != ip) {
        m_activeIp = ip;
        emit activeIpChanged();
    }
}

void AppState::setFlipH(bool val) {
    if (m_flipH != val) {
        m_flipH = val;
        emit flipHChanged();
    }
}

void AppState::setFlipV(bool val) {
    if (m_flipV != val) {
        m_flipV = val;
        emit flipVChanged();
    }
}

void AppState::setRotationAngle(int angle) {
    if (m_rotationAngle != angle) {
        m_rotationAngle = angle;
        emit rotationAngleChanged();
    }
}

void AppState::setShowGridOverlay(bool val) {
    if (m_showGridOverlay != val) {
        m_showGridOverlay = val;
        emit showGridOverlayChanged();
    }
}

void AppState::setIsProMode(bool val) {
    if (m_isProMode != val) {
        m_isProMode = val;
        emit isProModeChanged();

        QJsonObject json;
        json["cmd"] = "set_mode";
        json["mode"] = m_isProMode ? "pro" : "auto";
        emit cameraCommandDispatched(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
    }
}

void AppState::setExposureCompensation(double val) {
    if (qFuzzyCompare(m_exposureCompensation, val)) return;
    m_exposureCompensation = val;
    emit exposureCompensationChanged();

    QJsonObject json;
    json["cmd"] = "set_param";
    json["param"] = "ev";
    json["value"] = m_exposureCompensation;
    emit cameraCommandDispatched(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

void AppState::setIso(double val) {
    if (qFuzzyCompare(m_iso, val)) return;
    m_iso = val;
    emit isoChanged();

    QJsonObject json;
    json["action"] = "SetISO";
    json["auto"] = false;
    json["iso"] = static_cast<int>(m_iso);
    json["cmd"] = "set_param";
    json["param"] = "iso";
    json["value"] = m_iso;
    emit cameraCommandDispatched(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

void AppState::setShutterDenominator(double val) {
    if (qFuzzyCompare(m_shutterDenominator, val)) return;
    m_shutterDenominator = val;
    emit shutterDenominatorChanged();

    QJsonObject json;
    json["action"] = "SetShutter";
    json["auto"] = false;
    json["exposure_ns"] = static_cast<qint64>(1000000000.0 / m_shutterDenominator);
    json["cmd"] = "set_param";
    json["param"] = "shutter";
    json["value"] = 1.0 / m_shutterDenominator;
    emit cameraCommandDispatched(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

void AppState::setManualFocus(double val) {
    if (qFuzzyCompare(m_manualFocus, val)) return;
    m_manualFocus = val;
    emit manualFocusChanged();

    QJsonObject json;
    json["action"] = "SetFocus";
    json["mode"] = "MANUAL";
    json["distance_diopters"] = m_manualFocus;
    json["cmd"] = "set_param";
    json["param"] = "focus";
    json["value"] = m_manualFocus;
    emit cameraCommandDispatched(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

void AppState::setZoom(double val) {
    if (qFuzzyCompare(m_zoom, val)) return;
    m_zoom = val;
    emit zoomChanged();

    QJsonObject json;
    json["cmd"] = "set_param";
    json["param"] = "zoom";
    json["value"] = m_zoom;
    emit cameraCommandDispatched(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

void AppState::setWhiteBalanceKelvin(double val) {
    if (qFuzzyCompare(m_whiteBalanceKelvin, val)) return;
    m_whiteBalanceKelvin = val;
    emit whiteBalanceKelvinChanged();

    QJsonObject json;
    json["action"] = "SetWhiteBalance";
    json["mode"] = "MANUAL_KELVIN";
    json["kelvin"] = static_cast<int>(m_whiteBalanceKelvin);
    json["cmd"] = "set_param";
    json["param"] = "wb_kelvin";
    json["value"] = m_whiteBalanceKelvin;
    emit cameraCommandDispatched(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

void AppState::setTint(double val) {
    if (qFuzzyCompare(m_tint, val)) return;
    m_tint = val;
    emit tintChanged();

    QJsonObject json;
    json["cmd"] = "set_param";
    json["param"] = "tint";
    json["value"] = m_tint;
    emit cameraCommandDispatched(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

void AppState::setVirtualCameraActive(bool val) {
    if (m_virtualCameraActive != val) {
        m_virtualCameraActive = val;
        emit virtualCameraActiveChanged();
    }
}

void AppState::connectCamera(const QString &ip, int port) {
    qInfo() << "[AppState] Ініціалізація підключення до камери:" << ip << ":" << port;
    setActiveIp(ip);
    setIsConnected(true);
}

void AppState::disconnectCamera() {
    qInfo() << "[AppState] Відключення поточної камери:" << m_activeIp;
    setActiveIp("");
    setIsConnected(false);
    updateTelemetry(0, 0, 0, 0);
}

void AppState::resetCameraDefaults() {
    setIso(400.0);
    setShutterDenominator(120.0);
    setManualFocus(0.5);
    setZoom(1.0);
    setWhiteBalanceKelvin(5600.0);
    setTint(0.0);
    setExposureCompensation(0.0);
    setFlipH(false);
    setFlipV(false);
    setRotationAngle(0);
}

void AppState::updateTelemetry(double ping, double fps, double bitrate, double packetLoss) {
    if (!qFuzzyCompare(m_telemetryPing, ping)) {
        m_telemetryPing = ping;
        emit telemetryPingChanged();
    }
    if (!qFuzzyCompare(m_telemetryFps, fps)) {
        m_telemetryFps = fps;
        emit telemetryFpsChanged();
    }
    if (!qFuzzyCompare(m_telemetryBitrateMbps, bitrate)) {
        m_telemetryBitrateMbps = bitrate;
        emit telemetryBitrateMbpsChanged();
    }
    if (!qFuzzyCompare(m_telemetryPacketLoss, packetLoss)) {
        m_telemetryPacketLoss = packetLoss;
        emit telemetryPacketLossChanged();
    }
}

void AppState::updateUsbStatus(bool connected, const QString &name, const QString &mode) {
    if (m_usbConnected != connected) {
        m_usbConnected = connected;
        emit usbConnectedChanged();
    }
    if (m_usbDeviceName != name) {
        m_usbDeviceName = name;
        emit usbDeviceNameChanged();
    }
    if (m_usbTransportMode != mode) {
        m_usbTransportMode = mode;
        emit usbTransportModeChanged();
    }
}

void AppState::updateDiscoveredDevices(const QVariantList &devices) {
    m_discoveredDevicesModel = devices;
    emit discoveredDevicesModelChanged();
}

void AppState::setAdbPollingActive(bool active) {
    if (m_adbPollingActive != active) {
        m_adbPollingActive = active;
        emit adbPollingActiveChanged();
    }
}

void AppState::setActiveHwDecoderName(const QString &name) {
    if (m_activeHwDecoderName != name) {
        m_activeHwDecoderName = name;
        emit activeHwDecoderNameChanged();
    }
}
