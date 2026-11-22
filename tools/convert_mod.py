#!/usr/bin/env python3
# Ring Racers 3DS : prepares a character MOD (.pk3) for the 3DS.
# Repackages UNCOMPRESSED (ZIP_STORED): the 3DS reads lumps directly, without decompressing.
# Skin sprites stay as regular patches (like vanilla Sonic): they are painted
# with the skin color at runtime, which is why they CANNOT be pre-baked into ETC1.
#   py convert_mod.py <mod.pk3> [<mod2.pk3> ...] [-o <output_folder>]
# AUDIO: the mod's sound effects (DS* lumps, ogg/wav/dmx) are converted to RRS2 (DSP-ADPCM,
# 22050 Hz mono), the only thing the 3DS reads. Mod music (O_*) is NOT converted yet.
# Default output: mods_3ds\ next to the script (copy as is to SD:\3ds\RingRacers3ds\mods\)
import sys, os, zipfile, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert_audio as ca  # needs soundfile+soxr+numpy (pip) and dspenc next to this script

def sfx_a_rrs2(data):
    # returns RRS2 bytes or None if it could not be done (or it already is RRS2)
    if data[:4] == b'RRS2' or len(data) < 8: return None
    if data[:2] == bytes([3, 0]): data = ca.dmx_to_wav(data)
    pcm, _, _ = ca.ffmpeg_pcm(data, ca.SFX_HZ, 1)
    if not pcm: return None
    coefs, n, _, _, frames = ca.dsp_encode(pcm)
    return b'RRS2' + struct.pack('<II', ca.SFX_HZ, n) + coefs + frames

def convert_pk3(src, outdir):
    os.makedirs(outdir, exist_ok=True)
    dst = os.path.join(outdir, os.path.basename(src))
    zi = zipfile.ZipFile(src)
    n = 0; seen = set(); snd = 0; soundFails = []
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_STORED) as zo:
        for i in zi.infolist():
            if i.is_dir() or i.filename in seen: continue
            seen.add(i.filename)
            chunks = zi.read(i)
            base = i.filename.split('/')[-1]
            nombre = base.split('.')[0]  # DSKHITEM.ogg is also a sound
            if nombre.upper().startswith('DS') and len(nombre) > 2 and nombre.upper() not in ('DS_START', 'DS_END'):
                r = sfx_a_rrs2(chunks)
                if r: chunks = r; snd += 1
                elif chunks[:4] != b'RRS2': soundFails.append(i.filename)
            zo.writestr(zipfile.ZipInfo(i.filename, (2025, 1, 1, 0, 0, 0)), chunks, zipfile.ZIP_STORED)
            n += 1
    skins = sum(1 for x in seen if x.split('/')[-1].upper() == 'S_SKIN')
    print('converted sounds: %d, failed: %d %s' % (snd, len(soundFails), soundFails[:10]))
    print('%s: %d lumps, %d skins, %.1f MB -> %s' % (os.path.basename(src), n, skins, os.path.getsize(dst) / 1048576, dst))

def main():
    a = sys.argv[1:]; out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mods_3ds')
    if '-o' in a:
        k = a.index('-o'); out = a[k + 1]; del a[k:k + 2]
    if not a: print('usage: py convert_mod.py <mod.pk3> [<mod2.pk3> ...] [-o <output_folder>]'); sys.exit(1)
    for f in a: convert_pk3(f, out)

if __name__ == '__main__':
    main()
