#!/usr/bin/env python3
# Ring Racers 3DS : the 64 vanilla characters (chars.pk3) with LAZY LOADING.
# chars.pk3 has ~28,000 lumps (109 MB): mounting it whole costs 2-3 MB of RAM for the index alone.
# Sale:
# data/chars_idx.pk3       -> S_SKIN + faces (XTRA*) + sounds (RRS2) of each character. Mounted at startup.
# data/chars/<name>.pk3  -> all sprites of that character. The game mounts it only when
# someone runs with it (r_skins.c R_3DS_EnsureSkin).
# data/chars_3ds.pk3       -> full Sonic (always loaded) with his voices in RRS2.
# Everything UNCOMPRESSED (the 3DS reads lumps directly). Ships in the public PATCH (convert_chars.bat).
#   py convert_chars.py [chars.pk3] [-o output_folder]
# Default output: chars_3ds folder next to the script.
# Copy its data\ folder to SD:\3ds\RingRacers3ds\data\
import sys, os, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_mod import sfx_a_rrs2

DATE = (2025, 1, 1, 0, 0, 0)

def main():
    a = sys.argv[1:]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chars_3ds')
    if '-o' in a:
        k = a.index('-o'); out = a[k + 1]; del a[k:k + 2]
    src = a[0] if a else os.path.join(os.environ.get('RR_GAME_DIR', ''), 'data', 'chars.pk3')
    if not os.path.isfile(src):
        print('chars.pk3 not found. Drag the chars.pk3 of the game (data folder) onto convert_chars.bat')
        sys.exit(1)
    data = os.path.join(out, 'data'); cdir = os.path.join(data, 'chars')
    os.makedirs(cdir, exist_ok=True)
    zi = zipfile.ZipFile(src)
    per_skin = {}; sounds = {}
    for i in zi.infolist():
        if i.is_dir(): continue
        p = i.filename.split('/')
        if len(p) == 3 and p[0] == 'skins': per_skin.setdefault(p[1], []).append(i)
        elif len(p) == 3 and p[0] == 'sounds': sounds.setdefault(p[1], []).append(i)
    names = sorted(n for n in per_skin if n != 'sonic')
    nsnd = 0; mal = []
    with zipfile.ZipFile(os.path.join(data, 'chars_idx.pk3'), 'w', zipfile.ZIP_STORED) as zx:
        for n in names:
            lumps = per_skin[n]
            sk = [i for i in lumps if i.filename.endswith('/S_SKIN')]
            if not sk: print('no S_SKIN:', n); continue
            # index: S_SKIN first and then the faces (XTRA*)
            zx.writestr(zipfile.ZipInfo(sk[0].filename, DATE), zi.read(sk[0]))
            for i in lumps:
                if i.filename.split('/')[-1].upper().startswith('XTRA'):
                    zx.writestr(zipfile.ZipInfo(i.filename, DATE), zi.read(i))
            # own pk3 with everything (S_SKIN marks the start of the sprites)
            with zipfile.ZipFile(os.path.join(cdir, n + '.pk3'), 'w', zipfile.ZIP_STORED) as zs:
                zs.writestr(zipfile.ZipInfo(sk[0].filename, DATE), zi.read(sk[0]))
                for i in lumps:
                    if i is not sk[0]:
                        zs.writestr(zipfile.ZipInfo(i.filename, DATE), zi.read(i))
        # sounds of everyone (except sonic) into the index, converted to RRS2
        for n in sorted(sounds):
            if n == 'sonic': continue
            for i in sounds[n]:
                d = zi.read(i); r = sfx_a_rrs2(d)
                if r: d = r; nsnd += 1
                elif d[:4] != b'RRS2': mal.append(i.filename)
                zx.writestr(zipfile.ZipInfo(i.filename, DATE), d)
    # Sonic: chars_3ds.pk3 (skins/sonic + sounds/sonic, voices to RRS2)
    with zipfile.ZipFile(os.path.join(data, 'chars_3ds.pk3'), 'w', zipfile.ZIP_STORED) as zc:
        for i in per_skin.get('sonic', []):
            zc.writestr(zipfile.ZipInfo(i.filename, DATE), zi.read(i))
        for i in sounds.get('sonic', []):
            d = zi.read(i); r = sfx_a_rrs2(d)
            if r: d = r; nsnd += 1
            elif d[:4] != b'RRS2': mal.append(i.filename)
            zc.writestr(zipfile.ZipInfo(i.filename, DATE), d)
    print('characters: %d, converted sounds: %d, failed: %d %s' % (len(names), nsnd, len(mal), mal[:5]))
    print('index: %.1f MB -> %s' % (os.path.getsize(os.path.join(data, 'chars_idx.pk3')) / 1048576, data))

if __name__ == '__main__':
    main()
