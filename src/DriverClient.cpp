// ---------------------------------------------------------------------------
// DriverClient.cpp - user-space side of the /dev/library_driver interface.
//
// POSIX calls used: stat(2), open(2), write(2), read(2), close(2).
// ---------------------------------------------------------------------------

#include "DriverClient.h"

#include "Config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// The driver counts bytes, so cap a single event to something sane.
constexpr std::size_t MAX_EVENT_BYTES = 256;

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
DriverClient::DriverClient() {
    path_ = Config::DRIVER_DEVICE;
    open();
}

DriverClient::~DriverClient() {
    close();
}

// ---------------------------------------------------------------------------
// open() - check that the character device node exists and is a character
// device, then open it for reading and writing.
// ---------------------------------------------------------------------------
bool DriverClient::open() {
    if (fd_ >= 0) return true;

    struct stat st{};
    if (::stat(path_.c_str(), &st) != 0) {
        return false;                 // module not loaded - not an error
    }
    if (!S_ISCHR(st.st_mode)) {
        return false;                 // something else owns that path
    }

    const int fd = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        // Present but not writable (missing privileges) - stay degraded.
        return false;
    }

    fd_ = fd;
    return true;
}

// ---------------------------------------------------------------------------
// close()
// ---------------------------------------------------------------------------
void DriverClient::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// sendEvent()
// ---------------------------------------------------------------------------
bool DriverClient::sendEvent(const std::string& event) {
    if (fd_ < 0) return false;
    if (event.empty() || event.size() > MAX_EVENT_BYTES) return false;

    std::string payload = event;
    if (payload.back() != '\n') payload.push_back('\n');

    std::size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t n =
            ::write(fd_, payload.data() + sent, payload.size() - sent);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;      // signal - retry
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// readReport()
// ---------------------------------------------------------------------------
bool DriverClient::readReport(std::string& out) {
    if (fd_ < 0) return false;

    out.clear();
    char    buf[512];
    bool    gotAnything = false;

    // The driver answers immediately with one small report; read until EOF.
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;                  // EOF - driver closed its side
        out.append(buf, static_cast<std::size_t>(n));
        gotAnything = true;
        if (out.size() > MAX_EVENT_BYTES * 4) break;   // safety stop
    }

    return gotAnything;
}

// ---------------------------------------------------------------------------
// eventCount()
// ---------------------------------------------------------------------------
long long DriverClient::eventCount() {
    std::string report;
    if (!readReport(report)) return -1;

    const std::string key = "events=";
    const std::size_t pos = report.find(key);
    if (pos == std::string::npos) return -1;

    return std::atoll(report.c_str() + pos + key.size());
}