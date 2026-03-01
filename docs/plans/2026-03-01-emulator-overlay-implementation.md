# Emulator Overlay Menu — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a reusable in-game menu overlay for standalone emulators, starting with mupen64plus N64 / GLideN64.

**Architecture:** Shared menu logic library in `workspace/all/common/` with an abstract rendering backend API. The first backend is OpenGL ES, integrated into GLideN64's `swapBuffers()` as a blocking menu loop. Input is polled via SDL directly inside the menu loop — no custom input plugin needed. Settings are described in a JSON config file per emulator.

**Tech Stack:** C (menu library), C++ (GLideN64 integration), cJSON (JSON parsing), FreeType2 (text via GLideN64's TextDrawer), OpenGL ES 3.2, SDL2 (input polling)

**Design doc:** `docs/plans/2026-03-01-emulator-overlay-menu-design.md`

---

## Key Architecture Decision: Blocking Menu Loop

Instead of a custom input plugin + IPC, the overlay uses a **blocking loop inside `DisplayWindow::swapBuffers()`**:

1. GLideN64 polls SDL for Menu button (button 8) each frame in `swapBuffers()`
2. When pressed: captures current framebuffer as texture, enters blocking menu loop
3. Menu loop: polls SDL input → updates menu state → renders overlay → swaps buffers
4. Emulation is naturally paused (thread is blocked in swapBuffers)
5. On menu close: returns from loop, emulation resumes

This eliminates the need for CoreDoCommand(M64CMD_PAUSE), a custom input plugin, or any IPC. The game frame is captured once and drawn dimmed behind the menu.

## Key File Paths

```
# Shared menu library (reusable)
workspace/all/common/emu_overlay.h          — public API
workspace/all/common/emu_overlay.c          — menu state machine + navigation
workspace/all/common/emu_overlay_cfg.h      — config types + parser API
workspace/all/common/emu_overlay_cfg.c      — JSON parsing + INI read/write
workspace/all/common/emu_overlay_render.h   — abstract rendering backend
workspace/all/common/cjson/cJSON.h          — vendored JSON parser
workspace/all/common/cjson/cJSON.c

# GLideN64 integration (OpenGL ES backend + hook)
workspace/tg5040/cores/src/GLideN64-standalone/src/overlay/OverlayGL.h
workspace/tg5040/cores/src/GLideN64-standalone/src/overlay/OverlayGL.cpp

# Modified GLideN64 files
workspace/tg5040/cores/src/GLideN64-standalone/src/DisplayWindow.h      — add overlay member
workspace/tg5040/cores/src/GLideN64-standalone/src/DisplayWindow.cpp     — hook swapBuffers
workspace/tg5040/cores/src/GLideN64-standalone/src/CMakeLists.txt        — add overlay sources
workspace/tg5040/cores/src/GLideN64-standalone/src/mupenplus/MupenPlusAPIImpl.cpp — load CoreDoCommand
workspace/tg5040/cores/src/GLideN64-standalone/src/mupenplus/GLideN64_mupenplus.h — declare CoreDoCommand

# Config
skeleton/EXTRAS/Emus/shared/mupen64plus/overlay_settings.json
```

---

### Task 1: Vendor cJSON

**Files:**
- Create: `workspace/all/common/cjson/cJSON.h`
- Create: `workspace/all/common/cjson/cJSON.c`

**Step 1: Download cJSON (single-file JSON parser)**

```bash
cd workspace/all/common
mkdir -p cjson
curl -L "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h" -o cjson/cJSON.h
curl -L "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c" -o cjson/cJSON.c
```

**Step 2: Verify files exist and compile**

```bash
ls -la workspace/all/common/cjson/
# Expected: cJSON.h cJSON.c
head -5 workspace/all/common/cjson/cJSON.h
# Expected: copyright header with "cJSON" mention
```

**Step 3: Commit**

```bash
git add workspace/all/common/cjson/
git commit -m "vendor: add cJSON 1.7.18 for overlay config parsing"
```

---

### Task 2: Create emu_overlay_cfg — Config Types & Parser

**Files:**
- Create: `workspace/all/common/emu_overlay_cfg.h`
- Create: `workspace/all/common/emu_overlay_cfg.c`

**Step 1: Write the config header**

`emu_overlay_cfg.h` defines data structures for the parsed JSON config and API for reading/writing emulator settings:

```c
#ifndef EMU_OVERLAY_CFG_H
#define EMU_OVERLAY_CFG_H

#include <stdbool.h>

#define EMU_OVL_MAX_SECTIONS    16
#define EMU_OVL_MAX_ITEMS       32
#define EMU_OVL_MAX_VALUES      16
#define EMU_OVL_MAX_STR         128

// Setting item types
typedef enum {
    EMU_OVL_TYPE_BOOL,     // true/false toggle
    EMU_OVL_TYPE_CYCLE,    // cycle through discrete values
    EMU_OVL_TYPE_INT       // numeric with min/max/step
} EmuOvlItemType;

// A single setting item
typedef struct {
    char key[EMU_OVL_MAX_STR];          // config key (e.g. "UseNativeResolutionFactor")
    char label[EMU_OVL_MAX_STR];        // display label (e.g. "Resolution")
    char description[EMU_OVL_MAX_STR];  // description text
    EmuOvlItemType type;

    // For TYPE_CYCLE
    int values[EMU_OVL_MAX_VALUES];     // possible values
    char labels[EMU_OVL_MAX_VALUES][EMU_OVL_MAX_STR]; // display labels for each value
    int value_count;

    // For TYPE_INT
    int int_min, int_max, int_step;

    // Common
    int default_value;                  // default value (int or bool as 0/1)
    int current_value;                  // current runtime value
    int staged_value;                   // staged value (applied on close)
    bool dirty;                         // true if staged != current
} EmuOvlItem;

// A section of settings
typedef struct {
    char name[EMU_OVL_MAX_STR];         // section name (e.g. "Rendering")
    EmuOvlItem items[EMU_OVL_MAX_ITEMS];
    int item_count;
} EmuOvlSection;

// Top-level config
typedef struct {
    char emulator[EMU_OVL_MAX_STR];     // e.g. "mupen64plus-GLideN64"
    char config_file[EMU_OVL_MAX_STR];  // e.g. "mupen64plus.cfg"
    char config_section[EMU_OVL_MAX_STR]; // e.g. "Video-GLideN64"
    bool save_state;                    // emulator supports save state
    bool load_state;                    // emulator supports load state
    EmuOvlSection sections[EMU_OVL_MAX_SECTIONS];
    int section_count;
} EmuOvlConfig;

// Parse overlay_settings.json into config struct.
// Returns 0 on success, -1 on error.
int emu_ovl_cfg_load(EmuOvlConfig* cfg, const char* json_path);

// Free any dynamic allocations (currently none, but future-proofing).
void emu_ovl_cfg_free(EmuOvlConfig* cfg);

// Read current values from emulator's INI config file.
// Reads [config_section] from config_file, populates current_value for each item.
// Items not found in INI get their default_value.
int emu_ovl_cfg_read_ini(EmuOvlConfig* cfg, const char* ini_path);

// Write staged (dirty) values back to emulator's INI config file.
// Only modifies keys that changed. Preserves all other content.
int emu_ovl_cfg_write_ini(EmuOvlConfig* cfg, const char* ini_path);

// Reset all staged values to current values (discard changes).
void emu_ovl_cfg_reset_staged(EmuOvlConfig* cfg);

// Copy staged values to current values and clear dirty flags.
void emu_ovl_cfg_apply_staged(EmuOvlConfig* cfg);

// Check if any item has been modified.
bool emu_ovl_cfg_has_changes(EmuOvlConfig* cfg);

#endif // EMU_OVERLAY_CFG_H
```

**Step 2: Write the config implementation**

`emu_overlay_cfg.c` implements JSON parsing with cJSON and INI read/write:

```c
#include "emu_overlay_cfg.h"
#include "cjson/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- JSON parsing ---

static int parse_item(cJSON* jitem, EmuOvlItem* item) {
    cJSON* val;

    val = cJSON_GetObjectItem(jitem, "key");
    if (!val || !cJSON_IsString(val)) return -1;
    snprintf(item->key, EMU_OVL_MAX_STR, "%s", val->valuestring);

    val = cJSON_GetObjectItem(jitem, "label");
    if (val && cJSON_IsString(val))
        snprintf(item->label, EMU_OVL_MAX_STR, "%s", val->valuestring);

    val = cJSON_GetObjectItem(jitem, "description");
    if (val && cJSON_IsString(val))
        snprintf(item->description, EMU_OVL_MAX_STR, "%s", val->valuestring);

    val = cJSON_GetObjectItem(jitem, "type");
    if (!val || !cJSON_IsString(val)) return -1;

    if (strcmp(val->valuestring, "bool") == 0) {
        item->type = EMU_OVL_TYPE_BOOL;
    } else if (strcmp(val->valuestring, "cycle") == 0) {
        item->type = EMU_OVL_TYPE_CYCLE;

        cJSON* values = cJSON_GetObjectItem(jitem, "values");
        cJSON* labels = cJSON_GetObjectItem(jitem, "labels");
        if (!values || !cJSON_IsArray(values)) return -1;

        item->value_count = 0;
        int i = 0;
        cJSON* v;
        cJSON_ArrayForEach(v, values) {
            if (i >= EMU_OVL_MAX_VALUES) break;
            item->values[i] = (int)v->valuedouble;
            i++;
        }
        item->value_count = i;

        if (labels && cJSON_IsArray(labels)) {
            i = 0;
            cJSON_ArrayForEach(v, labels) {
                if (i >= item->value_count) break;
                if (cJSON_IsString(v))
                    snprintf(item->labels[i], EMU_OVL_MAX_STR, "%s", v->valuestring);
                i++;
            }
        }
    } else if (strcmp(val->valuestring, "int") == 0) {
        item->type = EMU_OVL_TYPE_INT;
        val = cJSON_GetObjectItem(jitem, "min");
        item->int_min = val ? (int)val->valuedouble : 0;
        val = cJSON_GetObjectItem(jitem, "max");
        item->int_max = val ? (int)val->valuedouble : 100;
        val = cJSON_GetObjectItem(jitem, "step");
        item->int_step = val ? (int)val->valuedouble : 1;
    } else {
        return -1;
    }

    val = cJSON_GetObjectItem(jitem, "default");
    if (val) {
        if (cJSON_IsBool(val))
            item->default_value = cJSON_IsTrue(val) ? 1 : 0;
        else
            item->default_value = (int)val->valuedouble;
    }

    item->current_value = item->default_value;
    item->staged_value = item->default_value;
    item->dirty = false;
    return 0;
}

int emu_ovl_cfg_load(EmuOvlConfig* cfg, const char* json_path) {
    memset(cfg, 0, sizeof(EmuOvlConfig));

    FILE* f = fopen(json_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;

    cJSON* val;
    val = cJSON_GetObjectItem(root, "emulator");
    if (val && cJSON_IsString(val))
        snprintf(cfg->emulator, EMU_OVL_MAX_STR, "%s", val->valuestring);

    val = cJSON_GetObjectItem(root, "config_file");
    if (val && cJSON_IsString(val))
        snprintf(cfg->config_file, EMU_OVL_MAX_STR, "%s", val->valuestring);

    val = cJSON_GetObjectItem(root, "config_section");
    if (val && cJSON_IsString(val))
        snprintf(cfg->config_section, EMU_OVL_MAX_STR, "%s", val->valuestring);

    val = cJSON_GetObjectItem(root, "save_state");
    cfg->save_state = val && cJSON_IsTrue(val);

    val = cJSON_GetObjectItem(root, "load_state");
    cfg->load_state = val && cJSON_IsTrue(val);

    cJSON* sections = cJSON_GetObjectItem(root, "sections");
    if (sections && cJSON_IsArray(sections)) {
        cJSON* jsec;
        cJSON_ArrayForEach(jsec, sections) {
            if (cfg->section_count >= EMU_OVL_MAX_SECTIONS) break;
            EmuOvlSection* sec = &cfg->sections[cfg->section_count];

            val = cJSON_GetObjectItem(jsec, "name");
            if (val && cJSON_IsString(val))
                snprintf(sec->name, EMU_OVL_MAX_STR, "%s", val->valuestring);

            cJSON* items = cJSON_GetObjectItem(jsec, "items");
            if (items && cJSON_IsArray(items)) {
                cJSON* jitem;
                cJSON_ArrayForEach(jitem, items) {
                    if (sec->item_count >= EMU_OVL_MAX_ITEMS) break;
                    if (parse_item(jitem, &sec->items[sec->item_count]) == 0)
                        sec->item_count++;
                }
            }
            cfg->section_count++;
        }
    }

    cJSON_Delete(root);
    return 0;
}

void emu_ovl_cfg_free(EmuOvlConfig* cfg) {
    // Currently all static, nothing to free
    (void)cfg;
}

// --- INI read/write ---

// Find value for key in [section] of INI file content.
// Returns pointer into buf (null-terminated value), or NULL if not found.
static const char* ini_find_value(const char* buf, const char* section, const char* key) {
    // Find [section]
    char sec_header[EMU_OVL_MAX_STR + 4];
    snprintf(sec_header, sizeof(sec_header), "[%s]", section);

    const char* sec_start = strstr(buf, sec_header);
    if (!sec_start) return NULL;
    sec_start += strlen(sec_header);

    // Find next section or end
    const char* sec_end = strchr(sec_start, '[');
    // Search backwards to make sure it's a section header (starts at line beginning)
    while (sec_end && sec_end > buf && *(sec_end - 1) != '\n')
        sec_end = strchr(sec_end + 1, '[');
    if (!sec_end) sec_end = buf + strlen(buf);

    // Search for key = value within section bounds
    const char* p = sec_start;
    size_t key_len = strlen(key);
    while (p < sec_end) {
        // Skip whitespace
        while (p < sec_end && (*p == ' ' || *p == '\t')) p++;
        // Check if line starts with key
        if (strncmp(p, key, key_len) == 0) {
            const char* after_key = p + key_len;
            // Skip whitespace after key
            while (after_key < sec_end && (*after_key == ' ' || *after_key == '\t')) after_key++;
            if (*after_key == '=') {
                after_key++;
                while (after_key < sec_end && (*after_key == ' ' || *after_key == '\t')) after_key++;
                // Return start of value (caller must handle line ending)
                return after_key;
            }
        }
        // Skip to next line
        while (p < sec_end && *p != '\n') p++;
        if (p < sec_end) p++;
    }
    return NULL;
}

static int parse_ini_int(const char* val_str) {
    // Handle True/False for bool values stored as strings
    if (strncasecmp(val_str, "True", 4) == 0) return 1;
    if (strncasecmp(val_str, "False", 5) == 0) return 0;
    return atoi(val_str);
}

int emu_ovl_cfg_read_ini(EmuOvlConfig* cfg, const char* ini_path) {
    FILE* f = fopen(ini_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    for (int s = 0; s < cfg->section_count; s++) {
        EmuOvlSection* sec = &cfg->sections[s];
        for (int i = 0; i < sec->item_count; i++) {
            EmuOvlItem* item = &sec->items[i];
            const char* val = ini_find_value(buf, cfg->config_section, item->key);
            if (val) {
                item->current_value = parse_ini_int(val);
            } else {
                item->current_value = item->default_value;
            }
            item->staged_value = item->current_value;
            item->dirty = false;
        }
    }

    free(buf);
    return 0;
}

int emu_ovl_cfg_write_ini(EmuOvlConfig* cfg, const char* ini_path) {
    // Read entire file
    FILE* f = fopen(ini_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    // Build output with replacements
    // For simplicity: read line by line, replace matching key = value lines
    char* output = (char*)malloc(len + 4096); // extra space for growth
    if (!output) { free(buf); return -1; }
    output[0] = '\0';

    char current_section[EMU_OVL_MAX_STR] = "";
    char* line = buf;

    while (line && *line) {
        char* newline = strchr(line, '\n');
        int line_len = newline ? (int)(newline - line) : (int)strlen(line);
        char line_buf[1024];
        if (line_len >= (int)sizeof(line_buf)) line_len = sizeof(line_buf) - 1;
        strncpy(line_buf, line, line_len);
        line_buf[line_len] = '\0';

        // Check if this is a section header
        if (line_buf[0] == '[') {
            char* end = strchr(line_buf, ']');
            if (end) {
                int sec_len = (int)(end - line_buf - 1);
                if (sec_len < EMU_OVL_MAX_STR) {
                    strncpy(current_section, line_buf + 1, sec_len);
                    current_section[sec_len] = '\0';
                }
            }
        }

        // Check if this line is a key we need to replace
        bool replaced = false;
        if (strcmp(current_section, cfg->config_section) == 0) {
            for (int s = 0; s < cfg->section_count && !replaced; s++) {
                EmuOvlSection* sec = &cfg->sections[s];
                for (int i = 0; i < sec->item_count && !replaced; i++) {
                    EmuOvlItem* item = &sec->items[i];
                    if (!item->dirty) continue;

                    size_t key_len = strlen(item->key);
                    const char* p = line_buf;
                    while (*p == ' ' || *p == '\t') p++;
                    if (strncmp(p, item->key, key_len) == 0) {
                        const char* after = p + key_len;
                        while (*after == ' ' || *after == '\t') after++;
                        if (*after == '=') {
                            // Replace this line
                            char new_line[512];
                            if (item->type == EMU_OVL_TYPE_BOOL) {
                                snprintf(new_line, sizeof(new_line), "%s = %s",
                                         item->key, item->staged_value ? "True" : "False");
                            } else {
                                snprintf(new_line, sizeof(new_line), "%s = %d",
                                         item->key, item->staged_value);
                            }
                            strcat(output, new_line);
                            strcat(output, "\n");
                            replaced = true;
                        }
                    }
                }
            }
        }

        if (!replaced) {
            strncat(output, line, line_len);
            strcat(output, "\n");
        }

        line = newline ? newline + 1 : NULL;
    }

    // Write output
    f = fopen(ini_path, "w");
    if (!f) { free(buf); free(output); return -1; }
    fputs(output, f);
    fclose(f);

    free(buf);
    free(output);
    return 0;
}

void emu_ovl_cfg_reset_staged(EmuOvlConfig* cfg) {
    for (int s = 0; s < cfg->section_count; s++) {
        EmuOvlSection* sec = &cfg->sections[s];
        for (int i = 0; i < sec->item_count; i++) {
            sec->items[i].staged_value = sec->items[i].current_value;
            sec->items[i].dirty = false;
        }
    }
}

void emu_ovl_cfg_apply_staged(EmuOvlConfig* cfg) {
    for (int s = 0; s < cfg->section_count; s++) {
        EmuOvlSection* sec = &cfg->sections[s];
        for (int i = 0; i < sec->item_count; i++) {
            sec->items[i].current_value = sec->items[i].staged_value;
            sec->items[i].dirty = false;
        }
    }
}

bool emu_ovl_cfg_has_changes(EmuOvlConfig* cfg) {
    for (int s = 0; s < cfg->section_count; s++) {
        EmuOvlSection* sec = &cfg->sections[s];
        for (int i = 0; i < sec->item_count; i++) {
            if (sec->items[i].dirty) return true;
        }
    }
    return false;
}
```

**Step 3: Commit**

```bash
git add workspace/all/common/emu_overlay_cfg.h workspace/all/common/emu_overlay_cfg.c
git commit -m "feat: add overlay config parser (JSON + INI read/write)"
```

---

### Task 3: Create emu_overlay — Menu State Machine & Navigation

**Files:**
- Create: `workspace/all/common/emu_overlay.h`
- Create: `workspace/all/common/emu_overlay.c`

**Step 1: Write the overlay public API header**

`emu_overlay.h` defines the menu state machine and the abstract rendering backend:

```c
#ifndef EMU_OVERLAY_H
#define EMU_OVERLAY_H

#include <stdbool.h>
#include "emu_overlay_cfg.h"
#include "emu_overlay_render.h"

// Menu states
typedef enum {
    EMU_OVL_STATE_CLOSED,
    EMU_OVL_STATE_MAIN_MENU,
    EMU_OVL_STATE_SECTION_LIST,
    EMU_OVL_STATE_SECTION_ITEMS,
    EMU_OVL_STATE_SAVE_SELECT,
    EMU_OVL_STATE_LOAD_SELECT,
} EmuOvlState;

// Actions returned by the menu loop
typedef enum {
    EMU_OVL_ACTION_NONE,
    EMU_OVL_ACTION_CONTINUE,     // resume game
    EMU_OVL_ACTION_SAVE_STATE,   // save state (slot in action_param)
    EMU_OVL_ACTION_LOAD_STATE,   // load state (slot in action_param)
    EMU_OVL_ACTION_QUIT,         // exit emulator
} EmuOvlAction;

// Input events
typedef struct {
    bool up, down, left, right;
    bool a, b;
    bool l1, r1;
    bool menu;     // menu button (toggle/close)
} EmuOvlInput;

// Main menu items (dynamically built based on config)
#define EMU_OVL_MAIN_MAX 5

// Overlay instance
typedef struct {
    EmuOvlConfig* config;
    EmuOvlRenderBackend* render;

    EmuOvlState state;
    int selected;               // selected index in current list
    int scroll_offset;          // scroll offset for paginated lists
    int items_per_page;         // items visible per page

    // Main menu
    const char* main_items[EMU_OVL_MAIN_MAX];
    EmuOvlAction main_actions[EMU_OVL_MAIN_MAX];
    int main_item_count;

    // Section items view
    int current_section;        // index into config->sections

    // Save/Load
    int save_slot;              // currently selected slot (0-7)

    // Action output
    EmuOvlAction action;
    int action_param;           // e.g. save slot number

    // Display info
    char game_name[256];
    int screen_w, screen_h;
} EmuOvl;

// Initialize overlay instance.
void emu_ovl_init(EmuOvl* ovl, EmuOvlConfig* config, EmuOvlRenderBackend* render,
                  const char* game_name, int screen_w, int screen_h);

// Process one frame of input. Returns true if overlay is still active.
bool emu_ovl_update(EmuOvl* ovl, const EmuOvlInput* input);

// Render current overlay state.
void emu_ovl_render(EmuOvl* ovl);

// Get the resulting action after overlay closes.
EmuOvlAction emu_ovl_get_action(EmuOvl* ovl);
int emu_ovl_get_action_param(EmuOvl* ovl);

// Open the overlay (set to main menu state).
void emu_ovl_open(EmuOvl* ovl);

// Check if overlay is active.
bool emu_ovl_is_active(EmuOvl* ovl);

#endif // EMU_OVERLAY_H
```

**Step 2: Write the rendering backend interface**

`emu_overlay_render.h`:

```c
#ifndef EMU_OVERLAY_RENDER_H
#define EMU_OVERLAY_RENDER_H

#include <stdbool.h>
#include <stdint.h>

// Font IDs
#define EMU_OVL_FONT_LARGE  0
#define EMU_OVL_FONT_SMALL  1
#define EMU_OVL_FONT_TINY   2

// Pill styles
#define EMU_OVL_PILL_LIGHT  0
#define EMU_OVL_PILL_DARK   1

// Colors (ARGB)
#define EMU_OVL_COLOR_WHITE     0xFFFFFFFF
#define EMU_OVL_COLOR_GRAY      0xFF999999
#define EMU_OVL_COLOR_BLACK     0xFF000000
#define EMU_OVL_COLOR_ACCENT    0xFF00D4AA  // teal accent (customizable)
#define EMU_OVL_COLOR_BG_DIM    0x66000000  // 40% black
#define EMU_OVL_COLOR_BAR_BG    0xB2000000  // 70% black
#define EMU_OVL_COLOR_PILL_DARK 0x80000000  // 50% black
#define EMU_OVL_COLOR_PILL_LIGHT 0x40FFFFFF // 25% white
#define EMU_OVL_COLOR_SELECTED_BG  0x40FFFFFF  // settings row background
#define EMU_OVL_COLOR_LABEL_BG     0x60FFFFFF  // settings label pill

// Abstract rendering backend
typedef struct EmuOvlRenderBackend {
    // Lifecycle
    int  (*init)(int screen_w, int screen_h);
    void (*destroy)(void);

    // Primitives
    void (*draw_rect)(int x, int y, int w, int h, uint32_t color);
    void (*draw_text)(const char* text, int x, int y, uint32_t color, int font_id);

    // Measurements
    int  (*text_width)(const char* text, int font_id);
    int  (*text_height)(int font_id);

    // Frame
    void (*begin_frame)(void);   // save graphics state
    void (*end_frame)(void);     // restore state, swap buffers

    // Captured game frame (dimmed background)
    void (*capture_frame)(void);
    void (*draw_captured_frame)(float dim);
} EmuOvlRenderBackend;

#endif // EMU_OVERLAY_RENDER_H
```

**Step 3: Write the overlay state machine implementation**

`emu_overlay.c` — this is the core menu logic. Key sections:

```c
#include "emu_overlay.h"
#include <string.h>
#include <stdio.h>

// Layout constants (pre-scaled values, matching NextUI defines.h)
#define OVL_PADDING         10
#define OVL_PILL_SIZE       30
#define OVL_BUTTON_SIZE     16
#define OVL_BUTTON_MARGIN   6
#define OVL_BUTTON_PADDING  10
#define OVL_SETTINGS_ROW_PAD 8
#define OVL_SAVE_SLOT_COUNT 8

// Scaling helper (set per-device)
static int ovl_scale = 2; // default for TG5050 (1280x720), TG5040 uses 3

#define S(x) ((x) * ovl_scale)

static void build_main_menu(EmuOvl* ovl) {
    ovl->main_item_count = 0;

    ovl->main_items[ovl->main_item_count] = "Continue";
    ovl->main_actions[ovl->main_item_count] = EMU_OVL_ACTION_CONTINUE;
    ovl->main_item_count++;

    if (ovl->config->save_state) {
        ovl->main_items[ovl->main_item_count] = "Save";
        ovl->main_actions[ovl->main_item_count] = EMU_OVL_ACTION_SAVE_STATE;
        ovl->main_item_count++;
    }

    if (ovl->config->load_state) {
        ovl->main_items[ovl->main_item_count] = "Load";
        ovl->main_actions[ovl->main_item_count] = EMU_OVL_ACTION_LOAD_STATE;
        ovl->main_item_count++;
    }

    if (ovl->config->section_count > 0) {
        ovl->main_items[ovl->main_item_count] = "Options";
        ovl->main_actions[ovl->main_item_count] = EMU_OVL_ACTION_NONE;
        ovl->main_item_count++;
    }

    ovl->main_items[ovl->main_item_count] = "Quit";
    ovl->main_actions[ovl->main_item_count] = EMU_OVL_ACTION_QUIT;
    ovl->main_item_count++;
}

void emu_ovl_init(EmuOvl* ovl, EmuOvlConfig* config, EmuOvlRenderBackend* render,
                  const char* game_name, int screen_w, int screen_h) {
    memset(ovl, 0, sizeof(EmuOvl));
    ovl->config = config;
    ovl->render = render;
    ovl->screen_w = screen_w;
    ovl->screen_h = screen_h;
    ovl->state = EMU_OVL_STATE_CLOSED;
    ovl->save_slot = 0;

    if (game_name)
        snprintf(ovl->game_name, sizeof(ovl->game_name), "%s", game_name);

    // Determine scale factor from screen height
    if (screen_h >= 720)
        ovl_scale = 2;
    else
        ovl_scale = 3; // TG5040 1024x768 uses scale 3

    ovl->items_per_page = (screen_h / S(OVL_PILL_SIZE)) - 2; // minus title + hint bar
    if (ovl->items_per_page < 4) ovl->items_per_page = 4;

    build_main_menu(ovl);
}

void emu_ovl_open(EmuOvl* ovl) {
    ovl->state = EMU_OVL_STATE_MAIN_MENU;
    ovl->selected = 0;
    ovl->scroll_offset = 0;
    ovl->action = EMU_OVL_ACTION_NONE;
    emu_ovl_cfg_reset_staged(ovl->config);
}

bool emu_ovl_is_active(EmuOvl* ovl) {
    return ovl->state != EMU_OVL_STATE_CLOSED;
}

EmuOvlAction emu_ovl_get_action(EmuOvl* ovl) { return ovl->action; }
int emu_ovl_get_action_param(EmuOvl* ovl) { return ovl->action_param; }

// --- Navigation helpers ---

static void nav_up(int* selected, int count) {
    *selected = (*selected - 1 + count) % count;
}

static void nav_down(int* selected, int count) {
    *selected = (*selected + 1) % count;
}

static void ensure_visible(EmuOvl* ovl, int selected, int count) {
    if (selected < ovl->scroll_offset)
        ovl->scroll_offset = selected;
    if (selected >= ovl->scroll_offset + ovl->items_per_page)
        ovl->scroll_offset = selected - ovl->items_per_page + 1;
}

// --- Cycling settings values ---

static void cycle_item_next(EmuOvlItem* item) {
    if (item->type == EMU_OVL_TYPE_BOOL) {
        item->staged_value = item->staged_value ? 0 : 1;
    } else if (item->type == EMU_OVL_TYPE_CYCLE) {
        int idx = 0;
        for (int i = 0; i < item->value_count; i++) {
            if (item->values[i] == item->staged_value) { idx = i; break; }
        }
        idx = (idx + 1) % item->value_count;
        item->staged_value = item->values[idx];
    } else if (item->type == EMU_OVL_TYPE_INT) {
        item->staged_value += item->int_step;
        if (item->staged_value > item->int_max)
            item->staged_value = item->int_min;
    }
    item->dirty = (item->staged_value != item->current_value);
}

static void cycle_item_prev(EmuOvlItem* item) {
    if (item->type == EMU_OVL_TYPE_BOOL) {
        item->staged_value = item->staged_value ? 0 : 1;
    } else if (item->type == EMU_OVL_TYPE_CYCLE) {
        int idx = 0;
        for (int i = 0; i < item->value_count; i++) {
            if (item->values[i] == item->staged_value) { idx = i; break; }
        }
        idx = (idx - 1 + item->value_count) % item->value_count;
        item->staged_value = item->values[idx];
    } else if (item->type == EMU_OVL_TYPE_INT) {
        item->staged_value -= item->int_step;
        if (item->staged_value < item->int_min)
            item->staged_value = item->int_max;
    }
    item->dirty = (item->staged_value != item->current_value);
}

// --- Update (process input for one frame) ---

static void close_overlay(EmuOvl* ovl, EmuOvlAction action, int param) {
    ovl->action = action;
    ovl->action_param = param;
    ovl->state = EMU_OVL_STATE_CLOSED;
}

bool emu_ovl_update(EmuOvl* ovl, const EmuOvlInput* in) {
    switch (ovl->state) {
    case EMU_OVL_STATE_MAIN_MENU: {
        if (in->up)   nav_up(&ovl->selected, ovl->main_item_count);
        if (in->down) nav_down(&ovl->selected, ovl->main_item_count);

        if (in->b || in->menu) {
            close_overlay(ovl, EMU_OVL_ACTION_CONTINUE, 0);
        } else if (in->a) {
            EmuOvlAction act = ovl->main_actions[ovl->selected];
            const char* label = ovl->main_items[ovl->selected];

            if (act == EMU_OVL_ACTION_CONTINUE || act == EMU_OVL_ACTION_QUIT) {
                close_overlay(ovl, act, 0);
            } else if (act == EMU_OVL_ACTION_SAVE_STATE) {
                ovl->state = EMU_OVL_STATE_SAVE_SELECT;
                ovl->selected = ovl->save_slot;
            } else if (act == EMU_OVL_ACTION_LOAD_STATE) {
                ovl->state = EMU_OVL_STATE_LOAD_SELECT;
                ovl->selected = ovl->save_slot;
            } else if (strcmp(label, "Options") == 0) {
                ovl->state = EMU_OVL_STATE_SECTION_LIST;
                ovl->selected = 0;
                ovl->scroll_offset = 0;
            }
        }
        break;
    }

    case EMU_OVL_STATE_SAVE_SELECT:
    case EMU_OVL_STATE_LOAD_SELECT: {
        if (in->left) {
            ovl->selected = (ovl->selected - 1 + OVL_SAVE_SLOT_COUNT) % OVL_SAVE_SLOT_COUNT;
            ovl->save_slot = ovl->selected;
        }
        if (in->right) {
            ovl->selected = (ovl->selected + 1) % OVL_SAVE_SLOT_COUNT;
            ovl->save_slot = ovl->selected;
        }
        if (in->a) {
            EmuOvlAction act = (ovl->state == EMU_OVL_STATE_SAVE_SELECT)
                               ? EMU_OVL_ACTION_SAVE_STATE : EMU_OVL_ACTION_LOAD_STATE;
            close_overlay(ovl, act, ovl->save_slot);
        }
        if (in->b) {
            ovl->state = EMU_OVL_STATE_MAIN_MENU;
            // Restore selected to the save/load item
            for (int i = 0; i < ovl->main_item_count; i++) {
                EmuOvlAction act = (ovl->state == EMU_OVL_STATE_SAVE_SELECT)
                                   ? EMU_OVL_ACTION_SAVE_STATE : EMU_OVL_ACTION_LOAD_STATE;
                // Actually we just went back, re-find the right item
            }
            ovl->selected = 1; // Save is typically index 1
        }
        break;
    }

    case EMU_OVL_STATE_SECTION_LIST: {
        int count = ovl->config->section_count;
        if (in->up)   nav_up(&ovl->selected, count);
        if (in->down) nav_down(&ovl->selected, count);
        ensure_visible(ovl, ovl->selected, count);

        if (in->a) {
            ovl->current_section = ovl->selected;
            ovl->state = EMU_OVL_STATE_SECTION_ITEMS;
            ovl->selected = 0;
            ovl->scroll_offset = 0;
        }
        if (in->b) {
            ovl->state = EMU_OVL_STATE_MAIN_MENU;
            // Find Options index
            for (int i = 0; i < ovl->main_item_count; i++) {
                if (strcmp(ovl->main_items[i], "Options") == 0) {
                    ovl->selected = i;
                    break;
                }
            }
        }
        break;
    }

    case EMU_OVL_STATE_SECTION_ITEMS: {
        EmuOvlSection* sec = &ovl->config->sections[ovl->current_section];
        int count = sec->item_count;

        if (in->up)   nav_up(&ovl->selected, count);
        if (in->down) nav_down(&ovl->selected, count);
        ensure_visible(ovl, ovl->selected, count);

        if (in->right || in->a) {
            cycle_item_next(&sec->items[ovl->selected]);
        }
        if (in->left) {
            cycle_item_prev(&sec->items[ovl->selected]);
        }

        if (in->b) {
            ovl->state = EMU_OVL_STATE_SECTION_LIST;
            ovl->selected = ovl->current_section;
            ovl->scroll_offset = 0;
            ensure_visible(ovl, ovl->selected, ovl->config->section_count);
        }
        break;
    }

    default:
        break;
    }

    return ovl->state != EMU_OVL_STATE_CLOSED;
}

// --- Rendering ---

static const char* get_item_display_value(EmuOvlItem* item) {
    static char buf[EMU_OVL_MAX_STR];
    if (item->type == EMU_OVL_TYPE_BOOL) {
        return item->staged_value ? "On" : "Off";
    } else if (item->type == EMU_OVL_TYPE_CYCLE) {
        for (int i = 0; i < item->value_count; i++) {
            if (item->values[i] == item->staged_value) {
                if (item->labels[i][0] != '\0')
                    return item->labels[i];
                snprintf(buf, sizeof(buf), "%d", item->staged_value);
                return buf;
            }
        }
        snprintf(buf, sizeof(buf), "%d", item->staged_value);
        return buf;
    } else {
        snprintf(buf, sizeof(buf), "%d", item->staged_value);
        return buf;
    }
}

static void render_title_pill(EmuOvl* ovl, const char* title) {
    EmuOvlRenderBackend* r = ovl->render;
    int pill_h = S(OVL_PILL_SIZE);
    int x = S(OVL_PADDING);
    int y = S(OVL_PADDING);
    int w = ovl->screen_w - S(OVL_PADDING) * 2;

    r->draw_rect(x, y, w, pill_h, EMU_OVL_COLOR_PILL_LIGHT);

    int text_x = x + S(OVL_BUTTON_PADDING);
    int text_y = y + (pill_h - r->text_height(EMU_OVL_FONT_LARGE)) / 2;
    r->draw_text(title, text_x, text_y, EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_LARGE);
}

static void render_button_hints(EmuOvl* ovl, const char* hints) {
    EmuOvlRenderBackend* r = ovl->render;
    int bar_h = S(OVL_BUTTON_SIZE) + S(OVL_BUTTON_MARGIN) * 2;
    int y = ovl->screen_h - bar_h;

    r->draw_rect(0, y, ovl->screen_w, bar_h, EMU_OVL_COLOR_BAR_BG);

    int text_y = y + (bar_h - r->text_height(EMU_OVL_FONT_SMALL)) / 2;
    int text_x = S(OVL_PADDING) + S(OVL_BUTTON_MARGIN);
    r->draw_text(hints, text_x, text_y, EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_SMALL);
}

static void render_main_menu(EmuOvl* ovl) {
    EmuOvlRenderBackend* r = ovl->render;
    int pill_h = S(OVL_PILL_SIZE);
    int total_h = ovl->main_item_count * pill_h;
    int oy = (ovl->screen_h - total_h) / 2;
    int x = S(OVL_PADDING);

    for (int i = 0; i < ovl->main_item_count; i++) {
        int y = oy + i * pill_h;
        int w = ovl->screen_w / 2 - S(OVL_PADDING); // left half

        if (i == ovl->selected) {
            r->draw_rect(x, y, w, pill_h, EMU_OVL_COLOR_PILL_DARK);
            r->draw_text(ovl->main_items[i],
                         x + S(OVL_BUTTON_PADDING),
                         y + (pill_h - r->text_height(EMU_OVL_FONT_LARGE)) / 2,
                         EMU_OVL_COLOR_ACCENT, EMU_OVL_FONT_LARGE);
        } else {
            r->draw_text(ovl->main_items[i],
                         x + S(OVL_BUTTON_PADDING),
                         y + (pill_h - r->text_height(EMU_OVL_FONT_LARGE)) / 2,
                         EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_LARGE);
        }
    }
}

static void render_save_slot_select(EmuOvl* ovl, const char* title) {
    EmuOvlRenderBackend* r = ovl->render;

    // Show slot number centered
    char slot_text[32];
    snprintf(slot_text, sizeof(slot_text), "< Slot %d >", ovl->save_slot + 1);
    int tw = r->text_width(slot_text, EMU_OVL_FONT_LARGE);
    int x = (ovl->screen_w - tw) / 2;
    int y = ovl->screen_h / 2;
    r->draw_text(slot_text, x, y, EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_LARGE);

    // Draw pagination dots
    int dot_y = y + S(OVL_PILL_SIZE);
    int dot_spacing = S(15);
    int total_w = OVL_SAVE_SLOT_COUNT * dot_spacing;
    int dot_x = (ovl->screen_w - total_w) / 2;

    for (int i = 0; i < OVL_SAVE_SLOT_COUNT; i++) {
        uint32_t color = (i == ovl->save_slot) ? EMU_OVL_COLOR_ACCENT : EMU_OVL_COLOR_GRAY;
        r->draw_rect(dot_x + i * dot_spacing, dot_y, S(8), S(8), color);
    }
}

static void render_section_list(EmuOvl* ovl) {
    EmuOvlRenderBackend* r = ovl->render;
    int pill_h = S(OVL_PILL_SIZE);
    int count = ovl->config->section_count;
    int total_h = count * pill_h;
    int oy = (ovl->screen_h - total_h) / 2;
    int x = S(OVL_PADDING);
    int w = ovl->screen_w - S(OVL_PADDING) * 2;

    for (int i = 0; i < count; i++) {
        int y = oy + i * pill_h;
        bool sel = (i == ovl->selected);

        if (sel) {
            r->draw_rect(x, y, w, pill_h, EMU_OVL_COLOR_PILL_DARK);
            r->draw_text(ovl->config->sections[i].name,
                         x + S(OVL_BUTTON_PADDING),
                         y + (pill_h - r->text_height(EMU_OVL_FONT_LARGE)) / 2,
                         EMU_OVL_COLOR_ACCENT, EMU_OVL_FONT_LARGE);
        } else {
            r->draw_text(ovl->config->sections[i].name,
                         x + S(OVL_BUTTON_PADDING),
                         y + (pill_h - r->text_height(EMU_OVL_FONT_LARGE)) / 2,
                         EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_LARGE);
        }
    }
}

static void render_section_items(EmuOvl* ovl) {
    EmuOvlRenderBackend* r = ovl->render;
    EmuOvlSection* sec = &ovl->config->sections[ovl->current_section];

    int item_h = ovl->screen_h / (ovl->items_per_page + 2); // +2 for title + desc
    int list_y = S(OVL_PADDING) + S(OVL_PILL_SIZE) + S(OVL_PADDING); // below title
    int x = S(OVL_PADDING);
    int w = ovl->screen_w - S(OVL_PADDING) * 2;

    int visible_start = ovl->scroll_offset;
    int visible_end = visible_start + ovl->items_per_page;
    if (visible_end > sec->item_count) visible_end = sec->item_count;

    for (int i = visible_start; i < visible_end; i++) {
        EmuOvlItem* item = &sec->items[i];
        int row = i - visible_start;
        int y = list_y + row * item_h;
        bool sel = (i == ovl->selected);

        if (sel) {
            // Full-width background
            r->draw_rect(x, y, w, item_h, EMU_OVL_COLOR_SELECTED_BG);

            // Label pill
            int label_w = r->text_width(item->label, EMU_OVL_FONT_SMALL)
                          + S(OVL_SETTINGS_ROW_PAD) * 2;
            r->draw_rect(x, y, label_w, item_h, EMU_OVL_COLOR_LABEL_BG);

            // Label text
            r->draw_text(item->label,
                         x + S(OVL_SETTINGS_ROW_PAD),
                         y + (item_h - r->text_height(EMU_OVL_FONT_SMALL)) / 2,
                         EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_SMALL);
        } else {
            // Label text (unselected)
            r->draw_text(item->label,
                         x + S(OVL_SETTINGS_ROW_PAD),
                         y + (item_h - r->text_height(EMU_OVL_FONT_SMALL)) / 2,
                         EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_SMALL);
        }

        // Value on right side
        const char* val_str = get_item_display_value(item);
        char display[EMU_OVL_MAX_STR + 8];
        if (sel) {
            snprintf(display, sizeof(display), "< %s >", val_str);
        } else {
            snprintf(display, sizeof(display), "%s", val_str);
        }

        int val_w = r->text_width(display, EMU_OVL_FONT_TINY);
        int val_x = x + w - val_w - S(OVL_SETTINGS_ROW_PAD);
        uint32_t val_color = sel ? EMU_OVL_COLOR_WHITE : EMU_OVL_COLOR_GRAY;
        r->draw_text(display, val_x,
                     y + (item_h - r->text_height(EMU_OVL_FONT_TINY)) / 2,
                     val_color, EMU_OVL_FONT_TINY);
    }

    // Description row
    if (ovl->selected >= 0 && ovl->selected < sec->item_count) {
        const char* desc = sec->items[ovl->selected].description;
        if (desc[0] != '\0') {
            int desc_y = list_y + ovl->items_per_page * item_h;
            int desc_w = r->text_width(desc, EMU_OVL_FONT_TINY);
            int desc_x = (ovl->screen_w - desc_w) / 2;
            r->draw_text(desc, desc_x, desc_y, EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
        }
    }
}

void emu_ovl_render(EmuOvl* ovl) {
    EmuOvlRenderBackend* r = ovl->render;
    r->begin_frame();

    // Draw dimmed game frame
    r->draw_captured_frame(0.4f);

    switch (ovl->state) {
    case EMU_OVL_STATE_MAIN_MENU:
        render_title_pill(ovl, ovl->game_name);
        render_main_menu(ovl);
        render_button_hints(ovl, "B BACK    A OK    MENU CLOSE");
        break;

    case EMU_OVL_STATE_SAVE_SELECT:
        render_title_pill(ovl, "Save State");
        render_save_slot_select(ovl, "Save State");
        render_button_hints(ovl, "LR SLOT    A SAVE    B BACK");
        break;

    case EMU_OVL_STATE_LOAD_SELECT:
        render_title_pill(ovl, "Load State");
        render_save_slot_select(ovl, "Load State");
        render_button_hints(ovl, "LR SLOT    A LOAD    B BACK");
        break;

    case EMU_OVL_STATE_SECTION_LIST:
        render_title_pill(ovl, "Options");
        render_section_list(ovl);
        render_button_hints(ovl, "B BACK    A SELECT");
        break;

    case EMU_OVL_STATE_SECTION_ITEMS:
        render_title_pill(ovl, ovl->config->sections[ovl->current_section].name);
        render_section_items(ovl);
        render_button_hints(ovl, "B BACK    LR CHANGE");
        break;

    default:
        break;
    }

    r->end_frame();
}
```

**Step 4: Commit**

```bash
git add workspace/all/common/emu_overlay.h workspace/all/common/emu_overlay.c workspace/all/common/emu_overlay_render.h
git commit -m "feat: add overlay menu state machine, navigation, and rendering logic"
```

---

### Task 4: Create OverlayGL — OpenGL ES Rendering Backend

**Files:**
- Create: `workspace/tg5040/cores/src/GLideN64-standalone/src/overlay/OverlayGL.h`
- Create: `workspace/tg5040/cores/src/GLideN64-standalone/src/overlay/OverlayGL.cpp`

This backend implements `EmuOvlRenderBackend` using OpenGL ES and GLideN64's existing `TextDrawer`.

**Step 1: Write the GL backend header**

```cpp
#ifndef OVERLAY_GL_H
#define OVERLAY_GL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "emu_overlay_render.h"

// Get the OpenGL ES rendering backend instance.
EmuOvlRenderBackend* overlay_gl_get_backend(void);

#ifdef __cplusplus
}
#endif

#endif // OVERLAY_GL_H
```

**Step 2: Write the GL backend implementation**

`OverlayGL.cpp` — uses GLES for quads and GLideN64's `g_textDrawer` for text:

```cpp
#include "OverlayGL.h"
#include "TextDrawer.h"
#include "Config.h"
#include "DisplayWindow.h"
#include "Graphics/Context.h"
#include "Graphics/OpenGLContext/GLFunctions.h"
#include "Graphics/OpenGLContext/ThreadedOpenGl/opengl_Wrapper.h"

extern TextDrawer g_textDrawer;

// --- GL state save/restore ---

struct SavedGLState {
    GLint viewport[4];
    GLint scissor[4];
    GLboolean blend;
    GLboolean depth_test;
    GLboolean cull_face;
    GLint blend_src, blend_dst;
};

static SavedGLState s_savedState;
static int s_screenW = 0, s_screenH = 0;

// Captured frame texture
static GLuint s_capturedTex = 0;
static bool s_frameCaptured = false;

// Simple quad shader
static GLuint s_quadProgram = 0;
static GLint s_quadColorLoc = -1;
static GLint s_quadPosLoc = -1;

// Textured quad shader (for captured frame)
static GLuint s_texProgram = 0;
static GLint s_texPosLoc = -1;
static GLint s_texCoordLoc = -1;
static GLint s_texSamplerLoc = -1;
static GLint s_texDimLoc = -1;

static const char* quad_vert_src =
    "#version 300 es\n"
    "in vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* quad_frag_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = uColor; }\n";

static const char* tex_vert_src =
    "#version 300 es\n"
    "in vec2 aPos;\n"
    "in vec2 aTexCoord;\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "  vTexCoord = aTexCoord;\n"
    "}\n";

static const char* tex_frag_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vTexCoord;\n"
    "uniform sampler2D uTexture;\n"
    "uniform float uDim;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec4 c = texture(uTexture, vTexCoord);\n"
    "  fragColor = vec4(c.rgb * uDim, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    return shader;
}

static GLuint create_program(const char* vert, const char* frag) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// --- Convert pixel coords to NDC ---
static float px_to_ndc_x(int px) {
    return (2.0f * px / s_screenW) - 1.0f;
}
static float px_to_ndc_y(int py) {
    return 1.0f - (2.0f * py / s_screenH); // Y is flipped in GL
}

// --- Backend implementation ---

static int gl_init(int screen_w, int screen_h) {
    s_screenW = screen_w;
    s_screenH = screen_h;

    if (s_quadProgram == 0) {
        s_quadProgram = create_program(quad_vert_src, quad_frag_src);
        s_quadPosLoc = glGetAttribLocation(s_quadProgram, "aPos");
        s_quadColorLoc = glGetUniformLocation(s_quadProgram, "uColor");
    }

    if (s_texProgram == 0) {
        s_texProgram = create_program(tex_vert_src, tex_frag_src);
        s_texPosLoc = glGetAttribLocation(s_texProgram, "aPos");
        s_texCoordLoc = glGetAttribLocation(s_texProgram, "aTexCoord");
        s_texSamplerLoc = glGetUniformLocation(s_texProgram, "uTexture");
        s_texDimLoc = glGetUniformLocation(s_texProgram, "uDim");
    }

    if (s_capturedTex == 0) {
        glGenTextures(1, &s_capturedTex);
    }

    return 0;
}

static void gl_destroy(void) {
    if (s_quadProgram) { glDeleteProgram(s_quadProgram); s_quadProgram = 0; }
    if (s_texProgram)  { glDeleteProgram(s_texProgram);  s_texProgram = 0; }
    if (s_capturedTex) { glDeleteTextures(1, &s_capturedTex); s_capturedTex = 0; }
    s_frameCaptured = false;
}

static void gl_draw_rect(int x, int y, int w, int h, uint32_t color) {
    float a = ((color >> 24) & 0xFF) / 255.0f;
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >>  8) & 0xFF) / 255.0f;
    float b = ((color >>  0) & 0xFF) / 255.0f;

    float x0 = px_to_ndc_x(x);
    float y0 = px_to_ndc_y(y);
    float x1 = px_to_ndc_x(x + w);
    float y1 = px_to_ndc_y(y + h);

    float verts[] = {
        x0, y0,  x1, y0,  x0, y1,
        x1, y0,  x1, y1,  x0, y1
    };

    glUseProgram(s_quadProgram);
    glUniform4f(s_quadColorLoc, r, g, b, a);
    glEnableVertexAttribArray(s_quadPosLoc);
    glVertexAttribPointer(s_quadPosLoc, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(s_quadPosLoc);
}

// Font size multipliers for the 3 font IDs
static float font_scales[] = { 1.0f, 0.75f, 0.625f }; // LARGE, SMALL, TINY

static void gl_draw_text(const char* text, int x, int y, uint32_t color, int font_id) {
    float a = ((color >> 24) & 0xFF) / 255.0f;
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >>  8) & 0xFF) / 255.0f;
    float b = ((color >>  0) & 0xFF) / 255.0f;

    float rgba[4] = { r, g, b, a };
    g_textDrawer.setTextColor(rgba);

    // Convert pixel coords to normalized coords used by TextDrawer
    // TextDrawer uses coords where (0,0) is top-left and (1,1) is bottom-right...
    // Actually TextDrawer uses screen-pixel coords internally
    float fx = (float)x / s_screenW;
    float fy = (float)y / s_screenH;

    // TextDrawer.drawText expects coords in NDC-like space
    // x: -1.0 to 1.0, y: -1.0 to 1.0
    float ndcX = px_to_ndc_x(x);
    float ndcY = px_to_ndc_y(y);

    g_textDrawer.drawText(text, ndcX, ndcY);
}

static int gl_text_width(const char* text, int font_id) {
    float w, h;
    g_textDrawer.getTextSize(text, w, h);
    // Convert from NDC width to pixel width
    return (int)(w * s_screenW / 2.0f);
}

static int gl_text_height(int font_id) {
    float w, h;
    g_textDrawer.getTextSize("Ay", w, h);
    return (int)(h * s_screenH / 2.0f);
}

static void gl_begin_frame(void) {
    // Save current GL state
    glGetIntegerv(GL_VIEWPORT, s_savedState.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s_savedState.scissor);
    s_savedState.blend = glIsEnabled(GL_BLEND);
    s_savedState.depth_test = glIsEnabled(GL_DEPTH_TEST);
    s_savedState.cull_face = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s_savedState.blend_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s_savedState.blend_dst);

    // Set overlay state
    glViewport(0, 0, s_screenW, s_screenH);
    glScissor(0, 0, s_screenW, s_screenH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void gl_end_frame(void) {
    // Restore GL state
    glViewport(s_savedState.viewport[0], s_savedState.viewport[1],
               s_savedState.viewport[2], s_savedState.viewport[3]);
    glScissor(s_savedState.scissor[0], s_savedState.scissor[1],
              s_savedState.scissor[2], s_savedState.scissor[3]);
    if (s_savedState.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s_savedState.depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (s_savedState.cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glBlendFunc(s_savedState.blend_src, s_savedState.blend_dst);
}

static void gl_capture_frame(void) {
    glBindTexture(GL_TEXTURE_2D, s_capturedTex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, s_screenW, s_screenH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    s_frameCaptured = true;
}

static void gl_draw_captured_frame(float dim) {
    if (!s_frameCaptured) return;

    float verts[] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,
         1.0f, -1.0f,  1.0f,  1.0f,  -1.0f, 1.0f
    };
    float texcoords[] = {
        0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,
        1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f
    };

    glUseProgram(s_texProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_capturedTex);
    glUniform1i(s_texSamplerLoc, 0);
    glUniform1f(s_texDimLoc, dim);

    glEnableVertexAttribArray(s_texPosLoc);
    glVertexAttribPointer(s_texPosLoc, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray(s_texCoordLoc);
    glVertexAttribPointer(s_texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texcoords);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(s_texPosLoc);
    glDisableVertexAttribArray(s_texCoordLoc);
}

// --- Backend struct ---

static EmuOvlRenderBackend s_glBackend = {
    gl_init,
    gl_destroy,
    gl_draw_rect,
    gl_draw_text,
    gl_text_width,
    gl_text_height,
    gl_begin_frame,
    gl_end_frame,
    gl_capture_frame,
    gl_draw_captured_frame
};

extern "C" EmuOvlRenderBackend* overlay_gl_get_backend(void) {
    return &s_glBackend;
}
```

**Step 3: Commit**

```bash
git add workspace/tg5040/cores/src/GLideN64-standalone/src/overlay/
git commit -m "feat: add OpenGL ES rendering backend for overlay menu"
```

---

### Task 5: Integrate into GLideN64

**Files:**
- Modify: `workspace/tg5040/cores/src/GLideN64-standalone/src/DisplayWindow.h`
- Modify: `workspace/tg5040/cores/src/GLideN64-standalone/src/DisplayWindow.cpp`
- Modify: `workspace/tg5040/cores/src/GLideN64-standalone/src/mupenplus/MupenPlusAPIImpl.cpp`
- Modify: `workspace/tg5040/cores/src/GLideN64-standalone/src/mupenplus/GLideN64_mupenplus.h`

**Step 1: Add CoreDoCommand to mupen64plus API**

In `GLideN64_mupenplus.h`, add after the existing extern declarations:

```cpp
extern ptr_CoreDoCommand CoreDoCommand;
```

In `MupenPlusAPIImpl.cpp`, add to `PluginStartup()` after existing dlsym calls:

```cpp
CoreDoCommand = (ptr_CoreDoCommand)DLSYM(_CoreLibHandle, "CoreDoCommand");
```

And add the variable declaration at the top with the other pointers:

```cpp
ptr_CoreDoCommand CoreDoCommand = nullptr;
```

**Step 2: Modify DisplayWindow to include overlay integration**

In `DisplayWindow.cpp`, add the overlay blocking loop. The key modification is in `swapBuffers()`:

```cpp
// New includes at top of DisplayWindow.cpp
#include <SDL2/SDL.h>
extern "C" {
#include "emu_overlay.h"
#include "overlay/OverlayGL.h"
}
#include "mupenplus/GLideN64_mupenplus.h"

// Overlay state (file-scope)
static EmuOvl s_overlay;
static EmuOvlConfig s_overlayConfig;
static bool s_overlayInitialized = false;
static char s_overlayJsonPath[512] = "";
static char s_overlayIniPath[512] = "";

// Menu button debounce
static bool s_menuBtnPrev = false;

// Set overlay config paths (called from launch script via environment)
static void overlay_init_paths() {
    if (s_overlayJsonPath[0] != '\0') return;

    const char* data_path = getenv("EMU_OVERLAY_JSON");
    if (data_path)
        snprintf(s_overlayJsonPath, sizeof(s_overlayJsonPath), "%s", data_path);

    const char* config_path = getenv("EMU_OVERLAY_INI");
    if (config_path)
        snprintf(s_overlayIniPath, sizeof(s_overlayIniPath), "%s", config_path);
}

static void overlay_ensure_init(int screen_w, int screen_h) {
    if (s_overlayInitialized) return;

    overlay_init_paths();
    if (s_overlayJsonPath[0] == '\0') return;

    if (emu_ovl_cfg_load(&s_overlayConfig, s_overlayJsonPath) != 0) return;

    if (s_overlayIniPath[0] != '\0')
        emu_ovl_cfg_read_ini(&s_overlayConfig, s_overlayIniPath);

    EmuOvlRenderBackend* backend = overlay_gl_get_backend();
    backend->init(screen_w, screen_h);

    // Extract game name from ROM (simplified — use filename)
    const char* game_name = getenv("EMU_OVERLAY_GAME");
    emu_ovl_init(&s_overlay, &s_overlayConfig, backend,
                 game_name ? game_name : "N64", screen_w, screen_h);

    s_overlayInitialized = true;
}

static EmuOvlInput poll_overlay_input() {
    EmuOvlInput input = {};
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_JOYBUTTONDOWN) {
            switch (event.jbutton.button) {
                case 8: input.menu = true; break; // Menu button
            }
        }
        if (event.type == SDL_JOYHATMOTION) {
            if (event.jhat.value & SDL_HAT_UP)    input.up = true;
            if (event.jhat.value & SDL_HAT_DOWN)  input.down = true;
            if (event.jhat.value & SDL_HAT_LEFT)  input.left = true;
            if (event.jhat.value & SDL_HAT_RIGHT) input.right = true;
        }
        if (event.type == SDL_JOYBUTTONDOWN) {
            switch (event.jbutton.button) {
                case 0: input.b = true; break;  // B
                case 1: break;                   // unused
                case 2: input.a = true; break;  // A
                case 4: input.l1 = true; break; // L1
                case 5: input.r1 = true; break; // R1
            }
        }
    }
    return input;
}

static bool check_menu_button() {
    // Check if menu button is currently pressed via SDL
    SDL_JoystickUpdate();

    // Open joystick if needed
    static SDL_Joystick* joy = nullptr;
    if (!joy && SDL_NumJoysticks() > 0)
        joy = SDL_JoystickOpen(0);
    if (!joy) return false;

    bool pressed = SDL_JoystickGetButton(joy, 8); // Menu button
    bool just_pressed = pressed && !s_menuBtnPrev;
    s_menuBtnPrev = pressed;
    return just_pressed;
}

// The blocking menu loop — called from swapBuffers when Menu is pressed
static EmuOvlAction run_overlay_loop(DisplayWindow& wnd) {
    EmuOvlRenderBackend* backend = s_overlay.render;

    // Capture current frame as background
    backend->capture_frame();

    // Mute audio
    SDL_PauseAudio(1);

    // Open menu
    emu_ovl_open(&s_overlay);

    // Blocking loop
    while (emu_ovl_is_active(&s_overlay)) {
        EmuOvlInput input = poll_overlay_input();
        emu_ovl_update(&s_overlay, &input);
        emu_ovl_render(&s_overlay);

        // Swap buffers directly (bypass normal pipeline)
        FunctionWrapper::CoreVideo_GL_SwapBuffers();

        // Small delay to cap menu at ~60fps and reduce CPU usage
        SDL_Delay(16);
    }

    // Resume audio
    SDL_PauseAudio(0);

    // Handle config changes
    EmuOvlAction action = emu_ovl_get_action(&s_overlay);
    if (emu_ovl_cfg_has_changes(&s_overlayConfig) && action != EMU_OVL_ACTION_QUIT) {
        emu_ovl_cfg_apply_staged(&s_overlayConfig);
        if (s_overlayIniPath[0] != '\0')
            emu_ovl_cfg_write_ini(&s_overlayConfig, s_overlayIniPath);

        // Reload GLideN64 config from file to pick up changes
        // This triggers GLideN64 to re-read its [Video-GLideN64] section
        config.resetToDefaults();
        Config_LoadConfig();
    }

    return action;
}
```

**Step 3: Modify swapBuffers()**

Replace the existing `DisplayWindow::swapBuffers()` in `DisplayWindow.cpp:47`:

```cpp
void DisplayWindow::swapBuffers()
{
    m_drawer.drawOSD();
    m_drawer.clearStatistics();

    // --- Overlay hook ---
    overlay_ensure_init(getWidth(), getHeight());
    if (s_overlayInitialized && check_menu_button()) {
        EmuOvlAction action = run_overlay_loop(*this);

        // Handle post-menu actions
        if (action == EMU_OVL_ACTION_QUIT) {
            if (CoreDoCommand)
                CoreDoCommand(M64CMD_STOP, 0, nullptr);
        } else if (action == EMU_OVL_ACTION_SAVE_STATE) {
            if (CoreDoCommand) {
                int slot = emu_ovl_get_action_param(&s_overlay);
                CoreDoCommand(M64CMD_STATE_SET_SLOT, slot, nullptr);
                CoreDoCommand(M64CMD_STATE_SAVE, 1, nullptr);
            }
        } else if (action == EMU_OVL_ACTION_LOAD_STATE) {
            if (CoreDoCommand) {
                int slot = emu_ovl_get_action_param(&s_overlay);
                CoreDoCommand(M64CMD_STATE_SET_SLOT, slot, nullptr);
                CoreDoCommand(M64CMD_STATE_LOAD, 0, nullptr);
            }
        }
    }
    // --- End overlay hook ---

    _swapBuffers();
    if (!RSP.LLE) {
        if ((config.generalEmulation.hacks & hack_doNotResetOtherModeL) == 0)
            gDP.otherMode.l = 0;
        if ((config.generalEmulation.hacks & hack_doNotResetOtherModeH) == 0)
            gDP.otherMode.h = 0x0CFF;
    }
    ++m_buffersSwapCount;
}
```

**Step 4: Commit**

```bash
git add workspace/tg5040/cores/src/GLideN64-standalone/src/DisplayWindow.cpp
git add workspace/tg5040/cores/src/GLideN64-standalone/src/mupenplus/MupenPlusAPIImpl.cpp
git add workspace/tg5040/cores/src/GLideN64-standalone/src/mupenplus/GLideN64_mupenplus.h
git commit -m "feat: integrate overlay menu into GLideN64 swapBuffers with blocking loop"
```

---

### Task 6: Create overlay_settings.json for N64

**Files:**
- Create: `skeleton/EXTRAS/Emus/shared/mupen64plus/overlay_settings.json`

**Step 1: Write the full JSON config with all GLideN64 settings**

Create the file with all sections from the design doc. Each item includes key, label, description, type, values/labels, and default. See the design doc for the complete list of settings per section.

The JSON should include these sections with all items:
- Rendering (UseNativeResolutionFactor, AspectRatio, FXAA, MultiSampling, anisotropy, bilinearMode, EnableHybridFilter, EnableHWLighting, EnableCoverage, EnableClipping, ThreadedVideo, BufferSwapMode)
- Texrect Fix (CorrectTexrectCoords, EnableNativeResTexrects, EnableTexCoordBounds)
- Texture Enhancement (txFilterMode, txEnhancementMode, txDeposterize, txFilterIgnoreBG)
- Hi-Res Textures (txHiresEnable, txHiresTextureFileStorage, txHiresFullAlphaChannel, txHresAltCRC)
- Dithering (EnableDitheringPattern, DitheringQuantization, RDRAMImageDitheringMode, EnableHiresNoiseDithering)
- Frame Buffer (EnableFBEmulation, EnableCopyColorToRDRAM, EnableCopyDepthToRDRAM, EnableCopyColorFromRDRAM, EnableN64DepthCompare, DisableFBInfo)
- Performance (EnableInaccurateTextureCoordinates, EnableLegacyBlending, EnableShadersStorage, EnableFragmentDepthWrite, BackgroundsMode)
- Gamma (ForceGammaCorrection, GammaCorrectionLevel)

Use the optimized defaults from the design doc / MEMORY.md.

**Step 2: Commit**

```bash
git add skeleton/EXTRAS/Emus/shared/mupen64plus/overlay_settings.json
git commit -m "feat: add GLideN64 overlay settings JSON config"
```

---

### Task 7: Update CMakeLists.txt

**Files:**
- Modify: `workspace/tg5040/cores/src/GLideN64-standalone/src/CMakeLists.txt`

**Step 1: Add overlay source files and include paths**

After the `set(GLideN64_SOURCES ...)` block (line 148), add:

```cmake
# Overlay menu sources
set(OVERLAY_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../all/common")
include_directories(${OVERLAY_COMMON_DIR})
list(APPEND GLideN64_SOURCES
  ${OVERLAY_COMMON_DIR}/emu_overlay.c
  ${OVERLAY_COMMON_DIR}/emu_overlay_cfg.c
  ${OVERLAY_COMMON_DIR}/cjson/cJSON.c
  overlay/OverlayGL.cpp
)
```

Also ensure SDL2 headers are available. After the `if(SDL)` block, add SDL2 include for non-SDL builds (mupen64plus provides SDL):

```cmake
# SDL2 needed for overlay input polling
find_package(PkgConfig)
pkg_check_modules(SDL2 sdl2)
if(SDL2_FOUND)
  include_directories(${SDL2_INCLUDE_DIRS})
endif()
```

**Step 2: Commit**

```bash
git add workspace/tg5040/cores/src/GLideN64-standalone/src/CMakeLists.txt
git commit -m "build: add overlay menu sources to GLideN64 CMakeLists"
```

---

### Task 8: Update Launch Scripts

**Files:**
- Modify: `skeleton/EXTRAS/Emus/tg5040/N64.pak/launch.sh`
- Modify: `skeleton/EXTRAS/Emus/tg5050/N64.pak/launch.sh`

**Step 1: Set environment variables for overlay**

Add before the mupen64plus launch command:

```bash
# Overlay menu config
export EMU_OVERLAY_JSON="$EMU_DIR/overlay_settings.json"
export EMU_OVERLAY_INI="$USERDATA_DIR/mupen64plus.cfg"
export EMU_OVERLAY_GAME="$(basename "$ROM" | sed 's/\.[^.]*$//')"
```

Where:
- `$EMU_DIR` = `skeleton/EXTRAS/Emus/shared/mupen64plus/` (shared data path)
- `$USERDATA_DIR` = user config directory
- Game name extracted from ROM filename without extension

Apply the same change to both tg5040 and tg5050 launch scripts.

**Step 2: Commit**

```bash
git add skeleton/EXTRAS/Emus/tg5040/N64.pak/launch.sh
git add skeleton/EXTRAS/Emus/tg5050/N64.pak/launch.sh
git commit -m "feat: pass overlay config paths to mupen64plus via environment"
```

---

### Task 9: Build GLideN64 and Test

**Step 1: Cross-compile GLideN64 with overlay**

Follow the existing build process (check for build scripts or CMake cross-compilation toolchain). The build should include the new overlay sources.

**Step 2: Verify compilation**

Ensure no compile errors from:
- C files (emu_overlay.c, emu_overlay_cfg.c, cJSON.c) compiled as C within a C++ project
- C++/C linkage (extern "C" wrappers)
- SDL2 headers found
- FreeType2 linked (already required by TextDrawer)

**Step 3: Deploy and test on device**

1. Copy built `mupen64plus-video-GLideN64.so` to device
2. Copy `overlay_settings.json` to shared mupen64plus directory
3. Launch an N64 game
4. Press Menu button — verify overlay appears with dimmed background
5. Navigate: UP/DOWN through main menu items
6. Select Options → verify section list appears
7. Enter a section → verify settings items with `< >` arrows
8. Change a value → verify it cycles through options
9. Press B to go back through menus
10. Close overlay → verify game resumes and settings are saved

**Step 4: Commit any fixes**

```bash
git add -u
git commit -m "fix: resolve build and runtime issues with overlay menu"
```

---

## Notes

### Design Deviation: No Custom Input Plugin

The original design called for a custom `mupen64plus-input-sdl.so`. The implementation uses **direct SDL polling inside GLideN64** instead, because:
- The input plugin source doesn't exist in the workspace (pre-built binary)
- Polling SDL directly is simpler and avoids IPC
- The blocking loop naturally pauses emulation, eliminating the need for CoreDoCommand(M64CMD_PAUSE)
- Input is consumed by the menu loop, so it doesn't leak to the game

### What Might Need Adjustment During Implementation

1. **TextDrawer coordinate system** — TextDrawer uses NDC coords internally. The exact mapping from pixel coords to TextDrawer coords may need calibration.
2. **SDL joystick button indices** — Button mappings (0=B, 2=A, etc.) are based on TrimUI device layout. Verify on actual hardware.
3. **Config reload** — After writing INI changes, GLideN64 needs to reload its config. The exact API (`config.resetToDefaults()` + `Config_LoadConfig()`) needs verification.
4. **FunctionWrapper::CoreVideo_GL_SwapBuffers** — Need to verify this can be called directly during the blocking loop without causing issues with mupen64plus's frame counter.
5. **Audio pause** — `SDL_PauseAudio(1)` assumes SDL audio is used. Verify the audio plugin uses SDL audio.
6. **Font sizes** — TextDrawer uses a single font size set at init. Multi-size support (LARGE/SMALL/TINY) may need the font atlas to be regenerated or use a single size with visual approximation.
