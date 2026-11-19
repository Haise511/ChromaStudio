#include "project_io/Journal.hpp"

#include "project_io/Assets.hpp"
#include "project_io/Crc32.hpp"
#include "project_io/Json.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string_view>

namespace chroma::project {
namespace {

namespace fs = std::filesystem;
using json::Value;

constexpr std::uint32_t kFileMagic = 0x314A5343;    // "CSJ1"
constexpr std::uint32_t kRecordMagic = 0x524A5343;  // "CSJR"
constexpr std::uint32_t kCommitMagic = 0x434A5343;  // "CSJC"
constexpr std::uint32_t kMaxHeaderBytes = 1u << 20;
constexpr std::size_t kMaxNameBytes = 512;
constexpr std::uint64_t kMaxFileBytes = std::uint64_t{32} << 30;  // the §10.3 project limit

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

template <class T>
void putLe(std::vector<std::byte>& out, T value) {  // x64 is little-endian
    const auto* b = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), b, b + sizeof value);
}

void putBytes(std::vector<std::byte>& out, std::span<const std::byte> data) { out.insert(out.end(), data.begin(), data.end()); }

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }

bool writeAll(HANDLE file, std::span<const std::byte> data) {
    std::size_t done = 0;
    while (done < data.size()) {
        DWORD wrote = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, std::size_t{1} << 30));
        if (!WriteFile(file, data.data() + done, chunk, &wrote, nullptr) || wrote == 0) return false;
        done += wrote;
    }
    return true;
}

class Cursor {
public:
    explicit Cursor(std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - at_; }
    template <class T>
    T le() {
        T value{};
        std::memcpy(&value, take(sizeof value).data(), sizeof value);
        return value;
    }
    std::span<const std::byte> take(std::uint64_t size) {
        if (size > remaining()) fail("запись журнала оборвана");
        const auto out = bytes_.subspan(at_, static_cast<std::size_t>(size));
        at_ += static_cast<std::size_t>(size);
        return out;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t at_ = 0;
};

std::vector<JournalChange> decodePayload(std::span<const std::byte> payload) {
    Cursor c(payload);
    const auto count = c.le<std::uint32_t>();
    if (count > 1'000'000) fail("слишком много изменений в записи журнала");
    std::vector<JournalChange> changes;
    changes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto op = c.le<std::uint8_t>();
        const auto nameLength = c.le<std::uint16_t>();
        if (nameLength == 0 || nameLength > kMaxNameBytes) fail("недопустимое имя в записи журнала");
        const auto name = c.take(nameLength);
        JournalChange change;
        change.name.assign(reinterpret_cast<const char*>(name.data()), name.size());
        if (op == 1) {
            const auto size = c.le<std::uint64_t>();
            const auto data = c.take(size);
            change.data.emplace(data.begin(), data.end());
        } else if (op != 2) {
            fail("неизвестная операция в записи журнала");
        }
        changes.push_back(std::move(change));
    }
    if (c.remaining() != 0) fail("лишние байты в записи журнала");
    return changes;
}

}  // namespace

std::string newSessionId() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) return "00000000-0000-0000-0000-000000000000";
    return std::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", g.Data1, g.Data2, g.Data3,
                       g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

std::unique_ptr<Journal> Journal::create(const fs::path& file, const JournalHeader& header, std::string& error) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    // Read sharing only: a second instance cannot take (or delete) a live journal.
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = std::format("не удалось создать журнал восстановления «{}» (ошибка Windows {})", utf8(file), GetLastError());
        return nullptr;
    }
    std::unique_ptr<Journal> journal(new Journal());
    journal->file_ = h;
    journal->path_ = file;
    journal->header_ = header;
    Value v = Value::object();
    v.set("session", Value::string(header.session));
    v.set("name", Value::string(header.name));
    v.set("base", Value::string(header.base));
    v.set("baseSha256", Value::string(header.baseSha256));
    v.set("created", Value::number(static_cast<double>(header.created)));
    const std::string text = json::serialize(v);
    std::vector<std::byte> out;
    putLe(out, kFileMagic);
    putLe(out, static_cast<std::uint32_t>(text.size()));
    putBytes(out, std::as_bytes(std::span(text)));
    putLe(out, crc32(std::as_bytes(std::span(text))));
    if (!writeAll(h, out) || !FlushFileBuffers(h)) {
        error = std::format("не удалось записать журнал восстановления (ошибка Windows {})", GetLastError());
        journal->remove();
        return nullptr;
    }
    journal->bytes_ = out.size();
    return journal;
}

Journal::~Journal() {
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
}

std::string Journal::append(const std::vector<JournalChange>& changes) {
    if (!file_) return "журнал закрыт";
    std::vector<std::byte> payload;
    putLe(payload, static_cast<std::uint32_t>(changes.size()));
    for (const JournalChange& c : changes) {
        if (c.name.empty() || c.name.size() > kMaxNameBytes) return "недопустимое имя записи: " + c.name;
        putLe(payload, static_cast<std::uint8_t>(c.data ? 1 : 2));
        putLe(payload, static_cast<std::uint16_t>(c.name.size()));
        putBytes(payload, std::as_bytes(std::span(c.name)));
        if (c.data) {
            putLe(payload, static_cast<std::uint64_t>(c.data->size()));
            putBytes(payload, *c.data);
        }
    }
    std::vector<std::byte> head;
    putLe(head, kRecordMagic);
    putLe(head, seq_ + 1);
    putLe(head, static_cast<std::uint64_t>(payload.size()));
    putLe(head, crc32(payload));
    std::vector<std::byte> tail;
    putLe(tail, kCommitMagic);
    HANDLE h = static_cast<HANDLE>(file_);
    // The commit marker goes last: a record cut anywhere before it is not replayed.
    if (!writeAll(h, head) || !writeAll(h, payload) || !writeAll(h, tail) || !FlushFileBuffers(h)) {
        return std::format("не удалось дописать журнал восстановления (ошибка Windows {})", GetLastError());
    }
    ++seq_;
    bytes_ += head.size() + payload.size() + tail.size();
    return {};
}

void Journal::remove() {
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
    file_ = nullptr;
    DeleteFileW(path_.c_str());
}

std::optional<RecoveredJournal> readJournal(const fs::path& file, std::string& error) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        error = code == ERROR_SHARING_VIOLATION ? "журнал открыт другим запущенным экземпляром"
                                                : std::format("не удалось открыть журнал (ошибка Windows {})", code);
        return std::nullopt;
    }
    std::vector<std::byte> bytes;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) <= kMaxFileBytes;
    if (ok) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t done = 0;
        while (ok && done < bytes.size()) {
            DWORD got = 0;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, std::size_t{1} << 30));
            ok = ReadFile(h, bytes.data() + done, chunk, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(h);
    if (!ok) {
        error = "не удалось прочитать журнал";
        return std::nullopt;
    }
    RecoveredJournal journal;
    journal.path = file;
    Cursor c(bytes);
    try {
        if (c.le<std::uint32_t>() != kFileMagic) fail("не журнал ChromaStudio");
        const auto length = c.le<std::uint32_t>();
        if (length > kMaxHeaderBytes) fail("заголовок журнала слишком велик");
        const auto text = c.take(length);
        if (c.le<std::uint32_t>() != crc32(text)) fail("заголовок журнала повреждён");
        const Value v = json::parse(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
        const auto str = [&](std::string_view key) {
            const Value* f = v.find(key);
            if (!f || !f->isString()) fail(std::format("в заголовке журнала нет поля {}", key));
            return f->asString();
        };
        journal.header.session = str("session");
        journal.header.name = str("name");
        journal.header.base = str("base");
        journal.header.baseSha256 = str("baseSha256");
        const Value* created = v.find("created");
        journal.header.created = created && created->isNumber() ? static_cast<std::int64_t>(created->asNumber()) : 0;
        if (!journal.header.base.empty() && !core::isSha256(journal.header.baseSha256)) fail("в заголовке журнала нет хеша основы");
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
    // The continuous prefix of complete records: stop at the first one that is cut or damaged.
    while (c.remaining() > 0) {
        try {
            if (c.le<std::uint32_t>() != kRecordMagic) fail("повреждённая метка записи");
            const auto seq = c.le<std::uint64_t>();
            if (seq != journal.records.size() + 1) fail(std::format("пропуск в номерах записей ({} после {})", seq, journal.records.size()));
            const auto length = c.le<std::uint64_t>();
            const auto crc = c.le<std::uint32_t>();
            const auto payload = c.take(length);
            if (crc32(payload) != crc) fail(std::format("запись {}: CRC не совпадает", seq));
            if (c.le<std::uint32_t>() != kCommitMagic) fail(std::format("запись {}: нет маркера commit", seq));
            journal.records.push_back(decodePayload(payload));
        } catch (const std::exception& e) {
            journal.tail = e.what();
            break;
        }
    }
    return journal;
}

LoadResult replayJournal(const RecoveredJournal& journal) {
    Entries entries;
    if (!journal.header.base.empty()) {
        const fs::path base = fromUtf8(journal.header.base);
        const auto bytes = readFileBytes(base);
        if (!bytes) return {std::nullopt, "основа журнала «" + journal.header.base + "» не найдена", std::nullopt};
        if (sha256Hex(*bytes) != journal.header.baseSha256) {
            return {std::nullopt, "файл «" + journal.header.base + "» изменён после начала журнала", std::nullopt};
        }
        LoadResult loaded = loadProject(base);
        if (!loaded.snapshot) return loaded;
        forEachEntry(*loaded.snapshot, [&](const std::string& name, std::span<const std::byte> data) {
            entries[name].assign(data.begin(), data.end());
        });
    }
    for (const auto& record : journal.records) {
        for (const JournalChange& c : record) {
            if (c.data) {
                entries[c.name] = *c.data;
            } else {
                entries.erase(c.name);
            }
        }
    }
    entries.erase("thumbnail.png");  // regenerated on the next save
    return loadFromEntries(entries);
}

JournalState JournalState::of(const ProjectSnapshot& snapshot) {
    return JournalState().diff(snapshot).second;
}

std::pair<std::vector<JournalChange>, JournalState> JournalState::diff(const ProjectSnapshot& snapshot) const {
    std::pair<std::vector<JournalChange>, JournalState> out;
    auto& [changes, next] = out;
    for (const TileEntry& t : snapshot.tiles) {
        std::string name = std::format("tiles/{}/{}_{}.bin", t.layer, t.tx, t.ty);
        const auto it = tiles_.find(name);
        if (it == tiles_.end() || it->second != t.data) {
            const auto bytes = std::as_bytes(std::span(t.data->halfs));
            changes.push_back({name, std::vector<std::byte>(bytes.begin(), bytes.end())});
        }
        next.tiles_.emplace(std::move(name), t.data);
    }
    forEachEntry(snapshot, [&](const std::string& name, std::span<const std::byte> data) {
        if (name.starts_with("tiles/") || name == "thumbnail.png") return;
        const auto it = other_.find(name);
        if (it == other_.end() || !std::equal(it->second.begin(), it->second.end(), data.begin(), data.end())) {
            changes.push_back({name, std::vector<std::byte>(data.begin(), data.end())});
        }
        next.other_.emplace(name, std::vector<std::byte>(data.begin(), data.end()));
    });
    for (const auto& [name, data] : tiles_) {
        if (!next.tiles_.contains(name)) changes.push_back({name, std::nullopt});
    }
    for (const auto& [name, data] : other_) {
        if (!next.other_.contains(name)) changes.push_back({name, std::nullopt});
    }
    return out;
}

}  // namespace chroma::project
