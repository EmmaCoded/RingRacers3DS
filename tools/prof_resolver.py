# resolves [PROF] addr=... from a log with nm of the .elf. Usage: py tools\prof_resolver.py <log> [last_report_N=1]
import os, re, subprocess, sys, bisect
NM = r"C:\devkitPro\devkitARM\bin\arm-none-eabi-nm.exe"
ELF = os.path.join(os.environ.get("RR_REPO", os.getcwd()), "build", "bin", "ringracers.elf")
log = sys.argv[1]
r = subprocess.run([NM, "-n", "-C", ELF], capture_output=True, text=True, errors="replace")
syms = []
for l in r.stdout.splitlines():
    m = re.match(r"([0-9a-f]{8}) [tTwW] (.+)", l)
    if m: syms.append((int(m.group(1), 16) & ~1, m.group(2)))
addrs = [a for a, _ in syms]
blocks, cur = [], []
for l in open(log, errors="replace"):
    m = re.search(r"\[PROF\] addr=([0-9a-f]+) us/tic=(\d+) calls/tic=([\d.]+)", l)
    if m: cur.append((int(m.group(1), 16), int(m.group(2)), float(m.group(3))))
    elif cur: blocks.append(cur); cur = []
if cur: blocks.append(cur)
b = blocks[-1] if blocks else []
for a, us, c in b[:int(sys.argv[2]) if len(sys.argv) > 2 else 40]:
    i = bisect.bisect_right(addrs, a & ~1) - 1
    print(f"{us:6d} us/tic  {c:8.1f} calls/tic  {syms[i][1][:90] if i >= 0 else hex(a)}")
