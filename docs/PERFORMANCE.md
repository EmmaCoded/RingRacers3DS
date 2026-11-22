# Performance notes (Old 3DS)

How the port is measured, what was found, what did not work and what the numbers say. All emulator figures
come from Azahar configured as an Old 3DS (64 MB, 100% CPU, simulated GPU timings). Hardware figures come from
logs captured on a real console.

## Methodology

- **`[METER]`** (`src/d_main.cpp`, always on in races, `meter=0` disables it) is a frame accountant that only
  reads the clock a few times per frame. Every 3 s it logs where the wall time of a frame goes:

  ```
  [METER] 2.3 fps | frame 430 ms (worst 513) with 11.0 tics/frame = pre 0.0 + TICS 245 + DRAW 175 + rest 0.5 + wait 8
  [METER]   per tic: total 22.3 ms = P_Ticker 19.5 + G_Ticker excl. P_Ticker 0.6 + outside G_Ticker 2.2 | per-frame draw: 3D 145 + present 25 + HUD and rest 2
  ```

- **`diag=1`** enables expensive diagnostics (`[MT]` per-object-type cost, `[CP]` collision checks per type,
  `[SUB]`/`[DIB]` drawing breakdown, `[TEXTOP]`, `[RAM]`...). Frame rates measured with it are *not* valid.
- **Per-function profilers** (`RR_PROF=1` for logic, `RR_PROFD=1` for drawing) instrument every call. Lesson learned
  twice: they overstate small, frequently called functions by 3-17 us per call, so only functions with real work per
  call (>= 10 us) can be trusted. Several "hot spots" (map waypoints, tiny decorations) disappeared once measured
  with the cheap frame accountant.
- A bench mode (`mode = bench` in `rr_config.txt`) runs a fixed race several times and averages the result;
  `[WORST]` records the per-map worst case (tic p95/p99/max, frame, draw, object count).

## The frame-time model

The game wants 35 logic tics per second. When a frame is slow it runs several tics per frame (capped at 11 on the
3DS so game time stays real). With `L` = logic ms per tic and `D` = drawing ms per frame, the steady-state frame time is

```
T = D / (1 - 0.035 * L)
```

so every millisecond removed from `L` is worth roughly 3x more than a millisecond removed from `D` when `L` is large.
For 10 fps (`T` = 100 ms):

| Logic per tic (L) | Draw per frame (D) must be <= |
|---|---|
| 18 ms | 37 ms |
| 12 ms | 58 ms |
| 8 ms | 72 ms |

25 fps does not fit in any split on this hardware for the heavy maps.

## Case study: Robotnik Coaster (4 racers)

Hardware log (Old 3DS): ~2.3 fps; frame ~430 ms = tics ~245 ms (11 tics x P_Ticker 19-21 ms) + drawing ~175 ms
(3D ~145 ms, present ~25 ms). The emulator gives 2.3-2.7 fps, so for this map it is a faithful proxy.

| Experiment (emulator) | Result |
|---|---|
| 1 / 2 / 5 racers | 5.5 / 3.9 / 2.4 fps: every bot costs ~75 ms per frame |
| Draw distance 12288 / 2048 / 1024 | 2.74 / 3.15 / 3.55 fps (1024 is unplayable visually) |
| Almost no sprites (`sprites_max=1`, `rings_max=1`) | 2.54 -> 2.65 fps: a sprite shader would gain <= 4% |
| Bots refresh finish-line distance 1 tic in 4 | 2.78 vs ~2.4-2.7 fps (within noise; kept) |
| Idle scenery collision checks 1 tic in 4 | collision checks 10 -> 2.5 per tic, fps unchanged (reverted) |
| Sleeping map markers (waypoints) | no change: the cost was the profiler's (reverted) |

Logic profile (per tic): players ~7 ms, thinkers ~11-12 ms. The most expensive items with real work are
`P_CheckPosition` (~66 calls per tic, ~26 lines each) and `K_PathfindAStar` (~6 calls per tic, ~0.25 ms each).
There is no single dominant function.

Conclusion: Coaster is limited by game logic (about 60%) and geometry (about 40%). What remains are structural
changes: fewer racers on heavy maps and offline geometry simplification (see [3DS_PORT.md](3DS_PORT.md)).

## Other findings

- **White/grey sprite quads** (`[DEFERRED]`, `[LINUSE] deferred=`): on hardware, 4270 deferrals in one race. Textures of
  1-2 KB were deferred while 4 MB of VRAM was free because linear memory (~140 KB free) could not satisfy a 192 KB
  reserve and the DMA ring was exhausted. Small textures now keep a 48 KB reserve (`defer_small`): deferrals drop from
  ~1590 to ~1080 and untextured frames from 578 to 391/3/0 (three runs). Evicting recently used textures was tried and
  made no difference.
- **Bots getting stuck**: running bot AI every second tic (`bot_ai_mask=1`) leaves a bot standing for the whole race;
  the default is 0 on 3DS.
- **Upstream `P_SetMobjState` recursion counter** was never decremented (28 KB cleared per state change); fixing it
  together with skipping collision checks for the fog objects cut Umbrella Rushwinds from 155 to 47 ms per tic (5 racers).
- **Collision checks of decorations** dominate on some maps; `[CP]` lists who requests them and was the most
  productive diagnostic.
