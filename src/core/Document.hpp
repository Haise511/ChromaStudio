#pragma once
// ChromaStudio Engine · core · document metadata carried by every history command (spec §10.2).
//
// "Параметры слоёв, reorder, crop, selection и векторные правки входят в ту же транзакцию": the layer
// tree, the selection and the canvas size travel together, so undo of a crop restores the size, the
// tiles and the selection in one step.

#include "core/LayerTree.hpp"
#include "core/Selection.hpp"
#include "core/Vector.hpp"

#include <map>

#include <optional>

namespace chroma::core {

struct DocumentMeta {
    LayerTree layers = LayerTree::single();
    std::optional<SelectionMask> selection;  // nullopt: no selection, writing is allowed everywhere
    int width = 1;
    int height = 1;
    std::map<std::uint64_t, VectorLayer> vectors;  // objects of each vector layer (§8)
    friend bool operator==(const DocumentMeta&, const DocumentMeta&) = default;
};

}  // namespace chroma::core
