#pragma once
// ChromaStudio Engine · project_io · the local recovery journal (spec §10.3).
//
// While a document has unsaved changes the shell appends, every 30 s, one record of the logical
// .rawproj records (§10.3 names) that changed since the previous record, plus the names that were
// removed. A record is framed as
//
//   "CSJR"  u32 LE      magic
//   seq     u64 LE      1, 2, 3, … without gaps
//   length  u64 LE      payload bytes
//   crc     u32 LE      CRC-32 of the payload
//   payload             u32 count, then per change: u8 op (1 put, 2 remove), u16 name length,
//                       UTF-8 name, and for a put u64 length + the bytes
//   "CSJC"  u32 LE      commit marker
//
// after a header "CSJ1", u32 length, JSON {session, name, base, baseSha256, created}, u32 CRC. The
// journal belongs to a session UUID and to the SHA-256 of its base file; a new document has no
// base, so its first record is the full snapshot. The writer keeps the file open with read sharing
// only, so another instance cannot take a live journal. Recovery replays only the continuous prefix
// of complete records over the base and validates the result exactly like an opened file.

#include "project_io/Project.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chroma::project {

struct JournalHeader {
    std::string session;     // UUID of the writing session
    std::string name;        // the document's display name
    std::string base;        // UTF-8 path of the base .rawproj, empty for a new document
    std::string baseSha256;  // of the base file's bytes when the journal started
    std::int64_t created = 0;  // Unix seconds
};

struct JournalChange {
    std::string name;
    std::optional<std::vector<std::byte>> data;  // nullopt: the record was removed
};

class Journal {
public:
    /// Creates `file` (it must not exist) and writes the header; nullptr with a reason on failure.
    static std::unique_ptr<Journal> create(const std::filesystem::path& file, const JournalHeader& header, std::string& error);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    /// Appends one committed record and flushes it to disk; empty on success.
    std::string append(const std::vector<JournalChange>& changes);
    /// Closes and deletes the file.
    void remove();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const JournalHeader& header() const noexcept { return header_; }
    [[nodiscard]] std::uint64_t records() const noexcept { return seq_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

private:
    Journal() = default;

    void* file_ = nullptr;  // HANDLE
    std::filesystem::path path_;
    JournalHeader header_;
    std::uint64_t seq_ = 0;
    std::uint64_t bytes_ = 0;
};

struct RecoveredJournal {
    std::filesystem::path path;
    JournalHeader header;
    std::vector<std::vector<JournalChange>> records;  // the continuous prefix of complete records
    std::string tail;  // why reading stopped before the end of the file, empty when it did not
};

/// Reads a journal that no live session holds; nullopt with a reason (in use, not a journal).
std::optional<RecoveredJournal> readJournal(const std::filesystem::path& file, std::string& error);

/// The base file (checked against its SHA-256) with the records applied, validated like an open.
LoadResult replayJournal(const RecoveredJournal& journal);

/// What a journal already holds, to find the next record cheaply: tiles by their shared data (an
/// unchanged version keeps its pointer), the other records by their bytes.
class JournalState {
public:
    /// The state of a base file just opened: nothing to record yet.
    [[nodiscard]] static JournalState of(const ProjectSnapshot& snapshot);
    /// The changes from this state to `snapshot`, and the state after them (adopt it only once the
    /// record is written).
    [[nodiscard]] std::pair<std::vector<JournalChange>, JournalState> diff(const ProjectSnapshot& snapshot) const;

private:
    std::map<std::string, std::shared_ptr<const core::TileData>> tiles_;
    std::map<std::string, std::vector<std::byte>> other_;
};

/// A new random session UUID (CoCreateGuid), lowercase without braces.
std::string newSessionId();

}  // namespace chroma::project
