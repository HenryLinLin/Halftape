#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
namespace ws {
class Jobs {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> pending;
    bool quit = false;
    std::thread worker;
public:
    explicit Jobs(std::function<void()> idle = {}) : worker([this, idle] {
        std::unique_lock lock(mutex);
        while (!quit) {
            if (!wake.wait_for(lock, std::chrono::seconds(60), [&] { return quit || !pending.empty(); })) {
                lock.unlock(); if (idle) idle(); lock.lock(); continue;
            }
            if (quit) break;
            auto job = std::move(pending.front()); pending.pop_front();
            lock.unlock(); job(); lock.lock();
        }
    }) {}
    void submit(std::function<void()> job, bool replace = false) {
        { std::lock_guard lock(mutex); if (replace) pending.clear(); pending.push_back(std::move(job)); }
        wake.notify_one();
    }
    void stop() {
        { std::lock_guard lock(mutex); quit = true; pending.clear(); }
        wake.notify_one(); if (worker.joinable()) worker.join();
    }
    ~Jobs() { stop(); }
};
}
