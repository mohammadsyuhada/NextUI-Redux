# System-Wide Screenshot Capture (L2+R2+X)

## Overview

Add a system-wide screenshot feature triggered by L2+R2+X that works regardless of what's running (NextUI shell, games, tools). Toggled on/off via Quick Menu.

## Architecture

A background daemon (`screenshot.elf`) follows the `keymon` pattern:
- Reads raw input from `/dev/input/event0-4` using `linux/input.h`
- Monitors for L2+R2+X combo
- Captures `/dev/fb0` framebuffer via ffmpeg as PNG
- PID tracked at `/tmp/screenshot.pid`
- Quick Menu toggle to start/stop

## Input Detection

From platform headers:
- **L2**: `EV_ABS` type, code `ABS_Z` (2), value > 0
- **R2**: `EV_ABS` type, code `ABS_RZ` (5), value > 0
- **X button**: `EV_KEY` type, code `BTN_WEST` (0x133), value = PRESSED

Daemon tracks L2/R2 axis state and triggers capture when X is pressed while both triggers are held.

## Screenshot Capture

Single-frame capture using ffmpeg (already on device):
```
ffmpeg -f fbdev -frames:v 1 -i /dev/fb0 -y /Images/Screenshots/SCR_YYYYMMDD_HHMMSS.png
```

## Quick Menu Integration

- `QUICK_SCREENSHOT` added to `QuickAction` enum in `types.h`
- `content.c`: show "Screenshot" toggle, state based on `/tmp/screenshot.pid`
- `quickmenu.c`: handle toggle to start/stop daemon
- Starting: fork + exec `screenshot.elf`, write PID
- Stopping: read PID, SIGTERM, cleanup

## Files

| File | Action |
|------|--------|
| `workspace/all/screenshot/screenshot.c` | New - daemon binary |
| `workspace/all/screenshot/makefile` | New - build config |
| `workspace/all/nextui/types.h` | Modify - add QUICK_SCREENSHOT |
| `workspace/all/nextui/content.c` | Modify - add toggle entry |
| `workspace/all/nextui/quickmenu.c` | Modify - handle toggle action |
| `workspace/makefile` | Modify - add screenshot to build |

## Save Location

`/Images/Screenshots/SCR_YYYYMMDD_HHMMSS.png`

## Daemon Lifecycle

1. Quick Menu → "Screenshot" toggle → daemon starts
2. Daemon polls input at 60fps, listens for L2+R2+X
3. Combo detected → capture framebuffer → save PNG
4. Quick Menu → "Screenshot" toggle → daemon stops
