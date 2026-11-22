#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
convert_textures.py — Ring Racers 3DS
Creates ringracers_3ds.pk3 with the world textures reduced for the Old 3DS.

- Reads the original textures_*.pk3, maps.pk3, scripts.pk3 and bios.pk3.
- Only keeps the textures used by maps / scripts / ANIMDEFS.
- Downscales using the "most frequent color" (sharp, no new colors).
- Sides are powers of 2, maximum 512 (limit of the 3DS GPU).
- Stores palette INDICES (not RGB): this way the palettes
  of every map and Encore mode keep working.
- The pk3 is written UNCOMPRESSED.

Usage (in cmd):
    py -m pip install pillow numpy
    py convert_textures.py
    py convert_textures.py --all          (do not filter by usage)
    py convert_textures.py --scale 0.25   (smaller)

Output format (inside the pk3):
  3DSTEXIX : "T3DI" u32 version=1, u32 count, then per texture 24 bytes:
             char nombre[8], u16 width_orig, u16 height_orig, u16 width_gpu,
             u16 height_gpu, u8 format (0=index, 1=index+alpha per pixel,
             2=index then 1 bit/pixel mask, LSB first), u8 0, 2 bytes padding,
             u32 offset into 3DSTEXDT
  3DSTEXDT : pixels, row by row from top to bottom.
"""
import argparse, glob, io, os, re, struct, sys, time, zipfile

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("numpy or pillow is missing. Run:  py -m pip install pillow numpy")
    sys.exit(1)

GAME_DIR = os.environ.get("RR_GAME_DIR", "")
DATA_DEF = os.path.join(GAME_DIR, "data")
BIOS_DEF = os.path.join(GAME_DIR, "bios.pk3")
MAXDIM, MINDIM = 512, 8


def lumpname(path):
    base = path.rsplit("/", 1)[-1]
    return base.split(".", 1)[0].upper()[:8]


# ----------------------------------------------------------------- images
def decode_patch(d):
    """Doom patch -> (idx uint8 HxW, alpha uint8 HxW)."""
    w, h, _, _ = struct.unpack_from("<HHhh", d, 0)
    if w == 0 or h == 0 or w > 8192 or h > 8192 or len(d) < 8 + 4 * w:
        return None
    idx = np.zeros((h, w), np.uint8)
    alp = np.zeros((h, w), np.uint8)
    for x in range(w):
        p = struct.unpack_from("<I", d, 8 + 4 * x)[0]
        top = -1
        while p < len(d):
            delta = d[p]
            if delta == 0xFF:
                break
            top = delta if delta > top else top + delta  # tall patches
            n = d[p + 1]
            col = d[p + 3:p + 3 + n]
            y1 = max(top, 0); y2 = min(top + len(col), h)
            if y2 > y1:
                idx[y1:y2, x] = np.frombuffer(col, np.uint8)[y1 - top:y2 - top]
                alp[y1:y2, x] = 255
            p += n + 4
    return idx, alp


def decode_png(d, pal_arr):
    im = Image.open(io.BytesIO(d))
    rgba = np.asarray(im.convert("RGBA"))
    rgb = rgba[:, :, :3].reshape(-1, 3).astype(np.int32)
    # closest palette color
    out = np.empty(len(rgb), np.uint8)
    for i in range(0, len(rgb), 4096):
        c = rgb[i:i + 4096, None, :] - pal_arr[None, :, :]
        out[i:i + 4096] = np.argmin((c * c).sum(2), 1)
    return out.reshape(rgba.shape[:2]), rgba[:, :, 3].copy()


def decode_flat(d):
    n = len(d)
    s = int(round(n ** 0.5))
    if s * s != n:
        return None
    return np.frombuffer(d, np.uint8).reshape(s, s).copy(), np.full((s, s), 255, np.uint8)


def decode_any(d, pal_arr, flat=False):
    if d[:8] == b"\x89PNG\r\n\x1a\n":
        return decode_png(d, pal_arr)
    if flat:
        return decode_flat(d)
    r = decode_patch(d)
    return r if r is not None else decode_flat(d)


# ------------------------------------------------------------------ scale
def pow2_near(v):
    v = max(v, 1)
    p = 1 << int(round(np.log2(v)))
    return max(MINDIM, min(MAXDIM, p))


def target_dims(w, h, scale):
    tw, th = w * scale, h * scale
    m = max(tw, th)
    if m > MAXDIM:  # so it fits in 512 keeping proportion
        tw, th = tw * MAXDIM / m, th * MAXDIM / m
    return pow2_near(tw), pow2_near(th)


def nearest(a, tw, th):
    h, w = a.shape
    ys = (np.arange(th) * h // th)
    xs = (np.arange(tw) * w // tw)
    return a[ys][:, xs]


def downscale(idx, alp, tw, th):
    """2x2 mode over a double-size resample: sharp and no new colors."""
    h, w = idx.shape
    if tw >= w and th >= h:
        return nearest(idx, tw, th), nearest(alp, tw, th)
    key = idx.astype(np.int32) + 1
    key[alp < 128] = 0                    # transparent counts as a "color"
    k = nearest(key, tw * 2, th * 2)
    a = nearest(alp, tw * 2, th * 2)
    p = [k[0::2, 0::2], k[0::2, 1::2], k[1::2, 0::2], k[1::2, 1::2]]
    q = [a[0::2, 0::2], a[0::2, 1::2], a[1::2, 0::2], a[1::2, 1::2]]
    res = p[0].copy(); ra = q[0].copy()
    for i, j in ((1, 2), (1, 3), (2, 3)):   # if two match and not with 0
        m = (p[i] == p[j]) & (p[0] != p[1]) & (p[0] != p[2]) & (p[0] != p[3])
        res[m] = p[i][m]; ra[m] = q[i][m]
    out_i = np.where(res > 0, res - 1, 0).astype(np.uint8)
    out_a = np.where(res > 0, ra, 0).astype(np.uint8)
    return out_i, out_a


# --------------------------------------------------------------- TEXTURES
def parse_textures_lump(txt):
    """Composite: {nombre: (w, h, [(patch, x, y, flipx, flipy)])}"""
    res = {}
    toks = re.findall(r'"[^"]*"|[^\s,{}]+|[{}]', re.sub(r"//[^\n]*", "", txt))
    i = 0
    while i < len(toks):
        t = toks[i].lower()
        if t in ("texture", "walltexture", "flat", "sprite", "graphic"):
            try:
                name = toks[i + 1].strip('"').upper()[:8]
                w, h = int(toks[i + 2]), int(toks[i + 3])
            except (IndexError, ValueError):
                i += 1; continue
            i += 4
            patches = []
            if i < len(toks) and toks[i] == "{":
                depth = 1; i += 1
                while i < len(toks) and depth:
                    tk = toks[i]
                    if tk == "{": depth += 1
                    elif tk == "}": depth -= 1
                    elif tk.lower() == "patch" and depth == 1:
                        try:
                            patches.append([toks[i + 1].strip('"').upper()[:8],
                                            int(toks[i + 2]), int(toks[i + 3]), False, False])
                            i += 3
                        except (IndexError, ValueError):
                            pass
                    elif tk.lower() == "flipx" and patches: patches[-1][3] = True
                    elif tk.lower() == "flipy" and patches: patches[-1][4] = True
                    i += 1
            res[name] = (w, h, patches)
        else:
            i += 1
    return res


# ------------------------------------------------------------------ usage
TOKEN = re.compile(rb'[A-Za-z0-9_\-\[\]\\]{3,8}')


def wad_lumps(d):
    if d[:4] not in (b"PWAD", b"IWAD"):
        return
    n, off = struct.unpack_from("<II", d, 4)
    for i in range(n):
        p, s, nm = struct.unpack_from("<II8s", d, off + 16 * i)
        yield nm.rstrip(b"\0").decode("latin1"), d[p:p + s]


def is_text(d):
    if not d or d[:8] == b"\x89PNG\r\n\x1a\n":
        return False
    smp = d[:65536]
    ok = sum(1 for c in smp if c in (9, 10, 13) or 32 <= c < 127)
    return ok >= len(smp) * 0.95


def is_definition(path):
    n = lumpname(path)
    return n.startswith(("TEXTURES", "BRIGHT", "ANIMDEFS", "TERRAIN", "RVFXANIM"))


def collect_used(zips, used, origen):
    """used: set of names. origin: {name: 'file:lump'} (first place where it appears)."""
    tex_re = re.compile(rb'(?:texture\w*|skytexture|sky)\s*=\s*"?([^";\s]+)', re.I)

    def add(names, label):
        for n in names:
            used.add(n)
            origen.setdefault(n, label)

    for z in zips:
        zn = os.path.basename(z.filename)
        for info in z.infolist():
            if info.is_dir():
                continue
            d = z.read(info)
            if info.filename.lower().endswith(".wad") or d[:4] in (b"PWAD", b"IWAD"):
                for nm, ld in wad_lumps(d):
                    lab = f"{zn}:{info.filename}:{nm}"
                    if nm == "TEXTMAP":
                        add((m.upper().decode("latin1")[:8] for m in tex_re.findall(ld)), lab)
                    elif nm in ("ANIMDEFS", "MAINCFG", "LUA", "SOC", "BEHAVIOR", "SCRIPTS") or nm.startswith(("ANIM", "SOC", "LUA")):
                        add((t.upper().decode("latin1") for t in TOKEN.findall(ld)), lab)
                continue
            # only TEXT files (lua, soc, ANIMDEFS...)
            if info.file_size < 2_000_000 and not is_definition(info.filename) and is_text(d):
                add((t.upper().decode("latin1") for t in TOKEN.findall(d)), f"{zn}:{info.filename}")


def animdefs_ranges(zips):
    rngs = []
    r = re.compile(rb'(?:texture|flat)\s+"?([^"\s]+)"?\s+range\s+"?([^"\s]+)', re.I)
    for z in zips:
        for info in z.infolist():
            if lumpname(info.filename).startswith("ANIMDEFS") or "ANIMDEFS" in info.filename.upper():
                for a, b in r.findall(z.read(info)):
                    rngs.append((a.decode("latin1").upper()[:8], b.decode("latin1").upper()[:8]))
    return rngs


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=DATA_DEF)
    ap.add_argument("--bios", default=BIOS_DEF)
    ap.add_argument("--output", dest="output", default=None)
    ap.add_argument("--scale", type=float, default=0.5)
    ap.add_argument("--all", dest="all", action="store_true")
    ap.add_argument("--max", type=int, default=256, help="maximum side (power of 2, <=512)")
    a = ap.parse_args()
    global MAXDIM
    MAXDIM = max(MINDIM, min(512, a.max))
    out = a.output or os.path.join(a.data, "ringracers_3ds.pk3")
    t0 = time.time()
    log = []

    bios = zipfile.ZipFile(a.bios)
    pal = np.frombuffer(bios.read("PLAYPAL")[:768], np.uint8).reshape(256, 3).astype(np.int32)

    texpk3 = sorted(glob.glob(os.path.join(a.data, "textures_*.pk3")), key=lambda p: p.lower())
    if not texpk3:
        print("textures_*.pk3 not found in", a.data); sys.exit(1)
    print("Textures from:", *[os.path.basename(p) for p in texpk3])
    tz = [zipfile.ZipFile(p) for p in texpk3]

    # 1) definitions in load order (the last ones override)
    # Same order as the engine (R_DefineTextures): per pk3, first Flats/,
    # then Textures/, then the TEXTURES lumps. Animations (ANIMDEFS)
    # go "from texture A to B" by NUMBER, so the order matters.
    order, defs, patches = [], {}, {}
    per = []  # per pk3: list of names in engine order
    for z in tz:
        o = []
        infos = [i for i in z.infolist() if not i.is_dir()]
        for info in infos:
            low = info.filename.lower()
            if low.startswith("patches/") or low.startswith("sprites/"):
                patches[lumpname(info.filename)] = (z, info)
        for folder, is_flat in (("flats/", True), ("textures/", False)):
            for info in infos:
                if info.filename.lower().startswith(folder):
                    n = lumpname(info.filename)
                    if n not in defs: order.append(n)
                    defs[n] = ("single", z, info, is_flat)
                    o.append(n)
        for info in infos:
            if lumpname(info.filename) == "TEXTURES":
                for n, c in parse_textures_lump(z.read(info).decode("latin1")).items():
                    if n not in defs: order.append(n)
                    defs[n] = ("comp",) + c
                    o.append(n)
        per.append((o, {n: k for k, n in enumerate(o)}))
    print(f"Defined: {len(defs)} textures")

    # 2) which ones are used
    if a.all_items:
        keep = set(defs)
    else:
        print("Looking for the textures the maps use (takes a while)...")
        src = [zipfile.ZipFile(os.path.join(a.data, f)) for f in ("maps.pk3", "scripts.pk3") if os.path.exists(os.path.join(a.data, f))]
        used, origen = set(), {}
        collect_used(src, used, origen)
        # texture pk3 files do NOT count as "usage": their BRIGHT/ANIMDEFS/
        # TERRAIN/TEXTURES only DEFINE things (they list almost all the names).
        keep = {n for n in defs if n in used}
        print(f"  by name: {len(keep)}")
        import collections
        cnt = collections.Counter(origen[n] for n in keep)
        log.append(f"Defined: {len(defs)}")
        log.append(f"Used by name: {len(keep)}")
        log.append("\n== Where they come from (file: how many textures it provides) ==")
        for lab, c in cnt.most_common(60):
            ej = [n for n in keep if origen[n] == lab][:8]
            log.append(f"{c:6d}  {lab}   ej: {' '.join(ej)}")
        log.append(f"  ({len(cnt)} different files provide them)")
        # Like the engine: an animation's range goes from A to B in the order of
        # the files WITHIN THE SAME pk3 (not mixing all three).
        extra = 0
        log.append("\n== Animations (start - end: frames added) ==")
        for s, e in animdefs_ranges(src + tz):
            if not (s in keep or e in keep):
                continue  # animation that no map uses
            for o, pz in per:
                if s in pz and e in pz:
                    lo, hi = min(pz[s], pz[e]), max(pz[s], pz[e])
                    before = extra
                    for n in o[lo:hi + 1]:
                        if n in defs and n not in keep: keep.add(n); extra += 1
                    log.append(f"{s} - {e}: range {hi - lo + 1}, new {extra - before}")
                    break
        print(f"  animation frames: +{extra}")
    print(f"In use: {len(keep)} textures")

    # 3) convert
    index, blob = [], bytearray()
    fails = 0; orig_px = 0; sizes = []
    names = [n for n in order if n in keep]
    for k, n in enumerate(names):
        dfn = defs[n]
        try:
            if dfn[0] == "single":
                r = decode_any(dfn[1].read(dfn[2]), pal, dfn[3])
                if r is None: raise ValueError("unknown format")
                idx, alp = r
            else:
                w, h, pl = dfn[1], dfn[2], dfn[3]
                idx = np.zeros((h, w), np.uint8); alp = np.zeros((h, w), np.uint8)
                for pn, px, py, fx, fy in pl:
                    src = patches.get(pn) or (defs[pn][1:3] if pn in defs and defs[pn][0] == "single" else None)
                    if not src: continue
                    r = decode_any(src[0].read(src[1]), pal)
                    if r is None: continue
                    pi, pa = r
                    if fx: pi, pa = pi[:, ::-1], pa[:, ::-1]
                    if fy: pi, pa = pi[::-1], pa[::-1]
                    ph, pw = pi.shape
                    x1, y1 = max(px, 0), max(py, 0)
                    x2, y2 = min(px + pw, w), min(py + ph, h)
                    if x2 <= x1 or y2 <= y1: continue
                    sub_i = pi[y1 - py:y2 - py, x1 - px:x2 - px]
                    sub_a = pa[y1 - py:y2 - py, x1 - px:x2 - px]
                    m = sub_a > 0
                    idx[y1:y2, x1:x2][m] = sub_i[m]
                    alp[y1:y2, x1:x2][m] = sub_a[m]
        except Exception as ex:
            fails += 1
            if fails <= 20: print(f"  warning: {n}: {ex}")
            continue
        oh, ow = idx.shape
        orig_px += ow * oh
        tw, th = target_dims(ow, oh, a.scale)
        ri, ra = downscale(idx, alp, tw, th)
        opaque = bool((ra == 255).all())
        off = len(blob)
        if opaque:
            blob += ri.tobytes(); fmt = 0
        elif bool(((ra == 0) | (ra == 255)).all()):
            # holes without semi-transparency: index + 1-bit mask
            blob += ri.tobytes()
            blob += np.packbits((ra == 255).reshape(-1), bitorder="little").tobytes(); fmt = 2
        else:
            blob += np.dstack([ri, ra]).tobytes(); fmt = 1
        while len(blob) % 4: blob.append(0)
        sizes.append((len(blob) - off, n, ow, oh, tw, th, fmt))
        index.append(struct.pack("<8sHHHHBB2xI", n.encode("latin1")[:8], ow, oh, tw, th, fmt, 0, off))
        if (k + 1) % 500 == 0:
            print(f"  {k + 1}/{len(names)}...")

    ix = b"T3DI" + struct.pack("<II", 1, len(index)) + b"".join(index)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as zo:
        zo.writestr("3DSTEXIX", ix)
        zo.writestr("3DSTEXDT", bytes(blob))
    mb = os.path.getsize(out) / 1048576
    sizes.sort(reverse=True)
    log.append(f"\n== Result: {len(index)} textures, {mb:.1f} MB (max side {MAXDIM}) ==")
    log.append(f"Original pixels: {orig_px / 1e6:.0f} million (~{orig_px / 1048576:.0f} MB at 1 byte/pixel)")
    import collections as _c
    tam = _c.Counter()
    for b, n, ow, oh, tw, th, f in sizes:
        tam[f"{ow}x{oh}->{tw}x{th} fmt{f}"] += b
    log.append("\n== What takes the most space (by size) ==")
    for k, v in tam.most_common(25):
        log.append(f"{v / 1048576:8.1f} MB  {k}")
    log.append("\n== 40 largest textures ==")
    for b, n, ow, oh, tw, th, f in sizes[:40]:
        log.append(f"{b // 1024:6d} KB  {n:8s} {ow}x{oh} -> {tw}x{th} fmt{f}")
    logpath = os.path.join(os.path.dirname(os.path.abspath(__file__)), "convert_textures_log.txt")
    with open(logpath, "w", encoding="utf-8") as fl:
        fl.write("\n".join(log) + "\n")
    print(f"Log: {logpath}")
    print(f"\nListo: {out}")
    print(f"{len(index)} textures, {mb:.1f} MB, {fails} failed, {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
