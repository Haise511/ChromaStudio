#pragma once
// ChromaStudio Engine · core · the layer tree of a document (spec §7, §10.2, §10.3).
//
// An ordered tree of raster layers and isolated groups; children are listed bottom to top. Every node
// keeps its ID, parent, name, visibility, opacity, blend mode, clip-to-base, alpha lock and edit lock.
// The tree is a value: each edit works on a copy that the caller publishes as one history command
// together with the tiles it affects (a deleted layer's tiles, a duplicate's shared tiles), so undo
// restores the structure, the properties and the active layer at once.
//
// Rules from §7: a new document holds one transparent raster layer; deleting the last drawing layer
// creates an empty one in the same edit; the active layer is kept by ID and, when it goes, the nearest
// sibling takes over, below first; a node never moves into its own subtree. Limits from §10.3: at
// most 4096 nodes and depth 64.

#include "core/Layer.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chroma::core {

enum class NodeKind : std::uint8_t { Raster, Group, Vector };

struct LayerNode {
    std::uint64_t id = 0;
    std::uint64_t parent = 0;  // 0: the root
    NodeKind kind = NodeKind::Raster;
    LayerProps props;
    std::vector<std::uint64_t> children;  // groups only, bottom to top
    friend bool operator==(const LayerNode&, const LayerNode&) = default;
};

inline constexpr std::size_t kMaxLayerNodes = 4096;
inline constexpr int kMaxLayerDepth = 64;
inline constexpr int kBlendModeCount = 27;

/// §7 table: 0 Normal … 26 Invert.
std::string_view blendModeName(int id) noexcept;

class LayerTree {
public:
    /// A new document: one transparent raster layer «Слой 1» with ID 1, active.
    static LayerTree single();

    struct Entry {
        std::uint64_t id = 0;
        std::uint64_t parent = 0;  // 0: the root
        NodeKind kind = NodeKind::Raster;
        LayerProps props;
    };
    /// From a file's flat list (siblings bottom to top). Checks IDs, parents, cycles and limits;
    /// returns nullopt with a reason in Russian otherwise.
    static std::optional<LayerTree> fromEntries(const std::vector<Entry>& entries, std::uint64_t active,
                                                std::uint64_t nextId, std::string& error);

    // ── Queries ──
    [[nodiscard]] const LayerNode* find(std::uint64_t id) const;
    /// Children of a group, or of the root for 0; empty for unknown IDs and raster layers.
    [[nodiscard]] const std::vector<std::uint64_t>& children(std::uint64_t parent) const;
    [[nodiscard]] std::uint64_t active() const noexcept { return active_; }
    [[nodiscard]] std::uint64_t nextId() const noexcept { return nextId_; }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    /// 1 for children of the root; 0 for unknown IDs.
    [[nodiscard]] int depth(std::uint64_t id) const;
    /// Height of the subtree: 1 for a raster layer or an empty group.
    [[nodiscard]] int height(std::uint64_t id) const;
    [[nodiscard]] bool isInSubtree(std::uint64_t id, std::uint64_t root) const;
    /// Position among its siblings (0 = bottom).
    [[nodiscard]] std::size_t indexOf(std::uint64_t id) const;
    /// The node and all its ancestors are visible.
    [[nodiscard]] bool effectivelyVisible(std::uint64_t id) const;
    /// The node or one of its ancestors is locked.
    [[nodiscard]] bool effectivelyLocked(std::uint64_t id) const;
    /// Every node, parents before children, siblings bottom to top: the file order (§10.3).
    [[nodiscard]] std::vector<std::uint64_t> preorder() const;
    /// Drawing layers (raster and vector) of the subtree rooted at `id` (0: the whole tree), in preorder.
    [[nodiscard]] std::vector<std::uint64_t> rasters(std::uint64_t id = 0) const;
    /// For a clipped node, the base it clips to: the nearest non-clipped sibling below. 0 for an
    /// orphan clip or a node that is not clipped.
    [[nodiscard]] std::uint64_t clipBase(std::uint64_t id) const;

    // ── Edits ── (false or 0 when refused; the tree is then unchanged)
    bool setActive(std::uint64_t id);
    /// A raster layer above the active node, in the active node's parent; it becomes active.
    std::uint64_t addRaster(std::string name = {});
    /// An empty group above the active node; it becomes active.
    std::uint64_t addGroup(std::string name = {});
    /// An empty vector layer above the active node (§8); it becomes active.
    std::uint64_t addVector(std::string name = {});
    /// A fresh document-wide ID for a vector object or knot (§8.1: the owner reserves it).
    std::uint64_t reserveId() { return nextId_++; }
    /// Turns a vector layer into a raster layer (its cached pixels stay with the caller).
    bool rasterize(std::uint64_t id);
    /// Copies the subtree of `id` right above it. `copies` receives (source raster, copy) pairs so the
    /// caller can share their tiles. The copy becomes active.
    std::uint64_t duplicate(std::uint64_t id, std::vector<std::pair<std::uint64_t, std::uint64_t>>& copies);
    /// Removes the subtree of `id`; `removed` receives its raster layers. When no raster layer is left,
    /// an empty one takes the place of the removed node in the same edit.
    bool remove(std::uint64_t id, std::vector<std::uint64_t>& removed);
    /// Moves `id` under `parent` (0 or a group) to sibling position `index` (0 = bottom), counted
    /// without the node itself. Refused into its own subtree, under a raster layer or past depth 64.
    bool move(std::uint64_t id, std::uint64_t parent, std::size_t index);
    /// Alt+↑ / Alt+↓: one position among the siblings; past the end of a group the node leaves it.
    bool step(std::uint64_t id, int direction);
    bool setProps(std::uint64_t id, const LayerProps& props);

    friend bool operator==(const LayerTree&, const LayerTree&) = default;

private:
    LayerNode* node(std::uint64_t id);
    std::vector<std::uint64_t>& siblingsOf(std::uint64_t parent);
    std::uint64_t insertAbove(LayerNode node, std::uint64_t anchor);
    std::uint64_t copySubtree(std::uint64_t source, std::uint64_t parent,
                              std::vector<std::pair<std::uint64_t, std::uint64_t>>& copies);
    [[nodiscard]] std::string freshName(NodeKind kind) const;

    std::map<std::uint64_t, LayerNode> nodes_;
    std::vector<std::uint64_t> root_;  // bottom to top
    std::uint64_t active_ = 0;
    std::uint64_t nextId_ = 1;
};

}  // namespace chroma::core
