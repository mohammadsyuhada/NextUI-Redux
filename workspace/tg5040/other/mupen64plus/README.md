# Mupen64Plus Standalone (N64 Emulator)

Standalone mupen64plus built from upstream sources with custom overlay menu integration.

## Components

| Directory | Description |
|---|---|
| `mupen64plus-core/` | Emulator core library (`libmupen64plus.so.2`) |
| `mupen64plus-ui-console/` | Console frontend binary (`mupen64plus`) |
| `mupen64plus-audio-sdl/` | Audio plugin (`mupen64plus-audio-sdl.so`) |
| `GLideN64-standalone/` | Video plugin (`mupen64plus-video-GLideN64.so`) |

## Source Modifications

### mupen64plus-ui-console

**`src/osal_dynamiclib_unix.c`** — Change `dlopen()` to use `RTLD_GLOBAL` so GLES/EGL
symbols from the core library are visible to plugins loaded later:

```c
*pLibHandle = dlopen(pccLibraryPath, RTLD_NOW | RTLD_GLOBAL);
```

### GLideN64-standalone

**`toolchain-aarch64.cmake`** — Cross-compilation toolchain for Docker builds.

**`src/overlay/OverlayGL.cpp`** — OpenGL ES render backend for the in-game overlay menu.

**`src/DisplayWindow.cpp`** — Overlay integration: init, menu button detection, overlay
loop, save/load state handling via `CoreDoCommand`.

**`src/CMakeLists.txt`** — Added overlay source files from `workspace/all/common/`.
Note: `include_directories(${OVERLAY_COMMON_DIR})` must come AFTER `include_directories(. inc)`
to avoid `config.h`/`Config.h` name collision on case-insensitive filesystems.

## Build (TG5040)

All builds run inside Docker using `ghcr.io/loveretro/tg5040-toolchain:latest`.

### 1. mupen64plus-core

**Important:** If switching build configurations (e.g., first build without dynarec, then with),
you must delete stale generated headers before rebuilding. The Makefile's `make clean` requires
cross-toolchain variables, so clean manually inside Docker first:

```sh
rm -rf _obj libmupen64plus.so* ../../src/asm_defines/asm_defines_gas.h ../../src/asm_defines/asm_defines_nasm.h
```

```sh
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c '
source ~/.bashrc
export PKG_CONFIG_PATH=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc
cd /root/workspace/tg5040/other/mupen64plus/mupen64plus-core/projects/unix
make -j$(nproc) all \
  CROSS_COMPILE=aarch64-nextui-linux-gnu- HOST_CPU=aarch64 \
  USE_GLES=1 NEON=1 PIE=1 VULKAN=0 \
  PKG_CONFIG=pkg-config \
  OPTFLAGS="-O3"
'
```

Output: `mupen64plus-core/projects/unix/libmupen64plus.so.2.0.0`

### 2. mupen64plus-ui-console

```sh
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c '
source ~/.bashrc
export PKG_CONFIG_PATH=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc
SDL_C="$(pkg-config --cflags sdl2)"
SDL_L="$(pkg-config --libs sdl2)"
cd /root/workspace/tg5040/other/mupen64plus/mupen64plus-ui-console/projects/unix
make -j$(nproc) all \
  CROSS_COMPILE=aarch64-nextui-linux-gnu- HOST_CPU=aarch64 PIE=1 \
  PKG_CONFIG=pkg-config \
  SDL_CFLAGS="$SDL_C" SDL_LDLIBS="$SDL_L" \
  APIDIR=/root/workspace/tg5040/other/mupen64plus/mupen64plus-core/src/api \
  COREDIR="./" PLUGINDIR="./" \
  OPTFLAGS="-O3"
'
```

Output: `mupen64plus-ui-console/projects/unix/mupen64plus`

### 3. mupen64plus-audio-sdl

Built from source to enable `src-sinc-fastest` resampler (requires libsamplerate, available in toolchain).
The stock binary only includes the `trivial` resampler.

```sh
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c '
source ~/.bashrc
export PKG_CONFIG_PATH=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc
SDL_C="$(pkg-config --cflags sdl2)"
SDL_L="$(pkg-config --libs sdl2)"
cd /root/workspace/tg5040/other/mupen64plus/mupen64plus-audio-sdl/projects/unix
make -j$(nproc) all \
  CROSS_COMPILE=aarch64-nextui-linux-gnu- HOST_CPU=aarch64 PIE=1 \
  PKG_CONFIG=pkg-config \
  SDL_CFLAGS="$SDL_C" SDL_LDLIBS="$SDL_L" \
  APIDIR=/root/workspace/tg5040/other/mupen64plus/mupen64plus-core/src/api \
  OPTFLAGS="-O3"
'
```

Output: `mupen64plus-audio-sdl/projects/unix/mupen64plus-audio-sdl.so`

**Note:** The device must have `libsamplerate.so.0` available at runtime. The launch script
includes `$SDCARD_PATH/.system/tg5040/lib` in `LD_LIBRARY_PATH` for this.

### 4. GLideN64 video plugin

```sh
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c '
source ~/.bashrc
cd /root/workspace/tg5040/other/mupen64plus/GLideN64-standalone/src
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../../toolchain-aarch64.cmake \
  -DMUPENPLUSAPI=ON -DEGL=ON -DMESA=ON \
  -DNEON_OPT=ON -DCRC_ARMV8=ON ..
make -j$(nproc) mupen64plus-video-GLideN64
'
```

Output: `GLideN64-standalone/src/build/plugin/Release/mupen64plus-video-GLideN64.so`

## Deployment

Copy built binaries to the N64.pak directory:

```
skeleton/EXTRAS/Emus/tg5040/N64.pak/
├── mupen64plus                    ← ui-console binary
├── libmupen64plus.so.2            ← core library
├── mupen64plus-audio-sdl.so       ← audio plugin (built from source, libsamplerate)
├── mupen64plus-input-sdl.so       ← stock input plugin
├── mupen64plus-rsp-hle.so         ← stock RSP plugin
├── launch.sh
└── default.cfg

skeleton/EXTRAS/Emus/shared/mupen64plus/
├── mupen64plus-video-GLideN64.so  ← video plugin (shared across platforms)
├── overlay_settings.json          ← overlay menu config
├── mupen64plus.ini                ← ROM database
├── InputAutoCfg.ini               ← input auto-config
├── mupencheat.txt                 ← cheat codes
└── libpng16.so.16                 ← libpng runtime
```

## Key Build Flags

| Flag | Purpose |
|---|---|
| `USE_GLES=1` | Use OpenGL ES instead of desktop GL |
| `NEON=1` | Enable ARM NEON SIMD optimizations |
| `PIE=1` | Position-independent executable |
| `VULKAN=0` | Disable Vulkan (not available on target) |
| `HOST_CPU=aarch64` | Target architecture (enables NEW_DYNAREC) |
| `COREDIR="./"` | Search for core library relative to CWD |
| `PLUGINDIR="./"` | Search for plugins relative to CWD |
| `PKG_CONFIG=pkg-config` | Override cross-prefix pkg-config lookup |
