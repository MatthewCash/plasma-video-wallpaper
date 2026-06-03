#ifndef MPVITEM_H
#define MPVITEM_H

#include <QQuickFramebufferObject>
#include <atomic>
#include <memory>

struct mpv_handle;
struct mpv_render_context;

struct MpvContext {
    mpv_handle *mpv = nullptr;
    mpv_render_context *gl = nullptr;
    ~MpvContext();
};

struct MpvCbCtx {
    std::atomic<class MpvItem *> item{nullptr};
};

class MpvItem : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
public:
    explicit MpvItem(QQuickItem *parent = nullptr);
    ~MpvItem() override;
    QString source() const { return m_source; }
    void setSource(const QString &source);
    Renderer *createRenderer() const override;
    Q_INVOKABLE void drainMpvEvents();
signals:
    void sourceChanged();
private:
    QString m_source;
    std::shared_ptr<MpvContext> m_ctx;
    std::shared_ptr<MpvCbCtx> m_cb;
};

#endif
