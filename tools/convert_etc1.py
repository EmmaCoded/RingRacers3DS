#!/usr/bin/env python3
# Ring Racers 3DS : converts the texture pack (ringracers_3ds.pk3,
# palette indices) to ETC1 / ETC1A4, the GPU's native compressed format
# of the 3DS (4 or 8 bits per pixel instead of 16). Uses tex3ds (devkitPro).
#   py convert_etc1.py <ringracers_3ds.pk3> <bios.pk3> <output.bin>
# Output: <output> = index "T3E1", u32 version=2, u32 n, n x {char name[8],
#          u16 width, u16 height, u8 format (0=ETC1, 1=ETC1A4), u8 part, u16 0,
# u32 offset within the part, u32 size}
# <output without .bin>_NN.bin = data parts of up to 24 MB (none
# texture crosses from one part to another)
# The data is already in GPU order (8x8 blocks) and with the image
# vertically flipped (row 0 = bottom), same as upload_tex.
# Only useful with the normal palette (no Encore or the map's own palette): in
# those cases the game uses the usual path (indices -> RGBA5551).
import os, sys, struct, zipfile, subprocess, tempfile, shutil
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from PIL import Image

TEX3DS = shutil.which('tex3ds') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tex3ds')

def main():
    pk3, bios, out = sys.argv[1], sys.argv[2], sys.argv[3]
    quality = sys.argv[4] if len(sys.argv) > 4 else 'medium'
    z = zipfile.ZipFile(pk3)
    ixn = [n for n in z.namelist() if n.split('/')[-1].upper().startswith('3DSTEXIX')][0]
    dtn = [n for n in z.namelist() if n.split('/')[-1].upper().startswith('3DSTEXDT')][0]
    ix = z.read(ixn); dt = z.read(dtn)
    ver, n = struct.unpack_from('<II', ix, 4)
    b = zipfile.ZipFile(bios)
    pal = np.frombuffer(b.read([x for x in b.namelist() if x.split('/')[-1].upper().startswith('PLAYPAL')][0])[:768], np.uint8).reshape(256, 3)

    ents = []
    for i in range(n):
        nm, ow, oh, gw, gh, fmt, _, off = struct.unpack_from('<8sHHHHBB2xI', ix, 12 + i * 24)
        ents.append((nm, gw, gh, fmt, off))

    tmp = tempfile.mkdtemp()
    def conv(i):
        nm, gw, gh, fmt, off = ents[i]
        if not gw or not gh: return i, None, 0
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
        if name.upper().startswith('SKY') and len(name) <= 5:
            a = np.ones(npx, bool)  # skies: no transparency (like HWR_GenerateTexture)
        rgba = np.zeros((npx, 4), np.uint8)
        rgba[:, :3] = pal[idx]
        rgba[:, 3] = np.where(a, 255, 0)
        img = Image.fromarray(rgba.reshape(gh, gw, 4), 'RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        alpha = not a.all()
        p = os.path.join(tmp, f'{i}.png'); o = os.path.join(tmp, f'{i}.bin')
        img.save(p)
        subprocess.run([TEX3DS, '-f', 'etc1a4' if alpha else 'etc1', '-q', quality, '-z', 'none', '-r', '-o', o, p],
                       check=True, capture_output=True)
        d = open(o, 'rb').read()[4:]  # remove the (fake) 4-byte compression header
        os.unlink(p); os.unlink(o)
        return i, d, 1 if alpha else 0

    res = [None] * n
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for k, (i, d, f) in enumerate(ex.map(conv, range(n))):
            res[i] = (d, f)
            if k % 1000 == 0: print(k, '/', n, flush=True)
    PART = 24 * 1024 * 1024
    base = out[:-4] if out.endswith('.bin') else out
    idxb = bytearray(); part = 0; cur = bytearray(); parts = []
    for i, (nm, gw, gh, fmt, off) in enumerate(ents):
        d, f = res[i]
        if d is None:
            idxb += nm + struct.pack('<HHBBHII', 0, 0, 0, 0, 0, 0, 0); continue
        if len(cur) + len(d) > PART:
            parts.append(cur); cur = bytearray(); part += 1
        idxb += nm + struct.pack('<HHBBHII', gw, gh, f, part, 0, len(cur), len(d))
        cur += d
        while len(cur) % 16: cur += b'\0'
    parts.append(cur)
    open(out, 'wb').write(b'T3E1' + struct.pack('<II', 2, n) + bytes(idxb))
    for k, p in enumerate(parts):
        open('%s_%02d.bin' % (base, k), 'wb').write(p)
    print('done:', n, 'textures,', sum(len(p) for p in parts) >> 20, 'MB in', len(parts), 'parts')

if __name__ == '__main__':
    main()
