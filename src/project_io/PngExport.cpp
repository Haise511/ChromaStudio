#include "project_io/PngExport.hpp"

#include "core/Color.hpp"
#include "core/Half.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <vector>

namespace chroma::project {
namespace {

using Microsoft::WRL::ComPtr;

std::string hresultText(const char* step, HRESULT hr) {
    return std::format("экспорт: {} не удался (0x{:08X})", step, static_cast<std::uint32_t>(hr));
}

struct Encoding {
    bool jpeg = false;
    std::array<float, 3> background{1, 1, 1};
};

}  // namespace

void pmaToStraightSrgb8(const std::uint16_t* halfs, std::uint8_t* rgba) {
    const float a = std::clamp(core::halfToFloat(halfs[3]), 0.0f, 1.0f);
    const auto alpha8 = static_cast<std::uint8_t>(std::lround(a * 255.0f));
    if (alpha8 == 0) {  // §10.3: RGB is exactly zero where alpha is zero
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        return;
    }
    for (int c = 0; c < 3; ++c) {
        const double straight = std::clamp(static_cast<double>(core::halfToFloat(halfs[c])) / a, 0.0, 1.0);
        rgba[c] = static_cast<std::uint8_t>(std::lround(core::linearToSrgb(straight) * 255.0));
    }
    rgba[3] = alpha8;
}

void pmaOverToSrgb8(const std::uint16_t* halfs, const std::array<float, 3>& background, std::uint8_t* rgb) {
    const double a = std::clamp(static_cast<double>(core::halfToFloat(halfs[3])), 0.0, 1.0);
    for (int c = 0; c < 3; ++c) {
        const double linear = std::clamp(static_cast<double>(core::halfToFloat(halfs[c])) + (1.0 - a) * background[static_cast<std::size_t>(c)], 0.0, 1.0);
        rgb[c] = static_cast<std::uint8_t>(std::lround(core::linearToSrgb(linear) * 255.0));
    }
}

std::vector<std::byte> encodePng(int width, int height, std::span<const std::uint8_t> rgba) {
    if (width < 1 || height < 1 || rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) return {};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
        return {};
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&format))) return {};
    std::vector<std::uint8_t> pixels(rgba.begin(), rgba.end());
    if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
    } else if (!IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA)) {
        return {};
    }
    if (FAILED(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(width) * 4, static_cast<UINT>(pixels.size()),
                                  pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return {};
    }
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return {};
    std::vector<std::byte> out(static_cast<std::size_t>(stat.cbSize.QuadPart));
    const LARGE_INTEGER zero{};
    ULONG read = 0;
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read)) || read != out.size()) {
        return {};
    }
    return out;
}

static std::string exportImage(const std::filesystem::path& path, int width, int height, const TileSource& tiles,
                               const Encoding& encoding) {
    if (width < 1 || height < 1) return "экспорт: пустое изображение";
    const std::filesystem::path directory = path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
    std::random_device device;
    const std::filesystem::path temp =
        directory / (path.filename().wstring() + std::format(L".tmp-{:08x}", device()));

    std::string error;
    {
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hresultText("создание фабрики WIC", hr);
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (FAILED(hr = factory->CreateStream(&stream))) return hresultText("создание потока", hr);
        if (FAILED(hr = stream->InitializeFromFilename(temp.c_str(), GENERIC_WRITE))) return hresultText("создание файла", hr);
        const auto run = [&]() -> std::string {
            if (FAILED(hr = factory->CreateEncoder(encoding.jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr,
                                                   &encoder))) {
                return hresultText("кодировщик", hr);
            }
            if (FAILED(hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return hresultText("кодировщик", hr);
            if (FAILED(hr = encoder->CreateNewFrame(&frame, &options))) return hresultText("кадр", hr);
            if (encoding.jpeg) {
                PROPBAG2 option{};
                option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                VARIANT value{};
                value.vt = VT_R4;
                value.fltVal = 0.92f;
                if (FAILED(hr = options->Write(1, &option, &value))) return hresultText("качество JPEG", hr);
            }
            if (FAILED(hr = frame->Initialize(options.Get()))) return hresultText("кадр", hr);
            if (FAILED(hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) return hresultText("размер", hr);
            WICPixelFormatGUID format = encoding.jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppRGBA;
            if (FAILED(hr = frame->SetPixelFormat(&format))) return hresultText("формат пикселей", hr);
            const bool bgra = !encoding.jpeg && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) != FALSE;
            if (encoding.jpeg ? !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)
                              : !bgra && !IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA)) {
                return std::string("экспорт: кодировщик не принял формат пикселей");
            }
            const std::size_t channels = encoding.jpeg ? 3 : 4;

            // One strip of tile rows at a time: a 32768² document never needs the whole image in memory.
            const std::size_t stride = static_cast<std::size_t>(width) * channels;
            std::vector<std::uint8_t> strip(stride * core::kTileSize);
            const int tilesX = (width + core::kTileSize - 1) / core::kTileSize;
            for (int y0 = 0; y0 < height; y0 += core::kTileSize) {
                const int rows = std::min(core::kTileSize, height - y0);
                std::fill(strip.begin(), strip.end(), std::uint8_t{0});
                if (encoding.jpeg) {  // transparent tiles show the background
                    const std::uint16_t clear[4] = {0, 0, 0, 0};
                    std::uint8_t bg[3];
                    pmaOverToSrgb8(clear, encoding.background, bg);
                    for (std::size_t i = 0; i + 2 < strip.size(); i += 3) {
                        strip[i] = bg[2];
                        strip[i + 1] = bg[1];
                        strip[i + 2] = bg[0];
                    }
                }
                for (int tx = 0; tx < tilesX; ++tx) {
                    const std::shared_ptr<const core::TileData> tile = tiles(tx, y0 / core::kTileSize);
                    if (!tile) continue;
                    const int columns = std::min(core::kTileSize, width - tx * core::kTileSize);
                    for (int y = 0; y < rows; ++y) {
                        for (int x = 0; x < columns; ++x) {
                            const std::uint16_t* p = &tile->halfs[(static_cast<std::size_t>(y) * core::kTileSize + x) * 4];
                            std::uint8_t* out = &strip[static_cast<std::size_t>(y) * stride +
                                                       (static_cast<std::size_t>(tx) * core::kTileSize + x) * channels];
                            if (encoding.jpeg) {
                                pmaOverToSrgb8(p, encoding.background, out);
                                std::swap(out[0], out[2]);  // BGR
                            } else {
                                pmaToStraightSrgb8(p, out);
                                if (bgra) std::swap(out[0], out[2]);
                            }
                        }
                    }
                }
                if (FAILED(hr = frame->WritePixels(static_cast<UINT>(rows), static_cast<UINT>(stride),
                                                   static_cast<UINT>(stride * rows), strip.data()))) {
                    return hresultText("запись строк", hr);
                }
            }
            if (FAILED(hr = frame->Commit())) return hresultText("завершение кадра", hr);
            if (FAILED(hr = encoder->Commit())) return hresultText("завершение файла", hr);
            if (FAILED(hr = stream->Commit(STGC_DEFAULT))) return hresultText("сброс на диск", hr);
            return {};
        };
        error = run();
    }  // the stream closes the file here
    if (error.empty() && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = std::format("экспорт: не удалось записать файл (ошибка Windows {})", GetLastError());
    }
    if (!error.empty()) DeleteFileW(temp.c_str());
    return error;
}

std::string exportPng(const std::filesystem::path& path, int width, int height, const TileSource& tiles) {
    return exportImage(path, width, height, tiles, {});
}

std::string exportJpeg(const std::filesystem::path& path, int width, int height, const TileSource& tiles,
                       const std::array<float, 3>& backgroundLinear) {
    return exportImage(path, width, height, tiles, {true, backgroundLinear});
}

}  // namespace chroma::project
