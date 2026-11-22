#!/bin/bash
# Installs the devkitPro packages that Ring Racers 3DS needs.
# Usage (in devkitPro's MSYS2): bash install_devkitpro.sh
cd "$(dirname "$0")"
if [ -f devkitpro_packages.txt ]; then
	echo ">> Installing the exact list (devkitpro_packages.txt)..."
	dkp-pacman -S --needed --noconfirm $(cat devkitpro_packages.txt) || exit 1
else
	echo ">> No exact list: installing the base packages..."
	dkp-pacman -S --needed --noconfirm 3ds-dev 3ds-cmake 3ds-pkg-config \
		3ds-zlib 3ds-libpng 3ds-libogg 3ds-libvorbis || exit 1
fi
pacman -S --needed --noconfirm make cmake 2>/dev/null
echo "OK Done. Now: bash tools/build3ds.sh clean"
