#include "mpvitem.h"
#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickWindow>
#include <QUrl>
#include <clocale>

#include <mpv/client.h>
#include <mpv/render_gl.h>

MpvContext::~MpvContext()
{
    if (mpv) mpv_terminate_destroy(mpv);
}

static void *get_proc(void *, const char *name)
{
    auto *c = QOpenGLContext::currentContext();
    return c ? reinterpret_cast<void *>(c->getProcAddress(QByteArray(name))) : nullptr;
}

static void wakeup(void *ctx)
{
    auto *cb = static_cast<MpvCbCtx *>(ctx);
    if (auto *item = cb->item.load())
        QMetaObject::invokeMethod(item, "update", Qt::QueuedConnection);
}

static void events(void *ctx)
{
    auto *cb = static_cast<MpvCbCtx *>(ctx);
    if (auto *item = cb->item.load())
        QMetaObject::invokeMethod(item, "drainMpvEvents", Qt::QueuedConnection);
}

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
    std::shared_ptr<MpvContext> m_ctx;
    std::shared_ptr<MpvCbCtx> m_cb;
    QQuickFramebufferObject *m_item = nullptr;
    QString m_source;
    QString m_loadedSource;
public:
    MpvRenderer(std::shared_ptr<MpvContext> c, std::shared_ptr<MpvCbCtx> cb)
        : m_ctx(std::move(c)), m_cb(std::move(cb)) {}

    ~MpvRenderer() override
    {
        if (!m_ctx || !m_ctx->gl) return;
        mpv_render_context_set_update_callback(m_ctx->gl, nullptr, nullptr);
        mpv_render_context_free(m_ctx->gl);
        m_ctx->gl = nullptr;
    }

    void render() override
    {
        if (!m_ctx || !m_ctx->mpv) return;
        auto *item = static_cast<MpvItem *>(m_item);
        if (!item || !item->window()) return;
        auto *fbo = framebufferObject();
        if (!fbo) return;

        if (!m_ctx->gl) {
            mpv_opengl_init_params gl{get_proc, nullptr};
            mpv_render_param par[] = {
                {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
                {MPV_RENDER_PARAM_INVALID, nullptr}};

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

        qreal dpr = item->window()->devicePixelRatio();
        mpv_opengl_fbo fbo_{
            .fbo = static_cast<int>(fbo->handle()),
            .w = static_cast<int>(item->width() * dpr),
            .h = static_cast<int>(item->height() * dpr),
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
        m_item = item;
        m_source = static_cast<MpvItem *>(item)->source();
    }
};

MpvItem::MpvItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    setlocale(LC_NUMERIC, "C");

    m_cb = std::make_shared<MpvCbCtx>();
    m_cb->item.store(this);

    m_ctx = std::make_shared<MpvContext>();
    m_ctx->mpv = mpv_create();
    if (!m_ctx->mpv) { qWarning() << "mpv_create failed"; m_ctx.reset(); return; }

    mpv_set_option_string(m_ctx->mpv, "hwdec", "auto-safe");
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

    mpv_set_wakeup_callback(m_ctx->mpv, events, m_cb.get());
}

MpvItem::~MpvItem()
{
    m_cb->item.store(nullptr);
    if (!m_ctx) return;
    if (m_ctx->mpv) mpv_set_wakeup_callback(m_ctx->mpv, nullptr, nullptr);
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
    }
}
