// ---------------------------------------------------------------------------
// Logger.cpp - asynchronous, thread-safe logging subsystem.
//
// Producers (UI, IPC reader, driver thread) only enqueue an Entry under a
// mutex; a dedicated worker thread owns the file and performs every write.
// That serialisation is what keeps concurrent log lines from interleaving.
// ---------------------------------------------------------------------------

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "Logger.h"

#include "Config.h"
#include "FileManager.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// mkdir -p for the log directory.
void ensureLogDirectory(const std::string& path) {
    const std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return;

    const std::string dir = path.substr(0, pos);
    std::string current;
    for (std::string::size_type i = 0; i <= dir.size(); ++i) {
        if ((i == dir.size() || dir[i] == '/') && !current.empty() && current != "/") {
            if (::mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) {
                std::cerr << "[Logger] cannot create log directory '" << current
                          << "': " << std::strerror(errno) << std::endl;
                return;
            }
        }
        if (i < dir.size()) current += dir[i];
    }
}

// Human-readable rendering of a std::thread::id.
std::string threadIdToString(std::thread::id tid) {
    std::ostringstream oss;
    oss << tid;
    return oss.str();
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
Logger::Logger() {
    path_ = Config::LOG_FILE;
    ensureLogDirectory(path_);

    stop_.store(false);
    worker_ = std::thread(&Logger::workerLoop, this);

    log(Event::SYSTEM_START, "Logger initialised; log file: " + path_);
}

Logger::~Logger() {
    if (!stop_.load()) {
        shutdown();
    } else if (worker_.joinable()) {
        worker_.join();
    }
}

// ---------------------------------------------------------------------------
// Producers - hand work to the worker thread
// ---------------------------------------------------------------------------
void Logger::log(Event event, const std::string& message) {
    Entry entry;
    entry.timestamp = nowTimestamp();
    entry.eventStr  = eventToString(event);
    entry.message   = message;
    entry.pidStr    = std::to_string(static_cast<long long>(::getpid()));
    entry.tidStr    = threadIdToString(std::this_thread::get_id());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(entry));
    }
    cv_.notify_one();
}

void Logger::log(Event event, const std::string& message, pid_t pid,
                 std::thread::id tid) {
    Entry entry;
    entry.timestamp = nowTimestamp();
    entry.eventStr  = eventToString(event);
    entry.message   = message;
    entry.pidStr    = std::to_string(static_cast<long long>(pid));
    entry.tidStr    = threadIdToString(tid);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(entry));
    }
    cv_.notify_one();
}

// ---------------------------------------------------------------------------
// flush() - detach pending entries from the queue and write them here.
// The worker only ever touches entries that are still inside queue_, so after
// the swap the batch is exclusively ours.
// ---------------------------------------------------------------------------
void Logger::flush() {
    std::queue<Entry> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(queue_);
    }

    if (pending.empty()) return;

    ensureLogDirectory(path_);

    std::ofstream out(path_, std::ios::out | std::ios::app);
    if (!out.is_open()) {
        // Put the entries back so the worker can retry later.
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pending.empty()) {
            queue_.push(std::move(pending.front()));
            pending.pop();
        }
        cv_.notify_one();
        std::cerr << "[Logger] flush(): cannot open '" << path_
                  << "': " << std::strerror(errno) << std::endl;
        return;
    }

    while (!pending.empty()) {
        const Entry& e = pending.front();
        out << "[" << e.timestamp << "] [" << e.eventStr << "] [PID:"
            << e.pidStr << "] [TID:" << e.tidStr << "] " << e.message << "\n";
        pending.pop();
    }
    out.flush();
    FileManager::setPermissions(path_, S_IRUSR | S_IWUSR | S_IRGRP);
}

// ---------------------------------------------------------------------------
// workerLoop() - the single consumer that owns the log file
// ---------------------------------------------------------------------------
void Logger::workerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);

    for (;;) {
        cv_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });

        if (queue_.empty()) {
            if (stop_.load()) return;      // drained and asked to stop
            continue;
        }

        std::queue<Entry> batch;
        batch.swap(queue_);

        lock.unlock();
        ensureLogDirectory(path_);

        std::ofstream out(path_, std::ios::out | std::ios::app);
        if (out.is_open()) {
            while (!batch.empty()) {
                const Entry& e = batch.front();
                out << "[" << e.timestamp << "] [" << e.eventStr << "] [PID:"
                    << e.pidStr << "] [TID:" << e.tidStr << "] " << e.message << "\n";
                batch.pop();
            }
            out.flush();
            if (out.fail()) {
                std::cerr << "[Logger] write to '" << path_
                          << "' failed: " << std::strerror(errno) << std::endl;
            }
        } else {
            std::cerr << "[Logger] worker cannot open '" << path_
                      << "': " << std::strerror(errno) << std::endl;
        }
        lock.lock();
    }
}

// ---------------------------------------------------------------------------
// shutdown() - stop the worker, then record the final event ourselves so the
// marker is guaranteed to be the last line in the file.
// ---------------------------------------------------------------------------
void Logger::shutdown() {
    if (!stop_.exchange(true)) {
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();

        std::queue<Entry> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(queue_);
        }

        Entry last;
        last.timestamp = nowTimestamp();
        last.eventStr  = eventToString(Event::SYSTEM_SHUTDOWN);
        last.message   = "Logger worker stopped";
        last.pidStr    = std::to_string(static_cast<long long>(::getpid()));
        last.tidStr    = threadIdToString(std::this_thread::get_id());
        pending.push(std::move(last));

        ensureLogDirectory(path_);
        std::ofstream out(path_, std::ios::out | std::ios::app);
        if (out.is_open()) {
            while (!pending.empty()) {
                const Entry& e = pending.front();
                out << "[" << e.timestamp << "] [" << e.eventStr << "] [PID:"
                    << e.pidStr << "] [TID:" << e.tidStr << "] " << e.message << "\n";
                pending.pop();
            }
            out.flush();
        } else {
            while (!pending.empty()) {
                std::cerr << "[" << pending.front().timestamp << "] ["
                          << pending.front().eventStr << "] [PID:"
                          << pending.front().pidStr << "] [TID:"
                          << pending.front().tidStr << "] "
                          << pending.front().message << std::endl;
                pending.pop();
            }
        }
    } else if (worker_.joinable()) {
        worker_.join();
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::string Logger::eventToString(Event e) noexcept {
    switch (e) {
        case Event::LOGIN:           return "LOGIN";
        case Event::LOGOUT:          return "LOGOUT";
        case Event::BOOK_ADD:        return "BOOK_ADD";
        case Event::BOOK_REMOVE:     return "BOOK_REMOVE";
        case Event::BOOK_UPDATE:     return "BOOK_UPDATE";
        case Event::MEMBER_ADD:      return "MEMBER_ADD";
        case Event::MEMBER_REMOVE:   return "MEMBER_REMOVE";
        case Event::MEMBER_UPDATE:   return "MEMBER_UPDATE";
        case Event::BOOK_ISSUE:      return "BOOK_ISSUE";
        case Event::BOOK_RETURN:     return "BOOK_RETURN";
        case Event::ERROR:           return "ERROR";
        case Event::SYSTEM_START:    return "SYSTEM_START";
        case Event::SYSTEM_SHUTDOWN: return "SYSTEM_SHUTDOWN";
        case Event::IPC_EVENT:       return "IPC_EVENT";
        case Event::DRIVER_EVENT:    return "DRIVER_EVENT";
    }
    return "UNKNOWN";
}

std::string Logger::nowTimestamp() {
    const auto now  = std::chrono::system_clock::now();
    const auto secs = std::chrono::system_clock::to_time_t(now);
    const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) % 1000;

    std::tm tmValue{};
    ::localtime_r(&secs, &tmValue);

    std::ostringstream oss;
    oss << std::put_time(&tmValue, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}
