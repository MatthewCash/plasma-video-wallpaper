#include "mpvitem.h"
#include "playbacksession.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QThread>
#include <QUrl>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <mpv/render_gl.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>
#include <stdexcept>
#include <sys/stat.h>

// Lifetime is deliberately tied to the application, not an item or QML engine.
// Qt's normal render-loop post routine destroys remaining scene graphs before
// application QObject children. Workers below do not need Qt events to exit.
class RuntimeService : public QObject {
    Q_OBJECT
public:
    explicit RuntimeService(QObject *parent) : QObject(parent), wake(std::make_shared<WakeFd>())
    {
        auto *notifier = new QSocketNotifier(wake->fd(), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] {
            wake->drain();
            // A statusChanged handler may destroy an item. Iterate a copy and
            // re-check membership, without touching a removed pointer.
            const auto current = items;
            for (auto *item : current)
                if (std::find(items.begin(), items.end(), item) != items.end()) item->pollSession();
            reap();
        });
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { stop(); }, Qt::DirectConnection);
    }
    ~RuntimeService() override
    {
        stop();
        for (auto *item : items) item->m_runtime = nullptr;
        for (auto &session : sessions) {
            session->join();
            quarantineIfNeeded(session);
        }
        qInfo() << "video wallpaper runtime drained";
    }
    static RuntimeService *get()
    {
        // Constructed only from MpvItem's GUI-thread constructor. qApp owns it.
        auto *app = QCoreApplication::instance();
        Q_ASSERT(QThread::currentThread() == app->thread());
        auto *runtime = app->findChild<RuntimeService *>(QStringLiteral("videoWallpaperRuntime"), Qt::FindDirectChildrenOnly);
        if (!runtime) {
            runtime = new RuntimeService(app);
            runtime->setObjectName(QStringLiteral("videoWallpaperRuntime"));
        }
        return runtime;
    }
    std::shared_ptr<PlaybackSession> create()
    {
        // Called by Qt's sync phase, while GUI operations (including stop) are
        // excluded. Callback threads never access this vector or these items.
        if (stopping) return {};
        auto session = std::make_shared<PlaybackSession>(wake);
        sessions.push_back(session);
        session->start();
        return session;
    }
    void stop()
    {
        stopping = true;
        for (auto &session : sessions) session->retire();
    }
    void reap()
    {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!(*it)->finished()) { ++it; continue; }
            (*it)->join();
            quarantineIfNeeded(*it);
            it = sessions.erase(it);
        }
    }
    static void quarantineIfNeeded(const std::shared_ptr<PlaybackSession> &session)
    {
        if (session->lost()) {
            qCritical() << "video wallpaper: GL context lost; retaining session" << session->id()
                        << "until process exit. Restart the host to reclaim it.";
            // Intentional exceptional leak: freeing GL with another context or
            // destroying the client before its render context is undefined.
            // The plugin stays resident through Qt's default PreventUnloadHint.
            (void)new std::shared_ptr<PlaybackSession>(session);
        }
    }
    std::vector<MpvItem *> items; // GUI thread only
private:
    bool stopping = false; // GUI, or synchronization phase with GUI blocked
    std::shared_ptr<WakeFd> wake;
    std::vector<std::shared_ptr<PlaybackSession>> sessions;
};

namespace {
void *getProc(void *, const char *name)
{
    auto *context = QOpenGLContext::currentContext();
    return context ? reinterpret_cast<void *>(context->getProcAddress(QByteArray(name))) : nullptr;
}

std::string renderNode()
{
    const EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT)
        throw std::runtime_error("Qt Quick must use OpenGL through EGL for direct VA-API");
    auto queryDisplay = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(eglGetProcAddress("eglQueryDisplayAttribEXT"));
    auto queryDevice = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));
    EGLAttrib device = 0;
    const char *node = nullptr;
    if (queryDisplay && queryDevice && queryDisplay(display, EGL_DEVICE_EXT, &device))
        node = queryDevice(reinterpret_cast<EGLDeviceEXT>(device), EGL_DRM_RENDER_NODE_FILE_EXT);
    const char *overrideNode = std::getenv("PLASMA_VIDEO_WALLPAPER_RENDER_NODE");
    if (overrideNode && *overrideNode) {
        struct stat requested{}, actual{};
        if (stat(overrideNode, &requested) || !S_ISCHR(requested.st_mode))
            throw std::runtime_error("invalid PLASMA_VIDEO_WALLPAPER_RENDER_NODE");
        if (node && (stat(node, &actual) || requested.st_rdev != actual.st_rdev))
            throw std::runtime_error("configured VA-API device does not match Qt's rendering device");
        return overrideNode;
    }
    if (!node || !*node)
        throw std::runtime_error("EGL did not identify a DRM render node; set PLASMA_VIDEO_WALLPAPER_RENDER_NODE explicitly");
    return node;
}

struct ResetGL {
    ~ResetGL()
    {
        auto *context = QOpenGLContext::currentContext();
        if (context && context->isValid()) QQuickOpenGLUtils::resetOpenGLState();
    }
};
}

class MpvRenderer final : public QQuickFramebufferObject::Renderer {
public:
    MpvRenderer(std::shared_ptr<PlaybackSession> session, uint64_t generation)
        : m_session(std::move(session)), m_generation(generation) {}
    ~MpvRenderer() override { cleanup(); }

    void synchronize(QQuickFramebufferObject *base) override
    {
        auto *item = static_cast<MpvItem *>(base);
        if (m_generation == item->m_generation) return;
        cleanup();
        m_generation = item->m_generation;
        m_configured = false;
        m_context = nullptr;
        m_thread = nullptr;
        m_session = item->m_runtime ? item->m_runtime->create() : nullptr;
        item->m_session = m_session;
        item->sendDesired();
    }

    void render() override
    {
        auto *fbo = framebufferObject();
        if (!fbo) return;
        auto *current = QOpenGLContext::currentContext();
        if (!current || !current->isValid()) { cleanup(); return; }
        ResetGL reset;
        auto clear = [current] {
            current->functions()->glClearColor(0, 0, 0, 1);
            current->functions()->glClear(GL_COLOR_BUFFER_BIT);
        };
        if (!m_session || m_session->stopping()) {
            cleanup();
            clear();
            return;
        }
        if (!m_configured) {
            try {
                bool software = false;
#ifdef WALLPAPER_TESTING
                software = std::getenv("WALLPAPER_TEST_SOFTWARE") != nullptr;
#endif
                const auto node = software ? std::string() : renderNode();
                qInfo() << "video wallpaper session" << m_session->id() << "render node:" << node.c_str();
                m_session->configure(node, software);
                m_configured = true;
            } catch (const std::exception &e) {
                m_session->fail(e.what());
            }
        }
        if (!m_gl) {
            int fd = -1;
            auto *client = m_session->acquireRenderer(fd);
            if (!client) { clear(); return; }
            m_context = current;
            m_thread = QThread::currentThread();
            m_eglContext = eglGetCurrentContext();
            m_eglDisplay = eglGetCurrentDisplay();
            qInfo() << "video wallpaper session" << m_session->id() << "render creating";
#ifdef WALLPAPER_TESTING
            if (const char *delay = std::getenv("WALLPAPER_TEST_RENDER_DELAY_MS"))
                std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(delay)));
#endif
            mpv_opengl_init_params init{getProc, nullptr};
            mpv_opengl_drm_params_v2 drm{-1, 0, 0, nullptr, fd};
            int advanced = 0;
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init},
                {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
                {fd >= 0 ? MPV_RENDER_PARAM_DRM_DISPLAY_V2 : MPV_RENDER_PARAM_INVALID, &drm},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            const int result = mpv_render_context_create(&m_gl, client, params);
            if (result < 0) {
                m_session->fail(std::string("render context creation: ") + mpv_error_string(result));
                cleanup();
                clear();
                return;
            }
            mpv_render_context_set_update_callback(m_gl, PlaybackSession::renderUpdate, m_session.get());
            if (!m_session->rendererReady()) { cleanup(); clear(); return; }
            qInfo() << "video wallpaper session" << m_session->id() << "render ready";
        }
        if (!ownsCurrentContext()) {
            m_session->fail("render context/thread changed without renderer recreation");
            cleanup();
            clear();
            return;
        }
        mpv_render_context_update(m_gl);
        const QSize size = fbo->size();
        mpv_opengl_fbo target{static_cast<int>(fbo->handle()), size.width(), size.height(), 0};
        int flip = 0;
        int block = 0;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &target},
            {MPV_RENDER_PARAM_FLIP_Y, &flip},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        const int result = mpv_render_context_render(m_gl, params);
        if (result < 0) m_session->fail(std::string("render: ") + mpv_error_string(result));
        // An FBO draw is not a window swap; omit the optional report_swap API.
    }

private:
    bool ownsCurrentContext() const
    {
        auto *current = QOpenGLContext::currentContext();
        return current && current == m_context && current->isValid()
            && QThread::currentThread() == m_thread
            && eglGetCurrentContext() == m_eglContext
            && eglGetCurrentDisplay() == m_eglDisplay;
    }
    void cleanup()
    {
        if (!m_session) return;
        m_session->retire();
        bool lost = false;
        if (m_gl) {
            // This API explicitly does not need a current OpenGL context.
            mpv_render_context_set_update_callback(m_gl, nullptr, nullptr);
            lost = !ownsCurrentContext();
            if (!lost) {
                mpv_render_context_free(m_gl);
                QQuickOpenGLUtils::resetOpenGLState();
            }
            m_gl = nullptr;
        }
        qInfo() << "video wallpaper session" << m_session->id() << "render released" << !lost;
        m_session->releaseRenderer(lost);
        m_session.reset();
    }
    std::shared_ptr<PlaybackSession> m_session;
    mpv_render_context *m_gl = nullptr;
    QOpenGLContext *m_context = nullptr; // identity only; never dereferenced
    QThread *m_thread = nullptr;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    bool m_configured = false;
    uint64_t m_generation;
};

MpvItem::MpvItem(QQuickItem *parent) : QQuickFramebufferObject(parent), m_runtime(RuntimeService::get())
{
    m_runtime->items.push_back(this);
    connect(this, &QQuickItem::visibleChanged, this, &MpvItem::sendDesired);
    connect(this, &QQuickItem::windowChanged, this, &MpvItem::observeWindow);
    // A parent supplied to the constructor may already belong to a window.
    observeWindow(window());
}
MpvItem::~MpvItem()
{
    // QQuickItem's base destructor emits windowChanged/visibleChanged after
    // MpvItem's members have been destroyed. QObject's automatic disconnect is
    // too late for connections targeting a derived method or capturing this.
    disconnect(this, nullptr, this, nullptr);
    disconnect(m_visibilityConnection);
    if (m_observedWindow) m_observedWindow->removeEventFilter(this);
    retireSession();
    if (!m_runtime) return;
    auto &items = m_runtime->items;
    items.erase(std::remove(items.begin(), items.end(), this), items.end());
}
void MpvItem::retireSession()
{
    if (m_session) m_session->retire();
    m_session.reset();
}
void MpvItem::releaseResources()
{
    retireSession();
    QQuickFramebufferObject::releaseResources();
}
QQuickFramebufferObject::Renderer *MpvItem::createRenderer() const
{
    if (m_session) m_session->retire();
    m_session = m_runtime ? m_runtime->create() : nullptr;
    if (m_session) const_cast<MpvItem *>(this)->sendDesired();
    return new MpvRenderer(m_session, m_generation);
}

void MpvItem::sendDesired()
{
    if (!m_session) return;
    const QUrl url(m_source);
    const auto path = (url.isLocalFile() ? url.toLocalFile() : m_source).toUtf8();
    m_session->setDesired(path.toStdString(), !isVisible() || !window() || !window()->isExposed(), m_revision);
}
void MpvItem::observeWindow(QQuickWindow *w)
{
    disconnect(m_visibilityConnection);
    if (m_observedWindow) m_observedWindow->removeEventFilter(this);
    m_observedWindow = w;
    if (w) {
        w->installEventFilter(this);
        m_visibilityConnection = connect(w, &QWindow::visibleChanged, this, &MpvItem::sendDesired);
    }
    sendDesired();
}
bool MpvItem::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_observedWindow && event->type() == QEvent::Expose)
        sendDesired();
    return QQuickFramebufferObject::eventFilter(watched, event);
}
void MpvItem::setSource(const QString &source)
{
    if (source == m_source) return;
    m_source = source;
    ++m_revision;
    if (m_session && m_session->stopping()) ++m_generation;
    sendDesired();
    update();
    emit sourceChanged();
}
void MpvItem::retry()
{
    ++m_revision;
    if (m_session && m_session->stopping()) ++m_generation;
    sendDesired();
    update();
}
void MpvItem::pollSession()
{
    if (!m_session || !m_session->takeUpdate()) return;
    const auto state = m_session->snapshot();
    const QString status = QString::fromStdString(state.status);
    const QString error = QString::fromStdString(state.error);
    const QString decoder = QString::fromStdString(state.decoder);
    update();
    if (m_status != status || m_error != error || m_decoder != decoder) {
        m_status = status;
        m_error = error;
        m_decoder = decoder;
        emit statusChanged();
    }
}

#include "mpvitem.moc"
