#include "mpvitem.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QTimer>
#include <QSocketNotifier>
#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <csignal>
#include <sys/signalfd.h>
#include <unistd.h>
#include <memory>
#include <vector>

// A Qt-only baseline for distinguishing compositor/platform leaks from mpv.
class ClearItem : public QQuickFramebufferObject {
public:
    using QQuickFramebufferObject::QQuickFramebufferObject;
    Renderer *createRenderer() const override
    {
        class ClearRenderer : public Renderer {
            void render() override
            {
                auto *gl = QOpenGLContext::currentContext()->functions();
                gl->glClearColor(0, 0, 0, 1);
                gl->glClear(GL_COLOR_BUFFER_BIT);
            }
        };
        return new ClearRenderer;
    }
};

int main(int argc, char **argv)
{
    // Model KDE's SIGTERM -> application quit without unsafe Qt signal handlers.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    const int signalFd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    QSocketNotifier notifier(signalFd, QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
        signalfd_siginfo info;
        if (read(signalFd, &info, sizeof(info)) == sizeof(info)) app.quit();
    });
    const auto args = app.arguments();
    if (args.size() < 2) return 2;
    const QString source = args.at(1);
    const int duration = args.size() > 2 ? args.at(2).toInt() : 5000;
    const bool churn = args.contains(QStringLiteral("--churn"));
    const bool neverShown = args.contains(QStringLiteral("--never-shown"));
    const bool lateWindow = args.contains(QStringLiteral("--late-window"));
    const bool noPlayer = args.contains(QStringLiteral("--no-player"));
    const bool persistent = args.contains(QStringLiteral("--persistent"));
    std::vector<std::unique_ptr<QQuickWindow>> windows;
    std::vector<MpvItem *> items;
    auto addWindow = [&] {
        auto window = std::make_unique<QQuickWindow>();
        window->setTitle(QStringLiteral("Wallpaper lifecycle test"));
        window->resize(320, 180);
        MpvItem *item = nullptr;
        if (!noPlayer) {
            item = new MpvItem(window->contentItem());
            item->setSize(QSizeF(320, 180));
            item->setSource(source);
            QObject::connect(item, &MpvItem::statusChanged, &app, [item] {
                qInfo() << "test status" << item->status() << item->hardwareDecoder() << item->errorString();
            });
        } else {
            auto *blank = new ClearItem(window->contentItem());
            blank->setSize(QSizeF(320, 180));
        }
        window->setPersistentGraphics(persistent);
        window->setPersistentSceneGraph(persistent);
        if (!neverShown) window->show();
        items.push_back(item);
        windows.push_back(std::move(window));
    };
    addWindow();
    addWindow();
    QTimer redraw;
    QObject::connect(&redraw, &QTimer::timeout, &app, [&] {
        for (const auto &window : windows)
            for (auto *item : window->contentItem()->childItems()) item->update();
    });
    if (noPlayer) redraw.start(16);
    int tick = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        ++tick;
        auto &window = windows.front();
        auto *&item = items.front();
        if (noPlayer) {
            if (tick % 8 == 5) { window->hide(); window->releaseResources(); }
            if (tick % 8 == 6) window->show();
            return;
        }
        switch (tick % 8) {
        case 0:
            delete item;
            item = new MpvItem(window->contentItem());
            item->setSize(QSizeF(320, 180));
            item->setSource(source);
            break;
        case 1: item->setSource(QStringLiteral("/nonexistent/wallpaper-test.mp4")); break;
        case 2: item->setSource(source); break;
        case 3: item->setSource({}); break;
        case 4: item->setSource(source); item->retry(); break;
        case 5: window->hide(); window->releaseResources(); break;
        case 6: window->show(); break;
        case 7:
            window->resize(360 + tick, 200);
            item->setSize(window->size());
            break;
        }
    });
    if (churn) timer.start(120);
    QTimer::singleShot(duration, &app, &QCoreApplication::quit);
    const int result = app.exec();
    if (lateWindow) {
        // Leave QQuickWindow objects to qApp, after RuntimeService in the
        // QObject child list: Qt's render-loop post routine must clean GL first.
        for (auto &window : windows) {
            window->QObject::setParent(&app);
            window.release();
        }
    }
    close(signalFd);
    return result;
}
