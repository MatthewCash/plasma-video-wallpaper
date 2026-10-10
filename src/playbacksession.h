#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct mpv_handle;

// Native notification storage, never a QObject. Producers retain it until
// callbacks have been unregistered and their mpv objects have been destroyed.
class WakeFd {
public:
    WakeFd();
    ~WakeFd();
    WakeFd(const WakeFd &) = delete;
    WakeFd &operator=(const WakeFd &) = delete;
    int fd() const { return m_fd; }
    void signal() const noexcept;
    void drain() const noexcept;
private:
    int m_fd;
};

class PlaybackSession {
public:
    enum class RenderState { Waiting, Creating, Live, Released, Lost };
    struct Snapshot {
        std::string status;
        std::string error;
        std::string decoder;
    };

    explicit PlaybackSession(std::shared_ptr<WakeFd> guiWake);
    ~PlaybackSession();
    void start();
    void join(); // RuntimeService only; never the renderer or an item destructor.

    void configure(std::string renderNode, bool software = false);
    void setDesired(std::string source, bool paused, uint64_t revision);
    void retire();
    bool stopping() const { return m_stopping.load(); }
    bool finished() const { return m_finished.load(); }
    bool lost() const;
    bool takeUpdate() { return m_update.exchange(false); }
    Snapshot snapshot() const;
    uint64_t id() const { return m_id; }

    // Render attachment is a lease on the client, not shared ownership of GL.
    // Once acquired, client destruction is forbidden until releaseRenderer().
    mpv_handle *acquireRenderer(int &renderFd);
    bool rendererReady();
    void releaseRenderer(bool contextLost = false);
    void fail(const std::string &message);
    static void renderUpdate(void *opaque) noexcept;

private:
    static void clientUpdate(void *opaque) noexcept;
    void notify() noexcept;
    void run();
    void runClient();
    void drainEvents();
    void updatePlayback();
    void setStatus(std::string status);
    bool option(const char *name, const char *value);

    const uint64_t m_id;
    std::shared_ptr<WakeFd> m_guiWake;
    WakeFd m_workerWake;
    std::thread m_worker;
    std::atomic<bool> m_update{true};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_finished{false};

    mutable std::mutex m_mutex; // NEVER held across an mpv call or join.
    RenderState m_renderState = RenderState::Waiting;
    bool m_configured = false;
    bool m_initialized = false;
    bool m_software = false;
    std::string m_renderNode;
    std::string m_source;
    bool m_paused = false;
    uint64_t m_revision = 0;
    Snapshot m_snapshot{"waiting", {}, {}};

    // Published under m_mutex; only the worker destroys these, after release.
    mpv_handle *m_mpv = nullptr;
    int m_renderFd = -1;

    // Worker-only command state. One outstanding playback command at a time.
    uint64_t m_nextCommand = 1;
    uint64_t m_pending = 0;
    uint64_t m_sentRevision = UINT64_MAX;
    bool m_sentPaused = false;
    bool m_decoderQueryPending = false;
    uint64_t m_decoderQueryRevision = 0;
};
