#include "Clipboard.hpp"

#include <objidl.h>
#include <wincodec.h>

#include <cstring>
#include <format>
#include <vector>

namespace chroma::desktop {
namespace {

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

UINT pngFormat() {
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

/// Straight RGBA rows → BGRA rows.
std::vector<std::uint8_t> toBgra(const core::Image8& image) {
    std::vector<std::uint8_t> bgra(image.rgba.size());
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        bgra[i] = image.rgba[i + 2];
        bgra[i + 1] = image.rgba[i + 1];
        bgra[i + 2] = image.rgba[i];
        bgra[i + 3] = image.rgba[i + 3];
    }
    return bgra;
}

HGLOBAL encodePng(const core::Image8& image) {
    Com<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return nullptr;
    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, FALSE, &stream))) return nullptr;
    Com<IWICBitmapEncoder> encoder;
    Com<IWICBitmapFrameEncode> frame;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height)))) {
        return nullptr;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    std::vector<std::uint8_t> bgra = toBgra(image);
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA ||
        FAILED(frame->WritePixels(static_cast<UINT>(image.height), static_cast<UINT>(image.width) * 4,
                                  static_cast<UINT>(bgra.size()), bgra.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return nullptr;
    }
    HGLOBAL memory = nullptr;
    return SUCCEEDED(GetHGlobalFromStream(stream.p, &memory)) ? memory : nullptr;
}

HGLOBAL encodeDibV5(const core::Image8& image) {
    const std::size_t pixels = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + pixels);
    if (!memory) return nullptr;
    auto* header = static_cast<BITMAPV5HEADER*>(GlobalLock(memory));
    std::memset(header, 0, sizeof(BITMAPV5HEADER));
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = image.width;
    header->bV5Height = image.height;  // bottom-up, as most readers expect
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = static_cast<DWORD>(pixels);
    header->bV5RedMask = 0x00FF0000;
    header->bV5GreenMask = 0x0000FF00;
    header->bV5BlueMask = 0x000000FF;
    header->bV5AlphaMask = 0xFF000000;
    header->bV5CSType = LCS_sRGB;
    header->bV5Intent = LCS_GM_IMAGES;
    const std::vector<std::uint8_t> bgra = toBgra(image);
    auto* out = reinterpret_cast<std::uint8_t*>(header + 1);
    const std::size_t row = static_cast<std::size_t>(image.width) * 4;
    for (int y = 0; y < image.height; ++y) {
        std::memcpy(out + static_cast<std::size_t>(image.height - 1 - y) * row, bgra.data() + static_cast<std::size_t>(y) * row, row);
    }
    GlobalUnlock(memory);
    return memory;
}

std::optional<core::Image8> decodePng(const void* data, std::size_t size) {
    Com<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return std::nullopt;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> converter;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), static_cast<DWORD>(size))) ||
        FAILED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return std::nullopt;
    }
    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 || w > 32768 || h > 32768) return std::nullopt;
    core::Image8 image;
    image.width = static_cast<int>(w);
    image.height = static_cast<int>(h);
    image.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data()))) return std::nullopt;
    return image;
}

std::optional<core::Image8> decodeDib(const void* data, std::size_t size) {
    if (size < sizeof(BITMAPINFOHEADER)) return std::nullopt;
    const auto* header = static_cast<const BITMAPINFOHEADER*>(data);
    const int w = header->biWidth;
    const int h = std::abs(header->biHeight);
    const bool topDown = header->biHeight < 0;
    const int bits = header->biBitCount;
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || (bits != 32 && bits != 24)) return std::nullopt;
    if (header->biCompression != BI_RGB && header->biCompression != BI_BITFIELDS) return std::nullopt;
    std::size_t offset = header->biSize;
    if (header->biCompression == BI_BITFIELDS && header->biSize == sizeof(BITMAPINFOHEADER)) offset += 12;  // masks
    offset += static_cast<std::size_t>(header->biClrUsed) * 4;
    const std::size_t stride = (static_cast<std::size_t>(w) * static_cast<std::size_t>(bits) / 8 + 3) & ~std::size_t{3};
    if (offset + stride * static_cast<std::size_t>(h) > size) return std::nullopt;
    const auto* pixels = static_cast<const std::uint8_t*>(data) + offset;
    core::Image8 image;
    image.width = w;
    image.height = h;
    image.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    bool anyAlpha = false;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = pixels + stride * static_cast<std::size_t>(topDown ? y : h - 1 - y);
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = row + static_cast<std::size_t>(x) * static_cast<std::size_t>(bits / 8);
            std::uint8_t* out = &image.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4];
            out[0] = p[2];
            out[1] = p[1];
            out[2] = p[0];
            out[3] = bits == 32 ? p[3] : 255;
            anyAlpha = anyAlpha || (bits == 32 && p[3] != 0);
        }
    }
    if (bits == 32 && !anyAlpha) {  // 32-bit DIBs without alpha leave the byte at zero: opaque
        for (std::size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 255;
    }
    return image;
}

}  // namespace

std::string copyToClipboard(HWND owner, const core::Image8& image) {
    if (image.width <= 0 || image.height <= 0) return "нечего копировать";
    HGLOBAL png = encodePng(image);
    HGLOBAL dib = encodeDibV5(image);
    if (!dib) {
        if (png) GlobalFree(png);
        return "не хватило памяти для изображения";
    }
    if (!OpenClipboard(owner)) {
        GlobalFree(dib);
        if (png) GlobalFree(png);
        return std::format("буфер обмена занят другой программой (ошибка Windows {})", GetLastError());
    }
    EmptyClipboard();
    const bool dibOk = SetClipboardData(CF_DIBV5, dib) != nullptr;
    if (!dibOk) GlobalFree(dib);
    if (png && !SetClipboardData(pngFormat(), png)) GlobalFree(png);
    CloseClipboard();
    return dibOk ? std::string() : "не удалось передать изображение в буфер обмена";
}

std::optional<core::Image8> pasteFromClipboard(HWND owner, std::string& error) {
    error.clear();
    if (!OpenClipboard(owner)) {
        error = "буфер обмена занят другой программой";
        return std::nullopt;
    }
    std::optional<core::Image8> image;
    const auto read = [&](UINT format, auto decode) {
        if (image || !IsClipboardFormatAvailable(format)) return;
        HANDLE handle = GetClipboardData(format);
        if (!handle) return;
        const void* data = GlobalLock(handle);
        if (!data) return;
        image = decode(data, GlobalSize(handle));
        GlobalUnlock(handle);
    };
    read(pngFormat(), decodePng);
    read(CF_DIBV5, decodeDib);
    read(CF_DIB, decodeDib);
    CloseClipboard();
    return image;
}

}  // namespace chroma::desktop
