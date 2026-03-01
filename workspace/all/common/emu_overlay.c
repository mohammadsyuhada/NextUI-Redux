#include "emu_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Layout constants (pre-scaled)
#define PADDING 10
#define PILL_SIZE 30
#define BUTTON_SIZE 16
#define BUTTON_MARGIN 6
#define BUTTON_PADDING 10
#define SETTINGS_ROW_PAD 8

static int ovl_scale = 2;
#define S(x) ((x) * ovl_scale)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void build_main_menu(EmuOvl* ovl) {
	int n = 0;

	snprintf(ovl->main_items[n].label, sizeof(ovl->main_items[n].label), "Continue");
	ovl->main_items[n].type = EMU_OVL_MAIN_CONTINUE;
	n++;

	if (ovl->config->save_state) {
		snprintf(ovl->main_items[n].label, sizeof(ovl->main_items[n].label), "Save State");
		ovl->main_items[n].type = EMU_OVL_MAIN_SAVE;
		n++;
	}

	if (ovl->config->load_state) {
		snprintf(ovl->main_items[n].label, sizeof(ovl->main_items[n].label), "Load State");
		ovl->main_items[n].type = EMU_OVL_MAIN_LOAD;
		n++;
	}

	if (ovl->config->section_count > 0) {
		snprintf(ovl->main_items[n].label, sizeof(ovl->main_items[n].label), "Options");
		ovl->main_items[n].type = EMU_OVL_MAIN_OPTIONS;
		n++;
	}

	snprintf(ovl->main_items[n].label, sizeof(ovl->main_items[n].label), "Quit");
	ovl->main_items[n].type = EMU_OVL_MAIN_QUIT;
	n++;

	ovl->main_item_count = n;
}

static int find_options_index(EmuOvl* ovl) {
	for (int i = 0; i < ovl->main_item_count; i++) {
		if (ovl->main_items[i].type == EMU_OVL_MAIN_OPTIONS)
			return i;
	}
	return 0;
}

static void cycle_item_next(EmuOvlItem* item) {
	switch (item->type) {
	case EMU_OVL_TYPE_BOOL:
		item->staged_value = item->staged_value ? 0 : 1;
		break;
	case EMU_OVL_TYPE_CYCLE: {
		int idx = -1;
		for (int i = 0; i < item->value_count; i++) {
			if (item->values[i] == item->staged_value) {
				idx = i;
				break;
			}
		}
		if (idx < 0)
			idx = 0;
		else
			idx = (idx + 1) % item->value_count;
		item->staged_value = item->values[idx];
		break;
	}
	case EMU_OVL_TYPE_INT:
		item->staged_value += item->int_step;
		if (item->staged_value > item->int_max)
			item->staged_value = item->int_min;
		break;
	}
	item->dirty = (item->staged_value != item->current_value);
}

static void cycle_item_prev(EmuOvlItem* item) {
	switch (item->type) {
	case EMU_OVL_TYPE_BOOL:
		item->staged_value = item->staged_value ? 0 : 1;
		break;
	case EMU_OVL_TYPE_CYCLE: {
		int idx = -1;
		for (int i = 0; i < item->value_count; i++) {
			if (item->values[i] == item->staged_value) {
				idx = i;
				break;
			}
		}
		if (idx < 0)
			idx = 0;
		else
			idx = (idx - 1 + item->value_count) % item->value_count;
		item->staged_value = item->values[idx];
		break;
	}
	case EMU_OVL_TYPE_INT:
		item->staged_value -= item->int_step;
		if (item->staged_value < item->int_min)
			item->staged_value = item->int_max;
		break;
	}
	item->dirty = (item->staged_value != item->current_value);
}

static const char* get_item_display_value(EmuOvlItem* item, char* buf, int buf_size) {
	switch (item->type) {
	case EMU_OVL_TYPE_BOOL:
		return item->staged_value ? "On" : "Off";
	case EMU_OVL_TYPE_CYCLE:
		for (int i = 0; i < item->value_count; i++) {
			if (item->values[i] == item->staged_value) {
				if (item->labels[i][0] != '\0')
					return item->labels[i];
				snprintf(buf, buf_size, "%d", item->staged_value);
				return buf;
			}
		}
		snprintf(buf, buf_size, "%d", item->staged_value);
		return buf;
	case EMU_OVL_TYPE_INT:
		snprintf(buf, buf_size, "%d", item->staged_value);
		return buf;
	}
	return "";
}

static void ensure_scroll(EmuOvl* ovl, int total_count) {
	if (ovl->selected < ovl->scroll_offset)
		ovl->scroll_offset = ovl->selected;
	else if (ovl->selected >= ovl->scroll_offset + ovl->items_per_page)
		ovl->scroll_offset = ovl->selected - ovl->items_per_page + 1;
	if (ovl->scroll_offset < 0)
		ovl->scroll_offset = 0;
	int max_scroll = total_count - ovl->items_per_page;
	if (max_scroll < 0)
		max_scroll = 0;
	if (ovl->scroll_offset > max_scroll)
		ovl->scroll_offset = max_scroll;
}

// ---------------------------------------------------------------------------
// Save-slot screenshot helpers
// ---------------------------------------------------------------------------

static void get_slot_screenshot_path(EmuOvl* ovl, int slot, char* buf, int buf_size) {
	// Match minarch format: <screenshot_dir>/<rom_file>.<slot>.bmp
	snprintf(buf, buf_size, "%s/%s.%d.bmp", ovl->screenshot_dir, ovl->rom_file, slot);
}

static void write_resume_slot(EmuOvl* ovl, int slot) {
	// Write resume slot file so game switcher knows which slot to show
	// Format: <screenshot_dir>/<rom_file>.txt containing the slot number
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.txt", ovl->screenshot_dir, ovl->rom_file);
	FILE* f = fopen(path, "w");
	if (f) {
		fprintf(f, "%d", slot);
		fclose(f);
	}
}

static void load_slot_screenshots(EmuOvl* ovl) {
	if (!ovl->render || !ovl->render->load_icon ||
		ovl->screenshot_dir[0] == '\0' || ovl->rom_file[0] == '\0')
		return;

	// Target height: ~40% of screen height
	int target_h = ovl->screen_h * 2 / 5;

	for (int i = 0; i < EMU_OVL_MAX_SLOTS; i++) {
		// Free old icon if loaded
		if (ovl->slot_icons[i] >= 0 && ovl->render->free_icon) {
			ovl->render->free_icon(ovl->slot_icons[i]);
			ovl->slot_icons[i] = -1;
		}
		char path[512];
		get_slot_screenshot_path(ovl, i, path, sizeof(path));
		ovl->slot_icons[i] = ovl->render->load_icon(path, target_h);
	}
}

static void free_slot_screenshots(EmuOvl* ovl) {
	if (!ovl->render || !ovl->render->free_icon)
		return;
	for (int i = 0; i < EMU_OVL_MAX_SLOTS; i++) {
		if (ovl->slot_icons[i] >= 0) {
			ovl->render->free_icon(ovl->slot_icons[i]);
			ovl->slot_icons[i] = -1;
		}
	}
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int emu_ovl_init(EmuOvl* ovl, EmuOvlConfig* cfg, EmuOvlRenderBackend* render,
				 const char* game_name, int screen_w, int screen_h) {
	memset(ovl, 0, sizeof(*ovl));
	ovl->config = cfg;
	ovl->render = render;
	ovl->state = EMU_OVL_STATE_CLOSED;
	ovl->action = EMU_OVL_ACTION_NONE;
	ovl->screen_w = screen_w;
	ovl->screen_h = screen_h;

	if (game_name)
		snprintf(ovl->game_name, sizeof(ovl->game_name), "%s", game_name);

	// Scale factor: match NextUI's FIXED_SCALE
	// Brick (1024x768) = 3x, Smart Pro / TG5050 (1280x720) = 2x
	if (screen_w <= 1024)
		ovl_scale = 3;
	else
		ovl_scale = 2;

	// Items per page: Brick = 5, Smart Pro / TG5050 = 9
	if (screen_w <= 1024)
		ovl->items_per_page = 5;
	else
		ovl->items_per_page = 8;

	build_main_menu(ovl);

	// Screenshot directory (matches minarch's .minui path for game switcher)
	ovl->screenshot_dir[0] = '\0';
	ovl->rom_file[0] = '\0';
	const char* ss_dir = getenv("EMU_OVERLAY_SCREENSHOT_DIR");
	if (ss_dir && ss_dir[0] != '\0')
		snprintf(ovl->screenshot_dir, sizeof(ovl->screenshot_dir), "%s", ss_dir);
	const char* rom_file = getenv("EMU_OVERLAY_ROMFILE");
	if (rom_file && rom_file[0] != '\0')
		snprintf(ovl->rom_file, sizeof(ovl->rom_file), "%s", rom_file);

	// Init slot screenshot icons
	for (int i = 0; i < EMU_OVL_MAX_SLOTS; i++)
		ovl->slot_icons[i] = -1;

	// Load button hint icons from resource directory
	ovl->icon_a = -1;
	ovl->icon_b = -1;
	ovl->icon_dpad_h = -1;
	const char* res_dir = getenv("EMU_OVERLAY_RES");
	if (res_dir && res_dir[0] != '\0' && render->load_icon) {
		int icon_h = S(BUTTON_SIZE); // match NextUI's btn_sz = SCALE1(BUTTON_SIZE)
		char path[512];
		snprintf(path, sizeof(path), "%s/nav_button_a.png", res_dir);
		ovl->icon_a = render->load_icon(path, icon_h);
		snprintf(path, sizeof(path), "%s/nav_button_b.png", res_dir);
		ovl->icon_b = render->load_icon(path, icon_h);
		snprintf(path, sizeof(path), "%s/nav_dpad_horizontal.png", res_dir);
		ovl->icon_dpad_h = render->load_icon(path, icon_h);
	}

	// Note: caller is responsible for calling render->init() before emu_ovl_init
	return 0;
}

void emu_ovl_open(EmuOvl* ovl) {
	ovl->state = EMU_OVL_STATE_MAIN_MENU;
	ovl->selected = 0;
	ovl->action = EMU_OVL_ACTION_NONE;
	ovl->action_param = 0;
	ovl->save_slot = 0;
	ovl->scroll_offset = 0;

	if (ovl->render && ovl->render->capture_frame)
		ovl->render->capture_frame();
}

bool emu_ovl_update(EmuOvl* ovl, EmuOvlInput* input) {
	if (ovl->state == EMU_OVL_STATE_CLOSED)
		return false;

	switch (ovl->state) {
	// ----- MAIN MENU -----
	case EMU_OVL_STATE_MAIN_MENU:
		if (input->up) {
			ovl->selected = (ovl->selected - 1 + ovl->main_item_count) % ovl->main_item_count;
		} else if (input->down) {
			ovl->selected = (ovl->selected + 1) % ovl->main_item_count;
		} else if (input->a) {
			EmuOvlMainItemType t = ovl->main_items[ovl->selected].type;
			switch (t) {
			case EMU_OVL_MAIN_CONTINUE:
				ovl->action = EMU_OVL_ACTION_CONTINUE;
				ovl->state = EMU_OVL_STATE_CLOSED;
				return false;
			case EMU_OVL_MAIN_SAVE:
				ovl->state = EMU_OVL_STATE_SAVE_SELECT;
				ovl->save_slot = 0;
				load_slot_screenshots(ovl);
				break;
			case EMU_OVL_MAIN_LOAD:
				ovl->state = EMU_OVL_STATE_LOAD_SELECT;
				ovl->save_slot = 0;
				load_slot_screenshots(ovl);
				break;
			case EMU_OVL_MAIN_OPTIONS:
				ovl->state = EMU_OVL_STATE_SECTION_LIST;
				ovl->selected = 0;
				ovl->scroll_offset = 0;
				ovl->current_section = 0;
				break;
			case EMU_OVL_MAIN_QUIT:
				ovl->action = EMU_OVL_ACTION_QUIT;
				ovl->state = EMU_OVL_STATE_CLOSED;
				return false;
			}
		} else if (input->b || input->menu) {
			ovl->action = EMU_OVL_ACTION_CONTINUE;
			ovl->state = EMU_OVL_STATE_CLOSED;
			return false;
		}
		break;

	// ----- SAVE / LOAD SELECT -----
	case EMU_OVL_STATE_SAVE_SELECT:
	case EMU_OVL_STATE_LOAD_SELECT:
		if (input->left) {
			ovl->save_slot = (ovl->save_slot - 1 + EMU_OVL_MAX_SLOTS) % EMU_OVL_MAX_SLOTS;
		} else if (input->right) {
			ovl->save_slot = (ovl->save_slot + 1) % EMU_OVL_MAX_SLOTS;
		} else if (input->a) {
			ovl->action = (ovl->state == EMU_OVL_STATE_SAVE_SELECT)
							  ? EMU_OVL_ACTION_SAVE_STATE
							  : EMU_OVL_ACTION_LOAD_STATE;
			ovl->action_param = ovl->save_slot;
			ovl->state = EMU_OVL_STATE_CLOSED;
			return false;
		} else if (input->b) {
			EmuOvlState prev_state = ovl->state;
			ovl->state = EMU_OVL_STATE_MAIN_MENU;
			free_slot_screenshots(ovl);
			// Restore selected to the correct Save/Load entry
			EmuOvlMainItemType target = (prev_state == EMU_OVL_STATE_SAVE_SELECT)
											? EMU_OVL_MAIN_SAVE
											: EMU_OVL_MAIN_LOAD;
			for (int i = 0; i < ovl->main_item_count; i++) {
				if (ovl->main_items[i].type == target) {
					ovl->selected = i;
					break;
				}
			}
		}
		break;

	// ----- SECTION LIST -----
	case EMU_OVL_STATE_SECTION_LIST:
		if (input->up) {
			ovl->selected = (ovl->selected - 1 + ovl->config->section_count) % ovl->config->section_count;
			ensure_scroll(ovl, ovl->config->section_count);
		} else if (input->down) {
			ovl->selected = (ovl->selected + 1) % ovl->config->section_count;
			ensure_scroll(ovl, ovl->config->section_count);
		} else if (input->a) {
			ovl->current_section = ovl->selected;
			ovl->state = EMU_OVL_STATE_SECTION_ITEMS;
			ovl->selected = 0;
			ovl->scroll_offset = 0;
		} else if (input->b) {
			ovl->state = EMU_OVL_STATE_MAIN_MENU;
			ovl->selected = find_options_index(ovl);
		}
		break;

	// ----- SECTION ITEMS -----
	case EMU_OVL_STATE_SECTION_ITEMS: {
		EmuOvlSection* sec = &ovl->config->sections[ovl->current_section];
		int total_rows = sec->item_count + 1; // +1 for "Reset to Default"
		if (input->up) {
			ovl->selected = (ovl->selected - 1 + total_rows) % total_rows;
			ensure_scroll(ovl, total_rows);
		} else if (input->down) {
			ovl->selected = (ovl->selected + 1) % total_rows;
			ensure_scroll(ovl, total_rows);
		} else if (input->right || input->a) {
			if (ovl->selected == sec->item_count) {
				// "Reset to Default" action
				emu_ovl_cfg_reset_section_to_defaults(sec);
			} else if (sec->item_count > 0) {
				cycle_item_next(&sec->items[ovl->selected]);
			}
		} else if (input->left) {
			if (ovl->selected < sec->item_count && sec->item_count > 0)
				cycle_item_prev(&sec->items[ovl->selected]);
		} else if (input->b) {
			ovl->state = EMU_OVL_STATE_SECTION_LIST;
			ovl->selected = ovl->current_section;
			ovl->scroll_offset = 0;
			ensure_scroll(ovl, ovl->config->section_count);
		}
		break;
	}

	case EMU_OVL_STATE_CLOSED:
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Rendering — settings-page style (matching NextUI's UI_renderSettingsPage)
// ---------------------------------------------------------------------------

// Draw a rounded rect using multiple draw_rect calls (scanline approximation)
static void draw_rounded_rect(EmuOvlRenderBackend* r, int x, int y, int w, int h,
							  uint32_t color) {
	int radius = S(14);
	if (radius > h / 2)
		radius = h / 2;
	if (radius > w / 2)
		radius = w / 2;

	// Middle section
	if (h - 2 * radius > 0)
		r->draw_rect(x, y + radius, w, h - 2 * radius, color);

	// Rounded corners via scanlines
	for (int dy = 0; dy < radius; dy++) {
		int yd = radius - dy;
		int inset = radius - (int)sqrtf((float)(radius * radius - yd * yd));
		int row_w = w - 2 * inset;
		if (row_w <= 0)
			continue;
		r->draw_rect(x + inset, y + dy, row_w, 1, color);
		r->draw_rect(x + inset, y + h - 1 - dy, row_w, 1, color);
	}
}

// Compute vertically centered list_y for n items between top bar and bottom bar
static int calc_centered_list_y(EmuOvl* ovl, int item_count) {
	int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;
	int top = bar_h;
	int bottom = ovl->screen_h - bar_h;
	int total_h = item_count * S(PILL_SIZE);
	return top + (bottom - top - total_h) / 2;
}

// Draw a menu bar (semi-transparent black bar with title text)
static void draw_menu_bar(EmuOvl* ovl, const char* title) {
	EmuOvlRenderBackend* r = ovl->render;
	int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;

	// Semi-transparent black bar
	r->draw_rect(0, 0, ovl->screen_w, bar_h, EMU_OVL_COLOR_BAR_BG);

	// Title text (left-aligned, matching content padding)
	int text_y = (bar_h - r->text_height(EMU_OVL_FONT_SMALL)) / 2;
	r->draw_text(title, S(PADDING), text_y,
				 EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_SMALL);
}

// Map a button name to its icon handle, or -1 if no icon loaded
static int get_hint_icon(EmuOvl* ovl, const char* btn_name) {
	if (strcmp(btn_name, "A") == 0)
		return ovl->icon_a;
	if (strcmp(btn_name, "B") == 0)
		return ovl->icon_b;
	if (strcmp(btn_name, "LEFT/RIGHT") == 0)
		return ovl->icon_dpad_h;
	return -1;
}

// Draw a button hint bar at the bottom
static void draw_hint_bar(EmuOvl* ovl, const char* hints[], int hint_count) {
	EmuOvlRenderBackend* r = ovl->render;
	int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;
	int bar_y = ovl->screen_h - bar_h;

	// Semi-transparent black bar
	r->draw_rect(0, bar_y, ovl->screen_w, bar_h, EMU_OVL_COLOR_BAR_BG);

	// Render hint pairs: "B" "BACK" "A" "OK" → [B icon] BACK   [A icon] OK
	int x = S(PADDING) + S(BUTTON_MARGIN);
	int text_y = bar_y + (bar_h - r->text_height(EMU_OVL_FONT_TINY)) / 2;
	for (int i = 0; i < hint_count; i += 2) {
		// Button: try icon first, fall back to text
		int icon_id = get_hint_icon(ovl, hints[i]);
		if (icon_id >= 0 && r->draw_icon) {
			int icon_y = bar_y + (bar_h - r->icon_height(icon_id)) / 2;
			r->draw_icon(icon_id, x, icon_y);
			x += r->icon_width(icon_id) + S(3);
		} else {
			r->draw_text(hints[i], x, text_y, EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
			x += r->text_width(hints[i], EMU_OVL_FONT_TINY) + S(3);
		}
		// Action label
		if (i + 1 < hint_count) {
			r->draw_text(hints[i + 1], x, text_y, EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_TINY);
			x += r->text_width(hints[i + 1], EMU_OVL_FONT_TINY) + S(BUTTON_MARGIN);
		}
	}
}

// Draw a settings row (label on left, optional value on right)
// Matches the visual style of UI_renderSettingsRow from ui_list.c
static void draw_settings_row(EmuOvl* ovl, int x, int y, int w, int h,
							  const char* label, const char* value,
							  bool selected, bool cycleable, int label_font) {
	EmuOvlRenderBackend* r = ovl->render;
	int row_pad = S(SETTINGS_ROW_PAD);

	if (selected) {
		if (value) {
			// 2-layer: full-width COLOR2 + label-width COLOR1
			draw_rounded_rect(r, x, y, w, h, EMU_OVL_COLOR_ROW_BG);

			int lw = r->text_width(label, label_font);
			int label_pill_w = lw + row_pad * 2;
			draw_rounded_rect(r, x, y, label_pill_w, h, EMU_OVL_COLOR_ROW_SEL);

			// Label text (black on white pill)
			int text_y_pos = y + (h - r->text_height(label_font)) / 2;
			r->draw_text(label, x + row_pad, text_y_pos,
						 EMU_OVL_COLOR_TEXT_SEL, label_font);

			// Value text (white, right-aligned, with arrows if cycleable)
			char display[192];
			if (cycleable)
				snprintf(display, sizeof(display), "< %s >", value);
			else
				snprintf(display, sizeof(display), "%s", value);

			int vw = r->text_width(display, EMU_OVL_FONT_TINY);
			int val_x = x + w - row_pad - vw;
			int val_y = y + (h - r->text_height(EMU_OVL_FONT_TINY)) / 2;
			r->draw_text(display, val_x, val_y, EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_TINY);
		} else {
			// Single label rect (no value)
			int lw = r->text_width(label, label_font);
			int label_pill_w = lw + row_pad * 2;
			draw_rounded_rect(r, x, y, label_pill_w, h, EMU_OVL_COLOR_ROW_SEL);

			int text_y_pos = y + (h - r->text_height(label_font)) / 2;
			r->draw_text(label, x + row_pad, text_y_pos,
						 EMU_OVL_COLOR_TEXT_SEL, label_font);
		}
	} else {
		// Unselected: no background, gray text
		int text_y_pos = y + (h - r->text_height(label_font)) / 2;
		r->draw_text(label, x + row_pad, text_y_pos,
					 EMU_OVL_COLOR_GRAY, label_font);

		if (value) {
			int vw = r->text_width(value, EMU_OVL_FONT_TINY);
			int val_x = x + w - row_pad - vw;
			int val_y = y + (h - r->text_height(EMU_OVL_FONT_TINY)) / 2;
			r->draw_text(value, val_x, val_y, EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
		}
	}
}

static void draw_centered_text(EmuOvlRenderBackend* r, const char* text, int cx, int cy,
							   uint32_t color, int font_id) {
	int tw = r->text_width(text, font_id);
	int th = r->text_height(font_id);
	r->draw_text(text, cx - tw / 2, cy - th / 2, color, font_id);
}

static void render_main_menu(EmuOvl* ovl) {
	EmuOvlRenderBackend* r = ovl->render;

	draw_menu_bar(ovl, ovl->game_name);

	int row_h = S(PILL_SIZE);
	int content_x = S(PADDING);
	int content_w = ovl->screen_w - S(PADDING) * 2;

	int vis_count = ovl->main_item_count;
	if (vis_count > ovl->items_per_page)
		vis_count = ovl->items_per_page;
	int list_y = calc_centered_list_y(ovl, vis_count);

	for (int i = 0; i < vis_count; i++) {
		int iy = list_y + i * row_h;
		bool sel = (i == ovl->selected);
		draw_settings_row(ovl, content_x, iy, content_w, row_h,
						  ovl->main_items[i].label, NULL, sel, false,
						  EMU_OVL_FONT_LARGE);
	}

	const char* hints[] = {"B", "BACK", "A", "OK"};
	draw_hint_bar(ovl, hints, 4);
}

static void render_slot_select(EmuOvl* ovl) {
	EmuOvlRenderBackend* r = ovl->render;
	bool is_save = (ovl->state == EMU_OVL_STATE_SAVE_SELECT);

	draw_menu_bar(ovl, is_save ? "Save State" : "Load State");

	int bar_h = S(BUTTON_SIZE) + S(BUTTON_MARGIN) * 2;
	int center_y = ovl->screen_h / 2;

	// Slot screenshot (centered above slot text)
	int icon_id = ovl->slot_icons[ovl->save_slot];
	if (icon_id >= 0 && r->draw_icon) {
		int iw = r->icon_width(icon_id);
		int ih = r->icon_height(icon_id);
		int ix = (ovl->screen_w - iw) / 2;
		int iy = bar_h + (center_y - bar_h - ih) / 2;
		if (iy < bar_h)
			iy = bar_h;
		r->draw_icon(icon_id, ix, iy);
	} else {
		// No screenshot: show "Empty" text
		draw_centered_text(r, "Empty", ovl->screen_w / 2,
						   bar_h + (center_y - bar_h) / 2,
						   EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_SMALL);
	}

	// Slot text below center
	char slot_text[32];
	snprintf(slot_text, sizeof(slot_text), "<  Slot %d  >", ovl->save_slot);
	draw_centered_text(r, slot_text, ovl->screen_w / 2, center_y + S(PILL_SIZE) / 2,
					   EMU_OVL_COLOR_WHITE, EMU_OVL_FONT_LARGE);

	// Pagination dots
	int dot_size = S(4);
	int dot_gap = S(6);
	int dots_w = EMU_OVL_MAX_SLOTS * dot_size + (EMU_OVL_MAX_SLOTS - 1) * dot_gap;
	int dots_x = (ovl->screen_w - dots_w) / 2;
	int dots_y = center_y + S(PILL_SIZE) + S(PILL_SIZE) / 2;

	for (int i = 0; i < EMU_OVL_MAX_SLOTS; i++) {
		uint32_t color = (i == ovl->save_slot) ? EMU_OVL_COLOR_ACCENT : EMU_OVL_COLOR_GRAY;
		r->draw_rect(dots_x + i * (dot_size + dot_gap), dots_y, dot_size, dot_size, color);
	}

	const char* hints[] = {"LEFT/RIGHT", "SELECT", "B", "BACK", "A", "OK"};
	draw_hint_bar(ovl, hints, 6);
}

static void render_section_list(EmuOvl* ovl) {
	EmuOvlRenderBackend* r = ovl->render;

	draw_menu_bar(ovl, "Options");

	int row_h = S(PILL_SIZE);
	int content_x = S(PADDING);
	int content_w = ovl->screen_w - S(PADDING) * 2;

	// Scroll
	ensure_scroll(ovl, ovl->config->section_count);

	int vis_count = ovl->items_per_page;
	if (vis_count > ovl->config->section_count)
		vis_count = ovl->config->section_count;
	int list_y = calc_centered_list_y(ovl, vis_count);

	for (int vi = 0; vi < vis_count; vi++) {
		int idx = ovl->scroll_offset + vi;
		if (idx >= ovl->config->section_count)
			break;

		int iy = list_y + vi * row_h;
		bool sel = (idx == ovl->selected);
		draw_settings_row(ovl, content_x, iy, content_w, row_h,
						  ovl->config->sections[idx].name, NULL, sel, false,
						  EMU_OVL_FONT_LARGE);
	}

	// Optional hint (e.g. "Restart game to apply changes")
	if (ovl->config->options_hint[0] != '\0') {
		int hint_y = list_y + vis_count * row_h + S(4);
		int tw = r->text_width(ovl->config->options_hint, EMU_OVL_FONT_TINY);
		r->draw_text(ovl->config->options_hint,
					 (ovl->screen_w - tw) / 2, hint_y,
					 EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
	}

	const char* hints[] = {"B", "BACK", "A", "OPEN"};
	draw_hint_bar(ovl, hints, 4);
}

static void render_section_items(EmuOvl* ovl) {
	EmuOvlRenderBackend* r = ovl->render;
	EmuOvlSection* sec = &ovl->config->sections[ovl->current_section];

	draw_menu_bar(ovl, sec->name);

	int row_h = S(PILL_SIZE);
	int items_per_page = ovl->items_per_page;
	int list_y = calc_centered_list_y(ovl, items_per_page);
	int content_x = S(PADDING);
	int content_w = ovl->screen_w - S(PADDING) * 2;

	int total_rows = sec->item_count + 1; // +1 for "Reset to Default"

	// Scroll
	ensure_scroll(ovl, total_rows);

	int vis_count = items_per_page;
	if (vis_count > total_rows)
		vis_count = total_rows;

	for (int vi = 0; vi < vis_count; vi++) {
		int idx = ovl->scroll_offset + vi;
		if (idx >= total_rows)
			break;

		int iy = list_y + vi * row_h;
		bool sel = (idx == ovl->selected);

		if (idx < sec->item_count) {
			EmuOvlItem* item = &sec->items[idx];
			char val_buf[64];
			const char* val_str = get_item_display_value(item, val_buf, sizeof(val_buf));
			draw_settings_row(ovl, content_x, iy, content_w, row_h,
							  item->label, val_str, sel, true,
							  EMU_OVL_FONT_SMALL);
		} else {
			// "Reset to Default" row
			draw_settings_row(ovl, content_x, iy, content_w, row_h,
							  "Reset to Default", NULL, sel, false,
							  EMU_OVL_FONT_SMALL);
		}
	}

	// Description for selected item / hint text area
	int desc_y = list_y + vis_count * row_h;
	int desc_cy = desc_y + row_h / 2 - r->text_height(EMU_OVL_FONT_TINY) / 2;

	if (ovl->selected < sec->item_count) {
		EmuOvlItem* sel_item = &sec->items[ovl->selected];
		if (sel_item->description[0] != '\0') {
			int tw = r->text_width(sel_item->description, EMU_OVL_FONT_TINY);
			r->draw_text(sel_item->description,
						 (ovl->screen_w - tw) / 2, desc_cy,
						 EMU_OVL_COLOR_GRAY, EMU_OVL_FONT_TINY);
		}
	}

	const char* hints[] = {"LEFT/RIGHT", "CHANGE", "B", "BACK"};
	draw_hint_bar(ovl, hints, 4);
}

void emu_ovl_render(EmuOvl* ovl) {
	if (ovl->state == EMU_OVL_STATE_CLOSED)
		return;

	EmuOvlRenderBackend* r = ovl->render;
	if (!r)
		return;

	r->begin_frame();
	r->draw_captured_frame(0.15f);

	switch (ovl->state) {
	case EMU_OVL_STATE_MAIN_MENU:
		render_main_menu(ovl);
		break;
	case EMU_OVL_STATE_SAVE_SELECT:
	case EMU_OVL_STATE_LOAD_SELECT:
		render_slot_select(ovl);
		break;
	case EMU_OVL_STATE_SECTION_LIST:
		render_section_list(ovl);
		break;
	case EMU_OVL_STATE_SECTION_ITEMS:
		render_section_items(ovl);
		break;
	case EMU_OVL_STATE_CLOSED:
		break;
	}

	r->end_frame();
}

bool emu_ovl_is_active(EmuOvl* ovl) {
	return ovl->state != EMU_OVL_STATE_CLOSED;
}

EmuOvlAction emu_ovl_get_action(EmuOvl* ovl) {
	return ovl->action;
}

int emu_ovl_get_action_param(EmuOvl* ovl) {
	return ovl->action_param;
}

int emu_ovl_save_slot_screenshot(EmuOvl* ovl, int slot) {
	if (!ovl || !ovl->render || !ovl->render->save_captured_frame)
		return -1;
	if (slot < 0 || slot >= EMU_OVL_MAX_SLOTS)
		return -1;
	if (ovl->screenshot_dir[0] == '\0' || ovl->rom_file[0] == '\0')
		return -1;

	char path[512];
	get_slot_screenshot_path(ovl, slot, path, sizeof(path));
	int ret = ovl->render->save_captured_frame(path);

	// Write resume slot file for game switcher
	if (ret == 0)
		write_resume_slot(ovl, slot);

	return ret;
}
