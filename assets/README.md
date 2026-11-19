# Local brush resources

ChromaStudio ships no bitmap textures; the seven built-in brushes are procedural. Images you add here
are copied next to `ChromaStudio.exe` at build time and packaged by `package_zip`.

| Folder | Files | Used for |
|---|---|---|
| `textures/` | PNG, up to 4096 px per side | tip masks (`tip.maskId`) and grain masks (`grain.maskId`) |
| `materials/` | JPEG | paper textures |

A brush profile refers to an image by the SHA-256 of the file's bytes, or — in PresetFormat — by its
file name without extension (`"textureId": "my_tip"`) or by a number through the alias
`texture_<n>.png`. Masks are read from one channel: `alpha`, `redAlpha` (r·a) or `inverseLumaAlpha`
((1 − 0.2126r − 0.7152g − 0.0722b)·a).

The same folders under `%LOCALAPPDATA%\ChromaStudio\brushes\assets` work without rebuilding.

Only add images you have the right to redistribute. The repository ignores `textures/` and
`materials/`, so nothing here is committed by accident.
