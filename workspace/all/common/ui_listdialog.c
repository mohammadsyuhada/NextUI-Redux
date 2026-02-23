#include "ui_listdialog.h"
#include "ui_list.h"
#include "ui_components.h"
#include "api.h"
#include "defines.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static char dialog_title[128];
static char dialog_status[128];
static ListDialogItem dialog_items[LISTDIALOG_MAX_ITEMS];
static int dialog_count;
static int dialog_selected;
static int dialog_scroll;

void ListDialog_init(const char* title) {
	strncpy(dialog_title, title, sizeof(dialog_title) - 1);
	dialog_title[sizeof(dialog_title) - 1] = '\0';
	dialog_status[0] = '\0';
	dialog_count = 0;
	dialog_selected = 0;
	dialog_scroll = 0;
	memset(dialog_items, 0, sizeof(dialog_items));
}

void ListDialog_setItems(ListDialogItem* items, int count) {
	if (count > LISTDIALOG_MAX_ITEMS)
		count = LISTDIALOG_MAX_ITEMS;
	if (items != NULL && count > 0)
		memcpy(dialog_items, items, count * sizeof(ListDialogItem));
	dialog_count = count;

	if (dialog_selected >= dialog_count)
		dialog_selected = dialog_count > 0 ? dialog_count - 1 : 0;
	if (dialog_scroll > dialog_selected)
		dialog_scroll = dialog_selected;
}

void ListDialog_setStatus(const char* status) {
	if (status) {
		strncpy(dialog_status, status, sizeof(dialog_status) - 1);
		dialog_status[sizeof(dialog_status) - 1] = '\0';
	} else {
		dialog_status[0] = '\0';
	}
}

ListDialogResult ListDialog_handleInput(void) {
	ListDialogResult result = {LISTDIALOG_NONE, -1};

	if (PAD_justPressed(BTN_B)) {
		result.action = LISTDIALOG_CANCEL;
		return result;
	}

	if (dialog_count == 0)
		return result;

	if (PAD_justPressed(BTN_A)) {
		result.action = LISTDIALOG_SELECTED;
		result.index = dialog_selected;
		return result;
	}

	PAD_navigateMenu(&dialog_selected, dialog_count);

	return result;
}

// Calculate total width of an icon array (-1 terminated)
static int calc_icons_width(const int* icons) {
	int w = 0;
	for (int i = 0; i < LISTDIALOG_MAX_ICONS && icons[i] >= 0; i++) {
		if (w > 0)
			w += SCALE1(BUTTON_MARGIN);
		SDL_Rect r;
		GFX_assetRect(icons[i], &r);
		w += r.w;
	}
	return w;
}

// Render an icon array horizontally, returns x after last icon
static int render_icons(SDL_Surface* screen, const int* icons,
						int x, int center_y, uint32_t color) {
	for (int i = 0; i < LISTDIALOG_MAX_ICONS && icons[i] >= 0; i++) {
		if (i > 0)
			x += SCALE1(BUTTON_MARGIN);
		SDL_Rect r;
		GFX_assetRect(icons[i], &r);
		int iy = center_y - r.h / 2;
		GFX_blitAssetColor(icons[i], NULL, screen,
						   &(SDL_Rect){x, iy, 0, 0}, color);
		x += r.w;
	}
	return x;
}

// Layout: [prepend_icons] Title ... [append_icons or detail text]
static void render_item(SDL_Surface* screen, ListLayout* layout,
						ListDialogItem* item, int y, bool selected) {
	// Measure prepend icons
	int prepend_w = calc_icons_width(item->prepend_icons);
	int prepend_gap = prepend_w > 0 ? SCALE1(BUTTON_MARGIN) : 0;

	// Measure append icons or detail text
	bool has_append = (item->append_icons[0] >= 0);
	int suffix_w = 0;
	if (has_append) {
		suffix_w = calc_icons_width(item->append_icons);
	} else if (item->detail[0]) {
		TTF_SizeUTF8(font.small, item->detail, &suffix_w, NULL);
	}
	int suffix_gap = suffix_w > 0 ? SCALE1(BUTTON_MARGIN) : 0;

	// Calculate pill width (extra space for prepend + suffix)
	int extra = prepend_w + prepend_gap + suffix_w + suffix_gap;
	char truncated[256];
	int pill_width = UI_calcListPillWidth(font.small, item->text, truncated,
										  layout->max_width, extra);

	// Draw rounded rect background for selected item
	if (selected) {
		int px = SCALE1(PADDING);
		int pw = pill_width;
		int h = layout->item_h;
		int r = h / 3;
		if (r > pw / 2)
			r = pw / 2;
		if (h - 2 * r > 0)
			SDL_FillRect(screen, &(SDL_Rect){px, y + r, pw, h - 2 * r}, THEME_COLOR1);
		for (int dy = 0; dy < r; dy++) {
			int yd = r - dy;
			int inset = r - (int)sqrtf((float)(r * r - yd * yd));
			int row_w = pw - 2 * inset;
			if (row_w <= 0)
				continue;
			SDL_FillRect(screen, &(SDL_Rect){px + inset, y + dy, row_w, 1}, THEME_COLOR1);
			SDL_FillRect(screen, &(SDL_Rect){px + inset, y + h - 1 - dy, row_w, 1}, THEME_COLOR1);
		}
	}

	int text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
	int text_y = y + (layout->item_h - TTF_FontHeight(font.small)) / 2;
	int center_y_pos = y + layout->item_h / 2;
	uint32_t icon_color = selected ? THEME_COLOR5 : THEME_COLOR4;

	// Prepend icons (left side, before title)
	if (prepend_w > 0) {
		render_icons(screen, item->prepend_icons, text_x, center_y_pos, icon_color);
		text_x += prepend_w + prepend_gap;
	}

	// Title text
	SDL_Color text_color = UI_getListTextColor(selected);
	int max_text_w = pill_width - SCALE1(BUTTON_PADDING * 2) - extra;
	SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font.small, truncated, text_color);
	if (text_surf) {
		SDL_Rect src = {0, 0, text_surf->w > max_text_w ? max_text_w : text_surf->w, text_surf->h};
		SDL_BlitSurface(text_surf, &src, screen,
						&(SDL_Rect){text_x, text_y, 0, 0});
		SDL_FreeSurface(text_surf);
	}

	// Append icons or detail text (right-aligned)
	int right_x = SCALE1(PADDING) + pill_width - SCALE1(BUTTON_PADDING) - suffix_w;

	if (has_append) {
		render_icons(screen, item->append_icons, right_x, center_y_pos, icon_color);
	} else if (item->detail[0]) {
		SDL_Color detail_color = selected ? UI_getListTextColor(true) : COLOR_GRAY;
		SDL_Surface* detail_surf = TTF_RenderUTF8_Blended(font.small, item->detail, detail_color);
		if (detail_surf) {
			int dy = center_y_pos - detail_surf->h / 2;
			SDL_BlitSurface(detail_surf, NULL, screen,
							&(SDL_Rect){right_x, dy, 0, 0});
			SDL_FreeSurface(detail_surf);
		}
	}
}

void ListDialog_render(SDL_Surface* screen) {
	GFX_clearLayers(LAYER_SCROLLTEXT);
	SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));

	int hw = screen->w;

	UI_renderMenuBar(screen, dialog_title);

	// Status text (if set and no items)
	if (dialog_status[0] && dialog_count == 0) {
		int status_h = TTF_FontHeight(font.small);
		int status_y = (screen->h - status_h) / 2;
		SDL_Surface* status_surf = TTF_RenderUTF8_Blended(font.small, dialog_status, COLOR_GRAY);
		if (status_surf) {
			int status_x = (hw - status_surf->w) / 2;
			SDL_BlitSurface(status_surf, NULL, screen,
							&(SDL_Rect){status_x, status_y, 0, 0});
			SDL_FreeSurface(status_surf);
		}
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		return;
	}

	if (dialog_count == 0) {
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		return;
	}

	// Calculate layout
	ListLayout layout;
	layout.list_y = SCALE1(PADDING + PILL_SIZE) + 10;
	layout.item_h = SCALE1(PILL_SIZE) * 3 / 4;
	layout.list_h = screen->h - layout.list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
	layout.items_per_page = layout.list_h / layout.item_h;
	layout.max_width = hw - SCALE1(PADDING * 2);

	UI_adjustListScroll(dialog_selected, &dialog_scroll, layout.items_per_page);

	int end = dialog_scroll + layout.items_per_page;
	if (end > dialog_count)
		end = dialog_count;

	for (int i = dialog_scroll; i < end; i++) {
		bool selected = (i == dialog_selected);
		int y = layout.list_y + (i - dialog_scroll) * layout.item_h;
		ListDialogItem* item = &dialog_items[i];

		render_item(screen, &layout, item, y, selected);
	}

	UI_renderScrollIndicators(screen, dialog_scroll, layout.items_per_page, dialog_count);
	UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "SELECT", NULL});
}

void ListDialog_quit(void) {
	dialog_count = 0;
	dialog_selected = 0;
	dialog_scroll = 0;
	dialog_title[0] = '\0';
	dialog_status[0] = '\0';
}
