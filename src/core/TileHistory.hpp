#pragma once
// ChromaStudio Engine · core · sparse tile table with transactional undo/redo (spec §2, §10.2).
//
// A missing entry is a transparent tile. A command records, per touched tile, the immutable version
// before the first write and the version after commit, plus optionally the document metadata before
// and after (layer properties travel in the same transaction); undo and redo only switch references.
//   * Changes whose before and after are the same object collapse; an empty command keeps redo.
//   * The redo branch is cut only when a new non-empty command commits.
//   * Undo at cursor = 0 and redo at cursor = count return nullptr ("no action").
//   * Over budget, whole oldest commands are dropped; the newest one always stays.
// `Ref` is a nullable shared handle (std::shared_ptr<const Tile> in the renderer), so equal content
// shared by consecutive versions is stored once. The byte count is conservative (per reference).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chroma::core {

struct TileKey {
    std::uint64_t layer = 0;
    std::int32_t tx = 0;
    std::int32_t ty = 0;
    friend bool operator==(const TileKey&, const TileKey&) = default;
};

struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const noexcept {
        std::uint64_t h = k.layer * 0x9E3779B97F4A7C15ull;
        const std::uint64_t xy = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.tx)) << 32) |
                                 static_cast<std::uint32_t>(k.ty);
        h ^= xy + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

/// Metadata placeholder for histories that carry only tiles.
struct NoMeta {
    friend bool operator==(const NoMeta&, const NoMeta&) = default;
};

template <class Ref, class Meta = NoMeta>
class TileHistory {
public:
    struct Change {
        TileKey key;
        Ref before;  // empty: the tile was transparent
        Ref after;
    };

    TileHistory(std::size_t budgetBytes, std::size_t tileBytes) : budget_(budgetBytes), tileBytes_(tileBytes) {}

    [[nodiscard]] Ref get(const TileKey& key) const {
        const auto it = tiles_.find(key);
        return it == tiles_.end() ? Ref{} : it->second;
    }

    [[nodiscard]] const Meta& meta() const noexcept { return meta_; }

    /// Changes the current metadata without a command and without a new revision: state that undo
    /// carries along but that is not an edit by itself (the active layer, §10.2).
    void amendMeta(Meta meta) { meta_ = std::move(meta); }

    /// Publishes one command: tile changes and, optionally, new metadata. Returns false when nothing
    /// changed.
    bool commit(std::vector<Change> changes, std::optional<Meta> meta = std::nullopt) {
        std::erase_if(changes, [](const Change& c) { return c.before == c.after; });
        const bool metaChanged = meta.has_value() && !(*meta == meta_);
        if (changes.empty() && !metaChanged) return false;
        while (commands_.size() > cursor_) {
            bytes_ -= commands_.back().bytes;
            commands_.pop_back();
        }
        Command command;
        command.changes = std::move(changes);
        command.revision = ++lastRevision_;
        command.bytes = weigh(command.changes);
        command.hasMeta = metaChanged;
        if (metaChanged) {
            command.metaBefore = meta_;
            command.metaAfter = *meta;
            meta_ = *meta;
        }
        apply(command.changes, true);
        bytes_ += command.bytes;
        commands_.push_back(std::move(command));
        cursor_ = commands_.size();
        enforceBudget();
        return true;
    }

    /// The tile changes that were switched back, or nullptr when there is nothing to undo.
    const std::vector<Change>* undo() {
        if (cursor_ == 0) return nullptr;
        const Command& command = commands_[--cursor_];
        apply(command.changes, false);
        if (command.hasMeta) meta_ = command.metaBefore;
        return &command.changes;
    }

    /// The tile changes that were applied again, or nullptr when there is nothing to redo.
    const std::vector<Change>* redo() {
        if (cursor_ == commands_.size()) return nullptr;
        const Command& command = commands_[cursor_++];
        apply(command.changes, true);
        if (command.hasMeta) meta_ = command.metaAfter;
        return &command.changes;
    }

    void clear() { replaceAll({}); }

    /// Starts a new document state (new / open): no history, these tiles and metadata current.
    void replaceAll(std::vector<std::pair<TileKey, Ref>> tiles, Meta meta = {}) {
        tiles_.clear();
        for (auto& [key, ref] : tiles) {
            if (ref) tiles_[key] = std::move(ref);
        }
        meta_ = std::move(meta);
        commands_.clear();
        cursor_ = 0;
        bytes_ = 0;
        baseRevision_ = ++lastRevision_;
    }

    /// Identifies the current state: equal revisions mean equal documents (undo back to the saved
    /// state makes the document clean again, spec §10.1).
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return cursor_ == 0 ? baseRevision_ : commands_[cursor_ - 1].revision;
    }

    template <class Fn>
    void forEachTile(Fn&& fn) const {
        for (const auto& [key, ref] : tiles_) fn(key, ref);
    }

    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }

private:
    struct Command {
        std::vector<Change> changes;
        std::size_t bytes = 0;
        std::uint64_t revision = 0;
        bool hasMeta = false;
        Meta metaBefore{};
        Meta metaAfter{};
    };

    [[nodiscard]] std::size_t weigh(const std::vector<Change>& changes) const {
        std::size_t refs = 0;
        for (const Change& c : changes) refs += (c.before ? 1u : 0u) + (c.after ? 1u : 0u);
        return refs * tileBytes_;
    }

    void apply(const std::vector<Change>& changes, bool forward) {
        for (const Change& c : changes) {
            const Ref& version = forward ? c.after : c.before;
            if (version) {
                tiles_[c.key] = version;
            } else {
                tiles_.erase(c.key);
            }
        }
    }

    void enforceBudget() {
        while (bytes_ > budget_ && commands_.size() > 1 && cursor_ > 1) {
            bytes_ -= commands_.front().bytes;
            baseRevision_ = commands_.front().revision;  // cursor 0 now means "after this command"
            commands_.pop_front();
            --cursor_;
        }
    }

    std::unordered_map<TileKey, Ref, TileKeyHash> tiles_;
    Meta meta_{};
    std::deque<Command> commands_;
    std::size_t cursor_ = 0;
    std::size_t bytes_ = 0;
    std::uint64_t lastRevision_ = 1;
    std::uint64_t baseRevision_ = 1;
    std::size_t budget_;
    std::size_t tileBytes_;
};

}  // namespace chroma::core
