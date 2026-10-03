// ---------------------------------------------------------------------------
// ipc.cpp - POSIX named pipe (FIFO) wrapper.
//
// Real system calls used: mkfifo(2), open(2) with O_RDWR|O_NONBLOCK,
// read(2), write(2), close(2), unlink(2), lstat(2).
//
// Note on O_RDWR for the write side: opening a FIFO write-only blocks until a
// reader appears.  We therefore open both ends with O_RDWR in the monitoring
// process so nothing ever deadlocks, while still using the plain blocking
// variant where a real peer is guaranteed to exist.
// ---------------------------------------------------------------------------

#include "system/ipc.h"

#include "Config.h"

#include <cerrno>
#include <cstdio>

// ---------------------------------------------------------------------------
// create()
// ---------------------------------------------------------------------------
bool IpcPipe::create(const std::string& path, mode_t mode) {
    struct stat st{};
    // Already there?  That is the normal case after the first run, and it is
    // not an error: the FIFO is reused.
    if (::lstat(path.c_str(), &st) == 0) {
        return S_ISFIFO(st.st_mode);
    }

    if (::mkfifo(path.c_str(), mode) != 0) {
        if (errno == EEXIST) return true;
        std::fprintf(stderr, "[IPC] mkfifo('%s') failed: %s\n",
                     path.c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// open()
// ---------------------------------------------------------------------------
bool IpcPipe::open(const std::string& path, Mode mode, bool nonBlock) {
    close();

    int flags = (mode == Mode::READ) ? O_RDONLY : O_WRONLY;
    if (nonBlock) flags |= O_NONBLOCK;

    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        std::fprintf(stderr, "[IPC] open('%s', %s) failed: %s\n",
                     path.c_str(),
                     mode == Mode::READ ? "read" : "write",
                     std::strerror(errno));
        return false;
    }

    fd_   = fd;
    mode_ = mode;
    path_ = path;
    return true;
}

// ---------------------------------------------------------------------------
// close()
// ---------------------------------------------------------------------------
void IpcPipe::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    path_.clear();
}

IpcPipe::~IpcPipe() {
    close();
}

// ---------------------------------------------------------------------------
// writeData() / readData()
// ---------------------------------------------------------------------------
ssize_t IpcPipe::writeData(const void* buf, size_t count) {
    if (fd_ < 0) return -1;
    return ::write(fd_, buf, count);
}

ssize_t IpcPipe::readData(void* buf, size_t count) {
    if (fd_ < 0) return -1;
    return ::read(fd_, buf, count);
}

// ---------------------------------------------------------------------------
// writeLine() - one message = "EVENT text\n".
//
// The reader cannot know the length up-front, so we retry on short writes and
// stop cleanly at end-of-file / error instead of returning a partial message.
// ---------------------------------------------------------------------------
bool IpcPipe::writeLine(const std::string& line) {
    if (fd_ < 0) return false;

    std::string payload = line;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');

    std::size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t n = ::write(fd_, payload.data() + sent, payload.size() - sent);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;      // interrupted - retry
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// readLine() - accumulate bytes until '\n' is seen.
// Returns false on EOF, error or a full buffer with no delimiter.
// ---------------------------------------------------------------------------
bool IpcPipe::readLine(std::string& line) {
    if (fd_ < 0) return false;

    line.clear();
    char ch = '\0';

    for (;;) {
        const ssize_t n = ::read(fd_, &ch, 1);
        if (n == 1) {
            if (ch == '\n') return true;
            line.push_back(ch);
            if (line.size() >= static_cast<std::size_t>(Config::IPC_MAX_MESSAGE)) {
                return false;   // over-long message - treat as a protocol error
            }
            continue;
        }
        if (n == 0) return false;                   // EOF
        if (errno == EINTR) continue;               // interrupted - retry
        return false;
    }
}

// ---------------------------------------------------------------------------
// remove()
// ---------------------------------------------------------------------------
void IpcPipe::remove(const std::string& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        std::fprintf(stderr, "[IPC] unlink('%s') failed: %s\n",
                     path.c_str(), std::strerror(errno));
    }
}