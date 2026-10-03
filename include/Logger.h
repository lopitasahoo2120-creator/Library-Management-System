#ifndef LIBRARY_LOGGER_H
#define LIBRARY_LOGGER_H

#include <sys/types.h>   // pid_t

#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <atomic>

// ---------------------------------------------------------------------------
// Logger - asynchronous, thread-safe logging subsystem.
//
// Architecture (as required by the spec):
//
//   Main application
//        |
//        v
//   Thread-safe log queue (std::queue + std::mutex + condition_variable)
//        |
//        v
//   Dedicated background worker thread
//        |
//        v
//   library.log
//
// Why concurrency is necessary:
//   Multiple threads (UI, logger, IPC reader) may emit log entries at any
//   time.  Writing directly to the file from every call site would interleave
//   lines and produce garbled output.  A single consumer thread serialises
//   writes while producers never block for long.
// ---------------------------------------------------------------------------
class Logger {
public:
    // Event types understood by the system.
    enum class Event {
        LOGIN, LOGOUT,
        BOOK_ADD, BOOK_REMOVE, BOOK_UPDATE,
        MEMBER_ADD, MEMBER_REMOVE, MEMBER_UPDATE,
        BOOK_ISSUE, BOOK_RETURN,
        ERROR, SYSTEM_START, SYSTEM_SHUTDOWN,
        IPC_EVENT, DRIVER_EVENT
    };

    Logger();
    ~Logger();

    // Non-copyable, non-movable.
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    // ---- public API (thread-safe) --------------------------------------
    void log(Event event, const std::string& message);
    void log(Event event, const std::string& message,
             pid_t pid, std::thread::id tid);

    void flush();

    // ---- helpers --------------------------------------------------------
    static std::string eventToString(Event e) noexcept;
    static std::string nowTimestamp();

    // ---- lifecycle ------------------------------------------------------
    void shutdown();   // graceful stop of the worker thread

private:
    void workerLoop(); // runs inside the background thread

    struct Entry {
        std::string timestamp;
        std::string eventStr;
        std::string message;
        std::string pidStr;
        std::string tidStr;
    };

    std::mutex              mutex_;
    std::condition_variable cv_;
    std::queue<Entry>       queue_;
    std::thread             worker_;
    std::atomic<bool>       stop_{false};
    std::string             path_;
};

#endif // LIBRARY_LOGGER_H