#pragma once
// ChromaStudio Engine · core · an R8 mask for a brush tip or a canvas grain (spec §4, §14.2).

#include <cstdint>
#include <vector>

namespace chroma::core {

struct MaskImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> r8;  // rows top to bottom
};

}  // namespace chroma::core
