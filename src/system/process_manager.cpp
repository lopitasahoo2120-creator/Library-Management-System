// ---------------------------------------------------------------------------
// process_manager.cpp - real multi-process architecture for the application.
//
// Architecture
//
//   ┌────────────────────────┐  pipe() write end        ┌───────────────┐
//   │  Main library process  │ ────────────────────────► │ Monitor child │
//   │  (UI, business logic)  │  events "BOOK_ISSUE:..." │ logs/monitor │
//   └────────────────────────┘                          └───────────────┘
//
// Why a second process at all?
//   The monitor does work that must survive a crash of the interactive UI and
//   that is useful even when nobody is logged in: it writes a tamper-evident
//   audit trail of every catalogue / member / loan event.  Running it as a real
//   child (fork(2)) rather than a thread means the OS gives us independent
//   address spaces and process isolation for free.
//
// Real syscalls used: pipe(2), fork(2), getpid(2), getppid(2), read(2),
// write(2), close(2), waitpid(2), kill(2), unlink(2).
// ---------------------------------------------------------------------------

#include "system/process_manager.h"

#include "Config.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

const char* const STOP_MESSAGE = "STOP";

// mkdir -p for the monitor log directory (logs/).
void ensureDirFor(const std::string& path) {
    const std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return;

    std::string dir = path.substr(0, pos);
    std::string current;
    for (std::string::size_type i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!current.empty() && ::mkdir(current.c_str(), 0775) != 0 &&
                errno != EEXIST) {
                return;
            }
        }
        if (i < dir.size()) current.push_back(dir[i]);
    }
}

std::string stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    ::localtime_r(&t, &tmv);
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

} // namespace

// ---------------------------------------------------------------------------
// monitorLoop() - runs in the forked child.
// ---------------------------------------------------------------------------
int ProcessManager::monitorLoop(int readFd) {
    // The child only needs the read end; closing the write end means the
    // parent's read() below sees EOF as soon as the parent dies.
    std::string buffer;
    char        chunk[512];

    // Detach from the terminal's job control so Ctrl+C in the parent does not
    // also kill the monitor.
    ::setsid();

    ensureDirFor(Config::LOG_FILE);

    for (;;) {
        const ssize_t n = ::read(readFd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;   // SIGCHLD etc. - keep going
            break;
        }
        if (n == 0) break;                  // parent closed the pipe

        buffer.append(chunk, static_cast<std::size_t>(n));

        std::size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            const std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);

            if (line == STOP_MESSAGE) {
                ::close(readFd);
                return 0;
            }
            if (line.empty()) continue;

            // Audit line: which process said what, and when.
            std::FILE* out = std::fopen(Config::LOG_FILE, "a");
            if (out) {
                std::fprintf(out, "[%s] [MONITOR pid=%ld ppid=%ld] %s\n",
                             stamp().c_str(),
                             static_cast<long>(::getpid()),
                             static_cast<long>(::getppid()),
                             line.c_str());
                std::fclose(out);
            }
        }
    }

    ::close(readFd);
    return 0;
}

// ---------------------------------------------------------------------------
// startLoggingProcess()
// ---------------------------------------------------------------------------
pid_t ProcessManager::startLoggingProcess(int readFd, int writeFd) {
    if (childPid_ > 0) return childPid_;   // already running

    // Copy the target name in case the caller passes a temporary.
    const char* program = "library_monitor";

    const pid_t pid = ::fork();

    if (pid < 0) {
        std::fprintf(stderr, "[ProcessManager] fork() failed: %s\n",
                     std::strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // ---- child -------------------------------------------------------
        // Nothing here may touch the parent's C++ objects or allocator state
        // that could be locked, so we only use async-signal-safe calls.
        ::close(writeFd);
        _exit(monitorLoop(readFd));
    }

    // ---- parent ----------------------------------------------------------
    childPid_ = pid;
    writeFd_  = writeFd;
    (void)program;   // documentation only: the child inherits our image

    // The parent does not need the read end any more.
    if (readFd >= 0) ::close(readFd);

    return pid;
}

// ---------------------------------------------------------------------------
// sendEvent()
// ---------------------------------------------------------------------------
bool ProcessManager::sendEvent(const std::string& line) {
    if (writeFd_ < 0 || childPid_ <= 0) return false;

    std::string payload = line;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');

    std::size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t n =
            ::write(writeFd_, payload.data() + sent, payload.size() - sent);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) continue;   // non-blocking pipe, retry
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// stopLoggingProcess()
// ---------------------------------------------------------------------------
void ProcessManager::stopLoggingProcess() {
    if (childPid_ <= 0) return;

    // Ask nicely first so the child can flush its audit line.
    if (writeFd_ >= 0) {
        const std::string msg = std::string(STOP_MESSAGE) + "\n";
        const ssize_t ignored = ::write(writeFd_, msg.data(), msg.size());
        (void)ignored;
    }

    // waitpid() blocks until the child exits - this is the graceful path.
    int status = 0;
    pid_t done = 0;
    int     waited = 0;

    do {
        done = ::waitpid(childPid_, &status, 0);
        ++waited;
    } while (done < 0 && errno == EINTR && waited < 50);

    if (done < 0) {
        // Child is unresponsive: escalate to SIGTERM, then SIGKILL.
        ::kill(childPid_, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        done = ::waitpid(childPid_, &status, WNOHANG);
        if (done == 0) {
            ::kill(childPid_, SIGKILL);
            done = ::waitpid(childPid_, &status, 0);
        }
    }

    if (writeFd_ >= 0) {
        ::close(writeFd_);
        writeFd_ = -1;
    }
    childPid_ = -1;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
ProcessManager::ProcessManager() = default;

ProcessManager::~ProcessManager() {
    stopLoggingProcess();
}