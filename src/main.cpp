#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/AppState.hpp"
#include "core/TelemetryService.hpp"
#include "core/DeviceDiscoveryService.hpp"
#include "pipeline/VideoPipeline.hpp"
#include "pipeline/VideoTextureProvider.hpp"
#include "network/ControlClient.hpp"
#include "network/MediaReceiver.hpp"

int main(int argc, char *argv[]) {
    // Увімкнення рендерингу Direct3D 11 / RHI для високої швидкодії та нульового копіювання
#if defined(EYECAM_PLATFORM_WINDOWS)
    qputenv("QSG_RHI_BACKEND", "d3d11");
#elif defined(EYECAM_PLATFORM_LINUX)
    qputenv("QSG_RHI_BACKEND", "opengl");
#endif

    // Налаштування стилю інтерфейсу
    QQuickStyle::setStyle("Basic");

    QGuiApplication app(argc, argv);
    app.setOrganizationName("EyeCam");
    app.setOrganizationDomain("eyecam.app");
    app.setApplicationName("EyeCam Studio");

    // 1. Створення центрального реактивного стану та фонових служб
    auto *appState = new AppState(&app);
    auto *telemetry = new TelemetryService(appState, &app);
    auto *discovery = new DeviceDiscoveryService(appState, &app);
    auto *pipeline = new VideoPipeline(appState, telemetry, &app);
    auto *controlClient = new EyeCam::Network::ControlClient(&app);
    auto *mediaReceiver = new EyeCam::Network::MediaReceiver(&app);

    // 2. Реєстрація кастомних C++ типів для QML
    qmlRegisterType<VideoTextureItem>("EyeCam.Pipeline", 1, 0, "EyeCamVideoItem");

    // 3. Зв'язування дій UI, мережі та відеотракту
    QObject::connect(appState, &AppState::activeIpChanged, [appState, pipeline, controlClient, mediaReceiver]() {
        if (!appState->activeIp().isEmpty()) {
            pipeline->startPipeline(appState->activeIp(), 8554);
            controlClient->connectToDevice(appState->activeIp(), 8554);
            mediaReceiver->startListening(8555);
        } else {
            pipeline->stopPipeline();
            controlClient->disconnectFromDevice();
            mediaReceiver->stopListening();
        }
    });

    QObject::connect(appState, &AppState::cameraCommandDispatched, [controlClient](const QString &cmdJson) {
        auto doc = QJsonDocument::fromJson(cmdJson.toUtf8());
        if (doc.isObject()) {
            auto obj = doc.object();
            QString action = obj["action"].toString();
            if (action == "SetISO") {
                controlClient->setISO(obj["auto"].toBool(), obj["iso"].toInt());
            } else if (action == "SetShutter") {
                controlClient->setShutter(obj["auto"].toBool(), obj["exposure_ns"].toVariant().toLongLong());
            } else if (action == "SetFocus") {
                controlClient->setFocus(obj["mode"].toString(), obj["distance_diopters"].toDouble());
            } else if (action == "SetWhiteBalance") {
                controlClient->setWhiteBalance(obj["mode"].toString(), obj["kelvin"].toInt());
            } else if (action == "SetCameraModule") {
                controlClient->setCameraModule(obj["module"].toString());
            }
        }
    });

    QObject::connect(mediaReceiver, &EyeCam::Network::MediaReceiver::sigKeyframeRequested,
                     controlClient, &EyeCam::Network::ControlClient::requestKeyframe);

    QObject::connect(controlClient, &EyeCam::Network::ControlClient::sigLatencyMetricsUpdated,
                     [telemetry](double rttMs, double jitterMs, double clockOffsetMs) {
        Q_UNUSED(jitterMs);
        Q_UNUSED(clockOffsetMs);
        telemetry->registerPingLatency(rttMs);
    });

    // 4. Запуск фонових сервісів моніторингу
    telemetry->startMonitoring();
    discovery->startDiscovery();

    // 5. Ініціалізація рушія QML
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("appState", appState);

    const QUrl url(u"qrc:/EyeCam/UI/src/ui/qml/Main.qml"_qs);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(url);

    // Якщо запуск з ресурсу не вдався (наприклад при прямому запуску без qrc), завантажуємо локальний файл
    if (engine.rootObjects().isEmpty()) {
        engine.load(QUrl::fromLocalFile("src/ui/qml/Main.qml"));
    }

    qInfo() << "[Main] EyeCam Studio успішно запущено в режимі ультра-низької затримки.";
    return app.exec();
}
