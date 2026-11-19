#pragma once
// ChromaStudio Engine · desktop · system file dialogs (IFileOpenDialog / IFileSaveDialog).
// The calling thread must have initialised COM (apartment threaded).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <optional>

namespace chroma::desktop {

std::optional<std::filesystem::path> askOpenProject(HWND owner, const std::filesystem::path& current);
std::optional<std::filesystem::path> askSaveProject(HWND owner, const std::filesystem::path& suggested);
std::optional<std::filesystem::path> askExportPng(HWND owner, const std::filesystem::path& suggested);
/// PNG or JPEG for a new layer (§10.3).
std::optional<std::filesystem::path> askImportImage(HWND owner, const std::filesystem::path& current);
/// §14: .png, .ipb and .json brush files (the extension only filters the list).
std::optional<std::filesystem::path> askImportBrush(HWND owner);

}  // namespace chroma::desktop
