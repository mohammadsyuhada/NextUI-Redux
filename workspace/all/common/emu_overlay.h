#ifndef EMU_OVERLAY_H
#define EMU_OVERLAY_H

#include "emu_overlay_cfg.h"
#include "emu_overlay_render.h"

#define EMU_OVL_MAX_MAIN_ITEMS 8
#define EMU_OVL_MAX_SLOTS 8

typedef enum {
	EMU_OVL_STATE_CLOSED,
	EMU_OVL_STATE_MAIN_MENU,
	EMU_OVL_STATE_SECTION_LIST,
	EMU_OVL_STATE_SECTION_ITEMS,
	EMU_OVL_STATE_SAVE_SELECT,
	EMU_OVL_STATE_LOAD_SELECT
} EmuOvlState;

typedef enum {
	EMU_OVL_ACTION_NONE,
	EMU_OVL_ACTION_CONTINUE,
	EMU_OVL_ACTION_SAVE_STATE,
	EMU_OVL_ACTION_LOAD_STATE,
	EMU_OVL_ACTION_QUIT
} EmuOvlAction;

typedef struct {
	bool up;
	bool down;
	bool left;
	bool right;
	bool a;
	bool b;
	bool l1;
	bool r1;
	bool menu;
} EmuOvlInput;

typedef enum {
	EMU_OVL_MAIN_CONTINUE,
	EMU_OVL_MAIN_SAVE,
	EMU_OVL_MAIN_LOAD,
	EMU_OVL_MAIN_OPTIONS,
	EMU_OVL_MAIN_QUIT
} EmuOvlMainItemType;

typedef struct {
	char label[64];
	EmuOvlMainItemType type;
} EmuOvlMainItem;

typedef struct {
	EmuOvlConfig* config;
	EmuOvlRenderBackend* render;

	EmuOvlState state;
	int selected;
	int scroll_offset;
	int items_per_page;

	EmuOvlMainItem main_items[EMU_OVL_MAX_MAIN_ITEMS];
	int main_item_count;

	int current_section;
	int save_slot;

	EmuOvlAction action;
	int action_param;

	char game_name[256];
	int screen_w;
	int screen_h;

	// Button hint icons (icon_id from render->load_icon, -1 = not loaded)
	int icon_a;
	int icon_b;
	int icon_dpad_h;

	// Save state screenshots (matches minarch's .minui path for game switcher)
	char screenshot_dir[512];		   // e.g. /mnt/SDCARD/.userdata/shared/.minui/N64
	char rom_file[256];				   // e.g. "Super Mario 64.z64"
	int slot_icons[EMU_OVL_MAX_SLOTS]; // icon_id per slot, -1 = none
} EmuOvl;

int emu_ovl_init(EmuOvl* ovl, EmuOvlConfig* cfg, EmuOvlRenderBackend* render,
				 const char* game_name, int screen_w, int screen_h);
void emu_ovl_open(EmuOvl* ovl);
bool emu_ovl_update(EmuOvl* ovl, EmuOvlInput* input);
void emu_ovl_render(EmuOvl* ovl);
bool emu_ovl_is_active(EmuOvl* ovl);
EmuOvlAction emu_ovl_get_action(EmuOvl* ovl);
int emu_ovl_get_action_param(EmuOvl* ovl);
int emu_ovl_save_slot_screenshot(EmuOvl* ovl, int slot);

#endif
