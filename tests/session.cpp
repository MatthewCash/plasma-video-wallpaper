#include "playbacksession.h"
#include <QCoreApplication>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>

// Force checks on even when built in Release/RelWithDebInfo.
#undef assert
#define assert(value) do { if (!(value)) std::abort(); } while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auto wake = std::make_shared<WakeFd>();
    for (int n = 0; n < 100; ++n) {
        PlaybackSession session(wake);
        session.start();
        if (n % 2) session.configure({}, true);
        session.retire(); // no renderer exists; must never wait for one
        int fd = -1;
        assert(session.acquireRenderer(fd) == nullptr);
        session.join();
        assert(session.finished());
        assert(!session.lost());
    }
    // The handle cannot be destroyed until its render-creation lease returns,
    // including cancellation while creation is in progress.
    for (int n = 0; n < 20; ++n) {
        PlaybackSession session(wake);
        session.start();
        session.configure({}, true);
        int fd = -1;
        mpv_handle *handle = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!(handle = session.acquireRenderer(fd))) {
            assert(!session.stopping());
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(fd == -1);
        session.retire();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(!session.finished());
        assert(!session.rendererReady());
        session.releaseRenderer();
        session.join();
        assert(session.finished());
    }
    std::cout << "session lifecycle tests passed\n";
}
