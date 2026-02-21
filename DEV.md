# NextUI Development Guide

## Desktop Development Setup

### Prerequisites

Install dependencies via Homebrew:

```bash
brew install gcc sdl2 sdl2_image sdl2_ttf sqlite libsamplerate clang-format
```

### One-time Setup

#### 1. Create GCC symlinks

The build expects `gcc` to be Homebrew's GCC (not Apple Clang). This script symlinks Homebrew's GCC binaries into `/usr/local/bin/`:

```bash
sudo ./workspace/desktop/macos_create_gcc_symlinks.sh
```

Verify with:

```bash
gcc --version  # Should say "Homebrew GCC", not "Apple clang"
```

#### 2. Prepare fake SD card root

The desktop build uses `/var/tmp/nextui/sdcard` as a stand-in for the device's SD card:

```bash
./workspace/desktop/prepare_fake_sd_root.sh
```

#### 3. Generate compile commands (for IDE support)

Generate `compile_commands.json` so clangd can resolve includes and provide diagnostics:

```bash
make compile-commands
```

This is gitignored since it contains absolute paths. Each developer must run this once after cloning.

### Building (Desktop)

#### Build libmsettings (required first)

```bash
cd workspace/desktop/libmsettings
make build CROSS_COMPILE=/usr/local/bin/ PREFIX=/opt/homebrew PREFIX_LOCAL=/opt/homebrew
```

#### Build nextui

```bash
cd workspace/all/nextui
make PLATFORM=desktop CROSS_COMPILE=/usr/local/bin/ PREFIX=/opt/homebrew PREFIX_LOCAL=/opt/homebrew UNAME_S=Darwin
```

The binary is output to `workspace/all/nextui/build/desktop/nextui.elf`.

### Running (Desktop)

```bash
cd workspace/all/nextui
DYLD_LIBRARY_PATH=/opt/homebrew/lib ./build/desktop/nextui.elf
```

## Quick Build (Device - Docker)

Build and push a specific component directly using docker:

```bash
# nextui
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c 'source ~/.bashrc && cd /root/workspace/all/nextui && make PLATFORM=tg5040' && adb push workspace/all/nextui/build/tg5040/nextui.elf /mnt/SDCARD/.system/tg5040/bin/ && adb shell reboot

# minarch
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c 'source ~/.bashrc && cd /root/workspace/all/minarch && make PLATFORM=tg5040' && adb push workspace/all/minarch/build/tg5040/minarch.elf /mnt/SDCARD/.system/tg5040/bin/ && adb shell reboot

# settings
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c 'source ~/.bashrc && cd /root/workspace/all/settings && make PLATFORM=tg5040' && adb push workspace/all/settings/build/tg5040/settings.elf /mnt/SDCARD/Tools/tg5040/Settings.pak/

# updater
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c 'source ~/.bashrc && cd /root/workspace/all/updater && make PLATFORM=tg5040' && adb push workspace/all/updater/build/tg5040/updater.elf /mnt/SDCARD/Tools/tg5040/Updater.pak/

# bootlogo
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c 'source ~/.bashrc && cd /root/workspace/all/bootlogo && make PLATFORM=tg5040' && adb push workspace/all/bootlogo/build/tg5040/bootlogo.elf /mnt/SDCARD/Tools/tg5040/Bootlogo.pak/

# battery
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c 'source ~/.bashrc && cd /root/workspace/all/battery && make PLATFORM=tg5040' && adb push workspace/all/battery/build/tg5040/battery.elf /mnt/SDCARD/Tools/tg5040/Battery.pak/
```

## Component Locations

| Component | Source | Output |
|-----------|--------|--------|
| nextui | workspace/all/nextui | build/tg5040/nextui.elf |
| minarch | workspace/all/minarch | build/tg5040/minarch.elf |
| settings | workspace/all/settings | build/tg5040/settings.elf |
| clock | workspace/all/clock | build/tg5040/clock.elf |
| battery | workspace/all/battery | build/tg5040/battery.elf |
| keymon | workspace/tg5040/keymon | keymon.elf |

## IDE Setup (VS Code)

The project uses **clangd** for code intelligence. Install the [clangd extension](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd).

If you also have the Microsoft C/C++ extension installed, disable its IntelliSense to avoid conflicts (this is already configured in `.vscode/settings.json`):

```json
"C_Cpp.intelliSenseEngine": "disabled"
```

After cloning, run `make compile-commands` to generate `compile_commands.json` for clangd.

Format-on-save is enabled via `.clang-format` + `.vscode/settings.json`.

## Code Formatting

Format all project source files:

```bash
make format
```

This runs `clang-format -i` on all tracked `.c` and `.h` files, respecting `.gitignore` and `.clang-format-ignore`.

Install the pre-commit hook to enforce formatting:

```bash
./scripts/install-hooks.sh
```

## Syncing with Upstream

Pull updates from the upstream repo while keeping a linear history:

```bash
git fetch upstream
git rebase upstream/main
git push --force-with-lease
```
