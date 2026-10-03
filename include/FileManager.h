#ifndef LIBRARY_FILE_MANAGER_H
#define LIBRARY_FILE_MANAGER_H

#include <sys/types.h>   // mode_t

#include <string>
#include <fstream>
#include <vector>
#include <stdexcept>

// ---------------------------------------------------------------------------
// FileManager - low-level binary file I/O wrapper with atomic updates.
//
// Design:
//   * Records are written as fixed-size binary blobs (POD-style).
//   * Updates use the "write to temp + rename" pattern so a crash cannot
//     leave a half-written file behind.
//   * File permissions are tightened for the admin credential file.
// ---------------------------------------------------------------------------
class FileManager {
public:
    FileManager() = default;

    // Open for read/write, creating parent directories if needed.
    // Throws std::runtime_error on failure.
    void open(const std::string& path, std::ios_base::openmode mode,
              bool strictAdminFile = false);

    void close() noexcept;

    // ---- generic binary read/write --------------------------------------
    template<typename T>
    bool readRecord(T& record) {
        if (!stream_.is_open()) return false;
        return record.deserialize(stream_);
    }

    template<typename T>
    bool writeRecord(const T& record) {
        if (!stream_.is_open()) return false;
        return record.serialize(stream_);
    }

    // ---- whole-file vector helpers --------------------------------------
    template<typename T>
    bool readAll(std::vector<T>& out) {
        out.clear();
        if (!stream_.is_open()) return false;
        T item;
        while (item.deserialize(stream_)) {
            out.push_back(std::move(item));
        }
        // clear any fail/eof state so the stream is reusable
        stream_.clear();
        return true;
    }

    template<typename T>
    bool writeAll(const std::vector<T>& items) {
        if (!stream_.is_open()) return false;
        for (const auto& it : items) {
            if (!it.serialize(stream_)) return false;
        }
        stream_.flush();
        return !stream_.fail();
    }

    // ---- atomic replace -------------------------------------------------
    // Writes `content` to a temp file in the same directory then rename()s
    // it over the target.  rename() is atomic on Linux.
    static bool atomicWrite(const std::string& path,
                            const std::string& content);

    // ---- helpers --------------------------------------------------------
    bool        isOpen()    const noexcept { return stream_.is_open(); }
    const std::string& path() const noexcept { return path_; }
    std::streamsize tellg() { return stream_.tellg(); }
    void seekg(std::streampos pos) { stream_.seekg(pos); }

    // ---- file permission helpers (POSIX) --------------------------------
    static bool setPermissions(const std::string& path,
                               mode_t mode, bool applyToAdmin = false);

private:
    std::string                 path_;
    std::fstream                stream_;
    std::ios_base::openmode     mode_;
};

// Exception type used by FileManager to signal I/O problems.
class FileIOException : public std::runtime_error {
public:
    explicit FileIOException(const std::string& msg)
        : std::runtime_error(msg) {}
};

#endif // LIBRARY_FILE_MANAGER_H