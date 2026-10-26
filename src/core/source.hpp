// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · source.hpp
//  A read-only byte range backed by either a memory-mapped file or an owned
//  buffer. Mapping keeps open() O(1) in file size and lets the OS decide what
//  stays resident, which is what makes 10 GB files open in milliseconds.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace tess {

class Source {
public:
    /// Memory-maps `path` read-only. Throws tess::Error on failure.
    [[nodiscard]] static std::shared_ptr<Source> open_file(const std::string& path);
    /// Owns `data` (used for transcoded UTF-16 and materialised JSONL).
    [[nodiscard]] static std::shared_ptr<Source> from_buffer(std::string data,
                                                             std::string name,
                                                             std::string origin_path = {});

    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    ~Source();

    [[nodiscard]] const char* data() const noexcept { return data_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] bool is_file() const noexcept { return is_file_; }
    /// Last modification time in Unix seconds (0 for buffers).
    [[nodiscard]] std::int64_t mtime_unix() const noexcept { return mtime_; }

    [[nodiscard]] std::string_view view(std::uint64_t offset, std::uint64_t length) const {
        if (offset > size_) return {};
        const std::uint64_t max_len = size_ - offset;
        return std::string_view(data_ + offset, static_cast<std::size_t>(length < max_len ? length : max_len));
    }

    [[nodiscard]] std::string_view all() const { return view(0, size_); }

    /// Full read into memory (used by isolated helpers and tests).
    [[nodiscard]] std::string read_all() const { return std::string(all()); }

private:
    Source() = default;

    const char* data_ = nullptr;
    std::uint64_t size_ = 0;
    std::string name_;
    std::string path_;
    std::string owned_;
    bool is_file_ = false;
    std::int64_t mtime_ = 0;

#if defined(TESSERA_WINDOWS)
    void* file_handle_ = nullptr;  // HANDLE
    void* mapping_handle_ = nullptr;
#else
    int fd_ = -1;
    bool mapped_ = false;
#endif
};

/// Reads a whole file into a string. Throws tess::Error with context on failure.
/// Handles files larger than 2 GiB on Windows, where the CRT's default `ftell`
/// is 32-bit and reports -1 for anything past that boundary.
[[nodiscard]] std::string read_file(const std::string& path);

/// Size in bytes of a regular file; nullopt when it is missing, is a directory,
/// or cannot be inspected. Cheaper than reading, and used to bound cache reads
/// before a byte is allocated.
[[nodiscard]] std::optional<std::uint64_t> file_size(const std::string& path) noexcept;

/// Reads a file only when it is at most `max_bytes`. Returns nullopt rather than
/// throwing when the file is absent, unreadable or larger than the limit — the
/// contract callers want for a cache they must be willing to distrust.
[[nodiscard]] std::optional<std::string> read_file_limited(const std::string& path, std::uint64_t max_bytes);

void write_file(const std::string& path, std::string_view contents);

/// Writes through a temporary file in the same directory and then renames it
/// over the destination, so an interrupted run can never leave a half-written
/// file that a later run mistakes for a complete one.
void write_file_atomic(const std::string& path, std::string_view contents);

} // namespace tess
