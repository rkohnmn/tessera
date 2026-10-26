// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · source.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "core/source.hpp"

#include "common/error.hpp"
#include "common/util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#if defined(TESSERA_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace tess {
namespace {

std::string base_name(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string errno_text() {
#if defined(TESSERA_WINDOWS)
    // Win32 API failures set LastError; CRT failures (fopen and friends) set
    // errno and may leave LastError untouched, so prefer whichever is set.
    const DWORD last = GetLastError();
    if (last != 0) return "system error " + std::to_string(static_cast<long>(last));
    return std::strerror(errno);
#else
    return std::strerror(errno);
#endif
}

// `std::ftell` is 32-bit on Windows and saturates at 2 GiB, which would make
// every large file unreadable there. These two wrappers always move a 64-bit
// offset and are the only seek/tell used for sizes.
int seek_end(std::FILE* f) noexcept {
#if defined(TESSERA_WINDOWS)
    return _fseeki64(f, 0, SEEK_END);
#else
    return ::fseeko(f, 0, SEEK_END);
#endif
}

std::int64_t tell_pos(std::FILE* f) noexcept {
#if defined(TESSERA_WINDOWS)
    return static_cast<std::int64_t>(_ftelli64(f));
#else
    return static_cast<std::int64_t>(::ftello(f));
#endif
}

std::int64_t current_pid() noexcept {
#if defined(TESSERA_WINDOWS)
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

} // namespace

std::shared_ptr<Source> Source::open_file(const std::string& path) {
    auto src = std::shared_ptr<Source>(new Source());
    src->path_ = path;
    src->name_ = base_name(path);
    src->is_file_ = true;

#if defined(TESSERA_WINDOWS)
    const std::wstring wide = utf8_to_wide(path);
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail("cannot open '" + path + "' (" + errno_text() + ")");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) {
        CloseHandle(file);
        fail("cannot stat '" + path + "'");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        fail("cannot determine the size of '" + path + "'");
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(file);
        fail("'" + path + "' is a directory, not a file");
    }

    ULARGE_INTEGER ft{};
    ft.LowPart = info.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = info.ftLastWriteTime.dwHighDateTime;
    // 100-ns ticks since 1601-01-01 -> seconds since 1970-01-01.
    src->mtime_ = static_cast<std::int64_t>(ft.QuadPart / 10000000ull) - 11644473600ll;
    src->size_ = static_cast<std::uint64_t>(size.QuadPart);
    src->file_handle_ = file;

    if (src->size_ > 0) {
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr) {
            CloseHandle(file);
            fail("cannot map '" + path + "' (" + errno_text() + ")");
        }
        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (view == nullptr) {
            CloseHandle(mapping);
            CloseHandle(file);
            fail("cannot map view of '" + path + "' (" + errno_text() + ")");
        }
        src->mapping_handle_ = mapping;
        src->data_ = static_cast<const char*>(view);
    }
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        fail("cannot open '" + path + "' (" + errno_text() + ")");
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        fail("cannot stat '" + path + "' (" + errno_text() + ")");
    }
    if (S_ISDIR(st.st_mode)) {
        ::close(fd);
        fail("'" + path + "' is a directory, not a file");
    }
    src->fd_ = fd;
    src->size_ = static_cast<std::uint64_t>(st.st_size);
    src->mtime_ = static_cast<std::int64_t>(st.st_mtime);

    if (src->size_ > 0) {
        void* view = ::mmap(nullptr, static_cast<std::size_t>(src->size_), PROT_READ, MAP_PRIVATE, fd, 0);
        if (view == MAP_FAILED) {
            // Mapping can fail on network filesystems; fall back to a full read.
            src->owned_ = read_file(path);
            src->size_ = src->owned_.size();
            src->data_ = src->owned_.data();
            src->mapped_ = false;
        } else {
            src->data_ = static_cast<const char*>(view);
            src->mapped_ = true;
        }
    }
#endif
    return src;
}

std::shared_ptr<Source> Source::from_buffer(std::string data, std::string name, std::string origin_path) {
    auto src = std::shared_ptr<Source>(new Source());
    src->path_ = origin_path.empty() ? name : std::move(origin_path);
    src->name_ = std::move(name);
    src->owned_ = std::move(data);
    src->size_ = src->owned_.size();
    src->data_ = src->owned_.data();
    return src;
}

Source::~Source() {
#if defined(TESSERA_WINDOWS)
    if (data_ != nullptr) UnmapViewOfFile(data_);
    if (mapping_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(mapping_handle_));
    if (file_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(file_handle_));
#else
    if (mapped_ && data_ != nullptr) {
        ::munmap(const_cast<char*>(data_), static_cast<std::size_t>(size_));
    }
    if (fd_ >= 0) ::close(fd_);
#endif
}

std::string read_file(const std::string& path) {
    std::FILE* f = fopen_utf8(path, "rb");
    if (f == nullptr) fail("cannot read '" + path + "' (" + errno_text() + ")");
    std::string out;
    if (seek_end(f) != 0) {
        std::fclose(f);
        fail("cannot determine the size of '" + path + "'");
    }
    const std::int64_t size = tell_pos(f);
    if (size < 0) {
        std::fclose(f);
        fail("cannot determine the size of '" + path + "'");
    }
    std::rewind(f);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        const std::size_t got = std::fread(out.data(), 1, out.size(), f);
        if (got != out.size()) {
            std::fclose(f);
            fail("failed while reading '" + path + "'");
        }
    }
    std::fclose(f);
    return out;
}

std::optional<std::uint64_t> file_size(const std::string& path) noexcept {
    try {
#if defined(TESSERA_WINDOWS)
        const std::wstring wide = utf8_to_wide(path);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &info) == 0) return std::nullopt;
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return std::nullopt;
        return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | static_cast<std::uint64_t>(info.nFileSizeLow);
#else
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) return std::nullopt;
        if (!S_ISREG(st.st_mode)) return std::nullopt;
        return static_cast<std::uint64_t>(st.st_size);
#endif
    } catch (...) {
        return std::nullopt; // utf8_to_wide throws on a path that is not UTF-8
    }
}

std::optional<std::string> read_file_limited(const std::string& path, std::uint64_t max_bytes) {
    const std::optional<std::uint64_t> size = file_size(path);
    if (!size.has_value() || *size > max_bytes) return std::nullopt;
    try {
        return read_file(path);
    } catch (const Error&) {
        return std::nullopt;
    }
}

void write_file(const std::string& path, std::string_view contents) {
    std::FILE* f = fopen_utf8(path, "wb");
    if (f == nullptr) fail("cannot create '" + path + "' (" + errno_text() + ")");
    const std::size_t written = contents.empty() ? 0 : std::fwrite(contents.data(), 1, contents.size(), f);
    const bool ok = (written == contents.size());
    if (std::fclose(f) != 0 || !ok) fail("failed while writing '" + path + "'");
}

void write_file_atomic(const std::string& path, std::string_view contents) {
    // Same directory as the destination so the rename stays on one filesystem,
    // and a pid suffix so two processes writing the same cache cannot collide.
    const std::string temporary = path + ".tmp-" + std::to_string(current_pid());
    try {
        write_file(temporary, contents);
    } catch (...) {
        std::remove(temporary.c_str());
        throw;
    }
#if defined(TESSERA_WINDOWS)
    const std::wstring from = utf8_to_wide(temporary);
    const std::wstring to = utf8_to_wide(path);
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
        DeleteFileW(from.c_str());
        fail("cannot replace '" + path + "' (" + errno_text() + ")");
    }
#else
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        fail("cannot replace '" + path + "' (" + errno_text() + ")");
    }
#endif
}

} // namespace tess
