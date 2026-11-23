"""Machine-specific locations used by the tools. Nothing here is hard-coded to one computer:
set these environment variables to match your setup (all are optional except where a tool needs the path).

  RR_REPO      this repository (default: the folder above tools/)
  RR_AZAHAR    folder of the Azahar emulator (contains azahar.exe and user\\)
  RR_GAME_DIR  folder of your own copy of Dr. Robotnik's Ring Racers (contains bios.pk3 and data\\)
  RR_WORK      scratch folder for generated files, logs and backups (default: <repo>/work, ignored by git)
  RR_LOGS      logs folder (default: <RR_WORK>/logs)
"""
import os

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("RR_REPO", os.path.dirname(TOOLS))
AZAHAR = os.environ.get("RR_AZAHAR", r"C:\Azahar")
GAME_DIR = os.environ.get("RR_GAME_DIR", "")
WORK = os.environ.get("RR_WORK", os.path.join(REPO, "work"))
LOGS = os.environ.get("RR_LOGS", os.path.join(WORK, "logs"))
TEXTURES = os.path.join(WORK, "textures3ds")      # converted textures/sprites produced by the convert_* tools
PATCH_OUT = os.path.join(WORK, "PATCH_3DS")       # prepare_3ds.exe built by make_patch3ds.py
PATCH_BUILD = os.path.join(WORK, "patch3ds_build")
GDB_DIR = os.path.join(WORK, "gdb")

SDMC = os.path.join(AZAHAR, "user", "sdmc")
SD_BASE = os.path.join(SDMC, "3ds", "RingRacers3ds")   # the game folder inside Azahar's virtual SD card


def msys_path(p):
    """C:\\foo\\bar -> /c/foo/bar (for the MSYS2 shell that devkitPro ships)."""
    p = os.path.abspath(p)
    if len(p) > 1 and p[1] == ":":
        return "/" + p[0].lower() + p[2:].replace("\\", "/")
    return p.replace("\\", "/")
