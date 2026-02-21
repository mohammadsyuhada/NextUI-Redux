#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "settings_menu.h"
#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "ui_list.h"

// ============================================
// Page Stack
// ============================================

#define MAX_PAGE_DEPTH 8
static SettingsPage* page_stack[MAX_PAGE_DEPTH];
static int stack_depth = 0;

void settings_menu_init(void) {
	stack_depth = 0;
	memset(page_stack, 0, sizeof(page_stack));
}

void settings_menu_push(SettingsPage* page) {
	if (stack_depth >= MAX_PAGE_DEPTH)
		return;
	if (page->on_show)
		page->on_show(page);
	page_stack[stack_depth++] = page;
}

void settings_menu_pop(void) {
	if (stack_depth <= 0)
		return;
	SettingsPage* page = page_stack[stack_depth - 1];
	if (page->on_hide)
		page->on_hide(page);
	stack_depth--;
}

SettingsPage* settings_menu_current(void) {
	if (stack_depth <= 0)
		return NULL;
	return page_stack[stack_depth - 1];
}

int settings_menu_depth(void) {
	return stack_depth;
}

// ============================================
// Visible Item Helpers
// ============================================

int settings_page_visible_count(SettingsPage* page) {
	int count = 0;
	for (int i = 0; i < page->item_count; i++) {
		if (page->items[i].visible)
			count++;
	}
	return count;
}

SettingItem* settings_page_visible_item(SettingsPage* page, int visible_idx) {
	int count = 0;
	for (int i = 0; i < page->item_count; i++) {
		if (!page->items[i].visible)
			continue;
		if (count == visible_idx)
			return &page->items[i];
		count++;
	}
	return NULL;
}

int settings_page_visible_to_actual(SettingsPage* page, int visible_idx) {
	int count = 0;
	for (int i = 0; i < page->item_count; i++) {
		if (!page->items[i].visible)
			continue;
		if (count == visible_idx)
			return i;
		count++;
	}
	return -1;
}

int settings_page_actual_to_visible(SettingsPage* page, int actual_idx) {
	int count = 0;
	for (int i = 0; i < page->item_count; i++) {
		if (!page->items[i].visible)
			continue;
		if (i == actual_idx)
			return count;
		count++;
	}
	return -1;
}

// ============================================
// Item Sync & Reset
// ============================================

void settings_item_sync(SettingItem* item) {
	if ((item->type != ITEM_CYCLE && item->type != ITEM_COLOR) || !item->get_value)
		return;

	int val = item->get_value();

	if (item->values) {
		// Find matching value in values array
		for (int i = 0; i < item->label_count; i++) {
			if (item->values[i] == val) {
				item->current_idx = i;
				return;
			}
		}
	} else {
		// Direct mapping: idx = value
		if (val >= 0 && val < item->label_count)
			item->current_idx = val;
	}
}

void settings_page_reset_all(SettingsPage* page) {
	for (int i = 0; i < page->item_count; i++) {
		SettingItem* item = &page->items[i];
		if (item->on_reset) {
			item->on_reset();
			settings_item_sync(item);
		}
	}
}

void settings_page_init_lock(SettingsPage* page) {
	pthread_rwlock_init(&page->lock, NULL);
}

void settings_page_destroy(SettingsPage* page) {
	if (page->dynamic_start >= 0)
		pthread_rwlock_destroy(&page->lock);
}

// ============================================
// Cycle item value change
// ============================================

static void cycle_item_next(SettingItem* item, int step) {
	if ((item->type != ITEM_CYCLE && item->type != ITEM_COLOR) || item->label_count <= 0)
		return;

	item->current_idx = (item->current_idx + step) % item->label_count;

	int val = item->values ? item->values[item->current_idx] : item->current_idx;
	if (item->set_value)
		item->set_value(val);
}

static void cycle_item_prev(SettingItem* item, int step) {
	if ((item->type != ITEM_CYCLE && item->type != ITEM_COLOR) || item->label_count <= 0)
		return;

	item->current_idx = ((item->current_idx - step) % item->label_count + item->label_count) % item->label_count;

	int val = item->values ? item->values[item->current_idx] : item->current_idx;
	if (item->set_value)
		item->set_value(val);
}

// ============================================
// Input Handling
// ============================================

void settings_menu_handle_input(bool* quit, bool* dirty) {
	SettingsPage* page = settings_menu_current();
	if (!page) {
		*quit = true;
		return;
	}

	int has_lock = (page->dynamic_start >= 0);
	if (has_lock)
		pthread_rwlock_rdlock(&page->lock);

	int vis_count = settings_page_visible_count(page);

	// Redraw when dynamic page has pending updates (scanner or async toggle)
	if (page->needs_layout)
		*dirty = true;

	// Tick callback (for dynamic pages)
	if (page->on_tick)
		page->on_tick(page);

	if (vis_count == 0) {
		if (has_lock)
			pthread_rwlock_unlock(&page->lock);

		// Allow back/exit even with no items
		if (PAD_justPressed(BTN_B)) {
			settings_menu_pop();
			*dirty = true;
			if (stack_depth <= 0)
				*quit = true;
		}
		return;
	}

	// Clamp selection
	if (page->selected >= vis_count)
		page->selected = vis_count - 1;
	if (page->selected < 0)
		page->selected = 0;

	SettingItem* sel = settings_page_visible_item(page, page->selected);
	int changed = 0;

	// Navigation
	if (PAD_justRepeated(BTN_UP)) {
		page->selected--;
		if (page->selected < 0)
			page->selected = vis_count - 1;
		changed = 1;
	}
	if (PAD_justRepeated(BTN_DOWN)) {
		page->selected++;
		if (page->selected >= vis_count)
			page->selected = 0;
		changed = 1;
	}

	// Value cycling (disabled when input_blocked)
	if (!page->input_blocked && sel && (sel->type == ITEM_CYCLE || sel->type == ITEM_COLOR)) {
		int r1 = PAD_justRepeated(BTN_R1);
		int l1 = PAD_justRepeated(BTN_L1);
		int step = (r1 || l1) ? 10 : 1;

		if (PAD_justRepeated(BTN_RIGHT) || r1) {
			cycle_item_next(sel, step);
			changed = 1;
		}
		if (PAD_justRepeated(BTN_LEFT) || l1) {
			cycle_item_prev(sel, step);
			changed = 1;
		}
	}

	if (has_lock)
		pthread_rwlock_unlock(&page->lock);

	// Confirm (A button)
	if (sel && PAD_justPressed(BTN_A)) {
		switch (sel->type) {
		case ITEM_BUTTON:
			if (sel->on_press)
				sel->on_press();
			changed = 1;
			break;
		case ITEM_SUBMENU:
			// Support lazy page creation: if submenu is NULL but on_press
			// and user_data are set, call on_press to create the page,
			// then read the result from user_data (a SettingsPage** pointer)
			if (!sel->submenu && sel->on_press && sel->user_data) {
				sel->on_press();
				sel->submenu = *(SettingsPage**)sel->user_data;
			}
			if (sel->submenu) {
				settings_menu_push(sel->submenu);
				changed = 1;
			}
			break;
		case ITEM_TEXT_INPUT: {
			// Use external keyboard binary
			extern char* UIKeyboard_open(const char* prompt);
			char* result = UIKeyboard_open(sel->name);
			if (result) {
				strncpy(sel->text_value, result, sizeof(sel->text_value) - 1);
				sel->text_value[sizeof(sel->text_value) - 1] = '\0';
				if (sel->on_text_set)
					sel->on_text_set(result);
				free(result);
			}
			// Clear input state so the B press from keyboard doesn't propagate
			PAD_reset();
			changed = 1;
			break;
		}
		default:
			break;
		}
	}

	// Back (B button)
	if (PAD_justPressed(BTN_B)) {
		settings_menu_pop();
		changed = 1;
		if (stack_depth <= 0)
			*quit = true;
	}

	if (changed)
		*dirty = true;
}

// ============================================
// Rendering: Category List Mode
// ============================================

static void render_list_mode(SDL_Surface* screen, SettingsPage* page, ListLayout* layout) {
	int vis_count = settings_page_visible_count(page);
	if (vis_count == 0)
		return;

	UI_adjustListScroll(page->selected, &page->scroll, layout->items_per_page);

	char truncated[256];
	int start = page->scroll;
	int end = start + layout->items_per_page;
	if (end > vis_count)
		end = vis_count;

	for (int vi = start; vi < end; vi++) {
		SettingItem* item = settings_page_visible_item(page, vi);
		if (!item)
			continue;

		int selected = (vi == page->selected);
		int y = layout->list_y + (vi - start) * layout->item_h;

		const char* text = item->name;
		if (item->type == ITEM_STATIC && item->get_display)
			text = item->get_display();

		ListItemPos pos = UI_renderListItemPill(screen, layout, font.large, text,
												truncated, y, selected, 0);

		SDL_Color text_color = UI_getListTextColor(selected);
		SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font.large, truncated, text_color);
		if (text_surf) {
			SDL_BlitSurface(text_surf, NULL, screen, &(SDL_Rect){pos.text_x, pos.text_y, 0, 0});
			SDL_FreeSurface(text_surf);
		}
	}

	UI_renderScrollIndicators(screen, page->scroll, layout->items_per_page, vis_count);
}

// ============================================
// Rendering: Settings Page Mode (2-layer pills)
// ============================================

static void render_settings_mode(SDL_Surface* screen, SettingsPage* page, ListLayout* layout) {
	int vis_count = settings_page_visible_count(page);
	if (vis_count == 0)
		return;

	int hw = screen->w;

	UI_adjustListScroll(page->selected, &page->scroll, layout->items_per_page);

	int start = page->scroll;
	int end = start + layout->items_per_page;
	if (end > vis_count)
		end = vis_count;

	for (int vi = start; vi < end; vi++) {
		SettingItem* item = settings_page_visible_item(page, vi);
		if (!item)
			continue;

		int selected = (vi == page->selected);
		int item_y = layout->list_y + (vi - start) * layout->item_h;

		// Custom draw override
		if (item->custom_draw) {
			item->custom_draw(screen, item, SCALE1(PADDING), item_y,
							  hw - SCALE1(PADDING * 2), layout->item_h, selected);
			continue;
		}

		// Build value string
		const char* label = item->name;
		const char* value_str = NULL;

		if ((item->type == ITEM_CYCLE || item->type == ITEM_COLOR) &&
			item->labels && item->current_idx >= 0 &&
			item->current_idx < item->label_count) {
			value_str = item->labels[item->current_idx];
		} else if (item->type == ITEM_STATIC) {
			if (item->get_display)
				value_str = item->get_display();
			else if (item->display_text[0])
				value_str = item->display_text;
		} else if (item->type == ITEM_TEXT_INPUT) {
			if (item->get_text)
				value_str = item->get_text();
			else if (item->text_value[0])
				value_str = item->text_value;
		}

		// Color swatch for ITEM_COLOR
		int swatch = -1;
		if (item->type == ITEM_COLOR && item->values &&
			item->current_idx >= 0 && item->current_idx < item->label_count) {
			swatch = (int)(uint32_t)item->values[item->current_idx];
		}

		// Format display value (add arrows for cycleable items when selected)
		char display_val[256];
		const char* display_ptr = NULL;
		if (value_str) {
			if (selected && (item->type == ITEM_CYCLE || item->type == ITEM_COLOR))
				snprintf(display_val, sizeof(display_val), "< %s >", value_str);
			else
				snprintf(display_val, sizeof(display_val), "%s", value_str);
			display_ptr = display_val;
		}

		UI_renderSettingsRow(screen, layout, label, display_ptr,
							 item_y, selected, swatch);
	}

	// Scroll indicators
	UI_renderScrollIndicators(screen, page->scroll, layout->items_per_page, vis_count);

	// Status message centered below items (e.g. "Scanning for networks...")
	if (page->status_msg && page->status_msg[0] && vis_count < layout->items_per_page) {
		int msg_row_y = layout->list_y + vis_count * layout->item_h;
		int empty_h = (layout->items_per_page - vis_count) * layout->item_h;
		int msg_y = msg_row_y + (empty_h - TTF_FontHeight(font.small)) / 2;
		SDL_Surface* msg_surf = TTF_RenderUTF8_Blended(font.small, page->status_msg, COLOR_GRAY);
		if (msg_surf) {
			int msg_x = (hw - msg_surf->w) / 2;
			SDL_BlitSurface(msg_surf, NULL, screen, &(SDL_Rect){msg_x, msg_y, 0, 0});
			SDL_FreeSurface(msg_surf);
		}
	}

	// Description text in the last row (row 9)
	SettingItem* sel = settings_page_visible_item(page, page->selected);
	if (sel && sel->desc && sel->desc[0]) {
		int desc_row_y = layout->list_y + layout->items_per_page * layout->item_h;
		int desc_y = desc_row_y + (layout->item_h - TTF_FontHeight(font.tiny)) / 2;
		int desc_max_w = hw - SCALE1(PADDING * 2);

		char truncated_desc[256];
		GFX_truncateText(font.tiny, sel->desc, truncated_desc, desc_max_w, 0);

		SDL_Surface* desc_surf = TTF_RenderUTF8_Blended(font.tiny, truncated_desc, COLOR_GRAY);
		if (desc_surf) {
			int desc_x = (hw - desc_surf->w) / 2;
			SDL_BlitSurface(desc_surf, NULL, screen, &(SDL_Rect){desc_x, desc_y, 0, 0});
			SDL_FreeSurface(desc_surf);
		}
	}
}

// ============================================
// Button Hint Bar helpers
// ============================================

static void render_hints_for_page(SDL_Surface* screen, SettingsPage* page) {
	SettingItem* sel = settings_page_visible_item(page, page->selected);
	int is_root = (stack_depth <= 1);

	char* back_label = is_root ? "EXIT" : "BACK";
	char* hints[8] = {NULL};

	if (!sel) {
		char* h[] = {"B", back_label, NULL};
		memcpy(hints, h, sizeof(h));
	} else if (page->is_list) {
		char* h[] = {"B", back_label, "A", "OPEN", NULL};
		memcpy(hints, h, sizeof(h));
	} else {
		switch (sel->type) {
		case ITEM_CYCLE:
		case ITEM_COLOR: {
			char* h[] = {"LEFT/RIGHT", "CHANGE", "B", back_label, NULL};
			memcpy(hints, h, sizeof(h));
			break;
		}
		case ITEM_BUTTON: {
			char* h[] = {"B", back_label, "A", "SELECT", NULL};
			memcpy(hints, h, sizeof(h));
			break;
		}
		case ITEM_SUBMENU: {
			char* h[] = {"B", back_label, "A", "OPEN", NULL};
			memcpy(hints, h, sizeof(h));
			break;
		}
		case ITEM_TEXT_INPUT: {
			char* h[] = {"B", back_label, "A", "EDIT", NULL};
			memcpy(hints, h, sizeof(h));
			break;
		}
		default: {
			char* h[] = {"B", back_label, NULL};
			memcpy(hints, h, sizeof(h));
			break;
		}
		}
	}

	UI_renderButtonHintBar(screen, hints);
}

// ============================================
// Main Render
// ============================================

void settings_menu_render(SDL_Surface* screen, IndicatorType show_setting) {
	SettingsPage* page = settings_menu_current();
	if (!page)
		return;

	GFX_clear(screen);

	// Menu bar at top
	UI_renderMenuBar(screen, page->title);

	// Calculate list layout
	ListLayout layout = UI_calcListLayout(screen);

	// For settings pages: use compact rows (9 rows: 8 items + 1 description)
	if (!page->is_list) {
		int total_rows = 9;
		layout.item_h = layout.list_h / total_rows;
		layout.items_per_page = total_rows - 1; // last row reserved for description
	}

	int has_lock = (page->dynamic_start >= 0);
	if (has_lock)
		pthread_rwlock_rdlock(&page->lock);

	// Render items
	if (page->is_list)
		render_list_mode(screen, page, &layout);
	else
		render_settings_mode(screen, page, &layout);

	if (has_lock)
		pthread_rwlock_unlock(&page->lock);

	// Button hints at bottom
	render_hints_for_page(screen, page);
}
