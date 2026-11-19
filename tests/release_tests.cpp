// ChromaStudio Engine · release checks on the built executable (spec §12, §12.1 "Автономность").
//
//   release_tests <path to ChromaStudio.exe>
//
// Static checks only, so CTest never opens a window: the import table (normal and delay-load) names
// no networking library, and the embedded manifest declares Per-Monitor V2 DPI awareness (§2, §12).
// The run-time half of "Автономность" — the process opens no sockets — is checked on the real
// window during acceptance.

#include "TestHarness.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

std::filesystem::path g_exe;

std::vector<unsigned char> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The DLL names a PE32+ image imports, lowercase; nullopt when the file is not one.
std::optional<std::set<std::string>> importedDlls(const std::vector<unsigned char>& file) {
    const auto read = [&](std::size_t offset, auto& out) {
        if (offset + sizeof out > file.size()) return false;
        std::memcpy(&out, file.data() + offset, sizeof out);
        return true;
    };
    IMAGE_DOS_HEADER dos{};
    if (!read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;
    IMAGE_NT_HEADERS64 nt{};
    if (!read(static_cast<std::size_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return std::nullopt;
    }
    const std::size_t sectionsAt =
        static_cast<std::size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (!read(sectionsAt + i * sizeof(IMAGE_SECTION_HEADER), sections[i])) return std::nullopt;
    }
    const auto toOffset = [&](DWORD rva) -> std::optional<std::size_t> {
        for (const auto& s : sections) {
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + std::max(s.Misc.VirtualSize, s.SizeOfRawData)) {
                return static_cast<std::size_t>(rva - s.VirtualAddress + s.PointerToRawData);
            }
        }
        return std::nullopt;
    };
    const auto nameAt = [&](DWORD rva) {
        std::string name;
        if (const auto at = toOffset(rva)) {
            for (std::size_t i = *at; i < file.size() && file[i] != 0 && name.size() < 260; ++i) {
                name.push_back(static_cast<char>(std::tolower(file[i])));
            }
        }
        return name;
    };
    std::set<std::string> dlls;
    const IMAGE_DATA_DIRECTORY imports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress != 0) {
        const auto at = toOffset(imports.VirtualAddress);
        if (!at) return std::nullopt;
        for (std::size_t i = 0;; ++i) {
            IMAGE_IMPORT_DESCRIPTOR d{};
            if (!read(*at + i * sizeof d, d)) return std::nullopt;
            if (d.Name == 0) break;
            dlls.insert(nameAt(d.Name));
        }
    }
    const IMAGE_DATA_DIRECTORY delayed = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed.VirtualAddress != 0) {
        const auto at = toOffset(delayed.VirtualAddress);
        if (!at) return std::nullopt;
        for (std::size_t i = 0;; ++i) {
            IMAGE_DELAYLOAD_DESCRIPTOR d{};
            if (!read(*at + i * sizeof d, d)) return std::nullopt;
            if (d.DllNameRVA == 0) break;
            dlls.insert(nameAt(d.DllNameRVA));
        }
    }
    return dlls;
}

}  // namespace

TEST_CASE(importsNoNetworking) {
    const auto dlls = importedDlls(readAll(g_exe));
    CHECK(dlls.has_value() && !dlls->empty());
    if (!dlls) return;
    for (const std::string& d : *dlls) std::printf("    imports %s\n", d.c_str());
    for (const char* banned : {"ws2_32.dll", "wsock32.dll", "mswsock.dll", "winhttp.dll", "wininet.dll", "urlmon.dll",
                               "dnsapi.dll", "iphlpapi.dll", "httpapi.dll", "webio.dll", "wsnmp32.dll", "netapi32.dll"}) {
        CHECK(!dlls->contains(banned));
    }
    CHECK(dlls->contains("opengl32.dll"));
}

TEST_CASE(manifestDeclaresPerMonitorV2) {
    HMODULE image = LoadLibraryExW(g_exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    CHECK(image != nullptr);
    if (!image) return;
    HRSRC resource = FindResourceW(image, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));  // RT_MANIFEST, ID 1
    CHECK(resource != nullptr);
    if (resource) {
        const HGLOBAL data = LoadResource(image, resource);
        const auto* text = static_cast<const char*>(LockResource(data));
        const std::string manifest(text, SizeofResource(image, resource));
        CHECK(manifest.find("PerMonitorV2") != std::string::npos);
        CHECK(manifest.find("UTF-8") != std::string::npos || manifest.find("utf-8") != std::string::npos);
    }
    FreeLibrary(image);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: release_tests <ChromaStudio.exe>\n");
        return 2;
    }
    g_exe = std::filesystem::path(argv[1]);
    return test::runAll();
}
