#pragma once

#include <QQuickFramebufferObject>
#include <QMetaObject>
#include <QPointer>
#include <memory>

class PlaybackSession;
class RuntimeService;
class MpvRenderer;

class MpvItem : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY statusChanged)
    Q_PROPERTY(QString hardwareDecoder READ hardwareDecoder NOTIFY statusChanged)
public:
    explicit MpvItem(QQuickItem *parent = nullptr);
    ~MpvItem() override;
    QString source() const { return m_source; }
    void setSource(const QString &source);
    QString status() const { return m_status; }
    QString errorString() const { return m_error; }
    QString hardwareDecoder() const { return m_decoder; }
    Q_INVOKABLE void retry();
    Renderer *createRenderer() const override;
signals:
    void sourceChanged();
    void statusChanged();
protected:
    void releaseResources() override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    friend class RuntimeService;
    friend class MpvRenderer;
    void pollSession();
    void sendDesired();
    void retireSession();
    void observeWindow(QQuickWindow *window);
    QString m_source;
    QString m_status = QStringLiteral("waiting");
    QString m_error;
    QString m_decoder;
    uint64_t m_revision = 0;
    uint64_t m_generation = 0;
    RuntimeService *m_runtime;
    QMetaObject::Connection m_visibilityConnection;
    QPointer<QQuickWindow> m_observedWindow;
    // createRenderer runs during synchronization with the GUI thread blocked.
    mutable std::shared_ptr<PlaybackSession> m_session;
};
