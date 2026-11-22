# Dr. Robotnik's Ring Racers

<p align="center">
  <a href="https://www.kartkrew.org">
    <img src="docs/logo.png" width="404" style="image-rendering:pixelated" alt="Dr. Robotnik's Ring Racers logo">
  </a>
</p>

> ## Unofficial fork: Ring Racers for the Nintendo 3DS (and Ring Racers E for PC)
>
> This repository is a fork of Kart Krew Dev's Ring Racers (base `49beb6b`, v2.4) that adds:
>
> - a **port to the Old Nintendo 3DS** (devkitPro / citro3d): renderer, memory manager, audio, asset pipeline,
>   bottom-screen UI and a large set of performance changes. It runs, but slowly on heavy maps.
> - the shared **"low-end" mode** and the PC edition **Ring Racers E** (multiple graphics APIs, online play for E only).
>
> Start here: [docs/3DS_PORT.md](docs/3DS_PORT.md) (changes, known problems, what is missing) ·
> [docs/PERFORMANCE.md](docs/PERFORMANCE.md) (measurements) · [docs/BUILD_3DS.md](docs/BUILD_3DS.md) ·
> [docs/CONFIG.md](docs/CONFIG.md) · [docs/TOOLS.md](docs/TOOLS.md) · [wiki](docs/wiki/Home.md) · [journey, sacrifices and dead ends](docs/wiki/Journey%20Sacrifices%20Dead%20Ends.md).
>
> Not affiliated with Kart Krew Dev. No game assets are included; you convert your own copy.
>
> The 3DS patch tool and the conversion of textures and music to 3DS formats were made with the help of Claude (Anthropic), an AI assistant.


Dr. Robotnik's Ring Racers is a kart racing video game originally based on the 3D Sonic the Hedgehog fangame [Sonic Robo Blast 2](https://srb2.org/), itself based on a modified version of [Doom Legacy](http://doomlegacy.sourceforge.net/).

Ring Racers' source code is available to users under the GNU General Public License version 2.0 or higher.

## Links

- [Kart Krew Dev Website](https://www.kartkrew.org/)
- [Kart Krew Dev Discord](https://www.kartkrew.org/discord)
- [SRB2 Forums](https://mb.srb2.org/)

## Disclaimer

Dr. Robotnik's Ring Racers is a work of fan art made available for free without intent to profit or harm the intellectual property rights of the original works it is based on. Kart Krew Dev is in no way affiliated with SEGA Corporation. We do not claim ownership of any of SEGA's intellectual property used in Dr. Robotnik's Ring Racers.

# Development

## Building from Source

Ring Racers is built using a compatible C++ toolchain (GCC, MinGW, Clang and Apple Clang as of this writing), CMake, and Microsoft vcpkg. The compiler and runtime libraries must support the ISO C++17 standard and ISO C11 standard.

On Linux platforms, you will need the following libraries available on the system.

- libcurl
- zlib
- libpng
- libogg
- libvorbis
- libvpx
- libyuv
- SDL2
- libopus

On Windows and macOS, you will need to install [vcpkg] instead to build these dependencies alongside the game.

[vcpkg]: https://vcpkg.io/en/

To configure and build the game, there are [CMake presets] (declared in `CMakePresets.json`). These presets require the ninja build script tool in addition to cmake and your C++ toolchain. Here is a non-exhaustive list of them:

- ninja-debug: non-optimized, assertions enabled
- ninja-develop: optimized, assertions enabled
- ninja-release: optimized
- ninja-x86_mingw_static_vcpkg-debug
- ninja-x86_mingw_static_vcpkg-develop
- ninja-x86_mingw_static_vcpkg-release
- ninja-x64_osx_vcpkg-debug
- ninja-x64_osx_vcpkg-develop
- ninja-x64_osx_vcpkg-release
- ninja-arm64_osx_vcpkg-debug
- ninja-arm64_osx_vcpkg-develop
- ninja-arm64_osx_vcpkg-release

[CMake presets]: https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html

These presets depend on the `VCPKG_ROOT` environment variable being specified before the first run of the `cmake` command. Their build directories are pre-configured as subdirectories of `build/`.

After all prerequisites are set-up, configure and build using the following commands, adjusting according to your target system:

    cmake --preset ninja-x86_mingw_static_vcpkg-develop
    cmake --build --preset ninja-x86_mingw_static_vcpkg-develop

## Contributing

We welcome external contributions from the community. If you are planning on making a large feature you intend to contribute to the project, please consider reaching out to us in the Kart Krew Dev public Discord server so we can coordinate with you.

Our primary source repository is [hosted on gitlab.com](https://gitlab.com/kart-krew-dev/ring-racers). The Github repository is a mirror of this. If you submit a Pull Request to the Github repository, please keep in mind that we do not consistently monitor that mirror and may not see your request.

All contributions must be made available under the GPL General Public License version 2.0, or public domain. Integrations for third party code must be made to code which is compatibly licensed.

