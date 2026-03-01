# Emu Overlay SDL Redesign Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the GL bitmap-font overlay rendering with an SDL+TTF backend that matches NextUI's settings page visual style (rounded rect pills, TTF fonts, same layout as portmaster.c).

**Architecture:** Create a new SDL render backend (`emu_overlay_sdl.c`) that implements `EmuOvlRenderBackend` using SDL_Surface + SDL_ttf. The `capture_frame()` uses `glReadPixels`, the menu renders onto an SDL surface using TTF fonts and rounded rects, and `end_frame()` uploads the surface as a GL texture for display. The overlay rendering in `emu_overlay.c` is restructured to produce a settings-page-style layout using the render backend's `draw_rect`/`draw_text` primitives.

**Tech Stack:** C, SDL2, SDL_ttf, OpenGL ES 3.0, existing EmuOvlRenderBackend interface

---

### Task 1: Create SDL render backend header

**Files:**
- Create: `workspace/all/common/emu_overlay_sdl.h`

**Step 1: Write the header file**

```c
#ifndef EMU_OVERLAY_SDL_H
#define EMU_OVERLAY_SDL_H

#include "emu_overlay_render.h"

// Get the SDL render backend.
// Before calling, set these environment variables:
//   EMU_OVERLAY_FONT — path to TTF font file
EmuOvlRenderBackend* overlay_sdl_get_backend(void);

#endif
```

**Step 2: Commit**

```bash
git add workspace/all/common/emu_overlay_sdl.h
git commit -m "feat(overlay): add SDL render backend header"
```

---

### Task 2: Create SDL render backend implementation

**Files:**
- Create: `workspace/all/common/emu_overlay_sdl.c`

This is the GL↔SDL bridge. It implements the `EmuOvlRenderBackend` interface:
- `init()` — create SDL surfaces, load TTF fonts, set up GL resources for texture upload
- `capture_frame()` — `glReadPixels()` → SDL_Surface (flip vertically)
- `begin_frame()` — save GL state, blit dimmed capture onto render surface
- `draw_rect()` — `SDL_FillRect()` with ARGB color
- `draw_text()` — `TTF_RenderUTF8_Blended()` + `SDL_BlitSurface()`
- `text_width()` — `TTF_SizeUTF8()`
- `text_height()` — `TTF_FontHeight()`
- `end_frame()` — upload surface → GL texture → fullscreen quad → restore GL state

**Step 1: Write `emu_overlay_sdl.c`**

Key implementation details:

```c
#include "emu_overlay_sdl.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <GLES3/gl3.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Font scale: 3 for small screens (<720p), 2 for larger
static int s_scale = 2;

// Font sizes (matching NextUI defines.h)
#define FONT_LARGE_SIZE  16
#define FONT_SMALL_SIZE  12
#define FONT_TINY_SIZE   10

// Fonts
static TTF_Font* s_fontLarge = NULL;  // font_id 0
static TTF_Font* s_fontSmall = NULL;  // font_id 1
static TTF_Font* s_fontTiny  = NULL;  // font_id 2

// Surfaces
static SDL_Surface* s_renderSurface = NULL;   // main render target
static SDL_Surface* s_captureSurface = NULL;  // captured game frame
static int s_screenW = 0, s_screenH = 0;

// GL resources for final display
static GLuint s_uploadTexture = 0;
static GLuint s_program = 0;
static GLint  s_locTexture = -1;
static GLuint s_vao = 0, s_vbo = 0;

// Saved GL state
static GLint s_savedViewport[4];
// ... (same GL state save/restore pattern as OverlayGL.cpp)

// --- Shaders (same as OverlayGL tex shader) ---
static const char* s_vs = "..."; // vertex shader
static const char* s_fs = "..."; // fragment shader (samples texture, no dim)
```

The font loading uses the same paths/sizes as NextUI:
- `s_fontLarge = TTF_OpenFont(fontPath, FONT_LARGE_SIZE * s_scale)`
- `s_fontSmall = TTF_OpenFont(fontPath, FONT_SMALL_SIZE * s_scale)`
- `s_fontTiny  = TTF_OpenFont(fontPath, FONT_TINY_SIZE  * s_scale)`

The `capture_frame()` does:
```c
static void ovl_sdl_capture_frame(void) {
    unsigned char* pixels = malloc(s_screenW * s_screenH * 4);
    glReadPixels(0, 0, s_screenW, s_screenH, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    // Flip vertically (GL is bottom-up)
    // Copy into s_captureSurface with row reversal
    // Apply dimming (multiply RGB by ~0.4)
    free(pixels);
}
```

The `draw_rect()` converts ARGB color to SDL format and calls `SDL_FillRect`.

The `draw_text()` renders with `TTF_RenderUTF8_Blended` using the font for `font_id`, then blits onto `s_renderSurface`.

The `end_frame()` uploads `s_renderSurface->pixels` via `glTexImage2D` and draws a fullscreen quad.

**Step 2: Commit**

```bash
git add workspace/all/common/emu_overlay_sdl.c
git commit -m "feat(overlay): implement SDL render backend with TTF fonts"
```

---

### Task 3: Rewrite emu_overlay.c rendering for settings-page style

**Files:**
- Modify: `workspace/all/common/emu_overlay.c`

Replace `render_main_menu()`, `render_section_list()`, `render_section_items()` with settings-page-style rendering.

**Step 1: Add helper functions**

Add these helpers to `emu_overlay.c`:

```c
// Draw a rounded rect using multiple draw_rect calls (scanline approximation)
static void draw_rounded_rect(EmuOvlRenderBackend* r, int x, int y, int w, int h,
                               uint32_t color) {
    int radius = S(7);
    if (radius > h / 2) radius = h / 2;
    if (radius > w / 2) radius = w / 2;

    // Middle section
    if (h - 2 * radius > 0)
        r->draw_rect(x, y + radius, w, h - 2 * radius, color);

    // Rounded corners via scanlines
    for (int dy = 0; dy < radius; dy++) {
        int yd = radius - dy;
        int inset = radius - (int)sqrtf((float)(radius * radius - yd * yd));
        int row_w = w - 2 * inset;
        if (row_w <= 0) continue;
        r->draw_rect(x + inset, y + dy, row_w, 1, color);
        r->draw_rect(x + inset, y + h - 1 - dy, row_w, 1, color);
    }
}

// Draw a menu bar (semi-transparent black bar with gray title text)
static void draw_menu_bar(EmuOvl* ovl, const char* title) {
    EmuOvlRenderBackend* r = ovl->render;
    int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;

    // Semi-transparent black bar
    r->draw_rect(0, 0, ovl->screen_w, bar_h, 0xB2000000);

    // Title text (left-aligned, gray)
    int text_y = (bar_h - r->text_height(EMU_OVL_FONT_SMALL)) / 2;
    r->draw_text(title, S(PADDING + BUTTON_PADDING), text_y,
                 EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_SMALL);
}

// Draw a button hint bar at the bottom
static void draw_hint_bar(EmuOvl* ovl, const char* hints[], int hint_count) {
    EmuOvlRenderBackend* r = ovl->render;
    int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;
    int bar_y = ovl->screen_h - bar_h;

    // Semi-transparent black bar
    r->draw_rect(0, bar_y, ovl->screen_w, bar_h, 0xB2000000);

    // Render hint pairs: "B" "BACK" "A" "OK" → "B BACK   A OK"
    int x = S(PADDING) + S(BUTTON_MARGIN);
    int text_y = bar_y + (bar_h - r->text_height(EMU_OVL_FONT_TINY)) / 2;
    for (int i = 0; i < hint_count; i += 2) {
        // Button name
        r->draw_text(hints[i], x, text_y, EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
        x += r->text_width(hints[i], EMU_OVL_FONT_TINY) + S(3);
        // Action label
        if (i + 1 < hint_count) {
            r->draw_text(hints[i+1], x, text_y, EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_TINY);
            x += r->text_width(hints[i+1], EMU_OVL_FONT_TINY) + S(BUTTON_MARGIN) * 2;
        }
    }
}

// Draw a settings row (label on left, optional value on right)
// Matches the visual style of UI_renderSettingsRow from ui_list.c
static void draw_settings_row(EmuOvl* ovl, int x, int y, int w, int h,
                                const char* label, const char* value,
                                bool selected, bool cycleable) {
    EmuOvlRenderBackend* r = ovl->render;
    int row_pad = S(8);  // SETTINGS_ROW_PADDING

    if (selected) {
        if (value) {
            // 2-layer: full-width COLOR2 + label-width COLOR1
            draw_rounded_rect(r, x, y, w, h, EMU_OVL_COLOR_ROW_BG);

            int lw = r->text_width(label, EMU_OVL_FONT_SMALL);
            int label_pill_w = lw + row_pad * 2;
            draw_rounded_rect(r, x, y, label_pill_w, h, EMU_OVL_COLOR_ROW_SEL);
        } else {
            // Single label rect
            int lw = r->text_width(label, EMU_OVL_FONT_SMALL);
            int label_pill_w = lw + row_pad * 2;
            draw_rounded_rect(r, x, y, label_pill_w, h, EMU_OVL_COLOR_ROW_SEL);
        }

        // Label text (white)
        int text_y = y + (h - r->text_height(EMU_OVL_FONT_SMALL)) / 2;
        r->draw_text(label, x + row_pad, text_y, EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_SMALL);

        // Value text (white, right-aligned, with arrows if cycleable)
        if (value) {
            char display[192];
            if (cycleable)
                snprintf(display, sizeof(display), "< %s >", value);
            else
                snprintf(display, sizeof(display), "%s", value);

            int vw = r->text_width(display, EMU_OVL_FONT_TINY);
            int val_x = x + w - row_pad - vw;
            int val_y = y + (h - r->text_height(EMU_OVL_FONT_TINY)) / 2;
            r->draw_text(display, val_x, val_y, EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_TINY);
        }
    } else {
        // Unselected: no background, gray text
        int text_y = y + (h - r->text_height(EMU_OVL_FONT_SMALL)) / 2;
        r->draw_text(label, x + row_pad, text_y, EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_SMALL);

        if (value) {
            int vw = r->text_width(value, EMU_OVL_FONT_TINY);
            int val_x = x + w - row_pad - vw;
            int val_y = y + (h - r->text_height(EMU_OVL_FONT_TINY)) / 2;
            r->draw_text(value, val_x, val_y, EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
        }
    }
}
```

**Step 2: Add new color defines to `emu_overlay_render.h`**

```c
// Settings row colors (matching NextUI theme defaults)
#define EMU_OVL_COLOR_ROW_BG    0xFF002222   // THEME_COLOR2 default (dark cyan)
#define EMU_OVL_COLOR_ROW_SEL   0xFFFFFFFF   // THEME_COLOR1 default (white)
#define EMU_OVL_COLOR_TEXT_SEL  0xFF000000   // THEME_COLOR5 default (black on white pill)
#define EMU_OVL_COLOR_TEXT_NORM 0xFFFFFFFF   // THEME_COLOR4 default (white)
```

Note: The text colors for selected items in the real NextUI are determined by THEME_COLOR5 (text on accent pill). With the default white accent + black text, selected labels appear black on white. We need to handle this.

**Step 3: Rewrite render_main_menu**

```c
static void render_main_menu(EmuOvl* ovl) {
    EmuOvlRenderBackend* r = ovl->render;

    draw_menu_bar(ovl, ovl->game_name);

    // Layout: match UI_calcListLayout
    int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;
    int list_y = S(PADDING + PILL_SIZE) + 10;
    int list_h = ovl->screen_h - list_y - bar_h;
    int total_rows = 9; // SETTINGS_ROW_COUNT
    int row_h = list_h / total_rows;
    int content_x = S(PADDING);
    int content_w = ovl->screen_w - S(PADDING) * 2;

    for (int i = 0; i < ovl->main_item_count; i++) {
        int iy = list_y + i * row_h;
        bool sel = (i == ovl->selected);
        draw_settings_row(ovl, content_x, iy, content_w, row_h,
                          ovl->main_items[i].label, NULL, sel, false);
    }

    const char* hints[] = {"B", "BACK", "A", "OK"};
    draw_hint_bar(ovl, hints, 4);
}
```

**Step 4: Rewrite render_section_list**

```c
static void render_section_list(EmuOvl* ovl) {
    EmuOvlRenderBackend* r = ovl->render;

    draw_menu_bar(ovl, "Options");

    int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;
    int list_y = S(PADDING + PILL_SIZE) + 10;
    int list_h = ovl->screen_h - list_y - bar_h;
    int total_rows = 9;
    int row_h = list_h / total_rows;
    int content_x = S(PADDING);
    int content_w = ovl->screen_w - S(PADDING) * 2;

    int count = ovl->config->section_count;
    for (int i = 0; i < count; i++) {
        int iy = list_y + i * row_h;
        bool sel = (i == ovl->selected);
        draw_settings_row(ovl, content_x, iy, content_w, row_h,
                          ovl->config->sections[i].name, NULL, sel, false);
    }

    const char* hints[] = {"B", "BACK", "A", "OPEN"};
    draw_hint_bar(ovl, hints, 4);
}
```

**Step 5: Rewrite render_section_items**

```c
static void render_section_items(EmuOvl* ovl) {
    EmuOvlRenderBackend* r = ovl->render;
    EmuOvlSection* sec = &ovl->config->sections[ovl->current_section];

    draw_menu_bar(ovl, sec->name);

    int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;
    int list_y = S(PADDING + PILL_SIZE) + 10;
    int list_h = ovl->screen_h - list_y - bar_h;
    int total_rows = 9;
    int row_h = list_h / total_rows;
    int items_per_page = total_rows - 1; // last row for description
    int content_x = S(PADDING);
    int content_w = ovl->screen_w - S(PADDING) * 2;

    // Scroll
    ensure_visible(ovl);

    int vis_count = items_per_page;
    if (vis_count > sec->item_count) vis_count = sec->item_count;

    for (int vi = 0; vi < vis_count; vi++) {
        int idx = ovl->scroll_offset + vi;
        if (idx >= sec->item_count) break;

        EmuOvlItem* item = &sec->items[idx];
        int iy = list_y + vi * row_h;
        bool sel = (idx == ovl->selected);

        char val_buf[64];
        const char* val_str = get_item_display_value(item, val_buf, sizeof(val_buf));

        draw_settings_row(ovl, content_x, iy, content_w, row_h,
                          item->label, val_str, sel, true);
    }

    // Scroll indicators (simple arrows)
    if (ovl->scroll_offset > 0) {
        int cx = ovl->screen_w / 2;
        int uy = list_y - S(6);
        r->draw_text("^", cx - r->text_width("^", EMU_OVL_FONT_TINY) / 2,
                     uy, EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
    }
    if (ovl->scroll_offset + items_per_page < sec->item_count) {
        int cx = ovl->screen_w / 2;
        int dy = list_y + items_per_page * row_h;
        r->draw_text("v", cx - r->text_width("v", EMU_OVL_FONT_TINY) / 2,
                     dy, EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
    }

    // Description for selected item (last row)
    if (ovl->selected < sec->item_count) {
        EmuOvlItem* sel_item = &sec->items[ovl->selected];
        if (sel_item->description[0] != '\0') {
            int desc_y = list_y + items_per_page * row_h;
            int desc_cy = desc_y + row_h / 2 - r->text_height(EMU_OVL_FONT_TINY) / 2;
            int tw = r->text_width(sel_item->description, EMU_OVL_FONT_TINY);
            r->draw_text(sel_item->description,
                         (ovl->screen_w - tw) / 2, desc_cy,
                         EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
        }
    }

    const char* hints[] = {"LEFT/RIGHT", "CHANGE", "B", "BACK"};
    draw_hint_bar(ovl, hints, 4);
}
```

**Step 6: Update items_per_page calculation in `emu_ovl_init`**

The `items_per_page` in `emu_ovl_init()` should use the new layout math:
```c
// Match settings page layout
int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;
int list_y = S(PADDING + PILL_SIZE) + 10;
int list_h = screen_h - list_y - bar_h;
int total_rows = 9;
int row_h = list_h / total_rows;
ovl->items_per_page = total_rows - 1; // 8 visible items, last row for description
```

**Step 7: Commit**

```bash
git add workspace/all/common/emu_overlay.c workspace/all/common/emu_overlay_render.h
git commit -m "feat(overlay): rewrite rendering for settings-page style layout"
```

---

### Task 4: Update CMakeLists.txt for SDL_ttf

**Files:**
- Modify: `workspace/tg5040/other/mupen64plus/GLideN64-standalone/src/CMakeLists.txt`
- Modify: `workspace/tg5040/other/mupen64plus/GLideN64-standalone/toolchain-aarch64.cmake`

**Step 1: Add SDL_ttf to toolchain**

In `toolchain-aarch64.cmake`, add SDL_ttf paths:
```cmake
# SDL_ttf for overlay menu fonts
set(SDL_TTF_INCLUDE_DIRS ${LIBC_ROOT}/usr/include/SDL2)
set(SDL_TTF_LIBRARIES ${LIBC_ROOT}/usr/lib/libSDL2_ttf.so)
```

**Step 2: Add emu_overlay_sdl.c to sources in CMakeLists.txt**

After the existing overlay sources block (line 155-159):
```cmake
list(APPEND GLideN64_SOURCES
  ${OVERLAY_COMMON_DIR}/emu_overlay.c
  ${OVERLAY_COMMON_DIR}/emu_overlay_cfg.c
  ${OVERLAY_COMMON_DIR}/emu_overlay_sdl.c
  ${OVERLAY_COMMON_DIR}/cjson/cJSON.c
)
```

**Step 3: Add SDL2 and SDL_ttf includes and linking**

Add include_directories for SDL2:
```cmake
# SDL2 and SDL_ttf for overlay menu rendering
include_directories(${SDL_TTF_INCLUDE_DIRS})
```

Add SDL_ttf to all target_link_libraries calls (both Debug and Release):
```cmake
target_link_libraries(${GLideN64_DLL_NAME} ... -lSDL2 -lSDL2_ttf)
```

**Step 4: Commit**

```bash
git add workspace/tg5040/other/mupen64plus/GLideN64-standalone/src/CMakeLists.txt
git add workspace/tg5040/other/mupen64plus/GLideN64-standalone/toolchain-aarch64.cmake
git commit -m "build(overlay): add SDL_ttf linking for overlay fonts"
```

---

### Task 5: Switch DisplayWindow.cpp to SDL backend

**Files:**
- Modify: `workspace/tg5040/other/mupen64plus/GLideN64-standalone/src/DisplayWindow.cpp`

**Step 1: Add SDL backend include**

Change:
```cpp
extern "C" {
#include "emu_overlay.h"
#include "emu_overlay_cfg.h"
#include "overlay/OverlayGL.h"
}
```
To:
```cpp
extern "C" {
#include "emu_overlay.h"
#include "emu_overlay_cfg.h"
#include "emu_overlay_sdl.h"
}
```

**Step 2: Switch backend in `overlay_ensure_init`**

Change:
```cpp
EmuOvlRenderBackend* render = overlay_gl_get_backend();
```
To:
```cpp
EmuOvlRenderBackend* render = overlay_sdl_get_backend();
```

**Step 3: Commit**

```bash
git add workspace/tg5040/other/mupen64plus/GLideN64-standalone/src/DisplayWindow.cpp
git commit -m "feat(overlay): switch to SDL render backend"
```

---

### Task 6: Update launch.sh with font path

**Files:**
- Modify: `skeleton/EXTRAS/Emus/tg5040/N64.pak/launch.sh`
- Modify: `skeleton/EXTRAS/Emus/tg5050/N64.pak/launch.sh`

**Step 1: Add font env var**

After the existing EMU_OVERLAY env vars, add:
```bash
# Font for overlay menu (TTF from NextUI system resources)
FONT_FILE=$(ls "$SDCARD_PATH/.system/res/"*.ttf 2>/dev/null | head -1)
export EMU_OVERLAY_FONT="${FONT_FILE:-$SDCARD_PATH/.system/res/font.ttf}"
```

**Step 2: Commit**

```bash
git add skeleton/EXTRAS/Emus/tg5040/N64.pak/launch.sh
git add skeleton/EXTRAS/Emus/tg5050/N64.pak/launch.sh
git commit -m "feat(overlay): pass font path to SDL overlay backend"
```

---

### Task 7: Duplicate changes for TG5050

**Files:**
- Verify: `workspace/tg5050/other/mupen64plus/` has the same GLideN64 source (typically symlinked or copied from tg5040)

**Step 1: Check if tg5050 shares or copies the GLideN64 source**

Check if `workspace/tg5050/other/mupen64plus/GLideN64-standalone/` exists and if it's the same as tg5040's copy. If it's separate, apply the same CMakeLists.txt and toolchain changes.

**Step 2: Commit if needed**

---

### Task 8: Build verification

**Step 1: Build GLideN64 for TG5040**

```bash
docker run --rm -v $(pwd)/workspace:/root/workspace ghcr.io/loveretro/tg5040-toolchain:latest /bin/bash -c '
source ~/.bashrc
cd /root/workspace/tg5040/other/mupen64plus/GLideN64-standalone/src
rm -rf build && mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../../toolchain-aarch64.cmake \
  -DMUPENPLUSAPI=ON -DEGL=ON -DMESA=ON \
  -DNO_OSD=ON -DNEON_OPT=ON -DCRC_ARMV8=ON ..
make -j$(nproc) mupen64plus-video-GLideN64
'
```

Expected: Build succeeds, produces `mupen64plus-video-GLideN64.so`

**Step 2: If build fails, diagnose and fix**

Common issues:
- Missing SDL_ttf headers → update toolchain include paths
- Undefined symbols → check all functions are implemented in emu_overlay_sdl.c
- GL header conflicts → ensure GLES3/gl3.h include order is correct

**Step 3: Deploy and test on device**

Copy the built `.so` to the device, launch an N64 game, press menu button, verify:
- Menu appears with captured game frame (dimmed) as background
- Title bar shows game name
- Settings rows show Continue/Save/Load/Options/Quit
- Navigation works (up/down/A/B)
- Options submenu shows sections with settings-page-style rows
- TTF font renders clearly (much better than bitmap font)

---

## Implementation Notes

### Color Scheme

The default color scheme matches NextUI defaults:
- `EMU_OVL_COLOR_ROW_BG` (0xFF002222) — dark cyan, the row background when selected
- `EMU_OVL_COLOR_ROW_SEL` (0xFFFFFFFF) — white, the label pill when selected
- Selected text on white pill should be black (0xFF000000)
- Unselected text is gray (0xFF999999)

### Scale Factor

The existing `ovl_scale` variable (2 for 720p+, 3 for <720p) is used for all layout calculations via `S(x)`. Font sizes are `FONT_*_SIZE * s_scale`.

### Fallback

If the SDL backend fails to initialize (missing font file, TTF_Init failure), the system should fall back gracefully. The `init()` function returns -1 on failure, which causes `overlay_ensure_init()` to retry next frame.

### Files Changed Summary

| File | Action |
|------|--------|
| `workspace/all/common/emu_overlay_sdl.h` | CREATE |
| `workspace/all/common/emu_overlay_sdl.c` | CREATE |
| `workspace/all/common/emu_overlay.c` | MODIFY — rewrite render functions |
| `workspace/all/common/emu_overlay_render.h` | MODIFY — add color defines |
| `workspace/tg5040/other/mupen64plus/GLideN64-standalone/src/CMakeLists.txt` | MODIFY |
| `workspace/tg5040/other/mupen64plus/GLideN64-standalone/toolchain-aarch64.cmake` | MODIFY |
| `workspace/tg5040/other/mupen64plus/GLideN64-standalone/src/DisplayWindow.cpp` | MODIFY |
| `skeleton/EXTRAS/Emus/tg5040/N64.pak/launch.sh` | MODIFY |
| `skeleton/EXTRAS/Emus/tg5050/N64.pak/launch.sh` | MODIFY |
