#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <memory>
#include <vector>

int main(int argc, char **argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const auto args = app.arguments();
    if (args.size() != 3) return 2;
    std::unique_ptr<QQmlEngine> engine;
    std::vector<std::unique_ptr<QObject>> roots;
    auto create = [&] {
        engine = std::make_unique<QQmlEngine>();
        engine->addImportPath(args.at(1));
        QQmlComponent component(engine.get());
        component.setData(R"(
            import QtQuick
            import org.kde.videowallpaper 1.0
            Window {
                id: root
                required property string videoSource
                width: 320; height: 180; visible: true
                title: "Packaged wallpaper QML test"
                MpvItem {
                    anchors.fill: parent
                    source: root.videoSource
                    onStatusChanged: console.info("packaged plugin", status, hardwareDecoder, errorString)
                }
            }
        )", QUrl());
        for (int n = 0; n < 2; ++n) {
            auto *object = component.createWithInitialProperties({{"videoSource", args.at(2)}});
            if (!object) qFatal("QML plugin import failed: %s", qPrintable(component.errorString()));
            roots.emplace_back(object);
        }
    };
    create();
    QTimer::singleShot(2000, &app, [&] {
        roots.clear();
        engine.reset();
        // RuntimeService and outstanding worker retirements must survive the
        // destruction of the QML engine and its plugin instance.
        create();
    });
    QTimer::singleShot(4000, &app, &QCoreApplication::quit);
    return app.exec();
}
