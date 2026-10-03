// ---------------------------------------------------------------------------
// FileManager.cpp - implementation of the binary file I/O wrapper.
//
// Responsibilities:
//   * create missing parent directories (mkdir -p semantics)
//   * open/close binary streams, tightening permissions for the admin file
//   * provide the record <-> stream plumbing (templates live in the header)
//   * crash-safe whole-file replacement via write-temp + POSIX rename()
// ---------------------------------------------------------------------------

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FileManager.h"

#include "Book.h"
#include "Member.h"
#include "Transaction.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

// Create every component of the directory part of `path` (mkdir -p style).
// Empty directories already present are not treated as errors.
void ensureParentDirectory(const std::string& path) {
    if (path.empty()) return;

    std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos) return;          // file lives in the cwd
    if (pos == 0) return;                           // file lives in "/"

    std::string dir = path.substr(0, pos);
    if (dir.empty()) return;

    std::string current;
    for (std::string::size_type i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!current.empty() && current != "/") {
                if (::mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) {
                    throw FileIOException("FileManager: cannot create directory '" +
                                          current + "': " + std::strerror(errno));
                }
            }
        }
        if (i < dir.size()) current += dir[i];
    }
}

} // namespace

// ---------------------------------------------------------------------------
// open()
// ---------------------------------------------------------------------------
void FileManager::open(const std::string& path, std::ios_base::openmode mode,
                       bool strictAdminFile) {
    close();

    ensureParentDirectory(path);

    stream_.open(path, mode);
    if (!stream_.is_open()) {
        throw FileIOException("FileManager: failed to open '" + path +
                              "': " + std::strerror(errno));
    }

    path_ = path;
    mode_ = mode;

    if (strictAdminFile) {
        // Credential store: owner read/write only.
        if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
            const std::string msg = std::strerror(errno);
            close();
            throw FileIOException("FileManager: cannot restrict permissions on '" +
                                  path + "': " + msg);
        }
    }
}

// ---------------------------------------------------------------------------
// close()
// ---------------------------------------------------------------------------
void FileManager::close() noexcept {
    if (stream_.is_open()) {
        try {
            stream_.close();
        } catch (...) {
            // close() is noexcept - never propagate.
        }
    }
    path_.clear();
    mode_ = std::ios_base::in;
}

// ---------------------------------------------------------------------------
// atomicWrite() - write temp file, fsync-free rename over the target.
// rename(2) within the same directory is atomic, so a reader either sees the
// old content or the new content, never a partially written file.
// ---------------------------------------------------------------------------
bool FileManager::atomicWrite(const std::string& path,
                              const std::string& content) {
    const std::string tmpPath = path + ".tmp";

    try {
        ensureParentDirectory(path);
    } catch (const std::exception& e) {
        std::cerr << "[FileManager] " << e.what() << std::endl;
        return false;
    }

    {
        std::ofstream tmp(tmpPath, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!tmp.is_open()) {
            std::cerr << "[FileManager] cannot create temp file '" << tmpPath
                      << "': " << std::strerror(errno) << std::endl;
            return false;
        }

        if (!content.empty()) {
            tmp.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        tmp.flush();

        if (tmp.fail()) {
            std::cerr << "[FileManager] write to '" << tmpPath
                      << "' failed: " << std::strerror(errno) << std::endl;
            tmp.close();
            ::unlink(tmpPath.c_str());
            return false;
        }
        tmp.close();
    }

    if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
        std::cerr << "[FileManager] rename '" << tmpPath << "' -> '" << path
                  << "' failed: " << std::strerror(errno) << std::endl;
        ::unlink(tmpPath.c_str());
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// setPermissions()
// ---------------------------------------------------------------------------
bool FileManager::setPermissions(const std::string& path, mode_t mode,
                                 bool applyToAdmin) {
    const mode_t effective = applyToAdmin ? (mode & (S_IRUSR | S_IWUSR)) : mode;

    if (::chmod(path.c_str(), effective) != 0) {
        std::cerr << "[FileManager] chmod(" << path << ") failed: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Explicit template instantiations for the record types used by the system.
// Keeping them here keeps the header light while still allowing the linker to
// resolve calls made from other translation units.
// ---------------------------------------------------------------------------
template bool FileManager::readRecord<Book>(Book&);
template bool FileManager::writeRecord<Book>(const Book&);
template bool FileManager::readAll<Book>(std::vector<Book>&);
template bool FileManager::writeAll<Book>(const std::vector<Book>&);

template bool FileManager::readRecord<Member>(Member&);
template bool FileManager::writeRecord<Member>(const Member&);
template bool FileManager::readAll<Member>(std::vector<Member>&);
template bool FileManager::writeAll<Member>(const std::vector<Member>&);

template bool FileManager::readRecord<Transaction>(Transaction&);
template bool FileManager::writeRecord<Transaction>(const Transaction&);
template bool FileManager::readAll<Transaction>(std::vector<Transaction>&);
template bool FileManager::writeAll<Transaction>(const std::vector<Transaction>&);
