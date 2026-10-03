#ifndef LIBRARY_DRIVER_CLIENT_H
#define LIBRARY_DRIVER_CLIENT_H

// ---------------------------------------------------------------------------
// DriverClient - user-space half of the kernel character device interface.
//
// The application talks to /dev/library_driver with the plain POSIX calls
// open(2) / write(2) / read(2) / close(2).  There is no ioctl(2) and no
// library wrapping the kernel: the contract is simply "write one text event
// per call, read back a small text report".
//
// Design rule from the specification: the library system must remain fully
// functional when the module is not loaded.  Every method here therefore
// degrades to a no-op / false instead of throwing or aborting.
// ---------------------------------------------------------------------------

#include <string>

class DriverClient {
public:
    DriverClient();
    ~DriverClient();

    DriverClient(const DriverClient&)            = delete;
    DriverClient& operator=(const DriverClient&) = delete;

    // Probe the device with stat(2) + open(2).  Never throws.
    bool open();
    void close() noexcept;

    bool isAvailable() const noexcept { return fd_ >= 0; }

    // Send one event such as "BOOK_ISSUE:101:5".  Returns false when the
    // driver is not loaded (the caller should log a debug note, not fail).
    bool sendEvent(const std::string& event);

    // Read back the driver report into `out`:
    //     events=<count>\nlatest=<last event>\n
    bool readReport(std::string& out);

    // Convenience wrapper around readReport(); -1 when unavailable.
    long long eventCount();

    const std::string& devicePath() const noexcept { return path_; }

private:
    std::string path_;
    int         fd_ = -1;
};

#endif // LIBRARY_DRIVER_CLIENT_H