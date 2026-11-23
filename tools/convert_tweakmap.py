#!/usr/bin/env python3
# Ring Racers 3DS: prepares on the PC, WITHOUT playing, the texture list for
# each map that has TWEAKMAP (the map's own color table: 19 maps,
# the Sealed Stars and a few more). Verified: effective game palette in
# Balconies = PLAYPAL[TWEAKMAP[i]] (hash 5aaad5b7, same as the one the 3DS recorded).
# Writes pal_<hash>.bin (same format the game records: "RPAL", 768 B RGB,
# u32 n, n x u32 id) into <output>; afterwards convert_etcpal.py does the ETC1.
#   py convert_tweakmap.py <data_dir> <bios.pk3> <ringracers_3ds.pk3> <output>
import os, sys, glob, re, struct, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_textures import animdefs_ranges, wad_lumps

TEX_RE = re.compile(rb'(?:texture\w*|skytexture|sky)\s*=\s*"?([^";\s]+)', re.I)

def fnv(rgb):
    h = 2166136261
    for i in range(256):
        c = rgb[i*3] | (rgb[i*3+1] << 8) | (rgb[i*3+2] << 16)
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h

def main():
    data, bios, pk3, out = sys.argv[1:5]
    os.makedirs(out, exist_ok=True)
    b = zipfile.ZipFile(bios)
    pal = b.read([n for n in b.namelist() if n.split('/')[-1].upper().startswith('PLAYPAL')][0])[:768]
    z = zipfile.ZipFile(pk3)
    ix = z.read([n for n in z.namelist() if n.split('/')[-1].upper().startswith('3DSTEXIX')][0])
    ntex = struct.unpack_from('<II', ix, 4)[1]
    names = [struct.unpack_from('<8s', ix, 12 + i * 24)[0].rstrip(b'\0').decode('latin1').upper() for i in range(ntex)]
    pos = {}
    for i, n in enumerate(names): pos.setdefault(n, i)
    # animations: from A to B in pack order (same order as the engine)
    zips = [zipfile.ZipFile(p) for p in glob.glob(os.path.join(data, '*.pk3'))
            if os.path.basename(p).lower().startswith(('maps', 'scripts', 'textures_'))]
    anim = {}
    for s, e in animdefs_ranges(zips):
        if s in pos and e in pos:
            lo, hi = sorted((pos[s], pos[e]))
            for k in range(lo, hi + 1): anim[names[k]] = (lo, hi)
    maps = zipfile.ZipFile(os.path.join(data, 'maps.pk3'))
    # each map's sky: "SkyTexture = X" in the SOC headers (Level RR_...)
    sky = {}
    for zz in zips:
        for n in zz.namelist():
            if '/soc/' not in '/' + n.lower() and not n.upper().split('/')[-1].startswith('SOC'): continue
            txt = zz.read(n).decode('latin1')
            for block in re.split(r'(?im)^\s*level\s+', txt)[1:]:
                nombre = block.split(None, 1)[0].upper() if block.strip() else ''
                m = re.search(r'(?im)^\s*skytexture\s*=\s*(\S+)', block)
                if nombre and m: sky[nombre] = m.group(1).upper()[:8]
    done_maps = 0
    for wn in maps.namelist():
        if not wn.lower().endswith('.wad'): continue
        lumps = dict(wad_lumps(maps.read(wn)))
        tw = lumps.get('TWEAKMAP')
        if not tw or len(tw) < 256 or 'TEXTMAP' not in lumps: continue
        eff = bytes(c for i in range(256) for c in pal[tw[i]*3:tw[i]*3+3])
        h = fnv(eff)
        used = {m.upper().decode('latin1')[:8] for m in TEX_RE.findall(lumps['TEXTMAP'])}
        mapName = os.path.basename(wn)[:-4].upper()
        if mapName in sky: used.add(sky[mapName])
        ids = set()
        for n in used:
            if n in anim:
                lo, hi = anim[n]; ids.update(range(lo, hi + 1))
            elif n in pos: ids.add(pos[n])
        dst = os.path.join(out, 'pal_%08x.bin' % h)
        if os.path.exists(dst):  # another map with the same table: merge
            d = open(dst, 'rb').read()
            k = struct.unpack_from('<I', d, 772)[0]
            ids.update(struct.unpack_from('<%dI' % k, d, 776))
        ids = sorted(ids)
        with open(dst, 'wb') as f:
            f.write(b'RPAL' + eff + struct.pack('<I', len(ids)) + struct.pack('<%dI' % len(ids), *ids))
        print('%-36s table %08x: %d textures' % (os.path.basename(wn), h, len(ids)))
        done_maps += 1
    print('done:', done_maps, 'maps with TWEAKMAP')

if __name__ == '__main__':
    main()
