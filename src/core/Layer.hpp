#pragma once
// ChromaStudio Engine · core · editable layer properties (spec §7). They travel in the same history
// transactions as tiles (spec §10.2: "Параметры слоёв ... входят в ту же транзакцию").

#include <string>

namespace chroma::core {

struct LayerProps {
    std::string name = "Слой 1";
    bool visible = true;
    double opacity = 1.0;
    int blendId = 0;
    bool clipToBase = false;
    bool alphaLock = false;
    bool locked = false;
    friend bool operator==(const LayerProps&, const LayerProps&) = default;
};

}  // namespace chroma::core
