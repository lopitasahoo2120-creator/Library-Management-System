#ifndef LIBRARY_PROCESS_MANAGER_H
#define LIBRARY_PROCESS_MANAGER_H

#include <vector>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// ProcessManager - demonstrates real Linux process management.
//
// The library spawns a dedicated background logging process (child) via
// fork().  The parent communicates with it through a POSIX pipe (see IPC).
// On shutdown the parent sends a stop signal and waitpid()s the child.
// ---------------------------------------------------------------------------
class ProcessManager {
public:
    ProcessManager();
    ~ProcessManager();

    ProcessManager(const ProcessManager&)            = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    // Start the background logging child process.
    // Returns the child PID, or -1 on failure.
    pid_t startLoggingProcess(int readFd, int writeFd);

    // Send one text line to the child over the pipe.  Returns false when no
    // child is running or the write failed.
    bool sendEvent(const std::string& line);

    // Gracefully stop the child process.
    void stopLoggingProcess();

    pid_t childPid() const noexcept { return childPid_; }
    bool  isRunning() const noexcept { return childPid_ > 0; }

    // Entry point of the forked child.  Reads newline separated event lines
    // from `readFd` and appends them to the monitor log until it sees "STOP"
    // or the pipe is closed.
    static int monitorLoop(int readFd);

private:
    pid_t childPid_ = -1;
    int   writeFd_  = -1;
};

#endif // LIBRARY_PROCESS_MANAGER_H