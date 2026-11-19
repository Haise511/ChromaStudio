#pragma once
// ChromaStudio Engine · project_io · CRC-32 (ISO-HDLC / ZIP: reflected 0xEDB88320, init and final ~0).

#include <cstddef>
#include <cstdint>
#include <span>

namespace chroma::project {

class Crc32 {
public:
    void update(std::span<const std::byte> data) noexcept;
    [[nodiscard]] std::uint32_t value() const noexcept { return ~state_; }

private:
    std::uint32_t state_ = 0xFFFFFFFFu;
};

[[nodiscard]] inline std::uint32_t crc32(std::span<const std::byte> data) noexcept {
    Crc32 crc;
    crc.update(data);
    return crc.value();
}

}  // namespace chroma::project
