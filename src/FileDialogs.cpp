#include "FileDialogs.hpp"

#include <shobjidl.h>
#include <wrl/client.h>

namespace chroma::desktop {
namespace {

using Microsoft::WRL::ComPtr;

const COMDLG_FILTERSPEC kProjectTypes[] = {{L"Проект ChromaStudio (*.rawproj)", L"*.rawproj"}};
const COMDLG_FILTERSPEC kPngTypes[] = {{L"Изображение PNG (*.png)", L"*.png"}, {L"Изображение JPEG (*.jpg)", L"*.jpg;*.jpeg"}};
const COMDLG_FILTERSPEC kPictureTypes[] = {{L"Изображения (*.png, *.jpg)", L"*.png;*.jpg;*.jpeg"}};
const COMDLG_FILTERSPEC kBrushTypes[] = {{L"Кисти (*.json, *.ipb, *.png)", L"*.json;*.ipb;*.png"}};

void startIn(IFileDialog* dialog, const std::filesystem::path& place) {
    if (place.empty()) return;
    const std::filesystem::path folder = place.has_filename() && place.has_parent_path() ? place.parent_path() : place;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) dialog->SetFolder(item.Get());
}

std::optional<std::filesystem::path> resultPath(IFileDialog* dialog) {
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return std::nullopt;
    PWSTR text = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &text))) return std::nullopt;
    std::filesystem::path path(text);
    CoTaskMemFree(text);
    return path;
}

std::optional<std::filesystem::path> askSave(HWND owner, const std::filesystem::path& suggested,
                                             const COMDLG_FILTERSPEC* types, UINT typeCount, const wchar_t* extension,
                                             const wchar_t* title) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    dialog->SetFileTypes(typeCount, types);
    dialog->SetDefaultExtension(extension);
    dialog->SetTitle(title);
    if (suggested.has_filename()) dialog->SetFileName(suggested.filename().c_str());
    startIn(dialog.Get(), suggested);
    if (FAILED(dialog->Show(owner))) return std::nullopt;  // includes "cancelled"
    return resultPath(dialog.Get());
}

}  // namespace

std::optional<std::filesystem::path> askOpenProject(HWND owner, const std::filesystem::path& current) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetFileTypes(1, kProjectTypes);
    dialog->SetTitle(L"Открыть проект");
    startIn(dialog.Get(), current);
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    return resultPath(dialog.Get());
}

std::optional<std::filesystem::path> askSaveProject(HWND owner, const std::filesystem::path& suggested) {
    return askSave(owner, suggested, kProjectTypes, 1, L"rawproj", L"Сохранить проект");
}

std::optional<std::filesystem::path> askExportPng(HWND owner, const std::filesystem::path& suggested) {
    return askSave(owner, suggested, kPngTypes, 2, L"png", L"Экспорт изображения");
}

std::optional<std::filesystem::path> askImportImage(HWND owner, const std::filesystem::path& current) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetFileTypes(1, kPictureTypes);
    dialog->SetTitle(L"Импорт изображения");
    startIn(dialog.Get(), current);
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    return resultPath(dialog.Get());
}

std::optional<std::filesystem::path> askImportBrush(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetFileTypes(1, kBrushTypes);
    dialog->SetTitle(L"Импортировать кисть");
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    return resultPath(dialog.Get());
}

}  // namespace chroma::desktop
