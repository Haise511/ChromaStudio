#pragma once
// ChromaStudio Engine · project_io · raw DEFLATE decoder (RFC 1951), for ZIP entries stored with
// method 8 (spec §10.3 allows store or deflate; ChromaStudio itself writes store).

#include <cstddef>
#include <span>
#include <vector>

namespace chroma::project {

/// Decodes a raw DEFLATE stream that must expand to exactly `expectedSize` bytes. Throws
/// std::runtime_error on malformed input, on output beyond `expectedSize`, or on a short result.
std::vector<std::byte> inflateRaw(std::span<const std::byte> input, std::size_t expectedSize);

/// The same for a stream of unknown length: at most `maxSize` bytes, the end of the stream is required.
std::vector<std::byte> inflateRawUpTo(std::span<const std::byte> input, std::size_t maxSize);

}  // namespace chroma::project
