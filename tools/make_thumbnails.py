# make_thumbnails.py - extracts the thumbnail (PICTURE) of each map in maps.pk3,
# shrinks it to 80x50 (the exact size the menu draws it at in 400x240 = pixel perfect)
# and gathers them into ONE small uncompressed file: thumbnails_3ds.bin.
# That way the game no longer decompresses the whole map (~1-2 MB) to show a picture.
# Usage: py make_thumbnails.py <maps.pk3> <bios.pk3> <output.bin> [width height]
import io, os, struct, sys, zipfile
import numpy as np
from PIL import Image

def wad_lumps(d):
    if d[:4] not in (b"PWAD", b"IWAD"): return
    n, off = struct.unpack_from("<II", d, 4)
    for i in range(n):
        p, s, nm = struct.unpack_from("<II8s", d, off + 16 * i)
        yield nm.rstrip(b"\0").decode("latin1").upper(), d[p:p + s]

def decode_patch(d):
    w, h = struct.unpack_from("<HH", d, 0)
    if not w or not h or w > 4096 or h > 4096 or len(d) < 8 + 4 * w: return None
    idx = np.zeros((h, w), np.uint8); alp = np.zeros((h, w), bool)
    for x in range(w):
        p = struct.unpack_from("<I", d, 8 + 4 * x)[0]; top = -1
        while p < len(d) and d[p] != 0xFF:
            delta = d[p]; top = delta if delta > top else top + delta
            n = d[p + 1]; col = d[p + 3:p + 3 + n]
            y1, y2 = max(top, 0), min(top + len(col), h)
            if y2 > y1:
                idx[y1:y2, x] = np.frombuffer(col, np.uint8)[y1 - top:y2 - top]; alp[y1:y2, x] = True
            p += n + 4
    return idx, alp

def a_rgb(d, pal):
    if d[:8] == b"\x89PNG\r\n\x1a\n":
        return np.asarray(Image.open(io.BytesIO(d)).convert("RGB"), np.float32)
    r = decode_patch(d)
    if r is None: return None
    return pal[r[0]].astype(np.float32)

def shrink(rgb, tw, th):
    h, w, _ = rgb.shape
    img = Image.fromarray(rgb.astype(np.uint8)).resize((tw, th), Image.BOX)  # average of each block
    return np.asarray(img, np.float32)

def a_palette(rgb, pal):
    p = pal.astype(np.float32)
    d = ((rgb[:, :, None, :] - p[None, None, :, :]) ** 2).sum(-1)
    return d.argmin(-1).astype(np.uint8)

def patch(idx):
    h, w = idx.shape
    cols = []
    for x in range(w):
        cols.append(bytes([0, h, 0]) + idx[:, x].tobytes() + bytes([0, 0xFF]))
    out = bytearray(struct.pack("<HHhh", w, h, 0, 0))
    off = 8 + 4 * w
    for c in cols: out += struct.pack("<I", off); off += len(c)
    for c in cols: out += c
    return bytes(out)

def main():
    if len(sys.argv) < 4: sys.exit("usage: py make_thumbnails.py maps.pk3 bios.pk3 output.bin [width height]")
    maps, bios, sal = sys.argv[1:4]
    tw, th = (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) > 5 else (80, 50)
    b = zipfile.ZipFile(bios)
    pal = np.frombuffer(b.read([n for n in b.namelist() if n.split('/')[-1].upper().startswith('PLAYPAL')][0])[:768], np.uint8).reshape(256, 3)
    z = zipfile.ZipFile(maps)
    ent = []
    for wn in z.namelist():
        if not wn.lower().endswith('.wad'): continue
        nombre = os.path.basename(wn)[:-4].upper()
        pic = dict(wad_lumps(z.read(wn))).get('PICTURE')
        if not pic: continue
        rgb = a_rgb(pic, pal)
        if rgb is None: print("  (could not read PICTURE of %s)" % nombre); continue
        ent.append((nombre, patch(a_palette(shrink(rgb, tw, th), pal))))
    ent.sort()
    with open(sal, 'wb') as f:
        f.write(b"RRMI" + struct.pack("<II", 1, len(ent)))
        off = 12 + 40 * len(ent)
        for n, d in ent:
            f.write(n.encode('latin1')[:31].ljust(32, b'\0') + struct.pack("<II", off, len(d))); off += len(d)
        for _, d in ent: f.write(d)
    print("done: %d thumbnails %dx%d -> %s (%d KB)" % (len(ent), tw, th, sal, os.path.getsize(sal) // 1024))

if __name__ == "__main__":
    main()
