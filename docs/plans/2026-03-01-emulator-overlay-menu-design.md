# Emulator Overlay Menu Design

## Overview

Reusable in-game menu overlay system for standalone emulators in NextUI. Replaces TrimUI's external tmenu with a native overlay rendered directly within the emulator's frame. First target: mupen64plus N64 with GLideN64 video plugin.

## Architecture

Hybrid design: shared menu logic library with swappable rendering backends.

```
+-----------------------------------------------------+
|  Menu Logic Library (workspace/all/common/)          |
|  emu_overlay.h/c     - menu state, navigation        |
|  emu_overlay_cfg.h/c - JSON config parser             |
|  emu_overlay_render.h - rendering backend API         |
+-----------------------------------------------------+
|  Rendering Backends                                   |
|  emu_overlay_gl.h/c  - OpenGL ES backend (GLideN64)   |
|  emu_overlay_sdl.h/c - SDL2 software backend (future) |
+-----------------------------------------------------+
|  Emulator Integration                                 |
|  Custom input plugin  - menu button + input routing    |
|  Video plugin hook    - overlay render in swapBuffers  |
|  JSON config file     - per-emulator settings          |
+-----------------------------------------------------+
```

## Menu Structure

### Main Menu

Same items as minarch in-game menu:

- **Continue** - close overlay, resume game
- **Save** - save state with slot selection and preview (conditional: only shown if emulator supports it)
- **Load** - load state with slot selection and preview (conditional)
- **Options** - opens settings section list
- **Quit** - exit emulator

Save/Load availability is declared in the JSON config via `save_state` and `load_state` booleans.

### Settings Navigation

```
Main Menu -> Options -> Section List -> Section Items
                           |                 |
                           B (back)          B (back to sections)
```

Options opens a list of setting sections (e.g., Rendering, Textures, Frame Buffer). Selecting a section shows its items in the settings item layout. B navigates back one level.

## Visual Layout

### Main Menu (minarch style)

- **Background**: dimmed game frame at 40% opacity
- **Title pill** (top): ROM name, light pill background (THEME_COLOR6 text), hardware indicators on right
- **Menu items** (center-left): vertically centered, left-aligned pills spaced by PILL_SIZE
  - Unselected: white text, no pill background
  - Selected: dark pill background (ASSET_WHITE_PILL dark variant), accent color text (THEME_COLOR5)
- **Save/Load preview** (center-right): preview window with screenshot, slot pagination dots (8 slots, L/R to cycle)
- **Button hints** (bottom): semi-transparent black bar (70% opacity), button icons with labels

### Settings Page (settings item layout)

- **Background**: same dimmed game frame
- **Title pill** (top): section name
- **Setting items**: 8 items per page, each row height = screen_height / 9
  - Selected: 2-layer pill (full-width THEME_COLOR2 background + THEME_COLOR1 label pill), value right-aligned in white with `< >` arrows
  - Unselected: label and value in COLOR_GRAY, no background
- **Description** (bottom row): selected item's description text, centered, COLOR_GRAY, font.tiny
- **Pagination**: scrolls with UP/DOWN when items exceed page size

### Dimensions (pre-scaled, multiply by FIXED_SCALE)

```
PILL_SIZE        = 30   (menu item height)
PADDING          = 10   (screen edge margin)
BUTTON_PADDING   = 10   (inside pills)
BUTTON_SIZE      = 16   (hint bar item height)
BUTTON_MARGIN    = 6    (hint bar spacing)
SETTINGS_ROW_PAD = 8    (settings row internal padding)
FONT_LARGE       = 16   (menu item text)
FONT_SMALL       = 12   (settings label text)
FONT_TINY        = 10   (settings value + description text)
```

## JSON Settings Config

Each emulator provides a JSON file describing its available settings. Located alongside the emulator pak (e.g., `skeleton/EXTRAS/Emus/shared/mupen64plus/overlay_settings.json`).

### Schema

```json
{
  "emulator": "mupen64plus-GLideN64",
  "config_file": "mupen64plus.cfg",
  "config_section": "Video-GLideN64",
  "save_state": true,
  "load_state": true,
  "sections": [
    {
      "name": "Rendering",
      "items": [
        {
          "key": "UseNativeResolutionFactor",
          "label": "Resolution",
          "description": "Internal rendering resolution multiplier",
          "type": "cycle",
          "values": [0, 1, 2, 3, 4],
          "labels": ["Auto", "1x", "2x", "3x", "4x"],
          "default": 2
        },
        {
          "key": "FXAA",
          "label": "Anti-Aliasing (FXAA)",
          "description": "Fast approximate anti-aliasing",
          "type": "bool",
          "default": false
        }
      ]
    }
  ]
}
```

### Item Types

- `bool` - toggles true/false, displayed as On/Off
- `cycle` - cycles through discrete values with display labels via L/R
- `int` - numeric value with min/max/step (for continuous values like gamma level)

### N64 Settings (all sections)

Rendering: UseNativeResolutionFactor, AspectRatio, FXAA, MultiSampling, anisotropy, bilinearMode, EnableHybridFilter, EnableHWLighting, EnableCoverage, EnableClipping, ThreadedVideo, BufferSwapMode

Texrect Fix: CorrectTexrectCoords, EnableNativeResTexrects, EnableTexCoordBounds

Texture Enhancement: txFilterMode, txEnhancementMode, txDeposterize, txFilterIgnoreBG

Hi-Res Textures: txHiresEnable, txHiresTextureFileStorage, txHiresFullAlphaChannel, txHresAltCRC

Dithering: EnableDitheringPattern, DitheringQuantization, RDRAMImageDitheringMode, EnableHiresNoiseDithering

Frame Buffer: EnableFBEmulation, EnableCopyColorToRDRAM, EnableCopyDepthToRDRAM, EnableCopyColorFromRDRAM, EnableN64DepthCompare, DisableFBInfo

Performance: EnableInaccurateTextureCoordinates, EnableLegacyBlending, EnableShadersStorage, EnableFragmentDepthWrite, BackgroundsMode

Gamma: ForceGammaCorrection, GammaCorrectionLevel

## Rendering Backend API

Abstract interface that the menu logic calls. Each platform implements it.

```c
typedef struct {
    // Lifecycle
    int  (*init)(int screen_w, int screen_h, const char* font_path, int font_size);
    void (*destroy)(void);

    // Primitives
    void (*draw_dimmed_bg)(float opacity);
    void (*draw_pill)(int x, int y, int w, int h, uint32_t color, int style);
    void (*draw_rect)(int x, int y, int w, int h, uint32_t color, float alpha);
    void (*draw_text)(const char* text, int x, int y, uint32_t color, int font_id);
    void (*draw_button_hint)(const char* button, const char* label, int x, int y);

    // Measurements
    int  (*text_width)(const char* text, int font_id);
    int  (*text_height)(int font_id);

    // Frame
    void (*begin_frame)(void);
    void (*end_frame)(void);
} OverlayRenderBackend;
```

### OpenGL ES Backend (emu_overlay_gl.c)

- `begin_frame()`: saves GL viewport, scissor, blend state
- Draws quads via vertex arrays + basic shader program
- Text rendering via FreeType2 glyph atlas (similar to GLideN64's TextDrawer)
- `end_frame()`: restores saved GL state

### SDL2 Backend (emu_overlay_sdl.c, future)

- Uses SDL_Surface / SDL_BlitSurface like minarch
- Can reuse GFX_blitPillDark, GFX_blitPillLight from common code
- `end_frame()`: blits overlay surface onto screen

## Input & Communication

The custom input plugin and video plugin run in the same mupen64plus process. They communicate via a shared exported symbol.

### Shared State

```c
typedef struct {
    // Input plugin -> Video plugin
    volatile int menu_pressed;       // Menu button just pressed

    // Video plugin -> Input plugin + Core
    volatile int menu_active;        // Overlay is currently shown
    volatile int emu_paused;         // Emulator paused

    // Navigation (when menu_active)
    volatile int btn_up, btn_down, btn_left, btn_right;
    volatile int btn_a, btn_b, btn_l1, btn_r1;
} OverlayInputState;
```

### Flow

1. Input plugin polls SDL joystick every frame
2. Menu button (SDL button 8) pressed -> sets `menu_pressed = 1`
3. GLideN64 checks in `swapBuffers()` -> toggles `menu_active`
4. On activate: calls `CoreDoCommand(M64CMD_PAUSE)` to freeze emulation
5. While active: input plugin routes D-pad/A/B/L1/R1 to overlay fields, suppresses N64 controller input
6. Overlay reads input fields for navigation, stages config changes
7. On deactivate: applies staged changes, writes config, calls `CoreDoCommand(M64CMD_RESUME)`

### Pause Behavior

When overlay is active, the emulator is fully paused via `CoreDoCommand(M64CMD_PAUSE)`. No CPU or audio processing occurs. This ensures:
- Game state is frozen while user browses settings
- No input leaks into the game
- Audio doesn't continue playing under the menu

## Settings Apply Behavior

- Changes are **staged** while the user browses settings
- On menu close: all staged changes are **applied** to the emulator's runtime config and **saved** to the config file
- If the user quits from the main menu, staged changes are discarded

## File Layout

```
workspace/all/common/
  emu_overlay.h            # Public API: init, update, render, cleanup
  emu_overlay.c            # Menu state machine, navigation logic
  emu_overlay_cfg.h        # JSON config parser + INI reader/writer
  emu_overlay_cfg.c
  emu_overlay_render.h     # Rendering backend interface (abstract)
  emu_overlay_gl.h         # OpenGL ES backend header
  emu_overlay_gl.c         # OpenGL ES backend implementation
  emu_overlay_sdl.h        # SDL2 backend header (future)
  emu_overlay_sdl.c        # SDL2 backend implementation (future)
  emu_overlay_input.h      # Shared input state struct

workspace/tg5040/cores/src/
  mupen64plus-input-sdl/   # Custom input plugin (built from source)
    src/plugin.c            # Modified: menu button + overlay input routing
  GLideN64-standalone/
    src/
      DisplayWindow.cpp     # Modified: overlay hook in swapBuffers()

skeleton/EXTRAS/Emus/shared/mupen64plus/
  overlay_settings.json    # GLideN64 settings descriptor

skeleton/EXTRAS/Emus/tg5040/N64.pak/
  mupen64plus-input-sdl.so # Custom-built input plugin
  launch.sh                # Updated: use custom input plugin

skeleton/EXTRAS/Emus/tg5050/N64.pak/
  mupen64plus-input-sdl.so # Custom-built input plugin (tg5050 build)
  launch.sh                # Updated: use custom input plugin
```

## Dependencies

- **cJSON** (or similar lightweight JSON parser): for parsing overlay_settings.json. Already commonly available on embedded Linux, or can be vendored as a single .c/.h file.
- **FreeType2**: for the GL backend's text rendering. Already used by GLideN64.
- **SDL2**: for input polling in the custom input plugin. Already linked by mupen64plus.

## Future Reuse

To add the overlay to another standalone emulator:

1. Create an `overlay_settings.json` describing that emulator's settings
2. Integrate the input detection (Menu button -> pause -> activate overlay)
3. Choose the appropriate rendering backend (GL or SDL2)
4. Call `emu_overlay_init()`, `emu_overlay_update()`, `emu_overlay_render()` in the emulator's frame loop
