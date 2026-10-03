// ---------------------------------------------------------------------------
// thread_manager.cpp - generic thread pool used by the application.
//
// Real OS primitives: std::thread (clone(2)), std::mutex + std::condition_variable
// (futex under the hood) and std::atomic.
//
// The pool is used for work that is "fire and forget but must not block the UI":
//   * the kernel-event forwarding task
//   * the periodic overdue-status refresh
// ---------------------------------------------------------------------------

#include "system/thread_manager.h"

#include <cstdio>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
ThreadManager::ThreadManager(size_t workerCount) {
    if (workerCount == 0) workerCount = 1;
    workers_.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back(&ThreadManager::workerLoop, this);
    }
}

ThreadManager::~ThreadManager() {
    shutdown();
    // Anything the workers did not get to must still be released (drain).
    std::queue<std::function<void()>> leftovers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        leftovers.swap(tasks_);
    }
    while (!leftovers.empty()) {
        leftovers.pop();
    }
}

// ---------------------------------------------------------------------------
// enqueue()
// ---------------------------------------------------------------------------
void ThreadManager::enqueue(std::function<void()> task) {
    if (!task) return;
    if (stop_.load()) return;          // pool is shutting down - drop the task

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
    }
    pending_.fetch_add(1);
    cv_.notify_one();
}

// ---------------------------------------------------------------------------
// workerLoop()
// ---------------------------------------------------------------------------
void ThreadManager::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Wait until work arrives or shutdown begins.
            cv_.wait(lock, [this] { return stop_.load() || !tasks_.empty(); });

            // On stop we keep draining: remaining work still has to run.
            if (tasks_.empty()) {
                if (stop_.load()) return;
                continue;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // A task that throws must not take the worker thread down with it.
        try {
            task();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ThreadManager] task threw: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[ThreadManager] task threw an unknown exception\n");
        }

        pending_.fetch_sub(1);
    }
}

// ---------------------------------------------------------------------------
// shutdown()
// ---------------------------------------------------------------------------
void ThreadManager::shutdown() {
    bool expected = false;
    if (!stop_.compare_exchange_strong(expected, true)) {
        // Already stopping - a concurrent caller may still be joining.
        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }
        return;
    }

    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
}

// ---------------------------------------------------------------------------
// pendingTasks()
// ---------------------------------------------------------------------------
size_t ThreadManager::pendingTasks() const {
    return pending_.load();
}