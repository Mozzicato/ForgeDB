#pragma once

// Thin wrapper over the OS file API. This is the only platform-specific code.
//
// Why positional I/O (pread/pwrite, ReadFile with an offset) instead of
// std::fstream or FILE*: a stream has ONE shared cursor, so two threads
// reading at different offsets would race on seek+read. Positional reads take
// the offset as an argument, so concurrent readers never share state.
//
// Errors are reported by throwing IoError.

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace forgedb {

class IoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LogFile {
public:
    // Opens (creating if needed) the file for reading and writing.
    explicit LogFile(const std::filesystem::path& path);
    ~LogFile();
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

    uint64_t size() const;
    std::string read_all() const;
    // Reads exactly `len` bytes at `offset`. Safe to call from many threads.
    std::string read_at(uint64_t offset, size_t len) const;
    // Writes all of `data` at `offset`.
    void write_at(uint64_t offset, std::string_view data);
    // Forces written data to stable storage (fdatasync / FlushFileBuffers).
    void sync();
    void truncate(uint64_t new_size);

private:
#ifdef _WIN32
    void* handle_;  // HANDLE, kept as void* so <windows.h> stays out of headers
#else
    int fd_;
#endif
    std::filesystem::path path_;
};

}  // namespace forgedb
