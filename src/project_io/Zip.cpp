#include "project_io/Zip.hpp"

#include "project_io/Crc32.hpp"
#include "project_io/Inflate.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace chroma::project {
namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034B50;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014B50;
constexpr std::uint32_t kEndSignature = 0x06054B50;
constexpr std::uint32_t kZip64EndSignature = 0x06064B50;
constexpr std::uint32_t kZip64LocatorSignature = 0x07064B50;
constexpr std::uint16_t kZip64ExtraId = 0x0001;
constexpr std::uint16_t kVersion = 45;  // 4.5: ZIP64
constexpr std::uint16_t kFlagEncrypted = 0x0001;
constexpr std::uint16_t kFlagStrongEncryption = 0x0040;
constexpr std::uint16_t kFlagUtf8 = 0x0800;
constexpr std::uint32_t kMax32 = 0xFFFFFFFFu;
constexpr std::uint16_t kMax16 = 0xFFFFu;
constexpr std::size_t kWriteBuffer = std::size_t{1} << 20;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

void put16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
    out.push_back(static_cast<std::byte>(v >> 8));
}
void put32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void put64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

std::uint16_t get16(const std::byte* p) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8));
}
std::uint32_t get32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<std::uint32_t>(p[i]);
    return v;
}
std::uint64_t get64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    return v;
}

bool validUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size()) return false;
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        const std::uint32_t minimum = n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u;
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

// Relative, forward slashes, no empty, "." or ".." segments, no drive letters or control characters.
bool safeName(std::string_view name) {
    if (name.empty() || name.size() > 4096 || name.front() == '/') return false;
    for (const char c : name) {
        if (c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = std::min(name.find('/', start), name.size());
        const std::string_view segment = name.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") return false;
        start = end + 1;
    }
    return true;
}

std::string systemMessage(DWORD code) {
    return std::format("ошибка Windows {}", code);
}

}  // namespace

// ── Writer ───────────────────────────────────────────────────────────────────

ZipWriter::ZipWriter(HANDLE file) : file_(file) {
    buffer_.reserve(kWriteBuffer + 65536);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    dosTime_ = static_cast<std::uint16_t>((now.wHour << 11) | (now.wMinute << 5) | (now.wSecond / 2));
    dosDate_ = static_cast<std::uint16_t>(((std::max<int>(now.wYear, 1980) - 1980) << 9) | (now.wMonth << 5) | now.wDay);
}

void ZipWriter::write(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::byte*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
    offset_ += size;
    if (buffer_.size() >= kWriteBuffer) flushBuffer();
}

void ZipWriter::flushBuffer() {
    std::size_t done = 0;
    while (done < buffer_.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(buffer_.size() - done, std::size_t{1} << 30));
        DWORD written = 0;
        if (!WriteFile(file_, buffer_.data() + done, chunk, &written, nullptr) || written != chunk) {
            const DWORD code = GetLastError();
            fail(code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL ? "на диске закончилось место"
                                                                           : "запись не удалась: " + systemMessage(code));
        }
        done += chunk;
    }
    buffer_.clear();
}

void ZipWriter::add(std::string_view name, std::span<const std::byte> data) {
    if (finished_) fail("zip: add after finish");
    if (!safeName(name) || name.size() > kMax16) fail("zip: invalid entry name");
    ZipEntry entry;
    entry.name = std::string(name);
    entry.flags = kFlagUtf8;
    entry.method = 0;
    entry.crc = crc32(data);
    entry.compressedSize = data.size();
    entry.uncompressedSize = data.size();
    entry.localHeaderOffset = offset_;

    const bool zip64Size = data.size() >= kMax32;
    std::vector<std::byte> header;
    header.reserve(30 + name.size() + 20);
    put32(header, kLocalHeaderSignature);
    put16(header, kVersion);
    put16(header, entry.flags);
    put16(header, entry.method);
    put16(header, dosTime_);
    put16(header, dosDate_);
    put32(header, entry.crc);
    put32(header, zip64Size ? kMax32 : static_cast<std::uint32_t>(data.size()));
    put32(header, zip64Size ? kMax32 : static_cast<std::uint32_t>(data.size()));
    put16(header, static_cast<std::uint16_t>(name.size()));
    put16(header, zip64Size ? 20 : 0);
    header.insert(header.end(), reinterpret_cast<const std::byte*>(name.data()),
                  reinterpret_cast<const std::byte*>(name.data()) + name.size());
    if (zip64Size) {
        put16(header, kZip64ExtraId);
        put16(header, 16);
        put64(header, data.size());
        put64(header, data.size());
    }
    write(header.data(), header.size());
    write(data.data(), data.size());
    entries_.push_back(std::move(entry));
}

void ZipWriter::finish() {
    if (finished_) return;
    const std::uint64_t directoryStart = offset_;
    for (const ZipEntry& e : entries_) {
        const bool bigSize = e.uncompressedSize >= kMax32;
        const bool bigOffset = e.localHeaderOffset >= kMax32;
        std::vector<std::byte> extra;
        if (bigSize || bigOffset) {
            put16(extra, kZip64ExtraId);
            put16(extra, static_cast<std::uint16_t>(8 * ((bigSize ? 2 : 0) + (bigOffset ? 1 : 0))));
            if (bigSize) {
                put64(extra, e.uncompressedSize);
                put64(extra, e.compressedSize);
            }
            if (bigOffset) put64(extra, e.localHeaderOffset);
        }
        std::vector<std::byte> header;
        header.reserve(46 + e.name.size() + extra.size());
        put32(header, kCentralHeaderSignature);
        put16(header, kVersion);  // made by: 4.5, MS-DOS attributes
        put16(header, kVersion);
        put16(header, e.flags);
        put16(header, e.method);
        put16(header, dosTime_);
        put16(header, dosDate_);
        put32(header, e.crc);
        put32(header, bigSize ? kMax32 : static_cast<std::uint32_t>(e.compressedSize));
        put32(header, bigSize ? kMax32 : static_cast<std::uint32_t>(e.uncompressedSize));
        put16(header, static_cast<std::uint16_t>(e.name.size()));
        put16(header, static_cast<std::uint16_t>(extra.size()));
        put16(header, 0);  // comment
        put16(header, 0);  // disk
        put16(header, 0);  // internal attributes
        put32(header, 0);  // external attributes: a regular file
        put32(header, bigOffset ? kMax32 : static_cast<std::uint32_t>(e.localHeaderOffset));
        header.insert(header.end(), reinterpret_cast<const std::byte*>(e.name.data()),
                      reinterpret_cast<const std::byte*>(e.name.data()) + e.name.size());
        header.insert(header.end(), extra.begin(), extra.end());
        write(header.data(), header.size());
    }
    const std::uint64_t directorySize = offset_ - directoryStart;
    const std::uint64_t count = entries_.size();

    std::vector<std::byte> tail;
    const std::uint64_t zip64End = offset_;
    put32(tail, kZip64EndSignature);
    put64(tail, 44);
    put16(tail, kVersion);
    put16(tail, kVersion);
    put32(tail, 0);
    put32(tail, 0);
    put64(tail, count);
    put64(tail, count);
    put64(tail, directorySize);
    put64(tail, directoryStart);
    put32(tail, kZip64LocatorSignature);
    put32(tail, 0);
    put64(tail, zip64End);
    put32(tail, 1);
    put32(tail, kEndSignature);
    put16(tail, 0);
    put16(tail, 0);
    put16(tail, count >= kMax16 ? kMax16 : static_cast<std::uint16_t>(count));
    put16(tail, count >= kMax16 ? kMax16 : static_cast<std::uint16_t>(count));
    put32(tail, directorySize >= kMax32 ? kMax32 : static_cast<std::uint32_t>(directorySize));
    put32(tail, directoryStart >= kMax32 ? kMax32 : static_cast<std::uint32_t>(directoryStart));
    put16(tail, 0);
    write(tail.data(), tail.size());
    flushBuffer();
    finished_ = true;
}

// ── Reader ───────────────────────────────────────────────────────────────────

ZipReader::ZipReader(const std::filesystem::path& path, const ZipLimits& limits) : limits_(limits) {
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) fail("не удалось открыть файл: " + systemMessage(GetLastError()));
    try {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size)) fail("не удалось узнать размер файла");
        size_ = static_cast<std::uint64_t>(size.QuadPart);
        parse();
    } catch (...) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        throw;
    }
}

ZipReader::~ZipReader() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

void ZipReader::readAt(std::uint64_t offset, void* data, std::size_t size) const {
    auto* out = static_cast<std::byte*>(data);
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, std::size_t{1} << 30));
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        if (!ReadFile(file_, out, chunk, &read, &at) || read != chunk) fail("файл обрывается раньше времени");
        out += chunk;
        offset += chunk;
        size -= chunk;
    }
}

const ZipEntry* ZipReader::find(std::string_view name) const {
    const auto it = index_.find(std::string(name));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

void ZipReader::parse() {
    if (size_ < 22) fail("это не ZIP-файл: слишком короткий");
    const std::size_t tailSize = static_cast<std::size_t>(std::min<std::uint64_t>(size_, 22 + 65535));
    std::vector<std::byte> tail(tailSize);
    readAt(size_ - tailSize, tail.data(), tailSize);

    std::size_t eocd = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = tailSize - 22 + 1; i-- > 0;) {
        if (get32(&tail[i]) == kEndSignature && get16(&tail[i + 20]) == tailSize - i - 22) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::numeric_limits<std::size_t>::max()) fail("это не ZIP-файл: нет конца центрального каталога");
    const std::byte* end = &tail[eocd];
    if (get16(end + 4) != 0 || get16(end + 6) != 0) fail("многотомные архивы не поддерживаются");
    std::uint64_t count = get16(end + 10);
    std::uint64_t directorySize = get32(end + 12);
    std::uint64_t directoryOffset = get32(end + 16);
    const std::uint64_t eocdOffset = size_ - tailSize + eocd;
    std::uint64_t bound = eocdOffset;

    if (eocdOffset >= 20) {
        std::byte locator[20];
        readAt(eocdOffset - 20, locator, sizeof(locator));
        if (get32(locator) == kZip64LocatorSignature) {
            const std::uint64_t zip64End = get64(locator + 8);
            if (zip64End > eocdOffset - 20 || eocdOffset - 20 - zip64End < 56) fail("повреждена запись ZIP64");
            std::byte record[56];
            readAt(zip64End, record, sizeof(record));
            if (get32(record) != kZip64EndSignature) fail("повреждена запись ZIP64");
            if (get32(record + 16) != 0 || get32(record + 20) != 0) fail("многотомные архивы не поддерживаются");
            count = get64(record + 32);
            directorySize = get64(record + 40);
            directoryOffset = get64(record + 48);
            bound = zip64End;
        }
    }

    // Limits before anything is allocated (spec §10.3).
    if (count > limits_.maxEntries) fail(std::format("слишком много записей: {}", count));
    if (directorySize > limits_.maxCentralDirectory) fail("слишком большой центральный каталог");
    if (directoryOffset > bound || directorySize > bound - directoryOffset) fail("центральный каталог вне файла");
    centralDirectoryOffset_ = directoryOffset;

    std::vector<std::byte> directory(static_cast<std::size_t>(directorySize));
    readAt(directoryOffset, directory.data(), directory.size());
    entries_.reserve(static_cast<std::size_t>(count));
    std::uint64_t totalUncompressed = 0;
    std::size_t pos = 0;
    for (std::uint64_t k = 0; k < count; ++k) {
        if (directory.size() - pos < 46) fail("центральный каталог обрывается");
        const std::byte* h = &directory[pos];
        if (get32(h) != kCentralHeaderSignature) fail("повреждён центральный каталог");
        const std::uint16_t madeBy = get16(h + 4);
        ZipEntry e;
        e.flags = get16(h + 8);
        e.method = get16(h + 10);
        e.crc = get32(h + 16);
        e.compressedSize = get32(h + 20);
        e.uncompressedSize = get32(h + 24);
        const std::size_t nameLength = get16(h + 28);
        const std::size_t extraLength = get16(h + 30);
        const std::size_t commentLength = get16(h + 32);
        const std::uint16_t disk = get16(h + 34);
        const std::uint32_t external = get32(h + 38);
        e.localHeaderOffset = get32(h + 42);
        if (directory.size() - pos - 46 < nameLength + extraLength + commentLength) fail("центральный каталог обрывается");
        e.name.assign(reinterpret_cast<const char*>(h + 46), nameLength);
        const std::byte* extra = h + 46 + nameLength;

        // ZIP64 extended information: present only for the fields set to all-ones.
        for (std::size_t x = 0; x + 4 <= extraLength;) {
            const std::uint16_t id = get16(extra + x);
            const std::size_t length = get16(extra + x + 2);
            if (x + 4 + length > extraLength) fail("повреждено дополнительное поле записи");
            if (id == kZip64ExtraId) {
                std::size_t f = x + 4;
                const auto take = [&](std::uint64_t& field) {
                    if (f + 8 > x + 4 + length) fail("повреждено поле ZIP64");
                    field = get64(extra + f);
                    f += 8;
                };
                if (e.uncompressedSize == kMax32) take(e.uncompressedSize);
                if (e.compressedSize == kMax32) take(e.compressedSize);
                if (e.localHeaderOffset == kMax32) take(e.localHeaderOffset);
            }
            x += 4 + length;
        }

        if (disk != 0 && disk != kMax16) fail("многотомные архивы не поддерживаются");
        if (e.flags & (kFlagEncrypted | kFlagStrongEncryption)) fail("зашифрованные записи не поддерживаются");
        if (e.method != 0 && e.method != 8) fail(std::format("метод сжатия {} не поддерживается", e.method));
        const bool asciiName = std::all_of(e.name.begin(), e.name.end(), [](char c) {
            return static_cast<unsigned char>(c) < 0x80;
        });
        if (!asciiName && (!(e.flags & kFlagUtf8) || !validUtf8(e.name))) fail("имя записи не в UTF-8");
        if (!safeName(e.name)) fail("недопустимый путь в архиве: " + e.name);
        const bool unixHost = (madeBy >> 8) == 3;
        if (unixHost && ((external >> 16) & 0xF000u) == 0xA000u) fail("символические ссылки не допускаются: " + e.name);
        if (e.method == 0 && e.compressedSize != e.uncompressedSize) fail("размеры несжатой записи не совпадают");
        if (e.localHeaderOffset > directoryOffset || e.compressedSize > directoryOffset - e.localHeaderOffset) {
            fail("запись выходит за пределы архива: " + e.name);
        }
        if (e.uncompressedSize > limits_.maxTotalUncompressed - totalUncompressed) {
            fail("распакованный проект больше 32 ГиБ");
        }
        totalUncompressed += e.uncompressedSize;
        if (!index_.emplace(e.name, entries_.size()).second) fail("повтор записи в архиве: " + e.name);
        entries_.push_back(std::move(e));
        pos += 46 + nameLength + extraLength + commentLength;
    }
    if (pos != directory.size()) fail("лишние данные в центральном каталоге");
}

std::vector<std::byte> ZipReader::read(const ZipEntry& entry) const {
    if (entry.uncompressedSize > limits_.maxEntryInMemory) fail("запись слишком велика: " + entry.name);
    std::byte header[30];
    readAt(entry.localHeaderOffset, header, sizeof(header));
    if (get32(header) != kLocalHeaderSignature) fail("повреждён локальный заголовок: " + entry.name);
    const std::uint64_t dataStart = entry.localHeaderOffset + 30 + get16(header + 26) + get16(header + 28);
    if (dataStart > centralDirectoryOffset_ || entry.compressedSize > centralDirectoryOffset_ - dataStart) {
        fail("данные записи выходят за пределы архива: " + entry.name);
    }
    std::vector<std::byte> data(static_cast<std::size_t>(entry.compressedSize));
    readAt(dataStart, data.data(), data.size());
    if (entry.method == 8) {
        try {
            data = inflateRaw(data, static_cast<std::size_t>(entry.uncompressedSize));
        } catch (const std::exception& error) {
            fail(entry.name + ": " + error.what());
        }
    }
    if (crc32(data) != entry.crc) fail("не совпадает CRC-32: " + entry.name);
    return data;
}

}  // namespace chroma::project
