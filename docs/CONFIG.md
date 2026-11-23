# Configuration (`rr_config.txt`) and log tags

All test/tuning options live in one file next to the game: `sdmc:/3ds/RingRacers3ds/rr_config.txt` on the console
(`prepared/rr_config.txt` for the PC map preparer). One option per line, `key = value  # comment`. A missing key uses its
default. On the console only `rr_config.txt` is read (no per-key files), because every failed `fopen` on the SD card
wastes time and file handles.

## Keys

| Key | Default | Meaning |
|---|---|---|
| `mode` | `normal` | `normal` = regular game, `race` = straight to `map`, `bench` = automatic benchmark |
| `map`, `item`, `laps` | - | Map for race/bench (e.g. `RR_765STADIUM`), fixed starting item, consecutive bench races |
| `64mb` | 0 | Emulator only: limit memory to 64 MB like an Old 3DS |
| `gpu3d` | 1 | 3D world on the GPU (citro3d) instead of software |
| `diag` | 0 | Expensive diagnostics in the log (frame rates are not valid) |
| `meter` | 1 | Always-on frame accountant `[METER]` |
| `max_racers` | 4 | Racers per race (you + bots), 4 maximum |
| `bots_bench` | 0 | Bench only: bot level and count |
| `genmap` | 0 | Map package generation mode (set by the preparer tools) |
| `bot_ai_mask` | 0 | `1` runs bot AI every second tic (can leave a bot stuck) |
| `bot_wp_mask`, `human_wp_mask` | 7, 1 | Waypoint update masks for bots / humans |
| `bot_effects` | 0 | Visual effects for bots |
| `dist` | 12288 | World draw distance (1024-32768) |
| `rings_max` | auto | Cap on rings projected per BSP pass (the budget governor drives it) |
| `sprites_max` | 0 (off) | Cap on non-ring sprites per frame |
| `sleeping_rings` | 1 | Idle rings sleep and are drawn from the clock |
| `far_sleep` | 1 | Far rings/effects think less or are removed |
| `fling_nocol` | 1 | Thrown rings skip a repeated collision check |
| `cheap_move` | 1 | Decorations move without `P_CheckPosition` |
| `slow_anim` | 3 | Animated textures >= 256x256 change 3x slower |
| `precip_density`, `precip_max`, `precip_follow` | 2, 512, 24 | Weather particles that follow the camera |
| `budget` | 1 | Runtime budget governor (adjusts the ring cap) |
| `budget_target_fps` | 14 | Target frame rate of the governor |
| `budget_fixed_level` | -1 | Force a level (0-3) for measuring |
| `pvs_cull` | 1 | Skip zones outside the precomputed visibility list (`.rrv`) |
| `fast_sub` | 1 | Per-zone fast drawing path |
| `sky_every` | 4 | Skybox texture redrawn 1 frame in N+1 |
| `sky_always`, `skyflip`, `noskytex`, `dbgflat`, `cpu_present`, `nostatic` | 0 | Debug switches |
| `etc_cache_kb` | 1024 | ETC texture cache size (capped at 1536) |
| `fixed_atlas` | 1 | Object atlas sheets pinned in VRAM |
| `defer_uploads` | 1 | Skip a texture for a frame instead of failing when memory is short |
| `defer_small` | 1 | Textures <= 16 KB use a 48 KB reserve instead of 192 KB |
| `vram_age_ms` | 2000 | Textures unused for this long may be evicted from VRAM |
| `kart_uploads` | 2 | Kart sprite uploads allowed per frame |
| `blend_slots` | 8 | Colormap blend tables kept in RAM |
| `skins_max` | 12 | Character packs mounted at once |
| `preload`, `preload_min_kb` | 1, 3072 | Per-map/per-character preload lists |
| `arena_malloc` | 1 | Serve big `malloc`/`new` from the arena |
| `zone_arena_mb` | auto | Arena size (0 disables the arena) |
| `small_heap_mb` | 4 | Heap left outside the arena |
| `relief_valve_kb` | 1536 | Release sprites when real free arena memory drops below this |
| `fragment_heap` | 0 | Emulator only: fragment the heap like the console |
| `music_sync` | 0 | `1` restores the per-frame music resync (costly on the SD) |
| `home_quit` | 0 | Quit when HOME is pressed (workaround for the HOME crash) |
| `wipe` | 1 | GPU-composited screen wipes (0 = turn them off) |
| `wipe_flip`, `wipe_mask_flip` | 0 | Orientation workarounds for the GPU wipe |
| `menu_gpu`, `menu_blend`, `deferred_present`, `map_background` | see code | Menu rendering switches |

## Log tags

Messages are prefixed with a tag and written to `rrlog_MMDD_HHMMSS.txt` (and `rrdiag_*.txt` with `diag=1`).

| Tag | Meaning |
|---|---|
| `[METER]` | Frame accountant (see [PERFORMANCE.md](PERFORMANCE.md)) |
| `[FPS]` | Average / min / max frame rate every 5 s |
| `[WORST]` | Worst-case summary of a bench run (tic and frame percentiles) |
| `[STALL]` | A frame longer than 1 s: where the time went |
| `[RAM]`, `[ARENA]`, `[ZARENA]`, `[FREEMEM]`, `[LINUSE]`, `[LINMARK]`, `[LINALLOC]` | Memory: heap, arena, linear memory, who allocates |
| `[TEXTOP]`, `[TEXEVICT]`, `[UPDETAIL]`, `[UPLOAD]`, `[DEFERRED]`, `[TEXALLOCFAIL]` | Texture memory and upload behaviour |
| `[SUB]`, `[DIB]`, `[FASTPATH]`, `[EST]`, `[SKYTEX]`, `[PVS]` | Drawing breakdown and caches (diag) |
| `[MT]`, `[CP]`, `[TIC]`, `[TICFULL]`, `[MK7]` | Logic breakdown per object type / collision checks / tic (diag) |
| `[BUDGET]` | Budget governor level changes |
| `[PRELIST]`, `[PRELOAD]`, `[SD]` | Preload lists and SD activity |
| `[GPUBLEND]`, `[GPUBLENDFAIL]`, `[FADE]` | GPU menu blend layers and wipes |
| `[TRAP]`, `[CRASH]` | Crash trap: registers, stack, memory state |
| `[FOPEN]` | File handle accounting |
| `[BENCH]`, `[GEN]`, `[VIS]` | Bench runs; map package / visibility generation |
