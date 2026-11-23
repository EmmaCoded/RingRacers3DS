#!/usr/bin/env python3
# Ring Racers 3DS : ETC1/ETC1A4 for CUSTOM PALETTES (Sealed Stars).
# The game, with a non-normal palette, saves in
#   sdmc:/3ds/ringracers/pal_<hash>.bin  ("RPAL", 768 B RGB, u32 n, n x u32 id)
# the palette and the textures it used. This script converts them with THAT palette:
#   <output_dir>/rr3ds_etcpal_<hash>.bin
#   "T3P1", u32 n, n x {u32 id, u16 w, u16 h, u8 fmt(0=ETC1,1=ETC1A4), u8 0, u16 0,
# u32 absolute offset, u32 size} and then the data (GPU order, like convert_etc1).
# Only rebuilds a file if there are newly recorded textures.
# py convert_etcpal.py <ringracers_3ds.pk3> <output_dir> <pal_*.bin folder> [another folder ...]
# gathers the lists of all folders (the game's and convert_tweakmap.py's)
import os, sys, glob, struct, zipfile, subprocess, tempfile, shutil
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
TEX3DS = shutil.which('tex3ds') or os.path.join(HERE, 'tex3ds')
for c in (r'C:\devkitPro\tools\bin\tex3ds.exe', r'D:\devkitPro\tools\bin\tex3ds.exe'):
    if not shutil.which('tex3ds') and os.path.exists(c): TEX3DS = c

def read_pal(p):
    d = open(p, 'rb').read()
    if d[:4] != b'RPAL': return None, None
    pal = np.frombuffer(d, np.uint8, 768, 4).reshape(256, 3)
    n = struct.unpack_from('<I', d, 772)[0]
    ids = sorted(set(struct.unpack_from('<%dI' % n, d, 776)))
    return pal, ids

def done_ids(out):
    if not os.path.exists(out): return set()
    d = open(out, 'rb').read(8)
    if d[:4] != b'T3P1': return set()
    n = struct.unpack_from('<I', d, 4)[0]
    ix = open(out, 'rb').read(8 + n * 20)
    return {struct.unpack_from('<I', ix, 8 + i * 20)[0] for i in range(n)}

def main():
    pk3, odir = sys.argv[1], sys.argv[2]
    quality = 'medium'
    groups = {}  # hash -> (palette, ids)
    for pdir in sys.argv[3:]:
        for pp in glob.glob(os.path.join(pdir, 'pal_*.bin')):
            pal, ids = read_pal(pp)
            if pal is None: continue
            h = os.path.basename(pp)[4:-4]
            if h in groups: groups[h][1].update(ids)
            else: groups[h] = (pal, set(ids))
    if not groups: print('etcpal: no palettes recorded yet'); return
    z = zipfile.ZipFile(pk3)
    ixn = [n for n in z.namelist() if n.split('/')[-1].upper().startswith('3DSTEXIX')][0]
    dtn = [n for n in z.namelist() if n.split('/')[-1].upper().startswith('3DSTEXDT')][0]
    ix = z.read(ixn); dt = z.read(dtn)
    ntex = struct.unpack_from('<II', ix, 4)[1]
    os.makedirs(odir, exist_ok=True)
    for h, (pal, ids) in sorted(groups.items()):
        ids = sorted(ids)
        out = os.path.join(odir, 'rr3ds_etcpal_%s.bin' % h)
        ids = [i for i in ids if i < ntex]
        if set(ids) <= done_ids(out):
            print('etcpal %s: up to date (%d textures)' % (h, len(ids))); continue
        tmp = tempfile.mkdtemp()
        def conv(i):
            nm, ow, oh, gw, gh, fmt, _, off = struct.unpack_from('<8sHHHHBB2xI', ix, 12 + i * 24)
            if not gw or not gh or gw < 8 or gh < 8: return i, None, 0, 0, 0
            npx = gw * gh
            if fmt == 1:
                raw = np.frombuffer(dt, np.uint8, npx * 2, off).reshape(-1, 2)
                idx = raw[:, 0]; a = (raw[:, 1] >= 128)
            elif fmt == 2:
                idx = np.frombuffer(dt, np.uint8, npx, off)
                m = np.frombuffer(dt, np.uint8, (npx + 7) // 8, off + npx)
                a = np.unpackbits(m, bitorder='little')[:npx].astype(bool)
            else:
                idx = np.frombuffer(dt, np.uint8, npx, off); a = np.ones(npx, bool)
            name = nm.rstrip(b'\0').decode('latin1')
            if name.upper().startswith('SKY') and len(name) <= 5: a = np.ones(npx, bool)
            rgba = np.zeros((npx, 4), np.uint8)
            rgba[:, :3] = pal[idx]; rgba[:, 3] = np.where(a, 255, 0)
            img = Image.fromarray(rgba.reshape(gh, gw, 4), 'RGBA').transpose(Image.FLIP_TOP_BOTTOM)
            alpha = not a.all()
            p = os.path.join(tmp, f'{i}.png'); o = os.path.join(tmp, f'{i}.bin')
            img.save(p)
            subprocess.run([TEX3DS, '-f', 'etc1a4' if alpha else 'etc1', '-q', quality, '-z', 'none', '-r', '-o', o, p],
                           check=True, capture_output=True)
            d = open(o, 'rb').read()[4:]
            os.unlink(p); os.unlink(o)
            return i, d, 1 if alpha else 0, gw, gh
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            res = [r for r in ex.map(conv, ids) if r[1] is not None]
        shutil.rmtree(tmp, ignore_errors=True)
        pos = 8 + len(res) * 20
        idxb = bytearray(); data = bytearray()
        for i, d, f, gw, gh in res:
            while (pos + len(data)) % 16: data += b'\0'
            idxb += struct.pack('<IHHBBHII', i, gw, gh, f, 0, 0, pos + len(data), len(d))
            data += d
        with open(out, 'wb') as fo:
            fo.write(b'T3P1' + struct.pack('<I', len(res)) + bytes(idxb) + bytes(data))
        print('etcpal %s: %d textures -> %d KB' % (h, len(res), (pos + len(data)) >> 10))

if __name__ == '__main__':
    main()
