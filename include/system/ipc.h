#ifndef LIBRARY_IPC_H
#define LIBRARY_IPC_H

#include <string>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

// ---------------------------------------------------------------------------
// IPC - POSIX named pipe (FIFO) wrapper.
//
// Why a FIFO?
//   * It is the simplest IPC mechanism that students understand.
//   * The kernel buffers messages, so the writer and reader can run at
//     different speeds without blocking each other.
//   * It works across processes with no shared memory setup.
//
// Architecture:
//   Library Process  --write-->  FIFO  --read-->  Logging Process
// ---------------------------------------------------------------------------
class IpcPipe {
public:
    enum class Mode { READ, WRITE };

    IpcPipe() = default;
    ~IpcPipe();

    IpcPipe(const IpcPipe&)            = delete;
    IpcPipe& operator=(const IpcPipe&) = delete;

    // Create the FIFO at `path` with the given permissions.
    // Safe to call multiple times; returns false if the file already exists
    // (which is the expected case after the first call).
    static bool create(const std::string& path, mode_t mode = 0666);

    // Open the FIFO for reading or writing.  Opening write-only blocks until
    // a reader opens the other end, so callers should do this in a worker
    // thread or with O_NONBLOCK.
    bool open(const std::string& path, Mode mode, bool nonBlock = false);

    void close() noexcept;

    // ---- low-level POSIX read/write -------------------------------------
    // Returns number of bytes transferred, or -1 on error.
    ssize_t writeData(const void* buf, size_t count);
    ssize_t readData(void* buf, size_t count);

    // ---- high-level line protocol ---------------------------------------
    bool writeLine(const std::string& line);
    bool readLine(std::string& line);

    bool isOpen() const noexcept { return fd_ >= 0; }
    int  fd()     const noexcept { return fd_; }

    static void remove(const std::string& path);

private:
    int     fd_ = -1;
    Mode    mode_;
    std::string path_;
};

// Convenience: RAII auto-close.
class IpcAutoClose {
public:
    explicit IpcAutoClose(IpcPipe& pipe) : pipe_(pipe) {}
    ~IpcAutoClose() { pipe_.close(); }
private:
    IpcPipe& pipe_;
};

#endif // LIBRARY_IPC_H