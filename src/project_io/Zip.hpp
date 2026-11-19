#pragma once
// ChromaStudio Engine · project_io · ZIP64 container (spec §10.3).
//
// Writer: store method, UTF-8 names (flag bit 11), CRC-32 per entry, sizes known up front (no data
// descriptors), ZIP64 extra fields when an offset passes 4 GiB, and always a ZIP64 end record.
// Reader: central directory first, with every limit checked before anything is allocated: entry
// count, total uncompressed size, entry bounds, duplicate names, encryption, symlinks and unsafe
// paths (absolute, drive letters, backslashes, "." or ".." segments). Methods store and deflate;
// each read verifies its CRC-32.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chroma::project {

struct ZipLimits {
    std::uint64_t maxEntries = 1'000'000;                     // spec §10.3
    std::uint64_t maxTotalUncompressed = std::uint64_t{32} << 30;  // 32 GiB
    std::uint64_t maxEntryInMemory = std::uint64_t{1} << 30;  // one read() buffer
    std::uint64_t maxCentralDirectory = std::uint64_t{512} << 20;
};

struct ZipEntry {
    std::string name;  // UTF-8
    std::uint16_t flags = 0;
    std::uint16_t method = 0;  // 0 store, 8 deflate
    std::uint32_t crc = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint64_t localHeaderOffset = 0;
};

/// Writes entries into an open file handle (not owned), sequentially from offset 0.
class ZipWriter {
public:
    explicit ZipWriter(HANDLE file);
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    void add(std::string_view name, std::span<const std::byte> data);
    /// Central directory and end records; flushes the internal buffer. No further add().
    void finish();
    [[nodiscard]] std::size_t entryCount() const noexcept { return entries_.size(); }

private:
    void write(const void* data, std::size_t size);
    void flushBuffer();

    HANDLE file_;
    std::uint64_t offset_ = 0;
    std::vector<std::byte> buffer_;
    std::vector<ZipEntry> entries_;
    std::uint16_t dosTime_ = 0;
    std::uint16_t dosDate_ = 0;
    bool finished_ = false;
};

class ZipReader {
public:
    /// Opens and validates the central directory; throws std::runtime_error.
    explicit ZipReader(const std::filesystem::path& path, const ZipLimits& limits = {});
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const ZipEntry* find(std::string_view name) const;
    /// Decompresses one entry and verifies its CRC-32; throws std::runtime_error.
    [[nodiscard]] std::vector<std::byte> read(const ZipEntry& entry) const;

private:
    void readAt(std::uint64_t offset, void* data, std::size_t size) const;
    void parse();

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0;
    std::uint64_t centralDirectoryOffset_ = 0;
    ZipLimits limits_;
    std::vector<ZipEntry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
};

}  // namespace chroma::project
