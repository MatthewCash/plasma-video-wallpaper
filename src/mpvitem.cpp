#include "mpvitem.h"
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QObject>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QPointer>
#include <QThread>
#include <QUrl>
#include <clocale>

#include <mpv/client.h>
#include <mpv/render_gl.h>

static constexpr uint64_t HwdecProperty = 1;

MpvContext::~MpvContext()
{
    Q_ASSERT(!gl);
    if (mpv) mpv_terminate_destroy(mpv);
}

class MpvEventProxy : public QObject
{
public:
    explicit MpvEventProxy(MpvItem *item)
        : m_item(item) {}

    void detachItem()
    {
        m_item.clear();
    }

    void requestUpdate()
    {
        if (m_item) m_item->update();
    }

    void drainEvents()
    {
        if (m_item) m_item->drainMpvEvents();
    }

private:
    QPointer<MpvItem> m_item;
};

static std::shared_ptr<MpvEventProxy> make_proxy(MpvItem *item)
{
    return std::shared_ptr<MpvEventProxy>(new MpvEventProxy(item), [](MpvEventProxy *proxy) {
        if (!proxy) return;
        if (QThread::currentThread() == proxy->thread())
            delete proxy;
        else
            proxy->deleteLater();
    });
}

static std::shared_ptr<MpvEventProxy> proxy_from(void *ctx)
{
    auto *cb = static_cast<MpvCbCtx *>(ctx);
    return cb ? cb->proxy.lock() : nullptr;
}

static void *get_proc(void *, const char *name)
{
    auto *c = QOpenGLContext::currentContext();
    return c ? reinterpret_cast<void *>(c->getProcAddress(QByteArray(name))) : nullptr;
}

static void wakeup(void *ctx)
{
    auto proxy = proxy_from(ctx);
    if (proxy) {
        QMetaObject::invokeMethod(proxy.get(), [proxy] { proxy->requestUpdate(); }, Qt::QueuedConnection);
    }
}

static void events(void *ctx)
{
    auto proxy = proxy_from(ctx);
    if (proxy) {
        QMetaObject::invokeMethod(proxy.get(), [proxy] { proxy->drainEvents(); }, Qt::QueuedConnection);
    }
}

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
    std::shared_ptr<MpvContext> m_ctx;
    std::shared_ptr<MpvCbCtx> m_cb;
    QString m_source;
    QString m_loadedSource;
public:
    MpvRenderer(std::shared_ptr<MpvContext> c, std::shared_ptr<MpvCbCtx> cb)
        : m_ctx(std::move(c)), m_cb(std::move(cb)) {}

    ~MpvRenderer() override
    {
        if (m_ctx && m_ctx->gl) {
            mpv_render_context_set_update_callback(m_ctx->gl, nullptr, nullptr);
            mpv_render_context_free(m_ctx->gl);
            m_ctx->gl = nullptr;
        }

        auto context = std::move(m_ctx);
        QMetaObject::invokeMethod(QCoreApplication::instance(),
            [context = std::move(context)]() mutable { context.reset(); },
            Qt::QueuedConnection);
    }

    void render() override
    {
        if (!m_ctx || !m_ctx->mpv) return;
        auto *fbo = framebufferObject();
        if (!fbo) return;

        if (!m_ctx->gl) {
            mpv_opengl_init_params gl{get_proc, nullptr};
            mpv_render_param par[4] = {
                {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl}};
            int param = 2;
            if (m_ctx->wlDisplay)
                par[param++] = {MPV_RENDER_PARAM_WL_DISPLAY, m_ctx->wlDisplay};
            par[param] = {MPV_RENDER_PARAM_INVALID, nullptr};

            const int result = mpv_render_context_create(&m_ctx->gl, m_ctx->mpv, par);
            if (result < 0) {
                qWarning() << "Failed to create mpv OpenGL render context" << result;
                return;
            }

            mpv_render_context_set_update_callback(m_ctx->gl, wakeup, m_cb.get());
        }

        if (m_source.isEmpty() && !m_loadedSource.isEmpty()) {
            const char *c[] = {"stop", nullptr};
            mpv_command_async(m_ctx->mpv, 0, c);
            m_loadedSource.clear();
        } else if (!m_source.isEmpty() && m_source != m_loadedSource) {
            const QUrl url(m_source);
            const QString source = url.isLocalFile() ? url.toLocalFile() : m_source;
            const QByteArray f = source.toUtf8();
            const char *c[] = {"loadfile", f.constData(), nullptr};
            mpv_command_async(m_ctx->mpv, 0, c);
            m_loadedSource = m_source;
        }

        const QSize size = fbo->size();
        if (size.isEmpty()) return;

        mpv_opengl_fbo fbo_{
            .fbo = static_cast<int>(fbo->handle()),
            .w = size.width(),
            .h = size.height(),
            .internal_format = 0};
        int flip = 0;
        mpv_render_param p[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo_},
            {MPV_RENDER_PARAM_FLIP_Y, &flip},
            {MPV_RENDER_PARAM_INVALID, nullptr}};
        mpv_render_context_render(m_ctx->gl, p);
        mpv_render_context_report_swap(m_ctx->gl);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        m_source = static_cast<MpvItem *>(item)->source();
    }
};

MpvItem::MpvItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    setlocale(LC_NUMERIC, "C");

    m_proxy = make_proxy(this);
    m_cb = std::make_shared<MpvCbCtx>();
    m_cb->proxy = m_proxy;

    m_ctx = std::make_shared<MpvContext>();
    if (auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        m_ctx->wlDisplay = wayland->display();
    else
        qWarning() << "Wayland display unavailable; direct VA-API interop is disabled";

    m_ctx->mpv = mpv_create();
    if (!m_ctx->mpv) { qWarning() << "mpv_create failed"; m_ctx.reset(); return; }

    mpv_set_option_string(m_ctx->mpv, "hwdec", "vaapi");
    mpv_set_option_string(m_ctx->mpv, "loop", "inf");
    mpv_set_option_string(m_ctx->mpv, "audio", "no");
    mpv_set_option_string(m_ctx->mpv, "keepaspect", "no");
    mpv_set_option_string(m_ctx->mpv, "vo", "libmpv");
    mpv_set_option_string(m_ctx->mpv, "gpu-api", "opengl");
    mpv_set_option_string(m_ctx->mpv, "load-scripts", "no");
    mpv_set_option_string(m_ctx->mpv, "osc", "no");
    mpv_set_option_string(m_ctx->mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_ctx->mpv, "terminal", "no");

    const int init = mpv_initialize(m_ctx->mpv);
    if (init < 0) { qWarning() << "mpv_initialize failed" << init; m_ctx.reset(); return; }

    const int observe = mpv_observe_property(m_ctx->mpv, HwdecProperty,
                                             "hwdec-current", MPV_FORMAT_STRING);
    if (observe < 0)
        qWarning() << "Failed to observe mpv hardware decoder" << mpv_error_string(observe);

    mpv_set_wakeup_callback(m_ctx->mpv, events, m_cb.get());

    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, &MpvItem::stopPlayback, Qt::DirectConnection);
}

MpvItem::~MpvItem()
{
    stopPlayback();
    if (m_proxy) m_proxy->detachItem();
    if (!m_ctx) return;
    if (m_ctx->mpv) mpv_set_wakeup_callback(m_ctx->mpv, nullptr, nullptr);
    m_proxy.reset();
}

void MpvItem::releaseResources()
{
    stopPlayback();
    QQuickFramebufferObject::releaseResources();
}

void MpvItem::stopPlayback()
{
    if (!m_ctx || !m_ctx->mpv) return;

    const char *command[] = {"stop", nullptr};
    const int result = mpv_command(m_ctx->mpv, command);
    if (result < 0)
        qWarning() << "Failed to stop mpv playback" << mpv_error_string(result);
}

QQuickFramebufferObject::Renderer *MpvItem::createRenderer() const
{
    return new MpvRenderer(m_ctx, m_cb);
}

void MpvItem::setSource(const QString &s)
{
    if (m_source == s) return;
    m_source = s;
    emit sourceChanged();
    update();
}

void MpvItem::drainMpvEvents()
{
    if (!m_ctx || !m_ctx->mpv) return;

    while (true) {
        mpv_event *event = mpv_wait_event(m_ctx->mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            break;
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE &&
            event->reply_userdata == HwdecProperty) {
            auto *property = static_cast<mpv_event_property *>(event->data);
            if (property && property->format == MPV_FORMAT_STRING && property->data) {
                const char *decoder = *static_cast<char **>(property->data);
                if (decoder)
                    qInfo() << "mpv hardware decoder:" << decoder;
            }
        }
    }
}
