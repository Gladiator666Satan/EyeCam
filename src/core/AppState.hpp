#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QTimer>

/**
 * @brief Центральний реактивний стан додатку EyeCam (C++20 / Qt 6)
 * 
 * Керує зв'язком між UI (QML), фоновими службами (ADB, mDNS, телеметрія)
 * та апаратним відеотрактом (GStreamer / FFmpeg / Virtual Camera Sink).
 */
class AppState : public QObject {
    Q_OBJECT

    // --- Стан підключення та пристроїв ---
    Q_PROPERTY(bool isConnected READ isConnected WRITE setIsConnected NOTIFY isConnectedChanged)
    Q_PROPERTY(QString activeIp READ activeIp WRITE setActiveIp NOTIFY activeIpChanged)
    Q_PROPERTY(bool usbConnected READ usbConnected NOTIFY usbConnectedChanged)
    Q_PROPERTY(QString usbDeviceName READ usbDeviceName NOTIFY usbDeviceNameChanged)
    Q_PROPERTY(QString usbTransportMode READ usbTransportMode NOTIFY usbTransportModeChanged)
    Q_PROPERTY(bool adbPollingActive READ adbPollingActive NOTIFY adbPollingActiveChanged)
    Q_PROPERTY(QVariantList discoveredDevicesModel READ discoveredDevicesModel NOTIFY discoveredDevicesModelChanged)

    // --- Телеметрія низької затримки ---
    Q_PROPERTY(double telemetryPing READ telemetryPing NOTIFY telemetryPingChanged)
    Q_PROPERTY(double telemetryFps READ telemetryFps NOTIFY telemetryFpsChanged)
    Q_PROPERTY(double telemetryBitrateMbps READ telemetryBitrateMbps NOTIFY telemetryBitrateMbpsChanged)
    Q_PROPERTY(double telemetryPacketLoss READ telemetryPacketLoss NOTIFY telemetryPacketLossChanged)

    // --- Керування орієнтацією та геометрією ---
    Q_PROPERTY(bool flipH READ flipH WRITE setFlipH NOTIFY flipHChanged)
    Q_PROPERTY(bool flipV READ flipV WRITE setFlipV NOTIFY flipVChanged)
    Q_PROPERTY(int rotationAngle READ rotationAngle WRITE setRotationAngle NOTIFY rotationAngleChanged)
    Q_PROPERTY(bool showGridOverlay READ showGridOverlay WRITE setShowGridOverlay NOTIFY showGridOverlayChanged)

    // --- Режим роботи камери (Auto / Pro) та параметри ---
    Q_PROPERTY(bool isProMode READ isProMode WRITE setIsProMode NOTIFY isProModeChanged)
    Q_PROPERTY(double exposureCompensation READ exposureCompensation WRITE setExposureCompensation NOTIFY exposureCompensationChanged)
    Q_PROPERTY(double iso READ iso WRITE setIso NOTIFY isoChanged)
    Q_PROPERTY(double shutterDenominator READ shutterDenominator WRITE setShutterDenominator NOTIFY shutterDenominatorChanged)
    Q_PROPERTY(double manualFocus READ manualFocus WRITE setManualFocus NOTIFY manualFocusChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(double whiteBalanceKelvin READ whiteBalanceKelvin WRITE setWhiteBalanceKelvin NOTIFY whiteBalanceKelvinChanged)
    Q_PROPERTY(double tint READ tint WRITE setTint NOTIFY tintChanged)

    // --- Відеопайплайн та віртуальна камера ---
    Q_PROPERTY(QString activeHwDecoderName READ activeHwDecoderName NOTIFY activeHwDecoderNameChanged)
    Q_PROPERTY(bool virtualCameraActive READ virtualCameraActive WRITE setVirtualCameraActive NOTIFY virtualCameraActiveChanged)

public:
    explicit AppState(QObject *parent = nullptr);

    // Геттери підключення
    bool isConnected() const { return m_isConnected; }
    QString activeIp() const { return m_activeIp; }
    bool usbConnected() const { return m_usbConnected; }
    QString usbDeviceName() const { return m_usbDeviceName; }
    QString usbTransportMode() const { return m_usbTransportMode; }
    bool adbPollingActive() const { return m_adbPollingActive; }
    QVariantList discoveredDevicesModel() const { return m_discoveredDevicesModel; }

    // Геттери телеметрії
    double telemetryPing() const { return m_telemetryPing; }
    double telemetryFps() const { return m_telemetryFps; }
    double telemetryBitrateMbps() const { return m_telemetryBitrateMbps; }
    double telemetryPacketLoss() const { return m_telemetryPacketLoss; }

    // Геттери геометрії
    bool flipH() const { return m_flipH; }
    bool flipV() const { return m_flipV; }
    int rotationAngle() const { return m_rotationAngle; }
    bool showGridOverlay() const { return m_showGridOverlay; }

    // Геттери камери
    bool isProMode() const { return m_isProMode; }
    double exposureCompensation() const { return m_exposureCompensation; }
    double iso() const { return m_iso; }
    double shutterDenominator() const { return m_shutterDenominator; }
    double manualFocus() const { return m_manualFocus; }
    double zoom() const { return m_zoom; }
    double whiteBalanceKelvin() const { return m_whiteBalanceKelvin; }
    double tint() const { return m_tint; }

    // Геттери пайплайну
    QString activeHwDecoderName() const { return m_activeHwDecoderName; }
    bool virtualCameraActive() const { return m_virtualCameraActive; }

public slots:
    // Сеттери та методи UI
    void setIsConnected(bool val);
    void setActiveIp(const QString &ip);
    void setFlipH(bool val);
    void setFlipV(bool val);
    void setRotationAngle(int angle);
    void setShowGridOverlay(bool val);

    void setIsProMode(bool val);
    void setExposureCompensation(double val);
    void setIso(double val);
    void setShutterDenominator(double val);
    void setManualFocus(double val);
    void setZoom(double val);
    void setWhiteBalanceKelvin(double val);
    void setTint(double val);

    void setVirtualCameraActive(bool val);

    // Дії користувача
    void connectCamera(const QString &ip, int port);
    void disconnectCamera();
    void resetCameraDefaults();

    // Оновлення стану від внутрішніх сервісів
    void updateTelemetry(double ping, double fps, double bitrate, double packetLoss);
    void updateUsbStatus(bool connected, const QString &name, const QString &mode);
    void updateDiscoveredDevices(const QVariantList &devices);
    void setAdbPollingActive(bool active);
    void setActiveHwDecoderName(const QString &name);

signals:
    void isConnectedChanged();
    void activeIpChanged();
    void usbConnectedChanged();
    void usbDeviceNameChanged();
    void usbTransportModeChanged();
    void adbPollingActiveChanged();
    void discoveredDevicesModelChanged();

    void telemetryPingChanged();
    void telemetryFpsChanged();
    void telemetryBitrateMbpsChanged();
    void telemetryPacketLossChanged();

    void flipHChanged();
    void flipVChanged();
    void rotationAngleChanged();
    void showGridOverlayChanged();

    void isProModeChanged();
    void exposureCompensationChanged();
    void isoChanged();
    void shutterDenominatorChanged();
    void manualFocusChanged();
    void zoomChanged();
    void whiteBalanceKelvinChanged();
    void tintChanged();

    void activeHwDecoderNameChanged();
    void virtualCameraActiveChanged();

    // Сигнал передачі команд у віддалений пристрій/телефон
    void cameraCommandDispatched(const QString &commandJson);

private:
    bool m_isConnected = false;
    QString m_activeIp = "";
    bool m_usbConnected = false;
    QString m_usbDeviceName = "Немає пристрою";
    QString m_usbTransportMode = "Очікування";
    bool m_adbPollingActive = true;
    QVariantList m_discoveredDevicesModel;

    double m_telemetryPing = 0.0;
    double m_telemetryFps = 0.0;
    double m_telemetryBitrateMbps = 0.0;
    double m_telemetryPacketLoss = 0.0;

    bool m_flipH = false;
    bool m_flipV = false;
    int m_rotationAngle = 0;
    bool m_showGridOverlay = false;

    bool m_isProMode = false;
    double m_exposureCompensation = 0.0;
    double m_iso = 400.0;
    double m_shutterDenominator = 120.0; // 1/120s
    double m_manualFocus = 0.5;
    double m_zoom = 1.0;
    double m_whiteBalanceKelvin = 5600.0;
    double m_tint = 0.0;

    QString m_activeHwDecoderName = "D3D11VA / NVDEC";
    bool m_virtualCameraActive = true;
};
