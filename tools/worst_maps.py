# worst_maps.py - measures the WORST CASE of each race map for the per-tic ms budget.
# Usage:  py tools\worst_maps.py                       (all race maps that are missing)
#         py tools\worst_maps.py RR_765Stadium RR_GreenHills   (only those)
#         py tools\worst_maps.py table                 (only shows the table of what was already measured)
#         py tools\worst_maps.py redo ...              (measures again even if already done)
# Each map: rrbench.py measure once (Old 3DS, CPU 100%, diag OFF, racers from max_racers/bots_bench in rr_config)
# and reads the [WORST] line from the log. Can be interrupted and resumed: what was measured stays in the CSV.
# Locations come from rr_paths.py (environment variables RR_AZAHAR, RR_LOGS...).
import os, sys, re, csv, time, glob, zipfile, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rr_paths as P

TOOLS = os.path.dirname(os.path.abspath(__file__))
BASE = P.SD_BASE
LOGS = P.LOGS
CSV = os.path.join(LOGS, "worst_maps.csv")
TIME_LIMIT = 15 * 60  # seconds per map (load + 1 lap)
FIELDS = ["map", "status", "tics", "tic_avg", "tic_p95", "tic_p99", "tic_max", "worst_sec",
          "frame_avg", "frame_p95", "frame_max", "draw_avg", "draw_p95", "draw_max",
          "mobjs_avg", "mobjs_max", "date"]
RX = re.compile(r"\[WORST\] (\w+) map=\S+ tics=(\d+) tic_ms avg=([\d.]+) p95=(\d+) p99=(\d+) max=([\d.]+) worst_sec=([\d.]+)"
                r" \| frame_ms avg=([\d.]+) p95=(\d+) max=([\d.]+) \| draw_ms avg=([\d.]+) p95=(\d+) max=([\d.]+)"
                r" \| mobjs avg=(\d+) max=(\d+)")


def race_maps():
    t = zipfile.ZipFile(os.path.join(BASE, r"data\maps.pk3")).read("soc/HEADERS_RACE.dat").decode("latin-1")
    out = []
    for b in re.split(r"(?im)^\s*level\s+", t)[1:]:
        if re.search(r"(?im)^\s*TypeOfLevel\s*=\s*.*race", b):
            out.append(b.split()[0])
    return out


def read_csv():
    path = CSV
    if not os.path.exists(path): return {}
    with open(path, encoding="utf-8", newline="") as f:
        rows = []
        for r in csv.DictReader(f):
            rows.append(dict(r))
    return {r["map"].upper(): r for r in rows}


def save_csv(rows):
    with open(CSV, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS); w.writeheader()
        for r in rows.values(): w.writerow({k: r.get(k, "") for k in FIELDS})


def table(rows):
    ok = [r for r in rows.values() if r.get("worst_sec")]
    ok.sort(key=lambda r: -float(r["worst_sec"]))
    fmt = "%-28s %-7s %7s %7s %8s %9s %8s %9s %6s"
    print(fmt % ("map", "status", "tic_p95", "tic_max", "worst_s", "frame_p95", "draw_p95", "frame_avg", "mobjs"))
    for r in ok:
        print(fmt % (r["map"], r["status"], r["tic_p95"], r["tic_max"], r["worst_sec"],
                     r["frame_p95"], r["draw_p95"], r["frame_avg"], r["mobjs_max"]))
    bad = [r["map"] + ":" + r["status"] for r in rows.values() if not r.get("worst_sec")]
    if bad: print("No data: " + " ".join(bad))


def measure(map_name):
    before = set(glob.glob(os.path.join(LOGS, "*_measure_*.txt")))
    try:
        subprocess.run([sys.executable, os.path.join(TOOLS, "rrbench.py"), "measure", "once", map_name], timeout=TIME_LIMIT)
        status = None
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/IM", "azahar.exe", "/F"], capture_output=True)
        status = "timeout"
        time.sleep(3)
    new = sorted(set(glob.glob(os.path.join(LOGS, "*_measure_*.txt"))) - before, key=os.path.getmtime)
    text = ""
    if new:
        text = open(new[-1], encoding="utf-8", errors="replace").read()
        if status is None:
            status = "ok" if new[-1].endswith("_ok.txt") else ("crash" if new[-1].endswith("_crash.txt") else "closed")
    elif status is None:
        # no copy of the log (a timeout kills before saving it): read the game's own log
        try: text = open(os.path.join(BASE, "ringracers_log.txt"), encoding="latin-1").read()
        except OSError: pass
        status = "nolog"
    if "MANUAL mode" in text: status = "manual"
    m = None
    for m in RX.finditer(text): pass
    row = {"map": map_name, "status": status, "date": time.strftime("%Y-%m-%d %H:%M")}
    if m:
        if m.group(1) != "end" and status == "ok": row["status"] = "partial"
        for k, v in zip(FIELDS[2:16], m.groups()[1:]): row[k] = v
    return row


def main():
    args = sys.argv[1:]
    os.makedirs(LOGS, exist_ok=True)
    rows = read_csv()
    if args[:1] == ["table"]: table(rows); return 0
    redo = "redo" in args
    asked = [a for a in args if a.upper().startswith("RR_")]
    wanted = asked or race_maps()
    todo = [p for p in wanted if redo or p.upper() not in rows or not rows[p.upper()].get("worst_sec")]
    print("== WORST CASE: %d maps, %d missing (~4 min each). You can stop and resume." % (len(wanted), len(todo)))
    for i, p in enumerate(todo):
        print("== [%d/%d] %s" % (i + 1, len(todo), p))
        r = measure(p)
        rows[p.upper()] = r
        save_csv(rows)
        print("== %s: %s worst_sec=%s tic_p95=%s frame_p95=%s mobjs_max=%s" % (p, r["status"], r.get("worst_sec"), r.get("tic_p95"), r.get("frame_p95"), r.get("mobjs_max")))
        time.sleep(5)  # breather for the PC
    print(); table(rows)
    print("== CSV: " + CSV)
    return 0


if __name__ == "__main__":
    sys.exit(main())
