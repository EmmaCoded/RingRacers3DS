# Building and installing the 3DS port

Ring Racers is not open-assets: **you convert the assets of your own copy of the game**. Nothing from the game is
distributed with this repository.

## 1. Toolchain (Windows)

1. Install devkitPro with the official installer (<https://devkitpro.org/wiki/Getting_Started>) and tick
   *3DS Development*. It ships its own MSYS2.
2. Open "MSYS2" from the devkitPro menu and install the exact packages:

   ```sh
   bash /path/to/RingRacers/install_devkitpro.sh
   ```

   (It installs `3ds-dev`, `3ds-cmake`, `3ds-pkg-config`, `3ds-zlib`, `3ds-libpng`, `3ds-libogg` and
   `3ds-libvorbis`; to freeze your own list use `dkp-pacman -Qq > devkitpro_packages.txt` and the script will use it.)

## 2. Build the `.3dsx`

```sh
bash tools/build3ds.sh clean          # configure from scratch and build
bash tools/build3ds.sh                # incremental build
```

Options (combinable): `clean` (delete `build/`), `3dsxonly` (do not touch assets), `assetsonly` (only copy assets),
`romfs` (embed the assets inside the `.3dsx`; final-distribution experiment). Edit `REPO` and `AZAHAR_3DS` at the top
of the script for your paths. Each build increments `src/rr_version.txt` and writes `src/rr_version.h`
(`1.0.N`). The result is `build/bin/ringracers.3dsx`.

## 3. Convert the assets

`tools/prepare_3ds.py` (or the single-file `prepare_3ds.exe` produced by `tools/make_patch3ds.py`) takes your game
folder and writes a ready-to-copy `RingRacers3ds` folder:

1. extract definitions, convert textures to ETC1/ETC1A4 (maximum 256 px), build the sprite atlas and thumbnails;
2. convert sound effects and music to DSP-ADPCM (`tools/dspenc.c` is the encoder; build it with any C compiler;
   tracker music uses `openmpt123`);
3. convert the 63 vanilla characters (lazy-mounted `.pk3` files) and, optionally, character mods
   (`tools/convert_mod.py <mod.pk3>`; character skins with their voices only, no music or maps).

Required external tools: Python 3 with `numpy`, `Pillow`, `soundfile`, `soxr`; devkitPro's `tex3ds`; `dspenc`;
`openmpt123` (optional, tracker music).

## 4. Install on the console

Copy the folder to `SD:/3ds/RingRacers3ds` (do not rename it) and put `ringracers.3dsx` inside it. Audio needs
`SD:/3ds/dspfirm.cdc` (dump it with DSP1 from the Homebrew Launcher). Config, profiles, saves, replays, mods
(`mods/*.pk3`, maximum 16) and logs live in that same folder.

## Running in the Azahar emulator

Configure Azahar as an **Old 3DS** (CPU 100%, simulated GPU timings) and set `64mb = 1` in `rr_config.txt` to
reproduce the memory limit. The emulator does not fragment the heap like the console; use `fragment_heap = 1` to
simulate it. See [CONFIG.md](CONFIG.md) for every option.

## Machine-specific paths (environment variables)

The tools never hard-code a location. Set these (all optional unless a tool needs them):

| Variable | Meaning |
|---|---|
| `RR_REPO` | this repository (default: the folder above `tools/`) |
| `RR_AZAHAR` | folder of the Azahar emulator (contains `azahar.exe` and `user/`) |
| `RR_GAME_DIR` | folder of your own copy of the game (contains `bios.pk3` and `data/`) |
| `RR_WORK` | scratch folder for generated files, logs and backups (default: `<repo>/work`, ignored by git) |
| `RR_LOGS` | logs folder (default: `<RR_WORK>/logs`) |

They are read by `tools/rr_paths.py` and by `tools/build3ds.sh`.
