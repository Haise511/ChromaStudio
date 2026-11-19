#include "core/LayerTree.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <unordered_set>

namespace chroma::core {

std::string_view blendModeName(int id) noexcept {
    static constexpr std::array<std::string_view, kBlendModeCount> kNames = {
        "Normal",       "Multiply",      "Screen",     "Overlay",     "Add (Linear Dodge)", "Color Dodge",
        "Linear Burn",  "Darken",        "Lighten",    "Color Burn",  "Darker Color",       "Lighter Color",
        "Soft Light",   "Hard Light",    "Vivid Light", "Linear Light", "Pin Light",        "Hard Mix",
        "Difference",   "Exclusion",     "Subtract",   "Divide",      "Hue",                "Saturation",
        "Color",        "Luminosity",    "Invert"};
    return id >= 0 && id < kBlendModeCount ? kNames[static_cast<std::size_t>(id)] : std::string_view{};
}

LayerTree LayerTree::single() {
    LayerTree tree;
    LayerNode layer;
    layer.id = 1;
    layer.props.name = "Слой 1";
    tree.nodes_.emplace(1, std::move(layer));
    tree.root_ = {1};
    tree.active_ = 1;
    tree.nextId_ = 2;
    return tree;
}

std::optional<LayerTree> LayerTree::fromEntries(const std::vector<Entry>& entries, std::uint64_t active,
                                                std::uint64_t nextId, std::string& error) {
    if (entries.empty() || entries.size() > kMaxLayerNodes) {
        error = "дерево слоёв должно содержать от 1 до 4096 узлов";
        return std::nullopt;
    }
    LayerTree tree;
    for (const Entry& e : entries) {
        if (e.id == 0 || !tree.nodes_.emplace(e.id, LayerNode{e.id, e.parent, e.kind, e.props, {}}).second) {
            error = std::format("повтор или нулевой id слоя {}", e.id);
            return std::nullopt;
        }
        if (e.id >= nextId) {
            error = "nextId должен быть больше всех id слоёв";
            return std::nullopt;
        }
    }
    // Children in file order: siblings are listed bottom to top.
    for (const Entry& e : entries) {
        if (e.parent == 0) {
            tree.root_.push_back(e.id);
            continue;
        }
        LayerNode* parent = tree.node(e.parent);
        if (!parent || parent->kind != NodeKind::Group) {
            error = std::format("родитель слоя {} не найден или не является группой", e.id);
            return std::nullopt;
        }
        parent->children.push_back(e.id);
    }
    for (const Entry& e : entries) {
        int depth = 0;
        for (std::uint64_t p = e.id; p != 0; p = tree.nodes_.at(p).parent) {
            if (++depth > kMaxLayerDepth) {
                error = "дерево слоёв глубже 64 уровней или содержит цикл";
                return std::nullopt;
            }
        }
    }
    if (!tree.find(active)) {
        error = "активный слой не найден";
        return std::nullopt;
    }
    tree.active_ = active;
    tree.nextId_ = nextId;
    return tree;
}

// ── Queries ──────────────────────────────────────────────────────────────────

const LayerNode* LayerTree::find(std::uint64_t id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

LayerNode* LayerTree::node(std::uint64_t id) {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

const std::vector<std::uint64_t>& LayerTree::children(std::uint64_t parent) const {
    static const std::vector<std::uint64_t> kNone;
    if (parent == 0) return root_;
    const LayerNode* n = find(parent);
    return n ? n->children : kNone;
}

std::vector<std::uint64_t>& LayerTree::siblingsOf(std::uint64_t parent) {
    return parent == 0 ? root_ : nodes_.at(parent).children;
}

int LayerTree::depth(std::uint64_t id) const {
    int d = 0;
    for (const LayerNode* n = find(id); n; n = n->parent ? find(n->parent) : nullptr) ++d;
    return d;
}

int LayerTree::height(std::uint64_t id) const {
    const LayerNode* n = find(id);
    if (!n) return 0;
    int h = 0;
    for (const std::uint64_t c : n->children) h = std::max(h, height(c));
    return h + 1;
}

bool LayerTree::isInSubtree(std::uint64_t id, std::uint64_t root) const {
    for (const LayerNode* n = find(id); n; n = n->parent ? find(n->parent) : nullptr) {
        if (n->id == root) return true;
    }
    return false;
}

std::size_t LayerTree::indexOf(std::uint64_t id) const {
    const LayerNode* n = find(id);
    if (!n) return 0;
    const auto& s = children(n->parent);
    return static_cast<std::size_t>(std::find(s.begin(), s.end(), id) - s.begin());
}

bool LayerTree::effectivelyVisible(std::uint64_t id) const {
    const LayerNode* n = find(id);
    if (!n) return false;
    for (; n; n = n->parent ? find(n->parent) : nullptr) {
        if (!n->props.visible) return false;
    }
    return true;
}

bool LayerTree::effectivelyLocked(std::uint64_t id) const {
    for (const LayerNode* n = find(id); n; n = n->parent ? find(n->parent) : nullptr) {
        if (n->props.locked) return true;
    }
    return false;
}

std::vector<std::uint64_t> LayerTree::preorder() const {
    std::vector<std::uint64_t> order;
    order.reserve(nodes_.size());
    const std::function<void(const std::vector<std::uint64_t>&)> walk = [&](const std::vector<std::uint64_t>& list) {
        for (const std::uint64_t id : list) {
            order.push_back(id);
            walk(nodes_.at(id).children);
        }
    };
    walk(root_);
    return order;
}

std::vector<std::uint64_t> LayerTree::rasters(std::uint64_t id) const {
    std::vector<std::uint64_t> out;
    const std::function<void(std::uint64_t)> walk = [&](std::uint64_t n) {
        const LayerNode& node = nodes_.at(n);
        if (node.kind != NodeKind::Group) out.push_back(n);
        for (const std::uint64_t c : node.children) walk(c);
    };
    if (id == 0) {
        for (const std::uint64_t r : root_) walk(r);
    } else if (find(id)) {
        walk(id);
    }
    return out;
}

std::uint64_t LayerTree::clipBase(std::uint64_t id) const {
    const LayerNode* n = find(id);
    if (!n || !n->props.clipToBase) return 0;
    const auto& s = children(n->parent);
    for (std::size_t i = indexOf(id); i-- > 0;) {
        if (!nodes_.at(s[i]).props.clipToBase) return s[i];
    }
    return 0;  // orphan
}

// ── Edits ────────────────────────────────────────────────────────────────────

bool LayerTree::setActive(std::uint64_t id) {
    if (!find(id) || id == active_) return false;
    active_ = id;
    return true;
}

std::string LayerTree::freshName(NodeKind kind) const {
    const std::string_view stem = kind == NodeKind::Group ? "Группа" : kind == NodeKind::Vector ? "Вектор" : "Слой";
    std::unordered_set<std::string> used;
    std::size_t sameKind = 0;
    for (const auto& [id, n] : nodes_) {
        used.insert(n.props.name);
        if (n.kind == kind) ++sameKind;
    }
    for (std::size_t k = sameKind + 1;; ++k) {
        std::string name = std::format("{} {}", stem, k);
        if (!used.contains(name)) return name;
    }
}

std::uint64_t LayerTree::insertAbove(LayerNode n, std::uint64_t anchor) {
    if (nodes_.size() >= kMaxLayerNodes) return 0;
    const LayerNode* a = find(anchor);
    const std::uint64_t parent = a ? a->parent : 0;
    if (depth(parent) + 1 > kMaxLayerDepth) return 0;
    std::vector<std::uint64_t>& s = siblingsOf(parent);
    const std::size_t index = a ? indexOf(anchor) + 1 : s.size();
    n.id = nextId_++;
    n.parent = parent;
    const std::uint64_t id = n.id;
    nodes_.emplace(id, std::move(n));
    s.insert(s.begin() + static_cast<std::ptrdiff_t>(index), id);
    active_ = id;
    return id;
}

std::uint64_t LayerTree::addRaster(std::string name) {
    LayerNode n;
    n.kind = NodeKind::Raster;
    n.props.name = name.empty() ? freshName(NodeKind::Raster) : std::move(name);
    return insertAbove(std::move(n), active_);
}

std::uint64_t LayerTree::addGroup(std::string name) {
    LayerNode n;
    n.kind = NodeKind::Group;
    n.props.name = name.empty() ? freshName(NodeKind::Group) : std::move(name);
    return insertAbove(std::move(n), active_);
}

std::uint64_t LayerTree::addVector(std::string name) {
    LayerNode n;
    n.kind = NodeKind::Vector;
    n.props.name = name.empty() ? freshName(NodeKind::Vector) : std::move(name);
    return insertAbove(std::move(n), active_);
}

bool LayerTree::rasterize(std::uint64_t id) {
    LayerNode* n = node(id);
    if (!n || n->kind != NodeKind::Vector) return false;
    n->kind = NodeKind::Raster;
    return true;
}

std::uint64_t LayerTree::copySubtree(std::uint64_t source, std::uint64_t parent,
                                     std::vector<std::pair<std::uint64_t, std::uint64_t>>& copies) {
    LayerNode copy = nodes_.at(source);
    copy.id = nextId_++;
    copy.parent = parent;
    copy.children.clear();
    const std::uint64_t id = copy.id;
    const std::vector<std::uint64_t> sourceChildren = nodes_.at(source).children;
    nodes_.emplace(id, std::move(copy));
    if (nodes_.at(id).kind != NodeKind::Group) copies.emplace_back(source, id);
    for (const std::uint64_t c : sourceChildren) {
        const std::uint64_t child = copySubtree(c, id, copies);
        nodes_.at(id).children.push_back(child);
    }
    return id;
}

std::uint64_t LayerTree::duplicate(std::uint64_t id, std::vector<std::pair<std::uint64_t, std::uint64_t>>& copies) {
    const LayerNode* n = find(id);
    if (!n) return 0;
    std::size_t count = 0;
    const std::function<void(std::uint64_t)> countAll = [&](std::uint64_t k) {
        ++count;
        for (const std::uint64_t c : nodes_.at(k).children) countAll(c);
    };
    countAll(id);
    if (nodes_.size() + count > kMaxLayerNodes) return 0;
    const std::uint64_t parent = n->parent;
    const std::size_t index = indexOf(id);
    const std::uint64_t copy = copySubtree(id, parent, copies);
    nodes_.at(copy).props.name = nodes_.at(id).props.name + " копия";
    std::vector<std::uint64_t>& s = siblingsOf(parent);
    s.insert(s.begin() + static_cast<std::ptrdiff_t>(index + 1), copy);
    active_ = copy;
    return copy;
}

bool LayerTree::remove(std::uint64_t id, std::vector<std::uint64_t>& removed) {
    const LayerNode* n = find(id);
    if (!n) return false;
    const std::uint64_t parent = n->parent;
    const std::size_t index = indexOf(id);
    const bool activeGoes = isInSubtree(active_, id);
    const std::vector<std::uint64_t> gone = [&] {
        std::vector<std::uint64_t> all;
        const std::function<void(std::uint64_t)> walk = [&](std::uint64_t k) {
            all.push_back(k);
            for (const std::uint64_t c : nodes_.at(k).children) walk(c);
        };
        walk(id);
        return all;
    }();
    for (const std::uint64_t k : gone) {
        if (nodes_.at(k).kind != NodeKind::Group) removed.push_back(k);
    }
    std::vector<std::uint64_t>& s = siblingsOf(parent);
    s.erase(s.begin() + static_cast<std::ptrdiff_t>(index));
    for (const std::uint64_t k : gone) nodes_.erase(k);

    if (activeGoes) {
        // §7: the nearest neighbour, below first, then above, then the parent group.
        const std::vector<std::uint64_t>& left = children(parent);
        if (index > 0) {
            active_ = left[index - 1];
        } else if (!left.empty()) {
            active_ = left[0];
        } else {
            active_ = parent;
        }
    }
    if (rasters().empty()) {
        // §7: deleting the last drawing layer creates an empty one in the same transaction.
        LayerNode fresh;
        fresh.id = nextId_++;
        fresh.parent = parent;
        fresh.props.name = freshName(NodeKind::Raster);
        const std::uint64_t freshId = fresh.id;
        nodes_.emplace(freshId, std::move(fresh));
        std::vector<std::uint64_t>& place = siblingsOf(parent);
        place.insert(place.begin() + static_cast<std::ptrdiff_t>(std::min(index, place.size())), freshId);
        active_ = freshId;
    }
    if (!find(active_)) active_ = root_.empty() ? 0 : root_.back();
    return true;
}

bool LayerTree::move(std::uint64_t id, std::uint64_t parent, std::size_t index) {
    const LayerNode* n = find(id);
    if (!n) return false;
    if (parent != 0) {
        const LayerNode* p = find(parent);
        if (!p || p->kind != NodeKind::Group || isInSubtree(parent, id)) return false;
    }
    if (depth(parent) + height(id) > kMaxLayerDepth) return false;
    const std::uint64_t oldParent = n->parent;
    const std::size_t oldIndex = indexOf(id);
    std::vector<std::uint64_t>& from = siblingsOf(oldParent);
    from.erase(from.begin() + static_cast<std::ptrdiff_t>(oldIndex));
    std::vector<std::uint64_t>& to = siblingsOf(parent);
    index = std::min(index, to.size());
    if (oldParent == parent && oldIndex == index) {
        to.insert(to.begin() + static_cast<std::ptrdiff_t>(index), id);  // no change
        return false;
    }
    to.insert(to.begin() + static_cast<std::ptrdiff_t>(index), id);
    nodes_.at(id).parent = parent;
    return true;
}

bool LayerTree::step(std::uint64_t id, int direction) {
    const LayerNode* n = find(id);
    if (!n || direction == 0) return false;
    const std::uint64_t parent = n->parent;
    const std::size_t i = indexOf(id);
    const std::size_t count = children(parent).size();
    if (direction > 0) {
        if (i + 1 < count) return move(id, parent, i + 1);
        if (parent == 0) return false;
        const LayerNode& group = nodes_.at(parent);
        return move(id, group.parent, indexOf(parent) + 1);  // out of the group, right above it
    }
    if (i > 0) return move(id, parent, i - 1);
    if (parent == 0) return false;
    const LayerNode& group = nodes_.at(parent);
    return move(id, group.parent, indexOf(parent));  // out of the group, right below it
}

bool LayerTree::setProps(std::uint64_t id, const LayerProps& props) {
    LayerNode* n = node(id);
    if (!n) return false;
    LayerProps clean = props;
    clean.opacity = std::clamp(clean.opacity, 0.0, 1.0);
    clean.blendId = std::clamp(clean.blendId, 0, kBlendModeCount - 1);
    if (n->props == clean) return false;
    n->props = std::move(clean);
    return true;
}

}  // namespace chroma::core
