#!/usr/bin/env python3
# Ring Racers 3DS : converts sounds.pk3 and music.pk3 to the format of the 3DS
# audio module (src/dummy/i_sound.c). Needs soundfile+soxr+numpy (pip); no longer ffmpeg.
# py convert_audio.py <data folder with the pk3 files> <output folder> [quality] [music_only]
# MUSIC quality (chosen by the player in the patch):
# high   = 32000 Hz stereo  (~1.2 GB all songs)  <- default
#     medium = 22050 Hz stereo  (~830 MB)
#     low    = 22050 Hz mono     (~410 MB)
# minimal = 16000 Hz mono     (~300 MB, sounds more muffled)
# music_only = does not rebuild the effects (sfx.rrp)
# mono .rrm = "RRMM" (same 100-byte header, 2nd channel zeroed, 1-channel blocks)
# Output: <output>/sfx/<lump>.rrs  and  <output>/mus/<lump>.rrm  in DSP-ADPCM
# (4 bits per sample, decoded by the console's DSP: 0% CPU).
# Needs the dspenc encoder (tools/dspenc.c) next to this script.
# .rrs = "RRS2", u32 hz, u32 samples, u16 coefs[16], 8-byte frames (mono, 22050 Hz)
# .rrm = "RRM2", u32 hz, u32 samples, u32 loop (multiple of 14), u32 frames_per_block,
# 2 x { u16 coefs[16], u16 ps_loop, h1_loop, h2_loop, u16 0 },
# blocks: [left channel frames][right channel frames] ... (32000 Hz, stereo)
import os, sys, struct, zipfile, subprocess, tempfile, json

DSPENC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dspenc.exe' if os.name == 'nt' else 'dspenc')
BLOCK = 1024  # frames (of 14 samples) per music block

def dsp_encode(pcm_mono, loop=0):
    with tempfile.TemporaryDirectory() as td:
        i = os.path.join(td, 'i.raw'); o = os.path.join(td, 'o.bin')
        open(i, 'wb').write(pcm_mono)
        subprocess.run([DSPENC, i, o, str(loop)], check=True)
        d = open(o, 'rb').read()
    coefs = d[0:32]; n, lp = struct.unpack_from('<II', d, 32); hist = d[40:48]; frames = d[48:]
    return coefs, n, lp, hist, frames

from concurrent.futures import ThreadPoolExecutor

SFX_HZ = 22050
MUS_HZ = 32000
MUS_CH = 2
QUALITIES = {'high': (32000, 2), 'medium': (22050, 2), 'low': (22050, 1), 'minimal': (16000, 1),
             }

def ogg_tags(data):
    # reads the Vorbis/Opus comments (LOOPPOINT, LOOPMS...) assembling the packets from the first Ogg pages
    pk = b''; pos = 0; pkts = []
    while pos + 27 <= len(data) and data[pos:pos + 4] == b'OggS' and len(pkts) < 3 and pos < 1 << 20:
        nseg = data[pos + 26]; segs = data[pos + 27:pos + 27 + nseg]; p = pos + 27 + nseg
        for s in segs:
            pk += data[p:p + s]; p += s
            if s < 255: pkts.append(pk); pk = b''
        pos = p
    tags = {}
    for pk in pkts:
        if pk[:7] == b'vorbis': q = 7
        elif pk[:8] == b'OpusTags': q = 8
        else: continue
        try:
            vl, = struct.unpack_from('<I', pk, q); q += 4 + vl
            n, = struct.unpack_from('<I', pk, q); q += 4
            for _ in range(n):
                l, = struct.unpack_from('<I', pk, q); q += 4
                kv = pk[q:q + l].decode('utf-8', 'replace'); q += l
                if '=' in kv:
                    k, v = kv.split('=', 1); tags[k.upper()] = v
        except struct.error:
            pass
    return tags

def tracker(data):
    # tracker music -> openmpt123.exe --render (wav next to the file) -> soundfile
    import shutil, soundfile as sf
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'openmpt123.exe')
    if not os.path.isfile(exe): exe = shutil.which('openmpt123')
    if not exe: return None, 0
    with tempfile.TemporaryDirectory() as td:
        ext = 'it' if data[:4] == b'IMPM' else 'xm' if data[:15] == b'Extended Module' else 's3m' if data[44:48] == b'SCRM' else 'mod'
        f = os.path.join(td, 'm.' + ext); open(f, 'wb').write(data)
        r = subprocess.run([exe, '--batch', '--render', '--samplerate', '48000', '--channels', '2', '-q', f],
                           capture_output=True)
        global TRACKER_ERR
        TRACKER_ERR = (r.returncode, (r.stdout + r.stderr)[-400:])
        if not os.path.isfile(f + '.wav'): return None, 0
        try:
            return sf.read(f + '.wav', dtype='float32', always_2d=True)
        except Exception:
            return None, 0

def ffmpeg_fallback(data, hz, ch):
    import shutil
    if not shutil.which('ffmpeg'): return b'', {}, 0
    with tempfile.NamedTemporaryFile(delete=False) as t:  # tracker needs a file (stdin does not work)
        t.write(data); tn = t.name
    try:
        r = subprocess.run(['ffmpeg', '-v', 'error', '-i', tn, '-f', 's16le', '-acodec', 'pcm_s16le',
                            '-ac', str(ch), '-ar', str(hz), '-'], capture_output=True)
    finally:
        os.unlink(tn)
    return r.stdout, {}, hz

def ffmpeg_pcm(data, hz, ch):
    # decodes with soundfile (libsndfile: ogg/opus/flac/wav/mp3) + soxr; no ffmpeg.
    # returns (pcm s16le interleaved, tags, original sample rate)
    import io, numpy as np, soundfile as sf, soxr
    try:
        x, sr = sf.read(io.BytesIO(data), dtype='float32', always_2d=True)
    except Exception:
        x, sr = tracker(data)  # .it/.xm/.mod/.s3m with openmpt123.exe (libopenmpt, BSD)
        if x is None: return ffmpeg_fallback(data, hz, ch)  # last resort: ffmpeg if the PC has it
    if x.shape[1] != ch:
        m = x.mean(axis=1, keepdims=True)
        x = m if ch == 1 else np.repeat(m, ch, axis=1) if x.shape[1] == 1 else x[:, :ch]
    if sr != hz and len(x):
        x = soxr.resample(x, sr, hz, quality='HQ')
    pcm = (np.clip(x, -1.0, 1.0) * 32767.0).round().astype('<i2').tobytes()
    return pcm, ogg_tags(data) if data[:4] == b'OggS' else {}, sr

def dmx_to_wav(data):
    # Doom DMX: u16 3, u16 hz, u32 n, 16 bytes padding, unsigned PCM8, 16 padding
    fmt, hz, n = struct.unpack_from('<HHI', data)
    pcm = data[8 + 16: 8 + n - 16] if n > 32 else data[8:8 + n]
    w = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, hz, hz, 1, 8)
    return w + b'data' + struct.pack('<I', len(pcm)) + pcm

def lumpname(path):
    b = path.rsplit('/', 1)[-1]
    if '.' in b: b = b.split('.', 1)[0]
    return b.lower()

def conv_sfx(z, name, out):
    data = z.read(name)
    if len(data) < 8: return None
    if data[:2] == b'\x03\x00': data = dmx_to_wav(data)
    pcm, _, _ = ffmpeg_pcm(data, SFX_HZ, 1)
    if not pcm: return name
    coefs, n, _, _, frames = dsp_encode(pcm)
    open(os.path.join(out, lumpname(name) + '.rrs'), 'wb').write(b'RRS2' + struct.pack('<II', SFX_HZ, n) + coefs + frames)

def conv_mus(z, name, out):
    data = z.read(name)
    if len(data) < 8: return None
    pcm, tags, srate = ffmpeg_pcm(data, MUS_HZ, MUS_CH)
    if not pcm: return name
    frames = len(pcm) // (2 * MUS_CH)
    loop = 0
    try:
        if 'LOOPPOINT' in tags and srate:
            loop = int(int(tags['LOOPPOINT']) * MUS_HZ / srate)
        elif 'LOOPMS' in tags:
            loop = int(int(tags['LOOPMS']) * MUS_HZ / 1000)
    except ValueError:
        loop = 0
    if loop >= frames: loop = 0
    import array
    if MUS_CH == 1:
        cl, n, lp, hl, fl = dsp_encode(pcm[:frames * 2], loop)
        nblk = (len(fl) // 8 + BLOCK - 1) // BLOCK
        fl += b'\0' * (nblk * BLOCK * 8 - len(fl))
        open(os.path.join(out, lumpname(name) + '.rrm'), 'wb').write(
            b'RRMM' + struct.pack('<IIII', MUS_HZ, n, lp, BLOCK) + cl + hl + b'\0' * 40 + bytes(fl))
        return
    a = array.array('h'); a.frombytes(pcm[:frames * 4])
    L = a[0::2].tobytes(); R = a[1::2].tobytes()
    cl, n, lp, hl, fl = dsp_encode(L, loop)
    cr, _, _, hr, fr = dsp_encode(R, loop)
    nfr = len(fl) // 8
    nblk = (nfr + BLOCK - 1) // BLOCK
    pad = b'\0' * (nblk * BLOCK * 8 - len(fl))
    fl += pad; fr += pad
    body = bytearray()
    for b in range(nblk):
        body += fl[b * BLOCK * 8:(b + 1) * BLOCK * 8]
        body += fr[b * BLOCK * 8:(b + 1) * BLOCK * 8]
    open(os.path.join(out, lumpname(name) + '.rrm'), 'wb').write(
        b'RRM2' + struct.pack('<IIII', MUS_HZ, n, lp, BLOCK) + cl + hl + cr + hr + bytes(body))

def main():
    global MUS_HZ, MUS_CH
    src, dst = sys.argv[1], sys.argv[2]
    cal = (sys.argv[3] if len(sys.argv) > 3 else 'high').lower()
    if cal not in QUALITIES: sys.exit('quality must be: ' + ', '.join(QUALITIES))
    music_only = 'music_only' in [a.lower() for a in sys.argv[4:]]
    MUS_HZ, MUS_CH = QUALITIES[cal]
    print('music: quality %s = %d Hz %s' % (cal, MUS_HZ, 'stereo' if MUS_CH == 2 else 'mono'))
    os.makedirs(os.path.join(dst, 'sfx'), exist_ok=True)
    os.makedirs(os.path.join(dst, 'mus'), exist_ok=True)
    fails = []
    jobs = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        zs = zipfile.ZipFile(os.path.join(src, 'sounds.pk3'))
        for n in ([] if music_only else zs.namelist()):
            b = n.rsplit('/', 1)[-1]
            if n.endswith('/') or not b.upper().startswith('DS'): continue
            jobs.append(ex.submit(conv_sfx, zipfile.ZipFile(os.path.join(src, 'sounds.pk3')), n, os.path.join(dst, 'sfx')))
        for pk in ('music.pk3', 'altmusic.pk3'):  # altmusic.pk3 (56 alternate songs) too
            if not os.path.isfile(os.path.join(src, pk)): continue
            zm = zipfile.ZipFile(os.path.join(src, pk))
            for n in zm.namelist():
                b = n.rsplit('/', 1)[-1]
                if n.endswith('/') or not b.upper().startswith('O_'): continue
                jobs.append(ex.submit(conv_mus, zipfile.ZipFile(os.path.join(src, pk)), n, os.path.join(dst, 'mus')))
        for j in jobs:
            r = j.result()
            if r: fails.append(r)
    print('failed:', len(fails), fails[:20])
    md = os.path.join(dst, 'mus')
    print('music: %d songs, %d MB' % (len(os.listdir(md)), sum(os.path.getsize(os.path.join(md, f)) for f in os.listdir(md)) >> 20))
    if not music_only:
        pack_sfx(os.path.join(dst, 'sfx'), os.path.join(dst, 'sfx.rrp'))

def pack_sfx(d, out):
    # A single file with all the effects (fewer files to open/copy):
    # "RRP1", u32 n, n x {char name[12], u32 offset, u32 size}, data (.rrs)
    names = sorted(f for f in os.listdir(d) if f.endswith('.rrs'))
    hdr = 8 + 20 * len(names)
    idx = bytearray(); data = bytearray()
    for f in names:
        b = open(os.path.join(d, f), 'rb').read()
        nm = f[:-4].encode()[:12].ljust(12, b'\0')
        idx += nm + struct.pack('<II', hdr + len(data), len(b))
        data += b
    open(out, 'wb').write(b'RRP1' + struct.pack('<I', len(names)) + bytes(idx) + bytes(data))
    print('sfx.rrp:', len(names), 'effects,', (hdr + len(data)) >> 10, 'KB')

if __name__ == '__main__':
    main()
