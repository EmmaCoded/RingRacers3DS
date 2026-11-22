# Copies the animation/brightness/terrain definitions from the textures_*.pk3 files
# originals into a small pk3: sdmc\3ds\data\ringracers_3ds_defs.pk3
# Usage:  py extract_defs.py      (writes a log to extract_defs_log.txt)
import glob, os, sys, zipfile
FIXED_DATA = os.path.join(os.environ.get("RR_GAME_DIR", ""), "data")
WANT = {"ANIMDEFS", "RVFXANIM", "BRIGHT", "TERRAIN", "ANIMATED", "SWITCHES"}
IMG = (".png", ".lmp", ".gif", ".jpg", ".bmp", ".tga")

def main():
    # optional arguments: <data folder> [<output folder>] (used by prepare_3ds.py)
    DATA = sys.argv[1] if len(sys.argv) > 1 else FIXED_DATA
    SAL = sys.argv[2] if len(sys.argv) > 2 else DATA
    log = []
    output = os.path.join(SAL, "ringracers_3ds_defs.pk3")
    sources = sorted(glob.glob(os.path.join(DATA, "textures_*.pk3")), key=str.lower)
    if not sources:
        print("textures_*.pk3 not found in", DATA); sys.exit(1)
    n = 0
    with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as zo:
        for f in sources:
            pre = os.path.splitext(os.path.basename(f))[0]
            with zipfile.ZipFile(f) as z:
                for info in z.infolist():
                    if info.is_dir(): continue
                    base = os.path.splitext(os.path.basename(info.filename))[0].upper()
                    ext = os.path.splitext(info.filename)[1].lower()
                    if base in WANT:
                        # at the root so that W_CheckNumForNamePwad finds it
                        # lump name = up to the first dot -> "ANIMDEFS"
                        zo.writestr(f"{base}.{n}", z.read(info))
                        log.append(f"COPIED  {pre}: {info.filename} ({info.file_size} B)")
                        n += 1
                    elif ext not in IMG and info.file_size < 2_000_000:
                        log.append(f"other    {pre}: {info.filename} ({info.file_size} B)")
    log.append(f"Total copied: {n} -> {output}")
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "extract_defs_log.txt"), "w", encoding="utf-8").write("\n".join(log))
    print("\n".join(l for l in log if not l.startswith("other")))


if __name__ == "__main__":
    main()
