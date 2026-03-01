#ifndef EMU_OVERLAY_CFG_H
#define EMU_OVERLAY_CFG_H

#include <stdbool.h>

#define EMU_OVL_MAX_SECTIONS 16
#define EMU_OVL_MAX_ITEMS 32
#define EMU_OVL_MAX_VALUES 16
#define EMU_OVL_MAX_STR 128

typedef enum {
	EMU_OVL_TYPE_BOOL,
	EMU_OVL_TYPE_CYCLE,
	EMU_OVL_TYPE_INT
} EmuOvlItemType;

typedef struct {
	char key[EMU_OVL_MAX_STR];
	char label[EMU_OVL_MAX_STR];
	char description[EMU_OVL_MAX_STR];
	EmuOvlItemType type;
	int values[EMU_OVL_MAX_VALUES];
	char labels[EMU_OVL_MAX_VALUES][EMU_OVL_MAX_STR];
	int value_count;
	int int_min, int_max, int_step;
	int float_scale; // >0: INI value is float; multiply by scale to get int, divide when writing
	int default_value;
	int current_value;
	int staged_value;
	bool dirty;
} EmuOvlItem;

typedef struct {
	char name[EMU_OVL_MAX_STR];
	EmuOvlItem items[EMU_OVL_MAX_ITEMS];
	int item_count;
} EmuOvlSection;

typedef struct {
	char emulator[EMU_OVL_MAX_STR];
	char config_file[EMU_OVL_MAX_STR];
	char config_section[EMU_OVL_MAX_STR];
	char options_hint[256];
	bool save_state;
	bool load_state;
	EmuOvlSection sections[EMU_OVL_MAX_SECTIONS];
	int section_count;
} EmuOvlConfig;

int emu_ovl_cfg_load(EmuOvlConfig* cfg, const char* json_path);
void emu_ovl_cfg_free(EmuOvlConfig* cfg);
int emu_ovl_cfg_read_ini(EmuOvlConfig* cfg, const char* ini_path);
int emu_ovl_cfg_write_ini(EmuOvlConfig* cfg, const char* ini_path);
void emu_ovl_cfg_reset_staged(EmuOvlConfig* cfg);
void emu_ovl_cfg_reset_section_to_defaults(EmuOvlSection* sec);
void emu_ovl_cfg_apply_staged(EmuOvlConfig* cfg);
bool emu_ovl_cfg_has_changes(EmuOvlConfig* cfg);

#endif
