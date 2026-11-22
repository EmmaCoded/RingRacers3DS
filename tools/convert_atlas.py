#!/usr/bin/env python3
# Ring Racers 3DS : ATLAS of common objects (rings, boxes, items...).
# Gathers sprites whose name starts with a prefix from the list into sheets of
# 512x512 ETC1A4. The 3DS uploads ONE sheet instead of many loose sprites and switches
# texture less when drawing (r_citro3d.cpp, atl_load / c3d_SetTexture).
# py convert_atlas.py <bios.pk3> <output.bin> <list.txt> <pk3> [<pk3> ...]
# Use the SAME pk3 files as convert_sprites.py (the key is CRC32 + lump length).
# atlas_list.txt: one prefix per line (e.g. RING), '#' = comment.
# Output "T3A1": u32 ver=1, u32 sheets, u32 n, n x {u32 hash, u32 length, u16 sheet,
#   u16 x, u16 y, u16 0}, padded to 16, sheets 512x512 ETC1A4 (262144 B, flipped).
import os, sys, struct, zipfile, subprocess, tempfile, shutil
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_sprites import decode_patch, fnv, p2, TEX3DS

DIM, PAD, MAXOBJ = 512, 1, 128   # sheet, repeated border, maximum side of a sprite

def main():
    bios, out, itemList, pk3s = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    pref = [l.split('#')[0].strip().upper() for l in open(itemList, encoding='utf-8')]
    pref = [p for p in pref if p]
    b = zipfile.ZipFile(bios)
    pal = np.frombuffer(b.read([x for x in b.namelist() if x.split('/')[-1].upper().startswith('PLAYPAL')][0])[:768], np.uint8).reshape(256, 3)
    objs = {}; skipped = 0
    for f in [bios] + pk3s:
        z = zipfile.ZipFile(f)
        for i in z.infolist():
            if i.is_dir(): continue
            parts = i.filename.split('/')
            if parts[0].lower() != 'sprites': continue
            nm = os.path.splitext(parts[-1])[0].upper()
            if not any(nm.startswith(p) for p in pref): continue
            d = z.read(i)
            if d[:4] == b'\x89PNG': continue
            k = (fnv(d), len(d))
            if k in objs: continue
            r = decode_patch(d)
            if not r: continue
            w, h, idx, a = r
            W, H = p2(w), p2(h)
            if W < 8 or H < 8 or W > MAXOBJ or H > MAXOBJ: skipped += 1; continue
            rgba = np.zeros((h, w, 4), np.uint8)
            rgba[..., :3] = pal[idx]; rgba[..., 3] = np.where(a, 255, 0)
            objs[k] = (nm, rgba)
    print(len(objs), 'objects (', skipped, 'skipped for size )', flush=True)
    # shelf packing, from tallest to shortest
    order = sorted(objs, key=lambda k: (-objs[k][1].shape[0], -objs[k][1].shape[1]))
    sheets = []; pos = {}
    cur = None; x = y = rowIdx = 0
    for k in order:
        h, w = objs[k][1].shape[:2]
        cw, ch = w + 2 * PAD, h + 2 * PAD
        if cur is None or x + cw > DIM:
            if cur is not None: x = 0; y += rowIdx; rowIdx = 0
            if cur is None or y + ch > DIM:
                cur = np.zeros((DIM, DIM, 4), np.uint8); sheets.append(cur); x = y = rowIdx = 0
        img = objs[k][1]
        e = np.pad(img, ((PAD, PAD), (PAD, PAD), (0, 0)), mode='edge')
        cur[y:y + ch, x:x + cw] = e
        pos[k] = (len(sheets) - 1, x + PAD, y + PAD)
        x += cw; rowIdx = max(rowIdx, ch)
    print(len(sheets), 'sheets de', DIM, 'x', DIM, flush=True)
    tmp = tempfile.mkdtemp(); chunks = []
    for n, hj in enumerate(sheets):
        img = Image.fromarray(hj, 'RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        p = os.path.join(tmp, f'{n}.png'); o = os.path.join(tmp, f'{n}.bin')
        img.save(p)
        subprocess.run([TEX3DS, '-f', 'etc1a4', '-q', 'medium', '-z', 'none', '-r', '-o', o, p], check=True, capture_output=True)
        dd = open(o, 'rb').read()[4:]
        assert len(dd) == DIM * DIM, len(dd)
        chunks.append(dd)
    shutil.rmtree(tmp, ignore_errors=True)
    ks = sorted(pos)
    idxb = b''.join(struct.pack('<IIHHHH', k[0], k[1], *pos[k], 0) for k in ks)
    head = b'T3A1' + struct.pack('<III', 1, len(sheets), len(ks)) + idxb
    while len(head) % 16: head += b'\0'
    with open(out, 'wb') as fo:
        fo.write(head)
        for dd in chunks: fo.write(dd)
    print('done:', len(ks), 'objects in', len(sheets), 'sheets,', (len(head) + sum(map(len, chunks))) >> 10, 'KB ->', out)

if __name__ == '__main__':
    if len(sys.argv) < 5: sys.exit('usage: py convert_atlas.py bios.pk3 output.bin atlas_list.txt pk3 [pk3...]')
    main()
