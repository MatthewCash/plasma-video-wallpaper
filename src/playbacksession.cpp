#include "playbacksession.h"

#include <QDebug>
#include <mpv/client.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>
#ifdef WALLPAPER_TESTING
#include <cstdlib>
#endif

namespace {
std::atomic<uint64_t> nextSession{1};
constexpr uint64_t DecoderProperty = 1;
constexpr uint64_t DecoderQuery = UINT64_MAX;
}

WakeFd::WakeFd() : m_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
{
    if (m_fd < 0) throw std::runtime_error("video wallpaper: eventfd failed");
}
WakeFd::~WakeFd() { close(m_fd); }
void WakeFd::signal() const noexcept
{
    uint64_t one = 1;
    while (write(m_fd, &one, sizeof(one)) < 0 && errno == EINTR) {}
    // EAGAIN means there is already a notification pending.
}
void WakeFd::drain() const noexcept
{
    uint64_t count;
    while (read(m_fd, &count, sizeof(count)) >= 0 || errno == EINTR) {}
}

PlaybackSession::PlaybackSession(std::shared_ptr<WakeFd> guiWake)
    : m_id(nextSession.fetch_add(1)), m_guiWake(std::move(guiWake)) {}
PlaybackSession::~PlaybackSession()
{
    // RuntimeService owns the join. A renderer dropping its lease must never
    // accidentally run a blocking client destructor through shared_ptr.
    Q_ASSERT(!m_worker.joinable());
    Q_ASSERT(!m_mpv);
    Q_ASSERT(m_renderFd == -1);
}
void PlaybackSession::start() { m_worker = std::thread([this] { run(); }); }
void PlaybackSession::join() { if (m_worker.joinable()) m_worker.join(); }
void PlaybackSession::notify() noexcept
{
    m_update.store(true);
    m_guiWake->signal();
}
void PlaybackSession::renderUpdate(void *opaque) noexcept
{
    static_cast<PlaybackSession *>(opaque)->notify();
}
void PlaybackSession::clientUpdate(void *opaque) noexcept
{
    static_cast<PlaybackSession *>(opaque)->m_workerWake.signal();
}

void PlaybackSession::configure(std::string node, bool software)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_configured || stopping()) return;
        m_renderNode = std::move(node);
        m_software = software;
        m_configured = true;
    }
    m_workerWake.signal();
}
void PlaybackSession::setDesired(std::string source, bool paused, uint64_t revision)
{
    {
        std::lock_guard lock(m_mutex);
        if (stopping()) return;
        m_source = std::move(source);
        m_paused = paused;
        m_revision = revision;
    }
    m_workerWake.signal();
}
void PlaybackSession::retire()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping.store(true);
        // Atomically exclude a renderer that has not yet claimed the handle.
        if (m_renderState == RenderState::Waiting)
            m_renderState = RenderState::Released;
    }
    m_workerWake.signal();
    notify();
}
mpv_handle *PlaybackSession::acquireRenderer(int &fd)
{
    std::lock_guard lock(m_mutex);
    if (stopping() || !m_initialized || m_renderState != RenderState::Waiting)
        return nullptr;
    m_renderState = RenderState::Creating;
    fd = m_renderFd;
    return m_mpv;
}
bool PlaybackSession::rendererReady()
{
    bool accepted;
    {
        std::lock_guard lock(m_mutex);
        Q_ASSERT(m_renderState == RenderState::Creating);
        accepted = !stopping();
        if (accepted) m_renderState = RenderState::Live;
    }
    m_workerWake.signal();
    return accepted;
}
void PlaybackSession::releaseRenderer(bool contextLost)
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping.store(true);
        m_renderState = contextLost ? RenderState::Lost : RenderState::Released;
    }
    m_workerWake.signal();
}
bool PlaybackSession::lost() const
{
    std::lock_guard lock(m_mutex);
    return m_renderState == RenderState::Lost;
}
PlaybackSession::Snapshot PlaybackSession::snapshot() const
{
    std::lock_guard lock(m_mutex);
    return m_snapshot;
}
void PlaybackSession::setStatus(std::string status)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_snapshot.status == status) return;
        m_snapshot.status = std::move(status);
    }
    notify();
}
void PlaybackSession::fail(const std::string &message)
{
    {
        std::lock_guard lock(m_mutex);
        m_snapshot.error = message;
        m_snapshot.status = "error";
    }
    qWarning() << "video wallpaper session" << m_id << message.c_str();
    retire();
}
bool PlaybackSession::option(const char *name, const char *value)
{
    const int error = mpv_set_option_string(m_mpv, name, value);
    if (error >= 0) return true;
    fail(std::string("option ") + name + ": " + mpv_error_string(error));
    return false;
}

void PlaybackSession::run()
{
    qInfo() << "video wallpaper session" << m_id << "worker started";
    try {
        runClient();
    } catch (const std::exception &e) {
        fail(e.what());
    }

    // Wait on native synchronization, NOT Qt event delivery. The renderer may
    // be destroyed after app.exec() returns. Do not hold a lock while calling
    // mpv: its wakeup callbacks may run synchronously from those calls.
    for (;;) {
        RenderState state;
        {
            std::lock_guard lock(m_mutex);
            state = m_renderState;
        }
        if (state == RenderState::Released || state == RenderState::Lost) break;
        pollfd fd{m_workerWake.fd(), POLLIN, 0};
        poll(&fd, 1, 100);
        m_workerWake.drain();
    }
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        if (!lost()) {
            mpv_terminate_destroy(m_mpv);
            m_mpv = nullptr;
        }
    }
    if (!lost() && m_renderFd >= 0) {
        close(m_renderFd);
        m_renderFd = -1;
    }
    // Lost GL contexts cannot legally be freed with a replacement context.
    // RuntimeService quarantines this session (including callbacks and fd)
    // until process exit rather than causing UAF, a deadlock, or an abort.
    qInfo() << "video wallpaper session" << m_id
            << (lost() ? "quarantined" : "client destroyed");
    m_finished.store(true);
    notify();
}

void PlaybackSession::runClient()
{
    for (;;) {
        bool configured;
        {
            std::lock_guard lock(m_mutex);
            configured = m_configured;
        }
        if (stopping()) return;
        if (configured) break;
        pollfd fd{m_workerWake.fd(), POLLIN, 0};
        poll(&fd, 1, -1);
        m_workerWake.drain();
    }
    setStatus("initializing");
#ifdef WALLPAPER_TESTING
    if (const char *delay = std::getenv("WALLPAPER_TEST_INIT_DELAY_MS"))
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(delay)));
#endif
    if (stopping()) return;
    if (!m_software) {
        m_renderFd = open(m_renderNode.c_str(), O_RDWR | O_CLOEXEC);
        if (m_renderFd < 0) {
            fail("cannot open render node " + m_renderNode + ": " + std::strerror(errno));
            return;
        }
    }
    m_mpv = mpv_create();
    if (!m_mpv) {
        fail("mpv_create failed; host LC_NUMERIC must be C");
        return;
    }
    if (!option("config", "no") || !option("vo", "libmpv") ||
        !option("hwdec", m_software ? "no" : "vaapi") ||
        !option("gpu-hwdec-interop", m_software ? "no" : "vaapi") ||
        !option("hwdec-software-fallback", "no") ||
        !option("audio", "no") || !option("loop-file", "inf") ||
        !option("keepaspect", "no") || !option("load-scripts", "no") ||
        !option("osc", "no") || !option("input-default-bindings", "no") ||
        !option("terminal", "no") || !option("video-timing-offset", "0")) return;
    // Isolate embedded playback from built-in interactive scripts as well.
    for (const char *name : {"load-auto-profiles", "load-console", "load-stats-overlay", "load-select"}) {
        const int error = mpv_set_option_string(m_mpv, name, "no");
        if (error < 0 && error != MPV_ERROR_OPTION_NOT_FOUND) {
            fail(std::string("option ") + name + ": " + mpv_error_string(error));
            return;
        }
    }
    mpv_request_log_messages(m_mpv, "v");
    const int initialized = mpv_initialize(m_mpv);
    if (initialized < 0) {
        fail(std::string("mpv_initialize: ") + mpv_error_string(initialized));
        return;
    }
    if (stopping()) return;
    mpv_set_wakeup_callback(m_mpv, clientUpdate, this);
    const int observe = mpv_observe_property(m_mpv, DecoderProperty, "hwdec-current", MPV_FORMAT_STRING);
    if (observe < 0) {
        fail(std::string("observe hwdec-current: ") + mpv_error_string(observe));
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_initialized = true;
    }
    setStatus("awaiting-renderer");
    notify();
    while (!stopping()) {
        m_workerWake.drain();
        drainEvents();
        if (!stopping()) updatePlayback();
        pollfd fd{m_workerWake.fd(), POLLIN, 0};
        // A finite timeout also bounds recovery from interrupted notifications.
        if (!stopping()) poll(&fd, 1, 100);
    }
}

void PlaybackSession::drainEvents()
{
    while (!stopping()) {
        const mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) break;
        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            retire();
        } else if ((event->event_id == MPV_EVENT_COMMAND_REPLY ||
                    event->event_id == MPV_EVENT_SET_PROPERTY_REPLY) &&
                   event->reply_userdata == m_pending) {
            m_pending = 0;
            if (event->error < 0) {
                // Allow a new source/retry revision to recover without silently
                // treating a failed load as successfully playing.
                {
                    std::lock_guard lock(m_mutex);
                    m_snapshot.error = mpv_error_string(event->error);
                }
                setStatus("error");
            }
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            setStatus("loaded");
        } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            setStatus("playing");
            if (!m_software && !m_decoderQueryPending) {
                m_decoderQueryRevision = m_sentRevision;
                m_decoderQueryPending = mpv_get_property_async(m_mpv, DecoderQuery,
                    "hwdec-current", MPV_FORMAT_STRING) >= 0;
            }
        } else if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata == DecoderQuery) {
            m_decoderQueryPending = false;
            // Initial detection can fall back even with software-fallback=no.
            // Never silently advertise software/copy-back as direct VA-API.
            const auto *prop = static_cast<const mpv_event_property *>(event->data);
            bool relevant;
            {
                std::lock_guard lock(m_mutex);
                relevant = m_revision == m_decoderQueryRevision && !m_source.empty();
            }
            if (relevant && event->error >= 0 && prop && prop->format == MPV_FORMAT_STRING && prop->data) {
                const char *decoder = *static_cast<char **>(prop->data);
                if (decoder && *decoder && std::strcmp(decoder, "vaapi") != 0)
                    fail(std::string("direct VA-API unavailable; refusing decoder ") + decoder);
            }
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            const auto *end = static_cast<const mpv_event_end_file *>(event->data);
            if (end && end->reason == MPV_END_FILE_REASON_ERROR) {
                {
                    std::lock_guard lock(m_mutex);
                    m_snapshot.error = mpv_error_string(end->error);
                }
                setStatus("error");
            }
        } else if (event->event_id == MPV_EVENT_QUEUE_OVERFLOW) {
            fail("mpv event queue overflow; session retired to avoid stale command state");
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            const auto *log = static_cast<const mpv_event_log_message *>(event->data);
            if (log && (log->log_level <= MPV_LOG_LEVEL_ERROR || std::strstr(log->text, "Using EGL dmabuf interop")))
                qInfo() << "video wallpaper session" << m_id << log->prefix << log->text;
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && event->reply_userdata == DecoderProperty) {
            const auto *prop = static_cast<const mpv_event_property *>(event->data);
            if (prop && prop->format == MPV_FORMAT_STRING && prop->data) {
                const char *decoder = *static_cast<char **>(prop->data);
                {
                    std::lock_guard lock(m_mutex);
                    m_snapshot.decoder = decoder ? decoder : "";
                }
                qInfo() << "video wallpaper session" << m_id << "decoder:" << decoder;
                notify();
            }
        }
    }
}

void PlaybackSession::updatePlayback()
{
    std::string source;
    bool paused;
    uint64_t revision;
    {
        std::lock_guard lock(m_mutex);
        if (m_renderState != RenderState::Live || stopping()) return;
        source = m_source;
        paused = m_paused;
        revision = m_revision;
    }
    if (m_pending) return;
    int result = 0;
    const uint64_t command = m_nextCommand++;
    if (m_sentPaused != paused) {
        int value = paused;
        result = mpv_set_property_async(m_mpv, command, "pause", MPV_FORMAT_FLAG, &value);
        m_sentPaused = paused;
    } else if (revision != m_sentRevision) {
        const char *load[] = {"loadfile", source.c_str(), "replace", nullptr};
        const char *stop[] = {"stop", nullptr};
        result = mpv_command_async(m_mpv, command, source.empty() ? stop : load);
        m_sentRevision = revision;
        {
            std::lock_guard lock(m_mutex);
            m_snapshot.error.clear();
            m_snapshot.decoder.clear();
        }
        setStatus(source.empty() ? "idle" : "loading");
    } else {
        return;
    }
    if (result < 0) {
        fail(std::string("cannot queue playback command: ") + mpv_error_string(result));
    } else {
        m_pending = command;
    }
}
