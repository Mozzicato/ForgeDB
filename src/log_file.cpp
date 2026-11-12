#include "log_file.h"

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace forgedb {

#ifdef _WIN32

namespace {
[[noreturn]] void fail(const std::string& what, const std::filesystem::path& path) {
    throw IoError(what + " failed for " + path.string() + " (error " + std::to_string(GetLastError()) + ")");
}

OVERLAPPED at_offset(uint64_t offset) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return ov;
}
}  // namespace

LogFile::LogFile(const std::filesystem::path& path) : path_(path) {
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) fail("open", path_);
}

LogFile::~LogFile() { CloseHandle(handle_); }

uint64_t LogFile::size() const {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(handle_, &sz)) fail("size", path_);
    return static_cast<uint64_t>(sz.QuadPart);
}

std::string LogFile::read_at(uint64_t offset, size_t len) const {
    std::string buf(len, '\0');
    size_t done = 0;
    while (done < len) {
        OVERLAPPED ov = at_offset(offset + done);
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(len - done, 1u << 30));
        if (!ReadFile(handle_, buf.data() + done, want, &got, &ov) || got == 0) fail("read", path_);
        done += got;
    }
    return buf;
}

void LogFile::write_at(uint64_t offset, std::string_view data) {
    size_t done = 0;
    while (done < data.size()) {
        OVERLAPPED ov = at_offset(offset + done);
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 30));
        if (!WriteFile(handle_, data.data() + done, want, &wrote, &ov)) fail("write", path_);
        done += wrote;
    }
}

void LogFile::sync() {
    if (!FlushFileBuffers(handle_)) fail("sync", path_);
}

void LogFile::truncate(uint64_t new_size) {
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(new_size);
    if (!SetFilePointerEx(handle_, pos, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) fail("truncate", path_);
}

#else  // POSIX

namespace {
[[noreturn]] void fail(const std::string& what, const std::filesystem::path& path) {
    throw IoError(what + " failed for " + path.string() + ": " + std::strerror(errno));
}
}  // namespace

LogFile::LogFile(const std::filesystem::path& path) : path_(path) {
    const bool existed = std::filesystem::exists(path);
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) fail("open", path_);
    if (!existed) {
        // A new file's directory entry is only durable once the directory is
        // synced. Without this, a crash could lose the whole file.
        const auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
        const int dfd = ::open(dir.c_str(), O_RDONLY);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }
}

LogFile::~LogFile() { ::close(fd_); }

uint64_t LogFile::size() const {
    struct stat st;
    if (::fstat(fd_, &st) != 0) fail("size", path_);
    return static_cast<uint64_t>(st.st_size);
}

std::string LogFile::read_at(uint64_t offset, size_t len) const {
    std::string buf(len, '\0');
    size_t done = 0;
    while (done < len) {
        const ssize_t n = ::pread(fd_, buf.data() + done, len - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail("read", path_);
        done += static_cast<size_t>(n);
    }
    return buf;
}

void LogFile::write_at(uint64_t offset, std::string_view data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::pwrite(fd_, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) fail("write", path_);
        done += static_cast<size_t>(n);
    }
}

void LogFile::sync() {
    if (::fdatasync(fd_) != 0) fail("sync", path_);
}

void LogFile::truncate(uint64_t new_size) {
    if (::ftruncate(fd_, static_cast<off_t>(new_size)) != 0) fail("truncate", path_);
}

#endif

std::string LogFile::read_all() const { return read_at(0, static_cast<size_t>(size())); }

}  // namespace forgedb
