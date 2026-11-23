# make_preload_lists.py - generates the preload lists (cache\pre\<MAP>.txt and skin_*.txt)
# by running 1 lap of each race map in Azahar (rrbench measure once). ~4 min per map.
# Usage:  py tools\make_preload_lists.py            (maps without a list; can be interrupted and resumed)
#         py tools\make_preload_lists.py RR_GreenHills ...   (only those)
#         py tools\make_preload_lists.py status     (how many there are)
#         py tools\make_preload_lists.py all        (also those that previously timed out or were already attempted)
import os, sys, csv, time, subprocess
import worst_maps as wm

PRE = os.path.join(wm.BASE, "cache", "pre")
ATTEMPTS = os.path.join(wm.LOGS, "preload_list_attempts.txt")
TIME_LIMIT = 15 * 60
# The "dev" mode (CPU 300%, with: fast) turned out SLOWER (it carries diag=1 and the PC cannot
# emulate at 300%: more than 10 min per map). "measure" stays (~4 min per map).
MODE = "dev" if "fast" in sys.argv else "measure"


def has_list(p): return os.path.exists(os.path.join(PRE, p.upper() + ".txt"))


def main():
    args = sys.argv[1:]
    maps = wm.race_maps()
    if args[:1] == ["status"]:
        done = [p for p in maps if has_list(p)]
        skins = [f for f in os.listdir(PRE) if f.startswith("skin_")] if os.path.isdir(PRE) else []
        print("maps with a list: %d of %d | characters with a list: %d" % (len(done), len(maps), len(skins)))
        print("without a list: " + " ".join(p for p in maps if not has_list(p)))
        return 0
    everything = "all" in args
    asked = [a for a in args if a.upper().startswith("RR_")]
    previous = wm.read_csv()
    attempted = set(open(ATTEMPTS).read().split()) if os.path.exists(ATTEMPTS) else set()
    missing = []
    for p in asked or maps:
        if has_list(p): continue
        if not asked and not everything:
            if p.upper() in attempted: continue
            if previous.get(p.upper(), {}).get("status") == "timeout": continue
        missing.append(p)
    print("== PRELOAD LISTS: %d maps missing (~4 min each)" % len(missing))
    for i, p in enumerate(missing):
        print("== [%d/%d] %s" % (i + 1, len(missing), p), flush=True)
        try:
            subprocess.run([sys.executable, os.path.join(wm.TOOLS, "rrbench.py"), MODE, "once", p], timeout=TIME_LIMIT,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/IM", "azahar.exe", "/F"], capture_output=True)
            time.sleep(3)
        with open(ATTEMPTS, "a", newline="") as f: f.write(p.upper() + "\n")
        print("== %s: %s" % (p, "list OK" if has_list(p) else "NO list"), flush=True)
        time.sleep(5)
    print("== done. Folder: " + PRE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
