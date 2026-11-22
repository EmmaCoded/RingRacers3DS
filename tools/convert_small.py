#!/usr/bin/env python3
# Ring Racers 3DS: SMALL version (max 256x256) of the large textures in the
# ringracers_3ds.pk3 pack, for maps with a custom palette (Sealed Stars),
# which cannot use the ETC1 package (built with the normal palette).
# Stores palette INDICES (not colors) -> works with any palette.
# Before, the game read the whole texture (1 MB for the sky, 9 MB SSRING06) to
# use 64 KB: 50-127 ms per texture and huge blocks on the heap.
#   py convert_small.py <ringracers_3ds.pk3> <output.bin>
# Output: "T3S1", u32 version=1, u32 n, n x {u16 width, u16 height, u8 format,
# u8 0, u16 0, u32 absolute offset, u32 size}  (width 0 = no small version)
# and then the data, in the SAME format as the pack:
# 0 = indices; 1 = (index, alpha) pairs; 2 = indices + bit mask.
# Rescaling = nearest neighbor, same as HWR_3DS_FromPack.
import sys, struct, zipfile
import numpy as np

MAXD = 256

def main():
    pk3, out = sys.argv[1], sys.argv[2]
    z = zipfile.ZipFile(pk3)
    ixn = [n for n in z.namelist() if n.split('/')[-1].upper().startswith('3DSTEXIX')][0]
    dtn = [n for n in z.namelist() if n.split('/')[-1].upper().startswith('3DSTEXDT')][0]
    ix = z.read(ixn); dt = z.read(dtn)
    ver, n = struct.unpack_from('<II', ix, 4)

    ents = []; chunks = []; pos = 12 + n * 16; count = 0
    for i in range(n):
        nm, ow, oh, gw, gh, fmt, _, off = struct.unpack_from('<8sHHHHBB2xI', ix, 12 + i * 24)
        if not gw or not gh or (gw <= MAXD and gh <= MAXD):
            ents.append((0, 0, 0, 0, 0)); continue
        dw, dh = gw, gh
        while dw > MAXD: dw >>= 1
        while dh > MAXD: dh >>= 1
        npx = gw * gh
        sy = (np.arange(dh) * gh // dh)
        sx = (np.arange(dw) * gw // dw)
        sel = (sy[:, None] * gw + sx[None, :]).ravel()
        if fmt == 1:
            raw = np.frombuffer(dt, np.uint8, npx * 2, off).reshape(-1, 2)
            d = raw[sel].tobytes()
        elif fmt == 2:
            idx = np.frombuffer(dt, np.uint8, npx, off)
            m = np.frombuffer(dt, np.uint8, (npx + 7) // 8, off + npx)
            a = np.unpackbits(m, bitorder='little')[:npx]
            d = idx[sel].tobytes() + np.packbits(a[sel], bitorder='little').tobytes()
        else:
            d = np.frombuffer(dt, np.uint8, npx, off)[sel].tobytes()
        ents.append((dw, dh, fmt, pos, len(d)))
        chunks.append(d); pos += len(d); count += 1

    with open(out, 'wb') as f:
        f.write(b'T3S1' + struct.pack('<II', 1, n))
        for dw, dh, fmt, o, sz in ents:
            f.write(struct.pack('<HHBBHII', dw, dh, fmt, 0, 0, o, sz))
        for d in chunks:
            f.write(d)
    print('done:', count, 'large textures shrunk to max', MAXD, '->', pos >> 10, 'KB')

if __name__ == '__main__':
    main()
