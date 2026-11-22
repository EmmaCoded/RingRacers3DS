# Ring Racers 3DS wiki

Only what this fork changed, and why. For the game itself see the [official repository](https://github.com/KartKrewDev/RingRacers).

- [Journey, sacrifices and dead ends](Journey%20Sacrifices%20Dead%20Ends.md) - how it got here, what was cut, what was tried and dropped
- Reference: [config keys and log tags](../CONFIG.md), [build](../BUILD_3DS.md), [tools](../TOOLS.md), [port overview](../3DS_PORT.md), [performance notes](../PERFORMANCE.md)

## Renderer

**Where:** `src/hardware/r_citro3d/`, `src/dummy/gpu_present_3ds.c`, `src/hardware/hw_main.cpp` (`rr3ds_*` namespaces).

The existing hardware renderer (HWR) is kept; a citro3d implementation of its driver API replaces OpenGL.

### Why textures are pre-converted
The 3DS has 6 MB of VRAM and the CPU cannot write to it. Textures are converted offline to ETC1/ETC1A4 (4 bits per
pixel), capped at 256 px, and read from the SD card on demand. Skin sprites stay RGBA4 because they carry live
colour; converting them to grey ETC1 plus a GPU tint was analysed and not done (an ETC1 read costs more than the
RGBA4 rebuild it would save).

### Texture memory
1. Upload goes through a linear staging ring and a GPU DMA copy; at most ~20 copies / 768 KB per frame (the GX queue
   is small, filling it crashes).
2. If VRAM is full, textures unused for ~2 s (`vram_age_ms`) are evicted; otherwise the texture lives in linear RAM.
3. If neither fits, the upload is **deferred** to a later frame instead of failing (a failure used to wipe every
   texture). Deferred textures show as a checkerboard until uploaded; small textures keep a smaller reserve so they
   are not starved (`defer_small`).

### Static geometry and visibility
Walls and planes are cached per sector/side signature and re-sent only when a signature changes. A per-zone fast
path skips most of the per-frame work. `.rrv` files (computed by the PC map preparer) list which zones can be seen
from each zone; the 3DS skips the rest. On open maps (e.g. Coaster) about 60% of the map is visible, so this saves
almost nothing there.

### Skybox
Drawn to a 512 KB texture and reused for `sky_every` frames. The reuse decision uses the previous frame's camera;
turning faster than the texture margin sampled outside it and stretched one edge texel over the screen (flat green
or blue sky). The offset is now clamped to the margin and the next frame is forced to redraw.

## Memory

**Where:** `src/z_zone.cpp`, `src/dummy/memtrack_3ds.cpp`, `src/hardware/hw_batching.cpp`, `src/hardware/hw_cache.cpp`.

An Old 3DS app gets 64 MB and the heap fragments badly: after a few maps there was 8 MB free but no block bigger
than ~50 KB.

- **Single arena.** At boot the arena takes all free heap except `small_heap_mb`; the zone allocator and every
  `malloc`/`new` of 4 KB or more come from it. Smaller requests use the normal heap.
- **Fixed buffers.** Polygon/vertex batching lives in `.bss` (8192 polys, 32768 vertices) and never grows; the
  drawnode buffer is reused instead of allocated and freed every frame (~90 KB per frame before).
- **Leaks fixed:** colormap allocations on every texture regeneration (1.5 -> 5.6 MB in five minutes); ACS argument
  strings (an upstream leak, ~280 KB/min on one map); the deep copy of the map that only online play needed.
- **No whole-WAD cache.** The stock game caches ~150 MB of lumps; here assets are raw files on the SD card and map data
  is read per sub-lump (no whole-WAD copy), which removed the 5.7 MB request that killed two maps.
- **Preload lists.** Each map/character records what it needs; the next run loads it in one pass so the SD card is
  idle during the race.
- **Simulating the console.** `fragment_heap=1` fills the heap with 48 KB blocks and frees half of them, reproducing
  the console's fragmentation in the emulator.
- **Pressure valve.** When real free arena memory drops below `relief_valve_kb`, cached sprites are released.

## Game logic ("low-end" mode)

**Where:** `RR_LOWEND_CODE` blocks in `p_mobj.c`, `p_map.c`, `p_tick.c`, `k_kart.c`, `k_bot.cpp`. Always on for 3DS;
selectable on PC (Video > Advanced) and forced off online and in replays because it changes the simulation.

| Change | Why |
|---|---|
| Idle rings sleep (`sleeping_rings`) and are animated from the clock when drawn | A map has 50-80 rings; each tic cost the same as an idle object |
| Far rings/effects think less or are removed (`far_sleep`) | Nobody sees them; distance measured from all karts |
| Decorations follow karts without `P_CheckPosition` (`cheap_move`) | Collision checks of overlays/fog were 100-160 us each per tic |
| Weather particles follow the camera | Rain/snow maps were the slowest and some ran out of memory |
| Smaller explosions, mines, capsules, Shrink | Hundreds of short-lived objects per hit |
| Max 4 racers; bots refresh finish distance 1 tic in 4 | Each bot costs ~75 ms per frame on heavy maps |
| Up to 11 tics per frame | Keeps game time real at low frame rates |

Rejected: running bot AI every second tic (`bot_ai_mask=1`) leaves a bot standing for the whole race; splitting logic
across cores (core 1 is capped at 30%).

## Performance model

Full write-up: [PERFORMANCE.md](../PERFORMANCE.md). In short:

- The game runs 35 tics/s. With `L` ms of logic per tic and `D` ms of drawing per frame, `T = D / (1 - 0.035 L)`.
- One ms of logic is worth ~3x one ms of drawing when `L` is large, so logic work pays more than draw work.
- `[METER]` reports where each frame's time goes with a handful of clock reads (no per-function hooks).
- Per-function profilers overstate small functions by 3-17 us per call; trust only functions with >= 10 us of work.
- Heaviest map measured (Robotnik Coaster): ~2.3 fps on hardware, 60% logic / 40% drawing, no single hot spot.

## Known problems

See [3DS_PORT.md](../3DS_PORT.md#known-problems) for the maintained list: heavy maps, linear-memory shrinkage over
long sessions, deferred-texture quads on the first frame of a race, the HOME button, and the missing CIA packaging.

## Roadmap

1. Offline geometry simplification for heavy maps (the only lever left for drawing).
2. Per-map racer count measured with `[METER]`.
3. Batched DMA upload of small textures (removes the remaining white/grey quads).
4. Find the ~0.5 MB per map linear-memory leak.
5. Drawing thread on core 1 (est. 4-8%).
6. CIA / RomFS packaging.

