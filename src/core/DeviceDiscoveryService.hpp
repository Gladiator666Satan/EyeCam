#pragma once

#include <QObject>
#include <QTimer>
#include <QProcess>
#include <QVariantMap>

class AppState;

/**
 * @brief Сервіс авто-виявлення пристроїв через локальну мережу (mDNS/SSDP)
 * та моніторингу прямого підключення USB через періодичний ADB Polling.
 */
class DeviceDiscoveryService : public QObject {
    Q_OBJECT

public:
    explicit DeviceDiscoveryService(AppState *state, QObject *parent = nullptr);

public slots:
    void startDiscovery();
    void stopDiscovery();

private slots:
    void pollUsbAdb();
    void scanWifiNetwork();

private:
    AppState *m_appState;
    QTimer m_adbPollTimer;
    QTimer m_wifiScanTimer;
    QProcess *m_adbProcess;

    void parseAdbOutput(const QString &output);
};
