# ChromaStudio Engine — build entry point.
#
# Targets (spec §12): chroma_core, chroma_render_gl, chroma_project_io, chroma_ui → `desktop`
# (ChromaStudio.exe); core_tests, io_tests, gpu_tests, ui_tests, release_tests → CTest (`engine_tests` builds them all).
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
#   or, from an "x64 Native Tools" prompt:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
#
# Spec: production_engine_spec.md §2 (OpenGL 3.3 core through WGL), §12 (toolchain, offline build).

cmake_minimum_required(VERSION 3.25)

project(ChromaStudio
    VERSION 0.1.0
    DESCRIPTION "ChromaStudio Engine - desktop painting editor for Windows x64"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ChromaStudio targets Windows x64 only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "An x64 toolchain is required (-A x64, or an x64 Native Tools prompt).")
endif()
if(NOT (MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    message(FATAL_ERROR "Supported compilers are MSVC and Clang (clang-cl or clang++); found ${CMAKE_CXX_COMPILER_ID}.")
endif()

# Spec §12: dependency versions are pinned in the source tree; configure and build never download.
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "Never download dependencies (spec §12)" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Spec §12: Debug and Release, x64.
get_property(CHROMA_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(CHROMA_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Build configurations" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration: Debug or Release" FORCE)
endif()

# Static CRT: the executable needs no separately installed VC++ redistributable (spec §1, the
# installer carries every library it needs).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(CHROMA_MULTI_CONFIG)
    set(CHROMA_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
else()
    set(CHROMA_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

option(CHROMA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ── Shared compiler settings ─────────────────────────────────────────────────

add_library(chroma_build_settings INTERFACE)
target_compile_definitions(chroma_build_settings INTERFACE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    # Windows 10 version 2004 is the oldest supported system (spec §1).
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000008)

if(MSVC)
    # cl.exe and clang-cl share the MSVC command line. Spec §12: /std:c++20 /utf-8 /W4 /fp:precise.
    target_compile_options(chroma_build_settings INTERFACE
        /utf-8 /W4 /fp:precise /permissive- /Zc:__cplusplus
        $<$<CXX_COMPILER_ID:MSVC>:/MP>
        $<$<BOOL:${CHROMA_WARNINGS_AS_ERRORS}>:/WX>)
else()
    # clang++ with the GNU driver (MSVC or MinGW target).
    target_compile_options(chroma_build_settings INTERFACE
        -Wall -Wextra -Wpedantic -ffp-model=precise -finput-charset=UTF-8
        $<$<BOOL:${CHROMA_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# ── Rendering dependency ─────────────────────────────────────────────────────
# Spec §2 / §12 render_gl: a single desktop OpenGL 3.3 core context created through WGL, GLSL 3.30
# programs and a local function loader. opengl32 from the Windows SDK is the only graphics library;
# GL 1.2+ entry points are resolved at run time with wglGetProcAddress (src/main.cpp).
find_package(OpenGL REQUIRED)

# ── Application manifest ─────────────────────────────────────────────────────
# Per-Monitor V2 is declared before the first HWND exists (spec §2, §12 "PMv2 manifest").
# main.cpp repeats the request at run time for toolchains that do not embed .manifest sources.
set(CHROMA_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/generated/ChromaStudio.manifest")
file(CONFIGURE OUTPUT "${CHROMA_MANIFEST}" CONTENT [[
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <!-- Windows 10 and Windows 11 -->
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2, PerMonitor</dpiAwareness>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
    </windowsSettings>
  </application>
</assembly>
]] @ONLY)

# ── core: geometry, stabiliser, brush profiles, view, tile history (spec §12: no HWND) ──

add_library(chroma_core STATIC
    src/core/BrushProfile.cpp
    src/core/BrushProfile.hpp
    src/core/Color.hpp
    src/core/Compositor.cpp
    src/core/Compositor.hpp
    src/core/Document.hpp
    src/core/Fx.cpp
    src/core/Fx.hpp
    src/core/Geometry.hpp
    src/core/Half.hpp
    src/core/Layer.hpp
    src/core/LayerTree.cpp
    src/core/LayerTree.hpp
    src/core/Log.cpp
    src/core/Log.hpp
    src/core/MaskImage.hpp
    src/core/RasterOps.cpp
    src/core/RasterOps.hpp
    src/core/Selection.cpp
    src/core/Selection.hpp
    src/core/Stabilizer.cpp
    src/core/Stabilizer.hpp
    src/core/StrokeBuilder.cpp
    src/core/StrokeBuilder.hpp
    src/core/TileData.cpp
    src/core/TileData.hpp
    src/core/TileHistory.hpp
    src/core/Vector.cpp
    src/core/Vector.hpp
    src/core/View.hpp)
target_include_directories(chroma_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(chroma_core PUBLIC chroma_build_settings)

# ── render_gl: WGL context, GLSL programs, GPU tiles (spec §12) ──────────────

add_library(chroma_render_gl STATIC
    src/render_gl/GlContext.cpp
    src/render_gl/GlContext.hpp
    src/render_gl/GlProgram.cpp
    src/render_gl/GlProgram.hpp
    src/render_gl/Shaders.hpp
    src/render_gl/TileRenderer.cpp
    src/render_gl/TileRenderer.hpp)
target_link_libraries(chroma_render_gl PUBLIC
    chroma_core
    OpenGL::GL      # opengl32: WGL and the GL 1.1 entry points
    user32          # bootstrap window
    gdi32)          # pixel formats, SwapBuffers

# ── project_io: local ZIP64, JSON, .rawproj, PNG export through WIC (spec §12) ─

add_library(chroma_project_io STATIC
    src/project_io/Assets.cpp
    src/project_io/Assets.hpp
    src/project_io/BrushImport.cpp
    src/project_io/BrushImport.hpp
    src/project_io/BrushJson.cpp
    src/project_io/BrushJson.hpp
    src/project_io/Crc32.cpp
    src/project_io/Crc32.hpp
    src/project_io/Inflate.cpp
    src/project_io/Inflate.hpp
    src/project_io/Journal.cpp
    src/project_io/Journal.hpp
    src/project_io/Json.cpp
    src/project_io/Json.hpp
    src/project_io/PngExport.cpp
    src/project_io/PngExport.hpp
    src/project_io/Project.cpp
    src/project_io/Project.hpp
    src/project_io/Zip.cpp
    src/project_io/Zip.hpp)
target_link_libraries(chroma_project_io PUBLIC
    chroma_core
    windowscodecs   # WIC PNG encoder and decoder
    ole32           # COM
    bcrypt)         # SHA-256 of resources

# ── ui: editor chrome — layout, theme, SDF shapes and GDI text on GL, controls (spec §11) ──

add_library(chroma_ui STATIC
    src/ui/BrushPreview.cpp
    src/ui/BrushPreview.hpp
    src/ui/EditorUi.cpp
    src/ui/EditorUi.hpp
    src/ui/Icons.cpp
    src/ui/Icons.hpp
    src/ui/Layout.cpp
    src/ui/Layout.hpp
    src/ui/Theme.cpp
    src/ui/Theme.hpp
    src/ui/UiRenderer.cpp
    src/ui/UiRenderer.hpp)
target_link_libraries(chroma_ui PUBLIC
    chroma_render_gl
    user32          # system colours, High Contrast
    gdi32)          # text rasterisation

# ── Desktop executable ───────────────────────────────────────────────────────

# Resources: the app icon (resources/app.ico, made by tools/make_icon.py) and the version, which the
# installer and Explorer read.
set(CHROMA_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
set(CHROMA_VERSION_RC "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
file(CONFIGURE OUTPUT "${CHROMA_VERSION_RC}" CONTENT [[
#include <winver.h>
1 ICON "@CHROMA_ICON@"
VS_VERSION_INFO VERSIONINFO
FILEVERSION @PROJECT_VERSION_MAJOR@,@PROJECT_VERSION_MINOR@,@PROJECT_VERSION_PATCH@,0
PRODUCTVERSION @PROJECT_VERSION_MAJOR@,@PROJECT_VERSION_MINOR@,@PROJECT_VERSION_PATCH@,0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "ProductName", "ChromaStudio"
      VALUE "FileDescription", "ChromaStudio"
      VALUE "ProductVersion", "@PROJECT_VERSION@"
      VALUE "FileVersion", "@PROJECT_VERSION@"
      VALUE "OriginalFilename", "ChromaStudio.exe"
      VALUE "LegalCopyright", "MIT License"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
]] @ONLY)

add_executable(desktop WIN32
    "${CHROMA_VERSION_RC}"
    src/main.cpp
    src/Clipboard.cpp
    src/Clipboard.hpp
    src/FileDialogs.cpp
    src/FileDialogs.hpp
    src/PlatformWindow.cpp
    src/PlatformWindow.hpp
    "${CHROMA_MANIFEST}")

set_target_properties(desktop PROPERTIES
    OUTPUT_NAME ChromaStudio
    VS_DEBUGGER_WORKING_DIRECTORY "${CHROMA_OUTPUT_DIR}")

target_compile_definitions(desktop PRIVATE CHROMA_VERSION="${PROJECT_VERSION}")

target_link_libraries(desktop PRIVATE
    chroma_ui
    chroma_render_gl
    chroma_project_io
    user32          # window, WM_POINTER*, pen history
    gdi32           # CPU/DIB fallback presentation
    dwmapi          # dark title bar
    ole32           # COM: file dialogs
    shell32         # IShellItem, CommandLineToArgvW
    uuid)           # dialog CLSIDs

if(MINGW)
    target_link_options(desktop PRIVATE -municode)  # wWinMain entry point
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT desktop)

# ── Assets ───────────────────────────────────────────────────────────────────
# Optional local resources next to the executable (spec §12 allows "in the EXE or next to it"):
# assets/textures/*.png (tip and grain masks, found by SHA-256 or file stem) and
# assets/materials/*.jpg (papers). The repository ships none; the built-in brushes are procedural.
# The copy reruns when a file under assets/ changes; CONFIGURE_DEPENDS re-globs when files are added.
set(CHROMA_ASSET_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/assets")
file(GLOB_RECURSE CHROMA_ASSET_FILES CONFIGURE_DEPENDS "${CHROMA_ASSET_SOURCE}/textures/*" "${CHROMA_ASSET_SOURCE}/materials/*")
set(CHROMA_ASSET_STAMP "${CMAKE_CURRENT_BINARY_DIR}/stamps/assets-$<CONFIG>.stamp")
add_custom_command(
    OUTPUT "${CHROMA_ASSET_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CHROMA_OUTPUT_DIR}/assets/textures" "${CHROMA_OUTPUT_DIR}/assets/materials"
    COMMAND "${CMAKE_COMMAND}" -DSRC=${CHROMA_ASSET_SOURCE} -DDST=${CHROMA_OUTPUT_DIR}/assets
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyAssets.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CHROMA_ASSET_STAMP}"
    DEPENDS ${CHROMA_ASSET_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyAssets.cmake"
    COMMENT "Copying local assets next to ChromaStudio.exe"
    VERBATIM)
add_custom_target(chroma_assets DEPENDS "${CHROMA_ASSET_STAMP}")
add_dependencies(desktop chroma_assets)

# ── Package (spec §12: per-user installation without elevation) ──────────────
# `cmake --build <dir> --config Release --target package_zip` stages ChromaStudio.exe, local assets
# and the PowerShell installer, then zips them. install.cmd installs into %LOCALAPPDATA%\Programs.
install(TARGETS desktop RUNTIME DESTINATION .)
install(DIRECTORY "${CHROMA_ASSET_SOURCE}/" DESTINATION assets
        FILES_MATCHING PATTERN "*.png" PATTERN "*.jpg" PATTERN "*.jpeg")
install(FILES packaging/Install-ChromaStudio.ps1 packaging/Uninstall-ChromaStudio.ps1 packaging/install.cmd DESTINATION .)
set(CHROMA_PACKAGE_NAME "ChromaStudio-${PROJECT_VERSION}-win64")
add_custom_target(package_zip
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_BINARY_DIR}/package/${CHROMA_PACKAGE_NAME}"
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --config $<CONFIG>
            --prefix "${CMAKE_BINARY_DIR}/package/${CHROMA_PACKAGE_NAME}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${CMAKE_BINARY_DIR}/package"
            "${CMAKE_COMMAND}" -E tar cf "${CHROMA_PACKAGE_NAME}.zip" --format=zip "${CHROMA_PACKAGE_NAME}"
    DEPENDS desktop
    COMMENT "Packaging ${CHROMA_PACKAGE_NAME}.zip"
    VERBATIM)

# ── engine_tests (spec §12: CTest fails with a non-zero code on any mismatch) ─

option(CHROMA_BUILD_TESTS "Build engine_tests" ON)
if(CHROMA_BUILD_TESTS)
    enable_testing()

    add_executable(core_tests tests/core_tests.cpp tests/TestHarness.hpp)
    target_link_libraries(core_tests PRIVATE chroma_core)
    add_test(NAME core COMMAND core_tests)

    add_executable(io_tests tests/io_tests.cpp tests/TestHarness.hpp)
    target_link_libraries(io_tests PRIVATE chroma_project_io)
    add_test(NAME io COMMAND io_tests)

    # Needs an OpenGL 3.3 driver; exits with 77 (reported as skipped) when there is none.
    add_executable(gpu_tests tests/gpu_tests.cpp tests/TestHarness.hpp)
    target_link_libraries(gpu_tests PRIVATE chroma_ui)  # the chrome is drawn as well
    add_test(NAME gpu COMMAND gpu_tests)
    set_tests_properties(gpu PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(ui_tests tests/ui_tests.cpp tests/TestHarness.hpp)
    target_link_libraries(ui_tests PRIVATE chroma_ui)
    add_test(NAME ui COMMAND ui_tests)

    # Static checks of the built executable: no networking imports, the Per-Monitor V2 manifest.
    add_executable(release_tests tests/release_tests.cpp tests/TestHarness.hpp)
    add_dependencies(release_tests desktop)
    add_test(NAME release COMMAND release_tests "$<TARGET_FILE:desktop>")

    add_custom_target(engine_tests DEPENDS core_tests io_tests gpu_tests ui_tests release_tests)

    # §12.2 measurement on the local GPU (run by hand; prints p50/p95/p99).
    add_executable(chroma_bench tools/bench.cpp)
    target_link_libraries(chroma_bench PRIVATE chroma_render_gl winmm dbghelp)
endif()
