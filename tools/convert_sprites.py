#!/usr/bin/env python3
# Ring Racers 3DS : SPRITES precomputed on the PC -> ETC1A4 / ETC1 (native
# GPU format, 1 byte/pixel instead of 4 bytes RGBA in RAM + 2 on the GPU).
# The 3DS no longer builds the sprite image (MakeBlock RGBA): it reads it from the file
# straight to the GPU. Only with the normal colormap (no skin color / map remap).
#   py convert_sprites.py <bios.pk3> <output.bin> <pk3> [<pk3> ...]
# Key = CRC32 of the lump bytes + length (the game computes the same
# when loading the patch in w_wad.cpp MakePatch), so neither name nor order matters.
# Output: "T3S1", u32 version=1, u32 n, n x {u32 hash, u32 length, u16 width, u16 height,
#          u8 format (0=ETC1, 1=ETC1A4), u8 0, u16 0, u32 offset (absolute), u32 size}
# sorted by (hash, length); then the data (16-byte aligned).
# Sizes = the same as HWR_MakePatch (power of 2 >= patch), FULL RESOLUTION (only >1024 is reduced: GPU limit)
# (half and half, same proportion), minimum 8 (ETC). Flipped image (row 0 = bottom).
import os, sys, zlib, struct, zipfile, subprocess, tempfile, shutil
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from PIL import Image

TEX3DS = shutil.which('tex3ds') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tex3ds')
for c in (r'C:\devkitPro\tools\bin\tex3ds.exe', r'D:\devkitPro\tools\bin\tex3ds.exe'):
    if not shutil.which('tex3ds') and os.path.exists(c): TEX3DS = c
MAX_SIDE = 1024  # 3DS GPU limit: full resolution except what does not fit

def fnv(b):
    return zlib.crc32(b) & 0xFFFFFFFF  # standard CRC32 (same as the game)

def p2(n):
    v = 1
    while v < n: v <<= 1
    return v

def decode_patch(d):
    if len(d) < 8: return None
    w, h, lo, to = struct.unpack_from('<HHhh', d, 0)
    if w == 0 or h == 0 or w > 2048 or h > 2048 or len(d) < 8 + 4 * w: return None
    idx = np.zeros((h, w), np.uint8); a = np.zeros((h, w), bool)
    for x in range(w):
        p = struct.unpack_from('<I', d, 8 + 4 * x)[0]
        top = -1
        while True:
            if p >= len(d): return None
            td = d[p]
            if td == 0xFF: break
            top = top + td if td <= top else td  # tall patches
            if p + 3 > len(d): return None
            ln = d[p + 1]
            px = d[p + 3:p + 3 + ln]
            y1 = min(h, top + len(px))
            if top < h:
                idx[top:y1, x] = np.frombuffer(px[:y1 - top], np.uint8)
                a[top:y1, x] = True
            p += ln + 4
    return w, h, idx, a

def main():
    bios, out, pk3s = sys.argv[1], sys.argv[2], sys.argv[3:]
    b = zipfile.ZipFile(bios)
    pal = np.frombuffer(b.read([x for x in b.namelist() if x.split('/')[-1].upper().startswith('PLAYPAL')][0])[:768], np.uint8).reshape(256, 3)
    lumps = {}
    for f in [bios] + pk3s:
        z = zipfile.ZipFile(f)
        for i in z.infolist():
            if i.is_dir(): continue
            top = i.filename.split('/')[0].lower()
            if top != 'sprites': continue  # skins: always with skin color (phase 2)
            d = z.read(i)
            if d[:4] == b'\x89PNG': continue  # (none in the base game)
            k = (fnv(d), len(d))
            if k not in lumps: lumps[k] = d
    keys = sorted(lumps)
    print(len(keys), 'unique sprites', flush=True)
    tmp = tempfile.mkdtemp()

    def conv(j):
        k = keys[j]
        r = decode_patch(lumps[k])
        if not r: return j, None, 0, 0, 0
        w, h, idx, a = r
        W, H = p2(w), p2(h)
        cw, ch = w, h  # zone with image (max_s = w/W of the game)
        # LARGE sprite (side >= 512) that is really an exact upscale (each pixel repeated kxk,
        # e.g. OPCBA0 and OPUPA0 of Opulence: 4x and 2x): it is stored at its real resolution. Lossless
        # detail and 4-16x less memory (a 1 MB 1024x1024 ETC1A4 did not fit in the fragmented VRAM).
        if max(W, H) >= 512:
            for k, sh in ((16, 4), (8, 3), (4, 2), (2, 1)):
                if h % k or w % k: continue
                bi = idx.reshape(h // k, k, w // k, k); bm = a.reshape(h // k, k, w // k, k)
                if (bi == bi[:, :1, :, :1]).all() and (bm == bm[:, :1, :, :1]).all():
                    while sh > 0 and max(W >> sh, H >> sh) < 64: sh -= 1
                    W >>= sh; H >>= sh
                    break
        while (W > MAX_SIDE or H > MAX_SIDE) and W > 8 and H > 8:
            W >>= 1; H >>= 1
        TW, TH = max(8, W), max(8, H)
        cw = max(1, round(TW * w / p2(w))); ch = max(1, round(TH * h / p2(h)))
        rgba = np.zeros((h, w, 4), np.uint8)
        rgba[..., :3] = pal[idx]; rgba[..., 3] = np.where(a, 255, 0)
        img = Image.fromarray(rgba, 'RGBA')
        if (cw, ch) != (w, h): img = img.resize((cw, ch), Image.NEAREST)
        full = Image.new('RGBA', (TW, TH), (0, 0, 0, 0)); full.paste(img, (0, 0))
        full = full.transpose(Image.FLIP_TOP_BOTTOM)
        alpha = not a.all()
        p = os.path.join(tmp, f'{j}.png'); o = os.path.join(tmp, f'{j}.bin')
        full.save(p)
        subprocess.run([TEX3DS, '-f', 'etc1a4' if alpha else 'etc1', '-q', 'medium', '-z', 'none', '-r', '-o', o, p],
                       check=True, capture_output=True)
        dd = open(o, 'rb').read()[4:]
        os.unlink(p); os.unlink(o)
        return j, dd, TW, TH, 1 if alpha else 0

    res = [None] * len(keys)
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for c, r in enumerate(ex.map(conv, range(len(keys)))):
            res[r[0]] = r
            if c % 2000 == 0: print(c, '/', len(keys), flush=True)
    ok = [r for r in res if r[1] is not None]
    hdr = 12 + 24 * len(ok)
    data = bytearray(); idxb = bytearray()
    while (hdr + len(data)) % 16: data += b'\0'
    for j, dd, TW, TH, f in ok:
        hsh, ln = keys[j]
        idxb += struct.pack('<IIHHBBHII', hsh, ln, TW, TH, f, 0, 0, hdr + len(data), len(dd))
        data += dd
        while len(data) % 16: data += b'\0'
    with open(out, 'wb') as fo:
        fo.write(b'T3S1' + struct.pack('<II', 1, len(ok)) + bytes(idxb))
        fo.write(bytes(data))
    shutil.rmtree(tmp, ignore_errors=True)
    print('done:', len(ok), 'sprites,', (hdr + len(data)) >> 20, 'MB ->', out)

if __name__ == '__main__':
    main()
