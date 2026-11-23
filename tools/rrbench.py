# rrbench.py - opens Azahar with the mode's configuration, runs the bench and closes it by itself.
# Usage: py rrbench.py dev      (dev mode: CPU 300%, New 3DS, diagnostics ON)
#       py rrbench.py measure  (OLD 3DS mode, 64 MB: CPU 100%, Old 3DS, diagnostics OFF)
# add "build" to build first (WITHOUT RomFS by default):  py rrbench.py dev build
# add "clean" to build from scratch; "romfs" to embed the assets (final version)
# with "build": "3dsxonly" | "assetsonly" | "both" (default)
# "diag" = diagnostics ON even when measuring (to analyze crashes/timing with 64 MB)
# "64mb" = limit to 64 MB even in dev (fast CPU but Old 3DS memory)
# "manual" = you drive (special stages and prisons). Ends when
# the phase/prison (or 1 lap on a regular map). No hang watchdog.
# add a map:  py rrbench.py measure RR_SealedStarBalconies
# detects ALL known crashes in the log ([TRAP], bad_alloc, I_Error, heap
# fragmented, Z_Malloc, hangs without writing...), closes Azahar by itself and saves
# the report in rr_crash.txt (with the stack resolved by addr2line if installed).
import os, sys, re, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rr_paths as P
os.makedirs(P.LOGS, exist_ok=True)

AZ      = P.AZAHAR
EXE     = os.path.join(AZ, "azahar.exe")
INI     = os.path.join(AZ, r"user\config\qt-config.ini")
GAME_INI   = os.path.join(AZ, r"user\config\custom\ringracers.3dsx.ini")  # the game's individual config
SDMC    = os.path.join(AZ, r"user\sdmc")
BASE    = os.path.join(SDMC, r"3ds\RingRacers3ds")  # the whole game lives here (sdmc:/3ds/RingRacers3ds)
LOG     = os.path.join(BASE, "ringracers_log.txt")
REPO    = P.REPO
GAME    = os.path.join(BASE, "ringracers.3dsx")
BUILT   = os.path.join(REPO, r"build\bin\ringracers.3dsx")
MAP   = "RR_RobotnikCoaster"
TIMEOUT = 20 * 60  # seconds
HANG_TIMEOUT = 4 * 60   # if the log does not grow in 4 min (the longest load measured: ~83 s) = hung
CRASH_TXT = os.path.join(P.LOGS, "rr_crash.txt")
ELF     = os.path.join(REPO, r"build\bin\ringracers.elf")
TOOLS   = os.path.dirname(os.path.abspath(__file__))  # all scripts live in RingRacers\tools
CFG     = os.path.join(BASE, "rr_config.txt")        # A SINGLE configuration file

# rr_config.txt replaces the loose rr_bench, rr_64mb, rr_gpu3d, rr_diag... files
LEGACY_FILES = ["bench", "64mb", "gpu3d", "diag", "dist", "genmap", "noskytex", "skyflip",
          "dbgflat", "cpu_present", "nostatic", "laps"]
HELP = [
    ("mode",        "normal = regular game (menus) | race = straight to the map, you drive | bench = automatic"),
    ("map",         "map for race/bench (e.g. RR_765STADIUM)"),
    ("item",        "fixed item at the start in race mode (e.g. SNEAKER)"),
    ("laps",        "bench: consecutive races without closing Azahar (1-10)"),
    ("64mb",        "1 = limit memory to 64 MB like Old 3DS (Azahar gives 96)"),
    ("gpu3d",       "1 = 3D world on the GPU (citro3d). 0 = software"),
    ("diag",        "1 = expensive diagnostics in the log (fps NOT valid)"),
    ("genmap",      "1 = generate a map package (set by rrbench generate)"),
    ("dist",        "draw distance (1024-32768, default 12288)"),
    ("noskytex",    "1 = sky without texture"),
    ("skyflip",     "flip sky: 0 no, 1 s, 2 t, 3 both"),
    ("dbgflat",     "draw debug 0-9 (0 = off)"),
    ("cpu_present", "1 = present through the CPU (old path, debug)"),
    ("nostatic",    "1 = turn off the static map cache (debug)"),
]

def read_cfg(base=BASE):
    """Reads rr_config.txt and MIGRATES the old loose rr_* files into it (deleting them)."""
    d = {}
    path = os.path.join(base, "rr_config.txt")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8", errors="replace"):
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1); d[k.strip().lower()] = v.strip()
    for k in LEGACY_FILES:
        p = os.path.join(base, "rr_" + k)
        if not os.path.isfile(p): continue
        v = open(p, encoding="utf-8", errors="replace").read().strip()
        if k == "bench":
            t = v.split()
            if "mode" not in d and t:
                m = t[0].upper()
                if m in ("FULLOFF", "FULL_OFF"): d["mode"] = "normal"
                elif m == "OFF": d["mode"] = "race"; t = t[1:]
                elif m == "ON": d["mode"] = "bench"; t = t[1:]
                else: d["mode"] = "bench"
                if t: d.setdefault("map", t[0])
                if len(t) > 1: d.setdefault("item", t[1])
        elif k not in d:
            d[k] = v or "1"   # empty file = on (as before)
        os.remove(p); print("== migrated into rr_config.txt: rr_" + k)
    return d

def write_cfg(d, base=BASE):
    lines = ["# Ring Racers 3DS - configuration. One option per line:  key = value",
              "# Changed by rrbench.py automatically; it can also be edited by hand.", ""]
    for k, help_text in HELP:
        if k in d: lines.append("%-12s = %-16s # %s" % (k, d[k], help_text))
    for k in d:
        if k not in dict(HELP): lines.append("%-12s = %s" % (k, d[k]))
    with open(os.path.join(base, "rr_config.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")

def console_cfg(base):
    """Clean config for the real console / package: normal game, no tests."""
    d = read_cfg(BASE) if os.path.exists(BASE) else {}
    for k in ("map", "item", "laps", "diag", "genmap", "64mb", "dbgflat", "cpu_present", "nostatic", "dist", "etc_cache_kb", "fragment_heap", "bots_bench"): d.pop(k, None)
    d["mode"] = "normal"; d.setdefault("gpu3d", "1")
    write_cfg(d, base)

# all the crashes seen so far. If one shows up, Azahar is closed.
CRASHES = [
    (r"\[TRAP\]",                       "game crash trap ([TRAP])"),
    (r"bad_alloc",                       "out of memory (std::bad_alloc in new)"),
    (r"std::terminate",                  "std::terminate"),
    (r"OUT OF MEMORY",                  "out of memory"),
    (r"process stopped",                "the game stopped by itself"),
    # "fragmented heap" is NOT a crash: it is a warning (the block is requested from linear and it carries on).
    (r"Z_Malloc.*(fall|fail|out of memory)", "Z_Malloc failed"),
    (r"Out of memory|out of memory",     "out of memory (Out of memory)"),
    (r"I_Error|Fatal error|FATAL",       "fatal engine error (I_Error)"),
    (r"\[FIN\] reason=",                 "the game reported [FIN]"),
    (r"Data abort|Prefetch abort|Undefined instruction", "ARM exception"),
]

MODES = {
    #            section     key                         value
    "dev":   [("Core",     "cpu_clock_percentage",     "300"),
              ("System",   "is_new_3ds",               "true"),
              ("Renderer", "simulate_3ds_gpu_timings", "false")],
    "measure": [("Core",     "cpu_clock_percentage",     "100"),
              ("System",   "is_new_3ds",               "false"),
              ("Renderer", "simulate_3ds_gpu_timings", "true")],
}
# The mode + frame_limit go in the game's INDIVIDUAL config (they do not touch other games).
# What is below only exists in the global config (debugging / window).
# frame_limit=100 (REAL speed). With 0 the game ran too fast when driving
# (special stages/prisons). What is measured does not change: the game measures with its emulated clock.
GAME_ONLY = [("Renderer", "frame_limit", "100")]
# Azahar ignored the game config's frame_limit (it ran at 400-600%).
# Also in the global one: ALWAYS real speed (100%), in every mode.
COMMON = [("Renderer",  "frame_limit",              "100"),
           ("Debugging", "enable_exception_handler", "true"),
           ("Core",      "use_cpu_jit",              "true"),
           ("UI",        "confirmClose",             "false"),
           ("UI",        "showConsole",              "false")]

def set_ini(lines, sec, key, val, mark="default", mval="false"):
    """Global: key\\default=false. Game: key\\use_global=false. And key=value."""
    ini = fin = None
    for i, l in enumerate(lines):
        if l.strip() == "[" + sec + "]": ini = i
        elif ini is not None and l.startswith("[") and i > ini: fin = i; break
    if ini is None:
        lines += ["", "[" + sec + "]"]; ini = len(lines) - 1; fin = len(lines)
    if fin is None: fin = len(lines)
    for k, v in ((key + "\\" + mark, mval), (key, val)):
        for i in range(ini + 1, fin):
            if lines[i].split("=", 1)[0] == k: lines[i] = k + "=" + v; break
        else:
            lines.insert(fin, k + "=" + v); fin += 1

def find_bash():
    """Finds MSYS2's bash (devkitPro ships its own)."""
    candidates = [r"C:\devkitPro\msys2\usr\bin\bash.exe",
             r"C:\msys64\usr\bin\bash.exe", r"D:\msys64\usr\bin\bash.exe",
             r"D:\devkitPro\msys2\usr\bin\bash.exe"]
    for c in candidates:
        if os.path.exists(c): return c
    for drive in "CDEFG":
        for root in (drive + ":\\",):
            try:
                for d in os.listdir(root):
                    c = os.path.join(root, d, r"usr\bin\bash.exe")
                    if "msys" in d.lower() and os.path.exists(c): return c
                    c = os.path.join(root, d, r"msys2\usr\bin\bash.exe")
                    if os.path.exists(c): return c
            except OSError: pass
    return None

def azahar_running():
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq azahar.exe"], capture_output=True, text=True)
    return "azahar.exe" in r.stdout.lower()

def find_addr2line():
    for c in (r"C:\devkitPro\devkitARM\bin\arm-none-eabi-addr2line.exe",
              r"D:\devkitPro\devkitARM\bin\arm-none-eabi-addr2line.exe"):
        if os.path.exists(c): return c
    return shutil.which("arm-none-eabi-addr2line")

def crash_report(reason, textStr):
    """Saves the reason, the end of the log and the resolved stack into rr_crash.txt."""
    lines = textStr.splitlines()
    out = ["REASON: " + reason, "DATE: " + time.strftime("%Y-%m-%d %H:%M:%S"), "",
           "--- last 60 log lines ---"] + lines[-60:]
    addrs = []
    for l in lines:
        if "[TRAP]" in l:
            addrs += re.findall(r"LR=0x([0-9a-fA-F]+)", l)
            if "stack:" in l: addrs += re.findall(r"\b([0-9a-fA-F]{6,8})\b", l.split("stack:", 1)[1])
    seen = []
    for d in addrs:
        if d not in seen: seen.append(d)
    a2l = find_addr2line()
    if seen and a2l and os.path.exists(ELF):
        r = subprocess.run([a2l, "-f", "-C", "-e", ELF] + seen, capture_output=True, text=True)
        res = r.stdout.splitlines()
        out += ["", "--- stack (addr2line) ---"]
        for i, d in enumerate(seen):
            fn = res[2*i] if 2*i < len(res) else "?"; ln = res[2*i+1] if 2*i+1 < len(res) else "?"
            out.append("0x%s  %s  %s" % (d, fn, ln))
    elif seen:
        out += ["", "(no addr2line: raw stack) " + " ".join(seen)]
    with open(CRASH_TXT, "w", encoding="utf-8") as f: f.write("\n".join(out) + "\n")

MODE_TXT = os.path.join(REPO, "build_mode.txt")  # switch: 1 / 2 / 3
def build_mode_file():
    """1 = 3dsxonly, 2 = assetsonly, 3 = both. If it does not exist or is not understood: both."""
    try: v = open(MODE_TXT, encoding="utf-8").read().strip()[:1]
    except OSError: v = "3"
    m = {"1": "3dsxonly", "2": "assetsonly", "3": "both"}.get(v, "both")
    print("== build_mode.txt = " + v + " -> " + m)
    return m

def generate_all(args):
    """Generates the .rr3 package for ALL the maps that are missing, one after another, by itself.
    Each map: opens Azahar (low priority, half the cores), waits for [GEN] DONE and closes.
    If a map fails or takes more than 20 min, it moves on to the next (next time it resumes)."""
    import zipfile
    pk = os.path.join(BASE, r"data\maps.pk3")
    maps = sorted({os.path.basename(n)[:-4] for n in zipfile.ZipFile(pk).namelist()
                    if n.lower().endswith(".wad") and os.path.basename(n).upper().startswith("RR_")})
    dest_dir = os.path.join(BASE, r"cache\maps")
    def is_complete(m):
        f = os.path.join(dest_dir, m + ".rr3")
        if not os.path.exists(f) or os.path.getsize(f) < 32: return False
        with open(f, "rb") as h:
            h.seek(-16, 2); return h.read(4) == b"RR3F"
    missing = [m for m in maps if not is_complete(m)]
    print("== GENERATE ALL: %d maps, %d missing. You can do something else; it closes by itself." % (len(maps), len(missing)))
    if "build" in args:
        r = subprocess.run([sys.executable, os.path.abspath(__file__), "measure", "build", "generate", missing[0] if missing else "RR_TestRun"], timeout=30 * 60)
    done, failed = 0, []
    if "build" in args and missing:
        # the 1st map was already attempted when building: do not repeat it in the same turn
        if is_complete(missing[0]): done += 1
        else: failed.append(missing[0])
        missing = missing[1:]
    for i, m in enumerate(missing):
        print("== [%d/%d] %s" % (i + 1, len(missing), m))
        # if the race ends before the package, the SAME map is reopened and the game
        # resumes where it left off (.prog), up to 6 attempts.
        for attempt in range(6):
            try:
                # without "manual": the bot drives by itself (no need to touch anything); ends when [GEN] DONE appears
                subprocess.run([sys.executable, os.path.abspath(__file__), "measure", "generate", m], timeout=20 * 60)
            except subprocess.TimeoutExpired:
                subprocess.run(["taskkill", "/IM", "azahar.exe", "/F"], capture_output=True)
            if is_complete(m) or not os.path.exists(os.path.join(dest_dir, m.upper() + ".rr3.prog")): break
            print("== %s is half done: reopening to continue (attempt %d)" % (m, attempt + 2))
            time.sleep(5)
        if is_complete(m): done += 1
        else: failed.append(m)
        time.sleep(5)  # breather for the PC between maps
    print("== GENERATE ALL finished: %d done, %d with problems %s" % (done, len(failed), failed if failed else ""))
    with open(os.path.join(P.LOGS,time.strftime("%Y-%m-%d_%H%M") + "_generate_all.txt"), "w") as f:
        f.write("done=%d\nproblems=%s\n" % (done, " ".join(failed)))
    print("== Result: \"done\"")
    return 0


# MOVE. Before, everything was loose in sdmc:/3ds (and the log in sdmc:/). Now it goes in
# sdmc:/3ds/RingRacers3ds. It happens once, by itself. dspfirm.cdc stays in sdmc:/3ds (the system requires it).
def migrate_layout():
    old_dir = os.path.join(SDMC, "3ds")
    os.makedirs(BASE, exist_ok=True)
    name_map = {"ringracers": "cache"}
    for n in os.listdir(old_dir):
        if n in ("RingRacers3ds", "dspfirm.cdc"): continue
        if not (n in ("bios.pk3", "data", "mus", "sfx.rrp", "ringracers", "ringracers.3dsx", "srvbans.json", "srvstats.dat")
                or n.startswith(("rr_", "ring")) or n.endswith((".cfg", ".dat", ".prf", ".bak"))): continue
        dst = os.path.join(BASE, name_map.get(n, n))
        if os.path.exists(dst): continue
        shutil.move(os.path.join(old_dir, n), dst); print("== moved: 3ds\\" + n + " -> 3ds\\RingRacers3ds\\" + name_map.get(n, n))
    for n in ("ringracers_log.txt", "ringracers_perf.txt"):
        v = os.path.join(SDMC, n)
        if os.path.exists(v): os.remove(v)

# PACKAGE for the real console: copies Azahar's sdmc:/3ds/RingRacers3ds to
# RingRacers\RingRacers3ds (without logs or test files) + a txt with instructions.
def build_package():
    dest_dir = os.path.join(REPO, "RingRacers3ds")
    excluded = ("ringracers_log.txt", "ringracers_perf.txt", "ram_actual.txt", "ram_table.csv")
    def ignore(d, name_map):
        return [n for n in name_map if n in excluded or n.startswith("rr_") or n.endswith((".prog", ".bak", ".dat", ".prf", ".json"))]
    print("== Building package in " + dest_dir + " (may take a while, ~1.6 GB)...")
    if os.path.exists(dest_dir): shutil.rmtree(dest_dir)
    shutil.copytree(BASE, dest_dir, ignore=ignore)
    console_cfg(dest_dir)  # normal game on the console
    for n in os.listdir(REPO):
        if n.startswith("COPY ") and n.endswith(".txt"): os.remove(os.path.join(REPO, n))
    with open(os.path.join(REPO, "COPY the RingRacers3ds folder to sdmc_3ds.txt"), "w", encoding="utf-8") as f:
        f.write("""RING RACERS 3DS - HOW TO INSTALL ON THE CONSOLE
===============================================

1. Power off the console and take out the SD card. Put it in the PC.
2. Open the SD. Go into the "3ds" folder (create it if it does not exist).
3. Copy the "RingRacers3ds" folder (the one next to this txt) INSIDE "3ds".
   It must end up like this:
       SD:/3ds/RingRacers3ds/ringracers.3dsx
       SD:/3ds/RingRacers3ds/bios.pk3
       SD:/3ds/RingRacers3ds/data/...
       SD:/3ds/RingRacers3ds/mus/...
       SD:/3ds/RingRacers3ds/cache/...
4. Audio: SD:/3ds/dspfirm.cdc is required (dump it with DSP1 from the Homebrew Launcher).
   Without that file the game runs without sound.
5. Put the SD in the console, open the Homebrew Launcher and pick "Ring Racers".

In that same folder (SD:/3ds/RingRacers3ds) the game stores its config, profiles, saves,
replays, mods (addons folder) and the log (ringracers_log.txt).

Do NOT rename the "RingRacers3ds" folder: the game looks for that exact name.
""")
    print("== Package ready: " + dest_dir)

def average_line(ends, laps):
    """Merges the [BENCH] end lines of several races into one line with the average."""
    if len(ends) == 1 and laps == 1: return ends[0]
    fps = [float(x) for f in ends for x in re.findall(r"avg_fps=([\d.]+)", f)]
    ms = [float(x) for f in ends for x in re.findall(r"ms_per_frame=([\d.]+)", f)]
    if not fps: return ends[-1]
    return ("AVERAGE of %d races: avg_fps=%.2f (min %.2f, max %.2f) ms_per_frame=%.1f | each: %s"
            % (len(fps), sum(fps) / len(fps), min(fps), max(fps), sum(ms) / len(ms) if ms else 0,
               " ".join("%.2f" % x for x in fps)))

def small_window(pid):
    """Resizes the Azahar window to its real size: 400 wide x 480 high
    (top screen 400x240 + the bottom one 320x240 below), plus the menu bar."""
    try:
        import ctypes
        from ctypes import wintypes
        u = ctypes.windll.user32
        for _ in range(60):  # up to ~30 s for the window to appear
            time.sleep(0.5)
            found = []
            def cb(h, _l):
                pid_h = wintypes.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(pid_h))
                if pid_h.value == pid and u.IsWindowVisible(h) and u.GetWindowTextLengthW(h) > 0: found.append(h)
                return True
            u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)(cb), 0)
            if not found: continue
            h = found[0]
            r = wintypes.RECT(); c = wintypes.RECT()
            u.GetWindowRect(h, ctypes.byref(r)); u.GetClientRect(h, ctypes.byref(c))
            border_x = (r.right - r.left) - c.right; border_y = (r.bottom - r.top) - c.bottom
            MENU = 45  # menu bar + Azahar status bar (inside the client area)
            u.ShowWindow(h, 1)  # un-maximize if it was
            # top-left corner of the screen (0,0), without taking focus (SWP_NOZORDER | SWP_NOACTIVATE): the user keeps working on the right
            u.SetWindowPos(h, 0, 0, 0, 400 + border_x, 480 + MENU + border_y, 0x0004 | 0x0010)
            return
    except Exception as e:
        print("(could not shrink the window: %s)" % e)

LAST_RESULT = os.path.join(P.LOGS, "rr_last_bench.txt")
def several_runs(raw_args, n):
    """Opens the game n times (builds only on the first) and computes the average fps."""
    fps, mspf, logs = [], [], []
    for i in range(n):
        args_i = [a for a in raw_args if i == 0 or a.lower() != "build"] + ["_una"]
        print("\n########## RUN %d of %d %s##########" % (i + 1, n, "(builds) " if "build" in [a.lower() for a in args_i] else ""))
        if os.path.exists(LAST_RESULT): os.remove(LAST_RESULT)
        r = subprocess.run([sys.executable, os.path.abspath(__file__)] + args_i)
        if r.returncode != 0 or not os.path.exists(LAST_RESULT):
            print("!! Run %d failed: stopping here." % (i + 1)); print("== Result: \"failed\""); return 1
        line = open(LAST_RESULT, encoding="utf-8").read().splitlines()
        m = re.search(r"avg_fps=([\d.]+)", line[0]); m2 = re.search(r"ms_per_frame=([\d.]+)", line[0])
        if m: fps.append(float(m.group(1)))
        if m2: mspf.append(float(m2.group(1)))
        logs.append(line[1] if len(line) > 1 else "?")
        if i + 1 < n: time.sleep(5)
    avg = sum(fps) / len(fps) if fps else 0
    txt = ("== AVERAGE of %d runs  [OLD 3DS]  %s:  avg_fps=%.2f  (min %.2f, max %.2f)  ms_per_frame=%.1f\n== each: %s"
           % (len(fps), MAP, avg, min(fps or [0]), max(fps or [0]), sum(mspf) / len(mspf) if mspf else 0,
              "  ".join("%.2f" % x for x in fps)))
    print("\n" + txt)
    try:
        LOGS = P.LOGS; os.makedirs(LOGS, exist_ok=True)
        with open(os.path.join(LOGS, time.strftime("%Y-%m-%d_%H%M") + "_average_" + MAP + ".txt"), "w", encoding="utf-8") as f:
            f.write(txt + "\n" + "\n".join(logs) + "\n")
    except Exception as e: print("(could not save the average: %s)" % e)
    print("== Result: \"done\""); return 0

def main():
    global MAP
    raw_args = sys.argv[1:]
    args = [a.lower() for a in raw_args]
    migrate_layout()
    if "package" in args: return build_package() or 0
    # "normal" = leave the game normal (menus) to play by hand in Azahar
    if args[:1] == ["normal"]:
        c = read_cfg(); c["mode"] = "normal"; c["64mb"] = c.get("64mb", "1"); write_cfg(c)
        print("== rr_config.txt: mode = normal")
        # "normal build" = ONLY builds (does not open Azahar); add "open" to open it: "normal build open"
        if "build" in args:
            if azahar_running():
                subprocess.run(["taskkill", "/IM", "azahar.exe", "/F"], capture_output=True); time.sleep(2)
            bash = find_bash()
            if not bash: print("!! Could not find MSYS2 (bash.exe)."); return 1
            env = dict(os.environ, MSYSTEM="MSYS", CHERE_INVOKING="1", RR_AZAHAR=AZ, RR_WORK=P.WORK)
            print("== Building with " + bash)
            if subprocess.run([bash, "-lc", "bash " + P.msys_path(os.path.join(P.TOOLS, "build3ds.sh")) + " 3dsxonly"], env=env).returncode != 0:
                print("!! Build failed"); return 1
        if "open" in args:  # Azahar is ONLY opened when requested ("normal open" or "normal build open")
            print("== Opening Azahar (regular game)...")
            pr = subprocess.Popen([EXE, GAME], cwd=AZ); small_window(pr.pid)
        return 0
    # "console <folder>" = writes a clean rr_config.txt (used by prepare_sd.bat)
    if args[:1] == ["console"] and len(raw_args) > 1: console_cfg(raw_args[1]); return 0
    if "generate" in args and "all" in args: os.makedirs(P.LOGS, exist_ok=True); return generate_all(args)
    run_mode = next((a for a in args if a in MODES), None)
    if not run_mode:
        print("Usage: py rrbench.py dev|measure [build] [3dsxonly|assetsonly|both] [clean] [romfs] [RR_Map]"); return 1
    map_arg = next((a for a in raw_args if a.upper().startswith("RR_")), None)
    if map_arg: MAP = map_arg
    # in "measure" mode the race is run 3 TIMES without closing Azahar (the game restarts
    # the map by itself) and averaged. "once" = a single race as before.
    laps = 3 if (run_mode == "measure" and not ({"manual", "generate", "once"} & set(args))) else 1
    if azahar_running():
        print("== Closing Azahar...")
        subprocess.run(["taskkill", "/IM", "azahar.exe", "/F"], capture_output=True); time.sleep(2)

    if "build" in args:
        bash = find_bash()
        if not bash:
            print("!! Could not find MSYS2 (bash.exe). Tell the maintainer where it is installed."); return 1
        print("== Building with " + bash)
        env = dict(os.environ, MSYSTEM="MSYS", CHERE_INVOKING="1", RR_AZAHAR=AZ, RR_WORK=P.WORK)
        # small texture package (max 256) for maps with a custom palette
        pk = os.path.join(AZ, r"user\sdmc\3ds\RingRacers3ds\data\ringracers_3ds.pk3")
        ch = os.path.join(P.TEXTURES, "ringracers_3ds_small.bin")
        if os.path.exists(pk) and (not os.path.exists(ch) or os.path.getmtime(ch) < os.path.getmtime(pk)):
            print("== Creating small textures (first time only)...")
            os.makedirs(os.path.dirname(ch), exist_ok=True)
            subprocess.run([sys.executable, os.path.join(TOOLS, "convert_small.py"), pk, ch])
        # ETC1 for custom palettes (Sealed Stars): converts what the game recorded
        if os.path.exists(pk):
            print("== ETC1 for maps with TWEAKMAP / custom palettes (new ones only)...")
            A3 = TOOLS
            TX = P.TEXTURES
            subprocess.run([sys.executable, os.path.join(A3, "convert_tweakmap.py"),
                            os.path.join(BASE, r"data"), os.path.join(BASE, r"bios.pk3"), pk, os.path.join(TX, "pal")])
            subprocess.run([sys.executable, os.path.join(A3, "convert_etcpal.py"),
                            pk, TX, os.path.join(TX, "pal"), os.path.join(BASE, "cache")])
        # precomputed SPRITES (ETC1A4) - only if missing or if the pk3/script changed
        A3 = TOOLS
        TX = P.TEXTURES
        spr = os.path.join(TX, "ringracers_3ds_spr.bin")
        sources = [os.path.join(BASE, r"bios.pk3"), os.path.join(BASE, r"data\gfx.pk3"),
                   os.path.join(BASE, r"data\followers.pk3"), os.path.join(A3, "convert_sprites.py")]
        if all(os.path.exists(f) for f in sources) and (not os.path.exists(spr)
                or os.path.getmtime(spr) < max(os.path.getmtime(f) for f in sources)):
            print("== Sprites to ETC1A4 (first time only, a few minutes)...")
            os.makedirs(TX, exist_ok=True)
            subprocess.run([sys.executable, os.path.join(A3, "convert_sprites.py")] + sources[:1] + [spr] + sources[1:3])
        # precomputed 80x50 thumbnails (fast menu) - only if missing or maps.pk3 changed
        mp = os.path.join(BASE, r"data\maps.pk3"); mini = os.path.join(BASE, r"data\thumbnails_3ds.bin")
        if os.path.exists(mp) and (not os.path.exists(mini) or os.path.getmtime(mini) < os.path.getmtime(mp)):
            print("== 80x50 thumbnails (first time only)...")
            subprocess.run([sys.executable, os.path.join(TOOLS, "make_thumbnails.py"), mp, os.path.join(BASE, "bios.pk3"), mini])
        # by default WITHOUT RomFS (fast, assets on the SD). "romfs" = like the final version.
        extra_args = ("clean " if "clean" in args else "") + ("romfs " if "romfs" in args else "") \
              + ("3dsxonly" if "3dsxonly" in args else "assetsonly" if "assetsonly" in args else build_mode_file())
        print("== build3ds.sh " + extra_args + ("  (RomFS ~1.6 GB, takes a while)" if "romfs" in extra_args else ""))
        r = subprocess.run([bash, "-lc", "bash " + P.msys_path(os.path.join(P.TOOLS, "build3ds.sh")) + " " + extra_args], env=env)
        if r.returncode != 0: print("!! Build failed"); return 1

    # Azahar configuration (backup the first time)
    def edit_ini(path, changes, mark):
        if os.path.exists(path) and not os.path.exists(path + ".original"): shutil.copy2(path, path + ".original")
        txt = open(path, encoding="utf-8", newline="").read() if os.path.exists(path) else ""
        nl = "\r\n" if "\r\n" in txt else "\n"
        lines = txt.split(nl) if txt else []
        for sec, k, v in changes: set_ini(lines, sec, k, v, mark, "false")
        with open(path, "w", encoding="utf-8", newline="") as f: f.write(nl.join(lines))
    edit_ini(GAME_INI, MODES[run_mode] + GAME_ONLY, "use_global")
    # your linked console is New 3DS; in Old 3DS mode Azahar closed when opening.
    # "Change console type in unique data" makes that data match Old 3DS.
    edit_ini(INI, COMMON + [("Debugging", "toggle_unique_data_console_type", "true" if run_mode == "measure" else "false")], "default")

    # Game files
    # all in rr_config.txt (before rr_diag, rr_laps, rr_bench, rr_64mb, rr_genmap)
    gen = "generate" in args  # generate a map package (96 MB, CPU 100%, Azahar at low priority)
    cfg = read_cfg()
    cfg["mode"] = "race" if "manual" in args else "bench"
    cfg["map"] = MAP
    cfg["laps"] = str(laps)  # consecutive races
    cfg["diag"] = "1" if ((run_mode == "dev" and "nodiag" not in args) or "diag" in args) else "0"
    cfg["genmap"] = "1" if gen else "0"
    # Azahar gives 96 MB even as Old 3DS: the game limits itself to 64 MB
    cfg["64mb"] = "1" if ((run_mode == "measure" or "64mb" in args) and not gen) else "0"
    write_cfg(cfg)
    if os.path.exists(BUILT) and (not os.path.exists(GAME) or os.path.getmtime(BUILT) > os.path.getmtime(GAME)):
        shutil.copy2(BUILT, GAME)

    start_size = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    title = {"dev": "BENCHMARK MODE (extended performance) - CPU 300%, New 3DS, diagnostics ON - fps NOT valid",
              "measure": "OLD 3DS MODE (64 MB) - CPU 100%, Old 3DS, simulated GPU, diagnostics OFF - REAL fps"}[run_mode]
    if "diag" in args: title = title.replace("diagnostics OFF - REAL fps", "diagnostics ON - fps NOT valid (only to hunt culprits)")
    print("=" * len(title)); print(title); print("=" * len(title)); print("== Map: " + MAP + ("  | diagnostics ON" if "diag" in args else "") + ("  | limited to 64 MB" if "64mb" in args else "") + ("  | MANUAL: you drive" if "manual" in args else ""))
    if os.path.exists(CRASH_TXT): os.remove(CRASH_TXT)
    print("== Opening Azahar...")
    p = subprocess.Popen([EXE, GAME], cwd=AZ)
    small_window(p.pid)
    if gen:
        # LIMITER for the PC (70 C limit): low priority and only half of the cores
        try:
            import ctypes
            k = ctypes.windll.kernel32
            k.SetPriorityClass(int(p._handle), 0x00004000)  # BELOW_NORMAL
            n = os.cpu_count() or 4
            k.SetProcessAffinityMask(int(p._handle), (1 << max(1, n // 2)) - 1)
            print("== Generate: Azahar at low priority and %d of %d cores" % (max(1, n // 2), n))
        except Exception as e:
            print("(could not lower Azahar's priority: %s)" % e)
    t0 = time.time(); result = None; pos = start_size; crash = None
    full_log = ""; last_growth = time.time(); warned64 = False
    ends = []; t_fin = None
    while time.time() - t0 < (60 * 60 if ("manual" in args or laps > 1) else TIMEOUT):  # driving = 60 min (pauses)
        time.sleep(3)
        if os.path.exists(LOG):
            size = os.path.getsize(LOG)
            if size < pos: pos = 0; full_log = ""  # the log restarted
            if size > pos: last_growth = time.time()
            with open(LOG, "rb") as f:
                f.seek(pos); fresh = f.read().decode("latin-1"); pos = size
            full_log += fresh
            for mf in re.findall(r"\[BENCH\] end:.*", fresh):
                ends.append(mf.strip()); t_fin = time.time()
                if laps > 1: print("== Race %d of %d: %s" % (len(ends), laps, mf.strip()))
            if len(ends) >= laps:
                result = average_line(ends, laps); break
            # if the game does not start measuring again within 4 min after an end: keep what there is
            if ends and time.time() - t_fin > 4 * 60:
                print("!! The game did not measure another race: keeping %d" % len(ends))
                result = average_line(ends, laps); break
            if gen:
                mg = re.search(r"\[GEN\] (DONE .*|.*is already complete)", fresh)
                if mg: result = mg.group(0); break
                for lg in re.findall(r"\[GEN\] \d+/\d+ zones.*", fresh): print("== " + lg)
                # if the race ends and the map starts again (loop), stop: what was done
                # is saved (.prog) and next time it resumes from there.
                if full_log.count("Speeding off to level") >= 2:
                    result = "map restarted before finishing (resumes next time)"; break
            # in measure mode the game must be seen with ~64 MB, not 96
            # Azahar ALWAYS gives 96 MB ([MEM_INIT] total=98304); the game sets aside 32 MB
            # and says so with [MEM64]. Only warn if [MEM64] does NOT appear in measure/64mb.
            if (run_mode == "measure" or "64mb" in args) and not gen and not warned64 and "LOADING UPDATE" in full_log:
                warned64 = True
                if "64MB mode (Old 3DS)" in full_log:  # [MEM64] is written BEFORE reserving (comes out 0 KB): look at the [3DS] 64MB mode line
                    print("== Memory: Old 3DS 64 MB simulated OK")
                else:
                    print("!! WARNING: the 64MB mode line did not appear: the game is using 96 MB, NOT Old 3DS")
            for pattern, reason in CRASHES:
                mc = re.search(pattern, fresh)
                if mc: crash = reason; break
            if crash:
                time.sleep(3)  # let it finish writing the complete [TRAP]
                with open(LOG, "rb") as f:
                    f.seek(pos); full_log += f.read().decode("latin-1")
                break
            if "MANUAL mode" in fresh and "manual" not in args:
                args.append("manual")
                print("== Special stage / prison: YOU DRIVE. Ends when the stage ends.")
            if "manual" not in args and time.time() - last_growth > HANG_TIMEOUT:
                crash = "hung: the log did not advance in %d min" % (HANG_TIMEOUT // 60); break
        if p.poll() is not None: break
    time.sleep(2)  # let it finish writing the log
    if p.poll() is None:
        subprocess.run(["taskkill", "/PID", str(p.pid), "/F"], capture_output=True)
    print("== Azahar closed.")
    # save THIS test's log separately (the game's restarts and is lost)
    try:
        LOGS = P.LOGS; os.makedirs(LOGS, exist_ok=True)
        status = "ok" if result else ("crash" if crash else "closed")
        log_name = time.strftime("%Y-%m-%d_%H%M") + "_" + run_mode + ("_64mb" if "64mb" in args else "") + ("_generate" if "generate" in args else "") + "_" + MAP + "_" + status + ".txt"
        with open(os.path.join(LOGS, log_name), "w", encoding="utf-8", errors="replace") as f: f.write(full_log)
        print("== Log of this test: " + os.path.join(LOGS, log_name))
    except Exception as e: print("(could not save a copy of the log: %s)" % e)
    if result and laps > 1:
        try:
            with open(os.path.join(LOGS, time.strftime("%Y-%m-%d_%H%M") + "_average_" + MAP + ".txt"), "w", encoding="utf-8") as f:
                f.write(result + "\n" + "\n".join(ends) + "\n")
        except Exception: pass
    if result and "_una" in args:
        with open(LAST_RESULT, "w", encoding="utf-8") as f: f.write(result + "\n" + log_name + "\n")
    if result:
        print("== OK  [" + ("BENCHMARK" if run_mode == "dev" else "OLD 3DS") + "]  " + result); print("== Result: \"done\""); return 0
    if not crash and p.poll() is not None and time.time() - t0 < (60 * 60 if "manual" in args else TIMEOUT):
        crash = "Azahar closed by itself before finishing (emulator crash)"
    if crash:
        crash_report(crash, full_log)
        print("!! CRASH: " + crash)
        print("!! Report saved to " + CRASH_TXT)
    else:
        print("!! Timed out before the bench finished")
    print("== Result: \"failed\""); return 1

if __name__ == "__main__":
    sys.exit(main())
