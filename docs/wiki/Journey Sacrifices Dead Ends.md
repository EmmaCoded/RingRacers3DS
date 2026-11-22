# The porting journey, the sacrifices and the dead ends

Three parts: [the journey](#the-porting-journey), [the sacrifices](#sacrifices) and [the dead ends](#dead-ends). Technical details of each area are in the [wiki](Home.md).


How Ring Racers got onto an Old Nintendo 3DS, in the order it happened: first a one-page summary, then the detail.

## The porting journey

### In one page

Ring Racers is a PC game built for machines with gigabytes of RAM and a GPU that runs shaders. The Old 3DS has
64 MB of application memory, a 268 MHz dual-core ARM11 (one core only partly available to games), 6 MB of VRAM and
a fixed-function-style GPU. The port was therefore **not a recompile, it was an adaptation**:

1. **Get it to boot and draw** with a new citro3d backend for the existing renderer (October).
2. **Make files, memory and audio fit** the console: ~16 open files, a fragmenting heap, DSP-ADPCM audio only.
3. **Move everything heavy off the console**: assets are converted once on a PC (ETC1 textures, DSP audio,
   precomputed visibility) and read raw from the SD card.
4. **Measure, then cut.** Every optimisation was measured first; the biggest wins were bugs and waste in the game
   itself, not clever tricks. Many ideas were tried and thrown away (see [Dead ends](#dead-ends)).
5. **Accept the limits.** Menus run at 20-28 fps, easy maps at ~16 fps on hardware, and the heaviest maps at ~2 fps.
   What was given up to get there is listed in [Sacrifices](#sacrifices).

### Phase by phase

#### 1. First picture (October)
- A `citro3d` implementation of the renderer's driver API, a software-style 2D layer for menus and HUD, and a
  libctru platform layer (input, timing, filesystem, threads).
- The first real-console tests showed **5-7 fps in a race** and crashes on entering video options (a `bad_alloc`
  with 8 MB free but no block larger than ~50 KB: the heap was fragmenting).

#### 2. The console is not the emulator
- Hardware limits turned up one by one: only ~16 files may be open at once (so `.pk3` archives got an LRU of 4
  handles and texture parts were capped at 3), `rename` does not overwrite, the CPU cannot write VRAM
  (uploads go through a linear staging ring and a GPU DMA copy), and the audio DSP only plays DSP-ADPCM.
- To measure honestly, Azahar was configured as an Old 3DS (CPU clock and memory limited) until it reproduced the
  console's 5-6 fps. From then on, changes were validated there first.

#### 3. Logic, not drawing, was the first bottleneck
- Profiling showed ~66 ms per tic spent *outside* `P_Ticker`: a music helper ran a 26 ms tick on every game tic
  . Fixing that and a per-frame music resync that re-read the SD card gave the first big wins.
- Idle rings, far decorations and visual-only objects stopped doing collision checks every tic.

#### 4. Memory
- A crash on the biggest playground map was **not fragmentation**; the map simply needed too much, through two
  leaks and a deep copy of the whole map that only online play used.
- The fix was structural: one **arena** takes the free heap at boot and serves the zone allocator and every large
  `malloc`, fixed-size batching buffers live in `.bss`, leaks were closed (colormaps, ACS strings), and a
  `fragment_heap` switch reproduces the console's fragmentation inside the emulator.

#### 5. Adaptation instead of imitation
- Decision: stop trying to copy the PC behaviour one-to-one and work to a **time budget per map**, measured on the
  worst map at its worst moment. A worst-case measurement was run over all 152 race maps.
- Findings per map were specific: weather particles (rain/snow maps were the slowest and some ran out of memory),
  whole-map copies when loading, and an upstream bug where `P_SetMobjState`'s recursion counter was never
  decremented (28 KB cleared on every state change).

#### 6. The asset pipeline
- Because the stock game caches ~150 MB of lumps, assets are converted off-device: textures to **ETC1/ETC1A4**
  (maximum 256 px), sprites into ETC1A4 and a fixed atlas, audio to DSP-ADPCM, 63 characters as lazily mounted
  packs, map thumbnails, per-map preload lists and precomputed visibility.
- Everything is wrapped in a single `prepare_3ds.exe` so a user with their own copy of the game can produce the SD
  card folder without installing a toolchain.

#### 7. First real-console runs and the long tail
- With the first full SD card on hardware: **~16 fps average on 765 Stadium**, menus 20-28 fps, mods working.
- Hardware-only problems appeared: file handles running out during a race, the sky flickering when the camera turned
  faster than the sky texture's margin, texture quads showing as white or grey when small textures were starved of
  linear memory, passwords failing because Argon2 asks for 8 MB from an empty C heap.
- Each was reproduced in the emulator when possible, otherwise fixed from the log.

#### 8. Measuring what is left
- A frame accountant (`[METER]`) now splits every frame into pre, tics, drawing, present and waiting with a few
  clock reads and no per-function hooks, because per-function profilers were shown to overstate small functions.
- On the heaviest map (Robotnik Coaster, 4 racers) a real-console log gave **~2.3 fps**, 60% game logic and 40%
  drawing, with no single hot spot. The frame-time model `T = D / (1 - 0.035 L)` explained why: every millisecond of
  logic per tic costs about three times as much as a millisecond of drawing. See [PERFORMANCE.md](../PERFORMANCE.md).

### What the process taught
- **Measure before changing.** The largest single improvements were found by profiling, not by guessing.
- **The emulator is a proxy.** Memory fragmentation, file handles and SD speed only exist on hardware.
- **Cheap instruments.** Heavy instrumentation changes what it measures; a handful of clock reads is enough.
- **Bugs hide in the original.** Three upstream bugs (state recursion counter, an out-of-bounds HUD write, an ACS
  string leak) were found only because the console has no slack to hide them.
- **Know when to stop.** For the heaviest maps the remaining levers change the game (fewer racers, simplified map
  geometry), so they were documented instead of silently applied.

## Sacrifices

Everything that was cut, reduced or changed to make Ring Racers run on an Old 3DS, with the reason. Nothing here is
hidden: if a PC player would notice it, it is in this table.

### Summary

| Area | Original | On the 3DS | Why |
|---|---|---|---|
| Online play | Supported | **Removed** | No networking stack fits in 64 MB next to the game; the deep map copy it needed alone cost 3.6 MB |
| Frame rate | 35 tics/s, display synced | 20-28 fps in menus, ~16 fps on light maps, ~2 fps on the heaviest | CPU and GPU limits (see [Performance model](Home.md#performance-model)) |
| Racers per race | Up to 16 | **4** (you + 3 bots) | Every bot costs ~75 ms per frame on heavy maps |
| Textures | Full resolution, 32-bit | **ETC1 / ETC1A4, maximum 256 px** | 6 MB of VRAM, CPU cannot write VRAM, SD read speed |
| Character skins | Any size, live colours | Skin sprites stay 16-bit (not ETC1) | They carry per-skin colour; ETC1 would need a GPU tint pass |
| Mods | Anything | **Character packs only** (skins + voices), max 16 packs, 12 mounted at once | Memory and open-file limits; no mod music or maps |
| Character loading | Instant | Lazy: ~1 s the first time a character is selected | 63 vanilla characters cannot all be resident |
| Draw distance | Long | Shorter by default (4096) | BSP and geometry cost grows with distance |
| Rings | All drawn | Capped per BSP pass (20/14/8/4 by the budget governor); far rings animate less or vanish | 50-110 sprites per frame in races |
| Sky | Redrawn every frame | Rendered to a texture and reused for several frames | Skybox pass cost ~15 ms on heavy maps |
| Animated textures | Normal speed | Textures >= 256x256 animate 3x slower | Upload cost |
| Weather | Whole map | Particles only near the camera | Rain/snow maps were the slowest and some ran out of memory |
| Explosions, mines, prison capsules, Shrink | Many small objects | Far fewer objects | Hundreds of short-lived objects per hit; changes RNG use, so replays differ from PC |
| Bot behaviour | Full AI every tic | Finish-line distance refreshed 1 tic in 4; extras limited | Pathfinding and collision per bot per tic |
| Music | Resynchronised per frame | Resync disabled | It re-read the SD card ~35 ms per frame |
| Whole-WAD caching | ~150 MB of lumps in RAM | Raw converted files streamed from the SD | 64 MB total |
| Distribution | One game file | Converted assets on the SD card, not a CIA (RomFS packaging not done) | Time; the converter produces roughly 1.3-2.3 GB depending on music quality |
| HOME button | Normal | Known crash in the HOME Menu process; quit from the game menu | Cause not found |

### Detail

#### Online play
The network layer and a deep copy of the map that only online play used do not fit.
Removing them freed memory the single-player game needed more.

#### Racer count
Per-kart logic is the biggest recurring cost: movement and collision (`P_CheckPosition`, ~26 lines per call),
waypoint pathfinding and AI. With four racers the cap is a measured compromise; one racer on Robotnik Coaster ran at
5.5 fps, two at 3.9 and five at 2.4.

#### Textures
ETC1 is 4 bits per pixel and is decoded by the GPU, so a texture costs a quarter of the VRAM. The conversion happens on
a PC; the 3DS never decodes images. The 256 px cap is a hard rule of the port because larger textures could not be
uploaded within the per-frame budget.

#### Mods
Character mods work because a skin is small and self-contained. Music and map mods would need more open files and RAM
than exist, so they were ruled out as a fixed decision.

#### Frame rate
The target of 35 fps was never reachable. The realistic target was "real game speed at about 10 fps on the worst map",
and the logic loop is capped at 11 tics per frame so game time stays real at low frame rates. The cost is a choppy
picture, not slow gameplay.

#### What was kept on purpose
Item behaviour, physics, track layout, lap and position logic, character stats and the HUD content are unchanged. The
sacrifices above are about *how much is simulated and drawn*, not about the rules of the race.

## Dead ends

Things that were tried, measured and thrown away. Listed so nobody repeats them, and because they explain why the
final design looks the way it does. All measurements are from the Old 3DS configuration of the Azahar emulator unless
noted; the heaviest test map is Robotnik Coaster.

| Idea | Result | Why it failed |
|---|---|---|
| Run game logic on core 1 | Rejected before building | Core 1 is capped at 30% of a core for games: the logic would be slower there |
| Mipmaps | Rejected | VRAM is already full in a race; +33% per texture means more evictions and re-uploads |
| Grey ETC1 sprites tinted on the GPU for skins | Rejected | An ETC1 read costs more than the RGBA4 rebuild it would replace |
| Faster FPU mode (RunFast) | No gain | The console already starts with flush-to-zero |
| Draw distance 1024 on Coaster | 2.74 -> 3.55 fps | Visually unplayable and still far from the goal |
| Almost no sprites (cap 1) on Coaster | 2.54 -> 2.65 fps | Sprites are not the cost; a sprite shader would gain at most ~4% |
| A shader for decoration | Not built | The measured cost is game logic, which a vertex shader cannot touch |
| Precomputed visibility (`.rrv`) on open maps | ~1% saved on Coaster | About 60% of the map is visible from anywhere |
| Putting map waypoints and markers to sleep | No change | The cost was the profiler's own overhead on 4 µs objects |
| Idle scenery collision check 1 tic in 4 | Collision checks 10 -> 2.5 per tic, fps unchanged | It was not where the time went |
| Evicting recently used textures from VRAM | No change in missing textures | The shortage was linear memory and the per-frame upload budget, not VRAM space |
| Bot AI every second tic | A bot stood still for the whole race | The recovery logic needs the AI every tic |
| Sprite caps and ring distance as the "quality level" | 0 ms on heavy maps | Only the ring cap moved the frame time (-6 to -9 ms) |

### Lessons from the dead ends
- A per-function profiler overstates small, frequently called functions by 3-17 µs per call; two of the dead ends
  above were chasing that overhead.
- The emulator hides memory fragmentation and SD behaviour, so some fixes could only be judged from a console log.
- When a map is limited by geometry plus logic, runtime quality knobs cannot save it. The remaining levers are
  structural: fewer racers on heavy maps and offline simplification of the map geometry.
