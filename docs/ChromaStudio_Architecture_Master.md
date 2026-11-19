# High-Performance Offline Digital Painting Engine (Architecture & Implementation Blueprint)

Полная техническая спецификация и архитектурный каркас для автономного графического редактора растровой живописи с аппаратным GPU-ускорением.

---

## 1. Архитектурная топология системы

```
┌────────────────────────────────────────────────────────────────────────┐
│                        VIEWPORT & TOUCH MANAGER                        │
│   • Multi-touch gesture resolver (Pan, Pinch-to-Zoom, Two-finger Rotate)│
│   • Stylus input pipeline: (X, Y, Pressure, Tilt, Timestamp)           │
│   • Coordinate projection: ScreenSpace (DIP) ──[Affine2D]──> CanvasSpace│
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼─────────────────────────────────────┐
│                         STROKE ENGINE & INTERPOLATOR                   │
│   • Catmull-Rom / Hermite spline curve fitting                         │
│   • Sub-pixel dab generation based on dynamic step spacing             │
│   • Dynamic parameter transfer: Radius = f(p), Opacity = g(p)          │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼─────────────────────────────────────┐
│                    GPU RENDERER & BRUSH COMPOSITOR                     │
│   • Active Stroke Scratchpad (Ping-Pong FBO)                           │
│   • Procedural Brush Tip Generator (Gaussian, Bristle, Texture Atlas)  │
│   • Dirty Rectangle (AABB) accumulator                                 │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼─────────────────────────────────────┐
│                       LAYER COMPOSITING STACK                          │
│   • N-Layer Tree (Blending, Clipping Masks, Alpha Lock, Opacity)       │
│   • Full Porter-Duff and Advanced Blend Mode Shaders (Multiply,        │
│     Screen, Overlay, Color Dodge, Linear Burn, Add)                    │
│   • Viewport Quad Blit to Framebuffer                                  │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼─────────────────────────────────────┐
│                     MEMORY & TRANSACTION MANAGER                       │
│   • Sparse Tile-Based Undo/Redo Ring Buffer                            │
│   • Zero-allocation memory pool for real-time stroke packets           │
│   • Fully offline project serialization (.archive: JSON + Raw RGBA)    │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Математический движок интерполяции штриха и физика стилуса

События тач-сенсора и дигитайзера поступают с частотой 60–240 Гц. Для устранения дискретности генератор штампов вычисляет кубический сплайн Catmull-Rom и распределяет оттиски кисти с шагом $\Delta d = \text{size} \times \text{spacing}$.

```cpp
// target: StrokePipeline.hpp
// runtime: C++20 / Standalone Engine Core

#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

struct InputPoint {
    float x;
    float y;
    float pressure; // Нормализованное значение [0.0 ... 1.0]
    float tilt;     // Угол наклона в радианах
};

struct BrushParameters {
    float baseSize = 24.0f;
    float minSizeRatio = 0.1f;
    float baseOpacity = 1.0f;
    float minOpacityRatio = 0.2f;
    float spacing = 0.08f;       // 8% от текущего радиуса кисти
    float hardness = 0.85f;
    float color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
};

struct BrushDab {
    float x;
    float y;
    float radius;
    float opacity;
};

class StrokeInterpolator {
public:
    explicit StrokeInterpolator(BrushParameters params) : m_params(params), m_distanceRemainder(0.0f) {}

    void Reset() {
        m_history.clear();
        m_distanceRemainder = 0.0f;
    }

    void PushPoint(const InputPoint& pt, std::vector<BrushDab>& outDabs) {
        m_history.push_back(pt);
        if (m_history.size() < 4) {
            if (m_history.size() == 1) {
                // Первый штамп при касании
                outDabs.push_back(EvaluateDab(pt));
            }
            return;
        }

        // Опорные точки для сплайна Catmull-Rom
        const size_t n = m_history.size();
        const InputPoint& p0 = m_history[n - 4];
        const InputPoint& p1 = m_history[n - 3];
        const InputPoint& p2 = m_history[n - 2];
        const InputPoint& p3 = m_history[n - 1];

        float segmentLength = std::hypot(p2.x - p1.x, p2.y - p1.y);
        if (segmentLength < 0.001f) return;

        float currentDist = -m_distanceRemainder;

        while (currentDist < segmentLength) {
            float t = (currentDist < 0.0f) ? 0.0f : (currentDist / segmentLength);
            t = std::clamp(t, 0.0f, 1.0f);

            InputPoint interpolated = InterpolateCatmullRom(p0, p1, p2, p3, t);
            BrushDab dab = EvaluateDab(interpolated);
            outDabs.push_back(dab);

            float step = std::max(1.0f, dab.radius * 2.0f * m_params.spacing);
            currentDist += step;
        }

        m_distanceRemainder = currentDist - segmentLength;
        m_history.erase(m_history.begin());
    }

private:
    BrushParameters m_params;
    std::vector<InputPoint> m_history;
    float m_distanceRemainder;

    BrushDab EvaluateDab(const InputPoint& p) const {
        // Нелинейная кубическая кривая чувствительности нажима
        float curvedPressure = p.pressure * p.pressure * (3.0f - 2.0f * p.pressure);
        
        float sizeFactor = m_params.minSizeRatio + (1.0f - m_params.minSizeRatio) * curvedPressure;
        float opacityFactor = m_params.minOpacityRatio + (1.0f - m_params.minOpacityRatio) * curvedPressure;

        return BrushDab{
            p.x,
            p.y,
            (m_params.baseSize * 0.5f) * sizeFactor,
            m_params.baseOpacity * opacityFactor
        };
    }

    static InputPoint InterpolateCatmullRom(const InputPoint& p0, const InputPoint& p1,
                                            const InputPoint& p2, const InputPoint& p3, float t) {
        float t2 = t * t;
        float t3 = t2 * t;

        auto solve = [t, t2, t3](float v0, float v1, float v2, float v3) {
            return 0.5f * ((2.0f * v1) +
                           (-v0 + v2) * t +
                           (2.0f * v0 - 5.0f * v1 + 4.0f * v2 - v3) * t2 +
                           (-v0 + 3.0f * v1 - 3.0f * v2 + v3) * t3);
        };

        return InputPoint{
            solve(p0.x, p1.x, p2.x, p3.x),
            solve(p0.y, p1.y, p2.y, p3.y),
            solve(p0.pressure, p1.pressure, p2.pressure, p3.pressure),
            solve(p0.tilt, p1.tilt, p2.tilt, p3.tilt)
        };
    }
};
```

---

## 3. Шейдеры блендинга слоев (GLSL ES 3.0)

Поддержка линейных и нелинейных режимов смешивания пикселей в линейном цветовом пространстве.

```glsl
// target: LayerCompositor.frag
#version 300 es
precision highp float;

in vec2 v_TexCoord;
out vec4 FragColor;

uniform sampler2D u_BottomLayer; // Текущий композитный буфер (Destination)
uniform sampler2D u_TopLayer;    // Накладываемый слой (Source)
uniform float u_TopOpacity;      // Прозрачность накладываемого слоя [0.0 - 1.0]
uniform int u_BlendMode;         // Режим наложения

float BlendOverlay(float b, float s) {
    return (b < 0.5) ? (2.0 * b * s) : (1.0 - 2.0 * (1.0 - b) * (1.0 - s));
}

vec3 BlendColorDodge(vec3 b, vec3 s) {
    return vec3(
        (s.r >= 1.0) ? 1.0 : min(1.0, b.r / (1.0 - s.r)),
        (s.g >= 1.0) ? 1.0 : min(1.0, b.g / (1.0 - s.g)),
        (s.b >= 1.0) ? 1.0 : min(1.0, b.b / (1.0 - s.b))
    );
}

vec3 BlendLinearBurn(vec3 b, vec3 s) {
    return max(b + s - vec3(1.0), vec3(0.0));
}

void main() {
    vec4 dst = texture(u_BottomLayer, v_TexCoord);
    vec4 src = texture(u_TopLayer, v_TexCoord);

    src.a *= u_TopOpacity;

    if (src.a <= 0.0001) {
        FragColor = dst;
        return;
    }

    vec3 b = (dst.a > 0.0) ? (dst.rgb / dst.a) : vec3(0.0);
    vec3 s = (src.a > 0.0) ? (src.rgb / src.a) : vec3(0.0);
    vec3 blendResult = s;

    switch (u_BlendMode) {
        case 0: // Normal
            blendResult = s;
            break;
        case 1: // Multiply
            blendResult = b * s;
            break;
        case 2: // Screen
            blendResult = 1.0 - (1.0 - b) * (1.0 - s);
            break;
        case 3: // Overlay
            blendResult = vec3(
                BlendOverlay(b.r, s.r),
                BlendOverlay(b.g, s.g),
                BlendOverlay(b.b, s.b)
            );
            break;
        case 4: // Add / Linear Dodge
            blendResult = min(b + s, vec3(1.0));
            break;
        case 5: // Color Dodge
            blendResult = BlendColorDodge(b, s);
            break;
        case 6: // Linear Burn
            blendResult = BlendLinearBurn(b, s);
            break;
    }

    float outAlpha = src.a + dst.a * (1.0 - src.a);
    vec3 outRgb = vec3(0.0);

    if (outAlpha > 0.0) {
        outRgb = (blendResult * src.a + dst.rgb * (1.0 - src.a)) / outAlpha;
    }

    FragColor = vec4(outRgb * outAlpha, outAlpha);
}
```

---

## 4. Менеджер холста, слоев и FBO (C++)

Архитектура активного слоя использует Double FBO Scratchpad для изоляции незавершенного штриха от постоянных пикселей слоя.

```cpp
// target: CanvasLayerManager.hpp
// runtime: C++20 / OpenGL ES 3.0+

#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

enum class LayerBlendMode : int {
    Normal = 0,
    Multiply = 1,
    Screen = 2,
    Overlay = 3,
    Add = 4,
    ColorDodge = 5,
    LinearBurn = 6
};

struct Layer {
    uint32_t id;
    std::string name;
    uint32_t textureId = 0;
    uint32_t fboId = 0;
    float opacity = 1.0f;
    bool isVisible = true;
    bool isAlphaLocked = false;
    bool isClippingMask = false;
    LayerBlendMode blendMode = LayerBlendMode::Normal;
};

class CanvasProject {
public:
    CanvasProject(int width, int height) : m_width(width), m_height(height), m_activeLayerIndex(0) {
        CreateInitialLayer();
    }

    ~CanvasProject() {
        CleanupBuffers();
    }

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    size_t GetActiveLayerIndex() const { return m_activeLayerIndex; }
    std::vector<Layer>& GetLayers() { return m_layers; }

    void AddLayer(const std::string& name) {
        Layer l;
        l.id = m_nextId++;
        l.name = name;
        AllocateTextureAndFbo(l.textureId, l.fboId, m_width, m_height);
        m_layers.push_back(l);
        m_activeLayerIndex = m_layers.size() - 1;
    }

    void RemoveLayer(size_t index) {
        if (m_layers.size() <= 1 || index >= m_layers.size()) return;
        FreeTextureAndFbo(m_layers[index].textureId, m_layers[index].fboId);
        m_layers.erase(m_layers.begin() + index);
        if (m_activeLayerIndex >= m_layers.size()) {
            m_activeLayerIndex = m_layers.size() - 1;
        }
    }

private:
    int m_width;
    int m_height;
    uint32_t m_nextId = 1;
    size_t m_activeLayerIndex;
    std::vector<Layer> m_layers;

    void CreateInitialLayer() {
        AddLayer("Background");
        AddLayer("Layer 1");
    }

    void AllocateTextureAndFbo(uint32_t& outTex, uint32_t& outFbo, int w, int h);
    void FreeTextureAndFbo(uint32_t tex, uint32_t fbo);
    void CleanupBuffers() {
        for (auto& l : m_layers) {
            FreeTextureAndFbo(l.textureId, l.fboId);
        }
        m_layers.clear();
    }
};
```

---

## 5. Система истории изменений (Tile-Based Diff Undo/Redo)

```cpp
// target: TileUndoManager.hpp
#pragma once
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstring>

constexpr int TILE_SIZE = 128;
constexpr size_t TILE_BYTES = TILE_SIZE * TILE_SIZE * 4;

struct TileCoord {
    int tx, ty;
    bool operator==(const TileCoord& o) const { return tx == o.tx && ty == o.ty; }
};

struct TileDelta {
    TileCoord coord;
    std::vector<uint8_t> beforeData;
    std::vector<uint8_t> afterData;
};

struct UndoAction {
    uint32_t targetLayerId;
    std::vector<TileDelta> modifiedTiles;
};

class TileUndoStack {
public:
    explicit TileUndoStack(size_t maxDepth = 64) : m_maxDepth(maxDepth) {}

    void PushAction(UndoAction action) {
        m_redoStack.clear();
        m_undoStack.push_back(std::move(action));
        if (m_undoStack.size() > m_maxDepth) {
            m_undoStack.erase(m_undoStack.begin());
        }
    }

    bool CanUndo() const { return !m_undoStack.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }

    UndoAction PopUndo() {
        UndoAction a = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        m_redoStack.push_back(a);
        return a;
    }

    UndoAction PopRedo() {
        UndoAction a = std::move(m_redoStack.back());
        m_redoStack.pop_back();
        m_undoStack.push_back(a);
        return a;
    }

private:
    size_t m_maxDepth;
    std::vector<UndoAction> m_undoStack;
    std::vector<UndoAction> m_redoStack;
};
```

---

## 6. Автономный формат файлов проектов (.rawproj)

Архив ZIP без внешних сетевых валидаций:
- `manifest.json`: Дерево слоев, параметры смешивания, размер холста.
- `thumbnail.png`: Превью для галереи.
- `layers/layer_N.bin`: Несжатые/RLE бинарные RGBA данные каждого слоя.

---

## 7. UX/UI Спецификация компонентов

1. **Главная панель инструментов (Dock Bar):**
   - Трансформация (матричные операции).
   - Выделение: Лассо, Волшебная палочка (битовая маска).
   - Кисть / Ластик.
   - Смазывание (Палец) / Размытие (GPU Convolution).
   - Заливка (Scanline Flood Fill).
2. **Палитра и быстрые слайдеры:**
   - Левый вертикальный слайдер: Размер кисти.
   - Правый вертикальный слайдер: Непрозрачность.
   - Быстрый выбор цвета / HSV-круг.
3. **Панель слоев:**
   - Перетаскивание z-index.
   - Режимы наложения (Normal, Multiply, Screen, Overlay, Add, Color Dodge и др.).
   - Альфа-замок (Alpha Lock) и кадрирование (Clipping Mask).
