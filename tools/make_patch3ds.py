#!/usr/bin/env python3
# Ring Racers 3DS: builds prepare_3ds.exe (ONE single exe for the public PATCH).
# py tools\make_patch3ds.py
# Output: <RR_WORK>\PATCH_3DS\prepare_3ds.exe
# Bundles in: the tools .py files, dspenc.exe, tex3ds.exe and extras\ taken from the Azahar SD
# (ringracers.3dsx, clean rr_config.txt, cache\maps .rr3/.rrv, cache\pvs, blend_*.bin, pal_*.bin).
# Needs: py -m pip install pyinstaller soundfile soxr numpy pillow
import os, sys, shutil, subprocess, glob
import rr_paths as P

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
AZ = P.SD_BASE
OUTPUT = P.PATCH_OUT
WORKDIR = P.PATCH_BUILD
TEX3DS = r'C:\devkitPro\tools\bin\tex3ds.exe'
MODULOS = ['rr_paths', 'convert_audio', 'convert_mod', 'convert_chars', 'convert_textures', 'convert_etc1',
           'convert_small', 'convert_tweakmap', 'convert_etcpal', 'convert_sprites', 'convert_atlas',
           'make_thumbnails', 'extract_defs']


def main():
    ex = os.path.join(WORKDIR, 'extras')
    shutil.rmtree(WORKDIR, ignore_errors=True)
    os.makedirs(os.path.join(ex, 'cache'))
    shutil.copy(os.path.join(AZ, 'ringracers.3dsx'), ex)
    shutil.copy(os.path.join(REPO, 'assets3ds', '3ds.pk3'), ex)
    shutil.copy(os.path.join(TOOLS, 'atlas_list.txt'), ex)
    shutil.copy(os.path.join(TOOLS, 'openmpt123_LICENSE.txt'), ex)
    sys.path.insert(0, TOOLS)
    import rrbench
    rrbench.console_cfg(ex)  # clean rr_config.txt (regular game)
    c = os.path.join(AZ, 'cache')
    for f in glob.glob(os.path.join(c, 'blend_*.bin')) + glob.glob(os.path.join(c, 'pal_*.bin')):
        shutil.copy(f, os.path.join(ex, 'cache'))
    shutil.copytree(os.path.join(c, 'maps'), os.path.join(ex, 'cache', 'maps'))
    if os.path.isdir(os.path.join(c, 'pvs')):
        shutil.copytree(os.path.join(c, 'pvs'), os.path.join(ex, 'cache', 'pvs'))
    # preload lists (maps + vanilla characters; mods' are not included)
    pre = os.path.join(c, 'pre')
    if os.path.isdir(pre):
        os.makedirs(os.path.join(ex, 'cache', 'pre'))
        chars = os.path.join(AZ, 'data', 'chars')
        vanilla = {f.lower() for f in os.listdir(chars)} if os.path.isdir(chars) else set()
        n = 0
        for f in os.listdir(pre):
            if not f.endswith('.txt'): continue
            if f.startswith('skin_') and f[5:-4].lower() not in vanilla: continue
            shutil.copy(os.path.join(pre, f), os.path.join(ex, 'cache', 'pre')); n += 1
        print('preload lists: %d' % n)
    cmd = [sys.executable, '-m', 'PyInstaller', '--onefile', '--console', '--noconfirm', '--clean',
           '--name', 'prepare_3ds', '--distpath', OUTPUT, '--workpath', os.path.join(WORKDIR, 'build'),
           '--specpath', WORKDIR, '--paths', TOOLS,
           '--add-binary', os.path.join(TOOLS, 'dspenc.exe') + os.pathsep + '.',
           '--add-binary', TEX3DS + os.pathsep + '.',
           '--add-binary', os.path.join(TOOLS, 'openmpt123.exe') + os.pathsep + '.',  # tracker music
           '--add-binary', os.path.join(TOOLS, 'openmpt-mpg123.dll') + os.pathsep + '.',  # needed by openmpt123
           '--add-data', ex + os.pathsep + 'extras',
           '--collect-all', 'soundfile', '--collect-all', 'soxr']
    for m in MODULOS: cmd += ['--hidden-import', m]
    cmd.append(os.path.join(TOOLS, 'prepare_3ds.py'))
    subprocess.run(cmd, check=True)
    exe = os.path.join(OUTPUT, 'prepare_3ds.exe')
    print('\nLISTO: %s (%d MB)' % (exe, os.path.getsize(exe) >> 20))


if __name__ == '__main__':
    main()
