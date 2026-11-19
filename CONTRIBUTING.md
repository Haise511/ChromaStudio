# Contributing to ChromaStudio

Thank you for helping! Issues, ideas and pull requests are welcome, in English or in Russian.

## Before you start

- For a bug, open an issue with the steps, what you expected and what happened. The log file
  `%LOCALAPPDATA%\ChromaStudio\logs\desktop.log` and your GPU and driver version help a lot.
- For a larger change, open an issue first so we can agree on the approach.

## Building and testing

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCHROMA_WARNINGS_AS_ERRORS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Every pull request must build without warnings (`/W4 /WX`) and pass all five CTest suites. Add a
test for behaviour you change: `tests/core_tests.cpp` for window-free logic, `tests/io_tests.cpp`
for files, `tests/gpu_tests.cpp` for rendering, `tests/ui_tests.cpp` for the interface.

## Style

- C++20, MSVC; no third-party dependencies and no network access in the build or at run time.
- Code comments in English; user-facing text in Russian (an English interface is on the roadmap).
- Keep `core` free of Win32 and OpenGL; it must stay testable without a window.
- Pixels are linear-light, premultiplied RGBA16F; keep GPU results within 0.002 of the CPU reference.

## Licence

By contributing you agree that your contribution is licensed under the [MIT License](LICENSE).
Do not add images, brushes or code you do not have the right to redistribute.
