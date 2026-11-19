#include "project_io/Assets.hpp"

#include <windows.h>

#include <bcrypt.h>
#include <wincodec.h>
#include <propidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

namespace chroma::project {
namespace {

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

}  // namespace

std::string sha256Hex(std::span<const std::byte> bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    const bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
                    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
                                   static_cast<ULONG>(bytes.size()), 0) == 0 &&
                    BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::string hex;
    for (const unsigned char b : digest) hex += std::format("{:02x}", b);
    return hex;
}

std::optional<ImageRgba> decodeImage(std::span<const std::byte> bytes, std::string& error) {
    Com<IWICImagingFactory> factory;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        error = "WIC недоступен";
        return std::nullopt;
    }
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        error = "изображение не декодируется";
        return std::nullopt;
    }
    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 || w > 4096 || h > 4096) {
        error = "размер изображения должен быть от 1 до 4096 px по осям";  // §10.3
        return std::nullopt;
    }
    ImageRgba image;
    image.width = static_cast<int>(w);
    image.height = static_cast<int>(h);
    image.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data()))) {
        error = "изображение не декодируется";
        return std::nullopt;
    }
    return image;
}

ImageRgba orient(const ImageRgba& image, int orientation) {
    if (orientation < 2 || orientation > 8) return image;
    const int w = image.width, h = image.height;
    const bool swap = orientation >= 5;
    ImageRgba out;
    out.width = swap ? h : w;
    out.height = swap ? w : h;
    out.rgba.resize(image.rgba.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int u = x, v = y;
            switch (orientation) {
            case 2: u = w - 1 - x; break;                    // mirrored
            case 3: u = w - 1 - x, v = h - 1 - y; break;     // 180°
            case 4: v = h - 1 - y; break;                    // mirrored vertically
            case 5: u = y, v = x; break;                     // transpose
            case 6: u = h - 1 - y, v = x; break;             // 90° clockwise
            case 7: u = h - 1 - y, v = w - 1 - x; break;     // transverse
            case 8: u = y, v = w - 1 - x; break;             // 90° counter-clockwise
            default: break;
            }
            std::memcpy(&out.rgba[(static_cast<std::size_t>(v) * out.width + u) * 4],
                        &image.rgba[(static_cast<std::size_t>(y) * w + x) * 4], 4);
        }
    }
    return out;
}

std::optional<ImageRgba> decodePicture(std::span<const std::byte> bytes, std::string& error, PictureInfo& info) {
    Com<IWICImagingFactory> factory;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        error = "WIC недоступен";
        return std::nullopt;
    }
    GUID container{};
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetContainerFormat(&container)) || FAILED(decoder->GetFrame(0, &frame))) {
        error = "файл не декодируется как изображение";
        return std::nullopt;
    }
    if (!IsEqualGUID(container, GUID_ContainerFormatPng) && !IsEqualGUID(container, GUID_ContainerFormatJpeg)) {
        error = "поддерживаются PNG и JPEG";
        return std::nullopt;
    }
    // EXIF orientation (tag 274) in the JPEG APP1 block or a PNG eXIf chunk.
    Com<IWICMetadataQueryReader> query;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&query))) {
        for (const wchar_t* path : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(query->GetMetadataByName(path, &value)) && value.vt == VT_UI2) {
                info.orientation = value.uiVal >= 1 && value.uiVal <= 8 ? value.uiVal : 1;
                PropVariantClear(&value);
                break;
            }
            PropVariantClear(&value);
        }
    }
    // An embedded ICC profile is converted to sRGB before linearisation; none means sRGB.
    IWICBitmapSource* source = frame.p;
    Com<IWICColorTransform> transform;
    UINT contexts = 0;
    if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &contexts)) && contexts > 0) {
        std::vector<IWICColorContext*> list(contexts, nullptr);
        for (auto& c : list) factory->CreateColorContext(&c);
        Com<IWICColorContext> srgb;
        UINT got = 0;
        bool profile = false;
        if (SUCCEEDED(frame->GetColorContexts(contexts, list.data(), &got))) {
            for (UINT i = 0; i < got && !profile; ++i) {
                WICColorContextType type{};
                if (list[i] && SUCCEEDED(list[i]->GetType(&type)) && type == WICColorContextProfile) {
                    profile = true;
                    if (SUCCEEDED(factory->CreateColorContext(&srgb)) && SUCCEEDED(srgb->InitializeFromExifColorSpace(1)) &&
                        SUCCEEDED(factory->CreateColorTransformer(&transform)) &&
                        SUCCEEDED(transform->Initialize(frame.p, list[i], srgb.p, GUID_WICPixelFormat32bppBGRA))) {
                        source = transform.p;
                        info.profileConverted = true;
                    }
                }
            }
        }
        for (auto* c : list) {
            if (c) c->Release();
        }
        info.profileIgnored = profile && !info.profileConverted;
    }
    Com<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        error = "изображение не декодируется";
        return std::nullopt;
    }
    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 || w > 32768 || h > 32768) {
        error = "размер изображения должен быть от 1 до 32768 px по осям";
        return std::nullopt;
    }
    ImageRgba image;
    image.width = static_cast<int>(w);
    image.height = static_cast<int>(h);
    image.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data()))) {
        error = "изображение не декодируется";
        return std::nullopt;
    }
    return orient(image, info.orientation);
}

core::MaskImage maskFrom(const ImageRgba& image, core::MaskChannel channel) {
    core::MaskImage mask;
    mask.width = image.width;
    mask.height = image.height;
    mask.r8.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));
    for (std::size_t i = 0; i < mask.r8.size(); ++i) {
        const double r = image.rgba[4 * i] / 255.0, g = image.rgba[4 * i + 1] / 255.0, b = image.rgba[4 * i + 2] / 255.0;
        const double a = image.rgba[4 * i + 3] / 255.0;
        double v = a;
        if (channel == core::MaskChannel::RedAlpha) v = r * a;
        if (channel == core::MaskChannel::InverseLumaAlpha) v = (1.0 - 0.2126 * r - 0.7152 * g - 0.0722 * b) * a;
        mask.r8[i] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    }
    return mask;
}

std::optional<std::vector<std::byte>> readFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::byte> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!in) return std::nullopt;
    return bytes;
}

void AssetStore::addDirectory(const std::filesystem::path& directory) {
    directories_.push_back(directory);
    indexed_ = false;
}

std::string AssetStore::addBlob(std::vector<std::byte> bytes) {
    std::string sha = sha256Hex(bytes);
    blobs_.emplace(sha, std::move(bytes));
    return sha;
}

void AssetStore::index() {
    if (indexed_) return;
    indexed_ = true;
    for (const auto& dir : directories_) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const auto bytes = readFileBytes(it->path());
            if (!bytes) continue;
            const std::string sha = sha256Hex(*bytes);
            files_.emplace(sha, it->path());
            names_.emplace(it->path().stem().string(), sha);
        }
    }
}

bool AssetStore::contains(const std::string& sha) {
    index();
    return blobs_.contains(sha) || files_.contains(sha);
}

std::optional<std::vector<std::byte>> AssetStore::bytes(const std::string& sha) {
    index();
    if (const auto it = blobs_.find(sha); it != blobs_.end()) return it->second;
    if (const auto it = files_.find(sha); it != files_.end()) return readFileBytes(it->second);
    return std::nullopt;
}

std::optional<std::string> AssetStore::byName(const std::string& stem) {
    index();
    const auto it = names_.find(stem);
    return it == names_.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::shared_ptr<const core::MaskImage> AssetStore::mask(const std::string& sha, core::MaskChannel channel) {
    const auto key = std::pair{sha, static_cast<int>(channel)};
    if (const auto it = masks_.find(key); it != masks_.end()) return it->second;
    const auto data = bytes(sha);
    if (!data) return nullptr;
    std::string error;
    const auto image = decodeImage(*data, error);
    if (!image) return nullptr;
    auto mask = std::make_shared<const core::MaskImage>(maskFrom(*image, channel));
    masks_.emplace(key, mask);
    return mask;
}

}  // namespace chroma::project
