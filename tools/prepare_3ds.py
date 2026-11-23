#!/usr/bin/env python3
# Ring Racers 3DS : PATCH in A SINGLE exe (prepare_3ds.exe, built by make_patch3ds.py).
# Usage:
# prepare_3ds.exe                     -> looks for the game (folder with bios.pk3 and data\) or asks for it
# prepare_3ds.exe <game folder> -> builds the folder for the SD
# drag one or more .pk3 mods onto it -> converts only those mods (characters)
# Output: RingRacers3ds folder next to the exe  ->  copy to SD:\3ds\RingRacers3ds
# If interrupted, when reopened it continues from the step that was missing (markers in _work\).
# Inside go: the tools .py files, dspenc.exe, tex3ds.exe (devkitPro, GPLv3) and extras\
# (ringracers.3dsx, 3ds.pk3, rr_config.txt, atlas_list.txt, cache\ with .rr3/.rrv/pvs/pal/blend).
import os, sys, shutil, time, glob, traceback

FROZEN = getattr(sys, 'frozen', False)
INTERNAL = getattr(sys, '_MEIPASS', os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(sys.executable if FROZEN else os.path.abspath(__file__))
EXTRAS = os.path.join(INTERNAL, 'extras')
os.environ['PATH'] = INTERNAL + os.pathsep + os.environ.get('PATH', '')  # tex3ds.exe / dspenc.exe from inside
sys.path.insert(0, INTERNAL)

# (name, description, MB of music). Measured: everything else (textures, sprites, characters,
# maps, effects, 3dsx) always weighs the same: ~1100 MB.
QUALITIES = [('minimal', '16000 Hz mono (tested on console)', 260), ('low', '22050 Hz mono', 440),
             ('medium', '22050 Hz stereo', 880), ('high', '32000 Hz stereo', 1270)]
BASE_MB = 1100
AS_IS = ['scripts.pk3', 'gfx.pk3', 'maps.pk3', 'unlocks.pk3', 'staffghosts.pk3', 'shaders.pk3']


def pause():
    if FROZEN:
        try: input('\nPress Enter to close...')
        except EOFError: pass


def run_step(modulo, args):
    """Calls main() of a tools script with sys.argv set up."""
    import importlib
    m = importlib.import_module(modulo)
    old = sys.argv
    sys.argv = [modulo + '.py'] + [str(a) for a in args]
    try:
        m.main()
    except SystemExit as e:
        if e.code not in (None, 0): raise RuntimeError('%s finished with an error: %s' % (modulo, e.code))
    finally:
        sys.argv = old


def is_game(d):
    return d and os.path.isfile(os.path.join(d, 'bios.pk3')) and os.path.isfile(os.path.join(d, 'data', 'maps.pk3'))


def find_game(args):
    for a in args:
        if os.path.isdir(a) and is_game(a): return os.path.abspath(a)
        if os.path.isdir(a) and is_game(os.path.dirname(os.path.abspath(a))): return os.path.dirname(os.path.abspath(a))
    d = HERE
    for _ in range(3):
        if is_game(d): return d
        d = os.path.dirname(d)
    while True:
        print('\nGame not found. Drag the Dr. Robotnik\'s Ring Racers folder here')
        print('(the one with bios.pk3 and the data folder) and press Enter:')
        try: r = input('> ').strip().strip('"')
        except EOFError: return None
        if is_game(r): return os.path.abspath(r)
        print('That folder has no bios.pk3 + data\\maps.pk3.')


def choose_quality():
    print('\nMusic quality:')
    for i, (n, d, mb) in enumerate(QUALITIES, 1):
        print('  %d = %-7s %-36s total approx. %.1f GB' % (i, n, d, (BASE_MB + mb) / 1024))
    try: r = input('Number (Enter = 1): ').strip()
    except EOFError: r = ''
    try: return QUALITIES[int(r) - 1][0]
    except (ValueError, IndexError): return QUALITIES[0][0]


def copy_if_changed(a, b):
    if not os.path.isfile(b) or os.path.getsize(a) != os.path.getsize(b):
        os.makedirs(os.path.dirname(b), exist_ok=True)
        shutil.copyfile(a, b)


def mods(itemList):
    out = os.path.join(HERE, 'RingRacers3ds', 'mods')
    os.makedirs(out, exist_ok=True)
    run_step('convert_mod', itemList + ['-o', out])
    print('\nDONE. Copy the contents of\n  %s\nto  SD:\\3ds\\RingRacers3ds\\mods\\' % out)


def prepare(game):
    J = game; JD = os.path.join(J, 'data'); BIOS = os.path.join(J, 'bios.pk3')
    OUT = os.path.join(HERE, 'RingRacers3ds'); OD = os.path.join(OUT, 'data')
    WORKDIR = os.path.join(HERE, '_work')
    for d in (OD, WORKDIR, os.path.join(OUT, 'mods'), os.path.join(OUT, 'cache')): os.makedirs(d, exist_ok=True)
    PK = os.path.join(OD, 'ringracers_3ds.pk3')
    cal_f = os.path.join(WORKDIR, 'quality.txt')
    cal = open(cal_f).read().strip() if os.path.isfile(cal_f) else choose_quality()
    open(cal_f, 'w').write(cal)
    tot = BASE_MB + dict((n, mb) for n, _, mb in QUALITIES).get(cal, 1270)
    print('\nGame:   %s\nOutput: %s\nMusic:  %s' % (J, OUT, cal))
    print('The converted files will weigh about %.1f GB (you need that free on the SD).' % (tot / 1024))
    freeAmount = shutil.disk_usage(HERE).free >> 20
    if freeAmount < tot + 500:
        print('WARNING: only %.1f GB are free on this disk; it may not be enough.' % (freeAmount / 1024))
    print()

    def extras():
        copy_if_changed(BIOS, os.path.join(OUT, 'bios.pk3'))
        for f in AS_IS: copy_if_changed(os.path.join(JD, f), os.path.join(OD, f))
        copy_if_changed(os.path.join(EXTRAS, 'ringracers.3dsx'), os.path.join(OUT, 'ringracers.3dsx'))
        copy_if_changed(os.path.join(EXTRAS, '3ds.pk3'), os.path.join(OD, '3ds.pk3'))
        if not os.path.isfile(os.path.join(OUT, 'rr_config.txt')):  # do not overwrite the player's config
            shutil.copyfile(os.path.join(EXTRAS, 'rr_config.txt'), os.path.join(OUT, 'rr_config.txt'))
        ce = os.path.join(EXTRAS, 'cache')
        for r, _, fs in os.walk(ce):
            for f in fs:
                if f.startswith('pal_'): continue  # only lists for convert_etcpal
                a = os.path.join(r, f); copy_if_changed(a, os.path.join(OUT, 'cache', os.path.relpath(a, ce)))

    def audio():
        run_step('convert_audio', [JD, OUT, cal])
        shutil.rmtree(os.path.join(OUT, 'sfx'), ignore_errors=True)  # everything already goes in sfx.rrp

    def sprites():
        run_step('convert_sprites', [BIOS, os.path.join(OD, 'ringracers_3ds_spr.bin'),
                                     os.path.join(JD, 'gfx.pk3'), os.path.join(JD, 'followers.pk3')])

    def atlas():
        run_step('convert_atlas', [BIOS, os.path.join(OD, 'ringracers_3ds_atlas.bin'), os.path.join(EXTRAS, 'atlas_list.txt'),
                                   os.path.join(JD, 'gfx.pk3'), os.path.join(JD, 'maps.pk3'), os.path.join(JD, 'followers.pk3')])

    steps = [
        ('copy game and port files', extras),
        ('texture definitions', lambda: run_step('extract_defs', [JD, OD])),
        ('textures (shrink to 256)', lambda: run_step('convert_textures', ['--data', JD, '--bios', BIOS, '--output', PK])),
        ('textures to ETC1 (the longest step)', lambda: run_step('convert_etc1', [PK, BIOS, os.path.join(OD, 'ringracers_3ds_etc.bin')])),
        ('small textures', lambda: run_step('convert_small', [PK, os.path.join(OD, 'ringracers_3ds_small.bin')])),
        ('map palettes (TWEAKMAP)', lambda: run_step('convert_tweakmap', [JD, BIOS, PK, os.path.join(WORKDIR, 'pal')])),
        ('ETC1 of custom palettes', lambda: run_step('convert_etcpal', [PK, OD, os.path.join(WORKDIR, 'pal'), os.path.join(EXTRAS, 'cache')])),
        ('sprites to ETC1A4', sprites),
        ('object atlas', atlas),
        ('map thumbnails', lambda: run_step('make_thumbnails', [os.path.join(JD, 'maps.pk3'), BIOS, os.path.join(OD, 'thumbnails_3ds.bin')])),
        ('audio (effects + music)', audio),
        ('characters (64 vanilla)', lambda: run_step('convert_chars', [os.path.join(JD, 'chars.pk3'), '-o', OUT])),
    ]
    t0 = time.time()
    for i, (nombre, f) in enumerate(steps, 1):
        mark = os.path.join(WORKDIR, 'done_%02d' % i)
        if os.path.isfile(mark) and i > 1:
            print('[%d/%d] %s: already done' % (i, len(steps), nombre)); continue
        print('\n[%d/%d] %s...' % (i, len(steps), nombre)); t = time.time()
        f()
        open(mark, 'w').write('ok')
        print('[%d/%d] %s: done in %d s' % (i, len(steps), nombre, time.time() - t))
    shutil.rmtree(WORKDIR, ignore_errors=True)
    print('\nDONE in %d min. Copy the folder\n  %s\nto the SD as  SD:\\3ds\\RingRacers3ds' % ((time.time() - t0) // 60, OUT))
    print('Audio: SD:\\3ds\\dspfirm.cdc is required (dump it with DSP1). Without that file there is no sound.')


def main():
    args = [a for a in sys.argv[1:]]
    print('Ring Racers 3DS - prepare the files for the SD card')
    try:
        pk3 = [a for a in args if a.lower().endswith('.pk3') and os.path.isfile(a)]
        if pk3:
            mods(pk3)
        else:
            j = find_game(args)
            if j: prepare(j)
    except Exception:
        traceback.print_exc()
        print('\nFAILED. Run it again: it resumes from the step that failed.')
    pause()


if __name__ == '__main__':
    main()
