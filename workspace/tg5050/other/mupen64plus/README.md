# Mupen64Plus Standalone (N64 Emulator) — TG5050

Standalone mupen64plus built from upstream sources with custom overlay menu integration.

See `workspace/tg5040/other/mupen64plus/README.md` for full documentation on source
modifications and deployment layout. This file covers TG5050-specific build differences.

## Components

| Directory | Description |
|---|---|
| `mupen64plus-core/` | Emulator core library (`libmupen64plus.so.2`) |
| `mupen64plus-ui-console/` | Console frontend binary (`mupen64plus`) |
| `mupen64plus-audio-sdl/` | Audio plugin (`mupen64plus-audio-sdl.so`) |
| `libpng-headers/` | libpng 1.6.37 headers (TG5050 toolchain has broken symlinks) |

GLideN64 is built from `workspace/tg5040/other/mupen64plus/GLideN64-standalone/` — the
video plugin .so is shared across platforms.

## TG5050 Toolchain Notes

The TG5050 toolchain (`ghcr.io/loveretro/tg5050-toolchain:latest`) has broken symlinks
for libpng headers (`png.h -> libpng16/png.h` where `libpng16/` doesn't exist). The
`libpng-headers/` directory provides the missing headers.

## Build (TG5050)

All builds run inside Docker using `ghcr.io/loveretro/tg5050-toolchain:latest`.

### 1. Download libpng headers (one-time)

```sh
mkdir -p workspace/tg5050/other/mupen64plus/libpng-headers
cd workspace/tg5050/other/mupen64plus/libpng-headers
curl -sL https://github.com/glennrp/libpng/archive/refs/tags/v1.6.37.tar.gz -o libpng.tar.gz
tar xf libpng.tar.gz
cp libpng-1.6.37/scripts/pnglibconf.h.prebuilt libpng-1.6.37/pnglibconf.h
```

### 2. mupen64plus-core

**Important:** If switching build configurations, delete stale generated headers before rebuilding
(the Makefile's `make clean` requires cross-toolchain variables). Clean manually inside Docker:

```sh
rm -rf _obj libmupen64plus.so* ../../src/asm_defines/asm_defines_gas.h ../../src/asm_defines/asm_defines_nasm.h
```

```sh
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5050-toolchain:latest /bin/bash -c '
source ~/.bashrc
export PKG_CONFIG_PATH=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc
PNG_HEADERS=/root/workspace/tg5050/other/mupen64plus/libpng-headers/libpng-1.6.37
cd /root/workspace/tg5050/other/mupen64plus/mupen64plus-core/projects/unix
make -j$(nproc) all \
  CROSS_COMPILE=aarch64-nextui-linux-gnu- HOST_CPU=aarch64 \
  USE_GLES=1 NEON=1 PIE=1 VULKAN=0 \
  PKG_CONFIG=pkg-config \
  LIBPNG_CFLAGS="-I${PNG_HEADERS}" LIBPNG_LDLIBS="-lpng16 -lz" \
  OPTFLAGS="-O3"
'
```

Output: `mupen64plus-core/projects/unix/libmupen64plus.so.2.0.0`

### 3. mupen64plus-ui-console

```sh
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5050-toolchain:latest /bin/bash -c '
source ~/.bashrc
export PKG_CONFIG_PATH=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc
SDL_C="$(pkg-config --cflags sdl2)"
SDL_L="$(pkg-config --libs sdl2)"
cd /root/workspace/tg5050/other/mupen64plus/mupen64plus-ui-console/projects/unix
make -j$(nproc) all \
  CROSS_COMPILE=aarch64-nextui-linux-gnu- HOST_CPU=aarch64 PIE=1 \
  PKG_CONFIG=pkg-config \
  SDL_CFLAGS="$SDL_C" SDL_LDLIBS="$SDL_L" \
  APIDIR=/root/workspace/tg5050/other/mupen64plus/mupen64plus-core/src/api \
  COREDIR="./" PLUGINDIR="./" \
  OPTFLAGS="-O3"
'
```

Output: `mupen64plus-ui-console/projects/unix/mupen64plus`

### 4. mupen64plus-audio-sdl

Built from source to enable `src-sinc-fastest` resampler (requires libsamplerate, available in toolchain).
The stock binary only includes the `trivial` resampler.

```sh
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5050-toolchain:latest /bin/bash -c '
source ~/.bashrc
export PKG_CONFIG_PATH=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc
SDL_C="$(pkg-config --cflags sdl2)"
SDL_L="$(pkg-config --libs sdl2)"
cd /root/workspace/tg5050/other/mupen64plus/mupen64plus-audio-sdl/projects/unix
make -j$(nproc) all \
  CROSS_COMPILE=aarch64-nextui-linux-gnu- HOST_CPU=aarch64 PIE=1 \
  PKG_CONFIG=pkg-config \
  SDL_CFLAGS="$SDL_C" SDL_LDLIBS="$SDL_L" \
  APIDIR=/root/workspace/tg5050/other/mupen64plus/mupen64plus-core/src/api \
  OPTFLAGS="-O3"
'
```

Output: `mupen64plus-audio-sdl/projects/unix/mupen64plus-audio-sdl.so`

**Note:** The device must have `libsamplerate.so.0` available at runtime. The launch script
includes `$SDCARD_PATH/.system/tg5050/lib` in `LD_LIBRARY_PATH` for this.

## Deployment

```
skeleton/EXTRAS/Emus/tg5050/N64.pak/
├── mupen64plus                    ← ui-console binary (TG5050 build)
├── libmupen64plus.so.2            ← core library (TG5050 build)
├── mupen64plus-audio-sdl.so       ← audio plugin (built from source, libsamplerate)
├── mupen64plus-input-sdl.so       ← stock input plugin
├── mupen64plus-rsp-hle.so         ← stock RSP plugin
├── launch.sh
└── default.cfg
```
