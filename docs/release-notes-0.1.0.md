The first public version of ChromaStudio — an offline painting editor for Windows with a GPU brush engine written from scratch in C++20.

**Install:** download `ChromaStudio-0.1.0-win64.zip`, unzip it and run `install.cmd` (per-user, no administrator rights), or start `ChromaStudio.exe` straight from the folder. Windows 10 2004+ or Windows 11 x64 and a GPU with OpenGL 3.3 are required.

**In this version**

- Pen input with pressure and tilt, stabiliser, pressure curves and tapering; seven built-in brushes plus smudge and blur.
- Watercolour and smudge that carry pigment; layers with 27 blend modes, groups, clipping and Alpha Lock.
- Lasso, magic wand, fill, move, crop and the clipboard; five filters with live preview.
- Editable vector line art with width per knot.
- `.rawproj` projects with atomic saving and a recovery journal every 30 s; PNG/JPEG import and export; brush import.
- No network code and no telemetry.

**Known limitations:** the interface is in Russian; painting needs a GPU; transform is move-only; imported brushes' own blend modes are drawn as Normal.

---

Первая публичная версия ChromaStudio — автономного графического редактора для Windows с GPU-движком кистей на C++20. Установка: скачайте ZIP, распакуйте и запустите `install.cmd` — права администратора не нужны.
