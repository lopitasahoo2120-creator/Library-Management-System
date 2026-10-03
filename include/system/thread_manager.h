#ifndef LIBRARY_THREAD_MANAGER_H
#define LIBRARY_THREAD_MANAGER_H

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <atomic>
#include <stdexcept>

// ---------------------------------------------------------------------------
// ThreadManager - small helper for managing background worker threads.
//
// Provides:
//   * a thread-safe task queue (mutex + condition_variable)
//   * a pool of worker threads
//   * graceful shutdown
//
// This is the generic machinery behind the dedicated logging thread.
// ---------------------------------------------------------------------------
class ThreadManager {
public:
    explicit ThreadManager(size_t workerCount = 1);
    ~ThreadManager();

    ThreadManager(const ThreadManager&)            = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;

    // Enqueue a unit of work.  May block briefly if the queue is full.
    void enqueue(std::function<void()> task);

    // Graceful shutdown: stop accepting tasks, drain remaining work, join.
    void shutdown();

    size_t pendingTasks() const;

private:
    void workerLoop();

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    std::atomic<bool>                 stop_{false};
    std::atomic<size_t>              pending_{0};
};

#endif // LIBRARY_THREAD_MANAGER_H