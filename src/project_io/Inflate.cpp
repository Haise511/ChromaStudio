#include "project_io/Inflate.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

// Canonical-Huffman decoding after the reference decoder structure of RFC 1951 §3.2
// (count/symbol tables, decoding one code bit at a time).

namespace chroma::project {
namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLengthCodes = 286;
constexpr int kMaxDistanceCodes = 30;
constexpr int kFixedLengthCodes = 288;

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("deflate: ") + what); }

struct Huffman {
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::array<std::uint16_t, kFixedLengthCodes> symbol{};
};

// Returns 0 for a complete code, > 0 for an incomplete one, < 0 for an over-subscribed one.
int build(Huffman& h, const std::uint8_t* lengths, int n) {
    h.count.fill(0);
    for (int s = 0; s < n; ++s) ++h.count[lengths[s]];
    if (h.count[0] == n) return 0;  // no codes: complete, but decoding will fail
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    std::array<std::uint16_t, kMaxBits + 1> offset{};
    for (int len = 1; len < kMaxBits; ++len) offset[len + 1] = static_cast<std::uint16_t>(offset[len] + h.count[len]);
    for (int s = 0; s < n; ++s) {
        if (lengths[s] != 0) h.symbol[offset[lengths[s]]++] = static_cast<std::uint16_t>(s);
    }
    return left;
}

class Decoder {
public:
    Decoder(std::span<const std::byte> input, std::size_t expectedSize, bool exact = true)
        : in_(input), limit_(expectedSize), exact_(exact) {
        if (exact) out_.reserve(expectedSize);
    }

    std::vector<std::byte> run() {
        bool last = false;
        do {
            last = bits(1) != 0;
            switch (bits(2)) {
            case 0: stored(); break;
            case 1: fixed(); break;
            case 2: dynamic(); break;
            default: fail("invalid block type");
            }
        } while (!last);
        if (exact_ && out_.size() != limit_) fail("stream shorter than the declared size");
        return std::move(out_);
    }

private:
    std::uint32_t bits(int need) {
        std::uint32_t value = bitBuffer_;
        while (bitCount_ < need) {
            if (pos_ >= in_.size()) fail("unexpected end of input");
            value |= static_cast<std::uint32_t>(in_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        bitBuffer_ = value >> need;
        bitCount_ -= need;
        return value & ((1u << need) - 1u);
    }

    void put(std::byte b) {
        if (out_.size() >= limit_) fail("output exceeds the declared size");
        out_.push_back(b);
    }

    void stored() {
        bitBuffer_ = 0;  // discard to the byte boundary
        bitCount_ = 0;
        if (pos_ + 4 > in_.size()) fail("truncated stored block");
        const unsigned len = static_cast<unsigned>(in_[pos_]) | (static_cast<unsigned>(in_[pos_ + 1]) << 8);
        const unsigned nlen = static_cast<unsigned>(in_[pos_ + 2]) | (static_cast<unsigned>(in_[pos_ + 3]) << 8);
        pos_ += 4;
        if (len != (~nlen & 0xFFFFu)) fail("stored block length check");
        if (pos_ + len > in_.size()) fail("truncated stored block");
        if (out_.size() + len > limit_) fail("output exceeds the declared size");
        out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(pos_),
                    in_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
        pos_ += len;
    }

    int decode(const Huffman& h) {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= static_cast<int>(bits(1));
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        fail("invalid Huffman code");
    }

    void codes(const Huffman& lengthCode, const Huffman& distanceCode) {
        static constexpr std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::uint16_t kDistanceBase[30] = {1,   2,   3,   4,   5,    7,    9,    13,   17,   25,
                                                            33,  49,  65,  97,  129,  193,  257,  385,  513,  769,
                                                            1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lengthCode);
            if (symbol < 256) {
                put(static_cast<std::byte>(symbol));
                continue;
            }
            if (symbol == 256) return;
            symbol -= 257;
            if (symbol >= 29) fail("invalid length symbol");
            const std::size_t length = kLengthBase[symbol] + bits(kLengthExtra[symbol]);
            const int d = decode(distanceCode);
            if (d >= 30) fail("invalid distance symbol");
            const std::size_t distance = kDistanceBase[d] + bits(kDistanceExtra[d]);
            if (distance > out_.size()) fail("distance before the start of the output");
            if (out_.size() + length > limit_) fail("output exceeds the declared size");
            const std::size_t from = out_.size() - distance;
            for (std::size_t i = 0; i < length; ++i) out_.push_back(out_[from + i]);  // may overlap
        }
    }

    void fixed() {
        static const auto tables = [] {
            std::array<std::uint8_t, kFixedLengthCodes> lengths{};
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < kFixedLengthCodes; ++s) lengths[s] = 8;
            std::pair<Huffman, Huffman> result;
            build(result.first, lengths.data(), kFixedLengthCodes);
            std::array<std::uint8_t, kMaxDistanceCodes> distances{};
            distances.fill(5);
            build(result.second, distances.data(), kMaxDistanceCodes);
            return result;
        }();
        codes(tables.first, tables.second);
    }

    void dynamic() {
        static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = static_cast<int>(bits(5)) + 257;
        const int ndist = static_cast<int>(bits(5)) + 1;
        const int ncode = static_cast<int>(bits(4)) + 4;
        if (nlen > kMaxLengthCodes || ndist > kMaxDistanceCodes) fail("too many length or distance codes");

        std::array<std::uint8_t, kMaxLengthCodes + kMaxDistanceCodes> lengths{};
        for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<std::uint8_t>(bits(3));
        Huffman codeLengths;
        if (build(codeLengths, lengths.data(), 19) != 0) fail("incomplete code-length code");

        lengths.fill(0);
        int index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(codeLengths);
            if (symbol < 16) {
                lengths[static_cast<std::size_t>(index++)] = static_cast<std::uint8_t>(symbol);
                continue;
            }
            std::uint8_t value = 0;
            int repeat = 0;
            if (symbol == 16) {
                if (index == 0) fail("repeat with no previous length");
                value = lengths[static_cast<std::size_t>(index - 1)];
                repeat = 3 + static_cast<int>(bits(2));
            } else if (symbol == 17) {
                repeat = 3 + static_cast<int>(bits(3));
            } else {
                repeat = 11 + static_cast<int>(bits(7));
            }
            if (index + repeat > nlen + ndist) fail("too many lengths");
            while (repeat--) lengths[static_cast<std::size_t>(index++)] = value;
        }
        if (lengths[256] == 0) fail("no end-of-block code");

        Huffman lengthCode;
        Huffman distanceCode;
        // Incomplete codes are only allowed when they consist of a single one-bit code.
        int err = build(lengthCode, lengths.data(), nlen);
        if (err < 0 || (err > 0 && nlen - lengthCode.count[0] != 1)) fail("invalid literal/length code");
        err = build(distanceCode, lengths.data() + nlen, ndist);
        if (err < 0 || (err > 0 && ndist - distanceCode.count[0] != 1)) fail("invalid distance code");
        codes(lengthCode, distanceCode);
    }

    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
    std::uint32_t bitBuffer_ = 0;
    int bitCount_ = 0;
    std::size_t limit_;
    bool exact_ = true;
    std::vector<std::byte> out_;
};

}  // namespace

std::vector<std::byte> inflateRaw(std::span<const std::byte> input, std::size_t expectedSize) {
    return Decoder(input, expectedSize).run();
}

std::vector<std::byte> inflateRawUpTo(std::span<const std::byte> input, std::size_t maxSize) {
    return Decoder(input, maxSize, false).run();
}

}  // namespace chroma::project
