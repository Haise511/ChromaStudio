# ChromaStudio — замеры §12.2 (2026-11-20)

Release x64, chroma_bench --seconds=30. Сцена §12.2: холст 4096², 12 растровых слоёв, 25 % тайлов, окно 1440×900, ввод 240 Гц, вывод 60 Гц, Твёрдое перо D = 64, spacing 0,1. Интерфейс окна в замер не входит.

## Вид целиком

```
ChromaStudio §12.2 measurement
  CPU      12th Gen Intel(R) Core(TM) i5-12400F, 6 physical cores
  RAM      31.8 GiB
  GPU      NVIDIA GeForce RTX 3060/PCIe/SSE2
  driver   3.3.0 NVIDIA 621.14
  display  240 Hz, system DPI 96
  scene    4096x4096, 12 layers, 25% tiles (3551 stored), window 1440x900, zoom fit (0.220 px/doc px), 1024 visible tiles
  brush    Твёрдое перо, D = 64, spacing 0.10; input 240 Hz, output 60 Hz; 30 s after a 3-s warm-up
  start    GL context 162 ms, scene in RAM 801 ms, programs 4 ms, first document frame 450 ms (uploads every tile); first stroke packet → presented 20.7 ms
  CPU frame preparation              p50    1.81   p95    2.38   p99    3.39 ms   (1800 samples, p95 within budget)
  GPU frame time                     p50    3.90   p95    6.53   p99    8.90 ms   (1800 samples, p95 within budget)
  packet → presented frame         p50   14.79   p95   24.60   p99   81.78 ms   (7200 samples, p95 within budget)
  memory   peak working set 610 MiB; composite cache 928 tiles
```

## Масштаб 100 %

```
ChromaStudio §12.2 measurement
  CPU      12th Gen Intel(R) Core(TM) i5-12400F, 6 physical cores
  RAM      31.8 GiB
  GPU      NVIDIA GeForce RTX 3060/PCIe/SSE2
  driver   3.3.0 NVIDIA 621.14
  display  240 Hz, system DPI 96
  scene    4096x4096, 12 layers, 25% tiles (3123 stored), window 1440x900, zoom 100% (1.000 px/doc px), 96 visible tiles
  brush    Твёрдое перо, D = 64, spacing 0.10; input 240 Hz, output 60 Hz; 30 s after a 3-s warm-up
  start    GL context 150 ms, scene in RAM 813 ms, programs 6 ms, first document frame 51 ms (uploads every tile); first stroke packet → presented 3.7 ms
  CPU frame preparation              p50    0.25   p95    0.40   p99    0.77 ms   (1800 samples, p95 within budget)
  GPU frame time                     p50    2.14   p95    5.01   p99    8.51 ms   (1800 samples, p95 within budget)
  packet → presented frame         p50   12.81   p95   21.35   p99   32.50 ms   (7200 samples, p95 within budget)
  memory   peak working set 486 MiB; composite cache 86 tiles
```

Бюджеты §12.2 по p95: подготовка кадра CPU ≤ 4 мс, кадр GPU ≤ 12 мс, «пакет → кадр» ≤ 33 мс — выполнены в обоих видах. Хвост p99 «пакет → кадр» в виде целиком (≈ 80 мс) — кадры фиксации штриха (чтение тайлов штриха в RAM), раз в 2 с.
