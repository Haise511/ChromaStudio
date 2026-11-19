#include "project_io/Crc32.hpp"

#include <array>
#include <cstring>

namespace chroma::project {
namespace {

// Slicing-by-8 tables: a project of 65 536 tiles is 8 GiB of CRC input.
using Tables = std::array<std::array<std::uint32_t, 256>, 8>;

constexpr Tables makeTables() {
    Tables t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
        for (std::size_t s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
    }
    return t;
}

constexpr Tables kTables = makeTables();

}  // namespace

void Crc32::update(std::span<const std::byte> data) noexcept {
    std::uint32_t c = state_;
    const std::byte* p = data.data();
    std::size_t n = data.size();
    while (n >= 8) {
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        lo ^= c;
        c = kTables[7][lo & 0xFFu] ^ kTables[6][(lo >> 8) & 0xFFu] ^ kTables[5][(lo >> 16) & 0xFFu] ^
            kTables[4][lo >> 24] ^ kTables[3][hi & 0xFFu] ^ kTables[2][(hi >> 8) & 0xFFu] ^
            kTables[1][(hi >> 16) & 0xFFu] ^ kTables[0][hi >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) c = kTables[0][(c ^ static_cast<std::uint32_t>(*p++)) & 0xFFu] ^ (c >> 8);
    state_ = c;
}

}  // namespace chroma::project
