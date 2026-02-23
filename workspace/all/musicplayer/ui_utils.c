#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ui_components.h"
#include "ui_utils.h"
#include "ui_fonts.h"
#include "ui_icons.h"
#include "module_common.h"

// Format duration as MM:SS
void format_time(char* buf, int ms) {
	int total_secs = ms / 1000;
	int mins = total_secs / 60;
	int secs = total_secs % 60;
	sprintf(buf, "%02d:%02d", mins, secs);
}

// Get format name string
const char* get_format_name(AudioFormat format) {
	switch (format) {
	case AUDIO_FORMAT_MP3:
		return "MP3";
	case AUDIO_FORMAT_FLAC:
		return "FLAC";
	case AUDIO_FORMAT_OGG:
		return "OGG";
	case AUDIO_FORMAT_WAV:
		return "WAV";
	case AUDIO_FORMAT_MOD:
		return "MOD";
	case AUDIO_FORMAT_M4A:
		return "M4A";
	case AUDIO_FORMAT_AAC:
		return "AAC";
	case AUDIO_FORMAT_OPUS:
		return "OPUS";
	default:
		return "---";
	}
}

#define SCROLL_GAP 30

// GPU scroll without background (for player title)
// Uses PLAT_drawOnLayer to render to GPU layer without pill background
void ScrollText_renderGPU_NoBg(ScrollTextState* state, TTF_Font* font,
							   SDL_Color color, int x, int y) {
	if (!state->text[0] || !state->needs_scroll || !state->cached_scroll_surface) {
		// Static text or no scroll needed - just clear layer
		PLAT_clearLayers(LAYER_SCROLLTEXT);
		return;
	}

	// Save render info
	state->last_x = x;
	state->last_y = y;
	state->last_font = font;
	state->last_color = color;

	int padding = SCALE1(SCROLL_GAP);
	int height = state->cached_scroll_surface->h;

	// Create clipped view at current scroll offset (only this is created per-frame)
	SDL_Surface* clipped = SDL_CreateRGBSurfaceWithFormat(0,
														  state->max_width, height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!clipped)
		return;

	SDL_FillRect(clipped, NULL, 0);
	SDL_SetSurfaceBlendMode(state->cached_scroll_surface, SDL_BLENDMODE_NONE);
	SDL_Rect src = {state->scroll_offset, 0, state->max_width, height};
	SDL_BlitSurface(state->cached_scroll_surface, &src, clipped, NULL);

	// Render to GPU layer
	PLAT_clearLayers(LAYER_SCROLLTEXT);
	PLAT_drawOnLayer(clipped, x, y, state->max_width, height, 1.0f, false, LAYER_SCROLLTEXT);
	SDL_FreeSurface(clipped);

	// Advance scroll offset (1 pixel per frame for smooth, slower scrolling)
	state->scroll_offset += 1;
	if (state->scroll_offset >= state->text_width + padding) {
		state->scroll_offset = 0;
	}

	PLAT_GPU_Flip();
}

// Render standard screen header (title pill + hardware status)

// Adjust scroll offset to keep selected item visible
void adjust_list_scroll(int selected, int* scroll, int items_per_page) {
	if (selected < *scroll) {
		*scroll = selected;
	}
	if (selected >= *scroll + items_per_page) {
		*scroll = selected - items_per_page + 1;
	}
}

// Render scroll up/down indicators for lists
void render_scroll_indicators(SDL_Surface* screen, int scroll, int items_per_page, int total_count) {
	if (total_count <= items_per_page)
		return;

	int hw = screen->w;
	int hh = screen->h;
	int ox = (hw - SCALE1(24)) / 2;

	if (scroll > 0) {
		// Position just below header with gap from first item
		GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE - BUTTON_MARGIN)});
	}
	if (scroll + items_per_page < total_count) {
		// Position at the end of the list area (just above button hints)
		GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN)});
	}
}

// ============================================
// Generic List Rendering Helpers
// ============================================

// Render a 2-row list item pill with optional right-side badge area
// Height is 1.5x PILL_SIZE (same as rich). Title (medium) + subtitle (small) inside pill.
// When badge_width > 0 and selected: THEME_COLOR2 outer capsule + THEME_COLOR1 inner capsule
// When badge_width == 0: single THEME_COLOR1 capsule
ListItemBadgedPos render_list_item_pill_badged(SDL_Surface* screen, ListLayout* layout,
											   const char* text, const char* subtitle,
											   char* truncated,
											   int y, bool selected, int badge_width,
											   int extra_subtitle_width) {
	ListItemBadgedPos pos;

	int item_h = SCALE1(PILL_SIZE) * 3 / 2;

	// Badge area: badge content + BUTTON_PADDING on each side
	int badge_area_w = badge_width > 0 ? badge_width + SCALE1(BUTTON_PADDING * 2) : 0;

	// Calculate title pill width (reduced max to leave room for badge area)
	int title_max_width = layout->max_width - badge_area_w;
	pos.pill_width = Fonts_calcListPillWidth(font.medium, text, truncated, title_max_width, 0);

	// Expand pill if subtitle is wider than title
	if (subtitle && subtitle[0]) {
		int sub_w;
		TTF_SizeUTF8(font.small, subtitle, &sub_w, NULL);
		sub_w += extra_subtitle_width;
		int sub_pill_w = MIN(title_max_width, sub_w + SCALE1(BUTTON_PADDING * 2));
		if (sub_pill_w > pos.pill_width)
			pos.pill_width = sub_pill_w;
	}

	if (selected) {
		int px = SCALE1(PADDING);

		if (badge_area_w > 0) {
			// Layer 1: THEME_COLOR2 outer capsule covering title + badge area
			int total_w = pos.pill_width + badge_area_w;
			int r = item_h / 3;
			if (r > total_w / 2)
				r = total_w / 2;
			if (item_h - 2 * r > 0) {
				SDL_FillRect(screen, &(SDL_Rect){px, y + r, total_w, item_h - 2 * r}, THEME_COLOR2);
			}
			for (int dy = 0; dy < r; dy++) {
				int yd = r - dy;
				int inset = r - (int)sqrtf((float)(r * r - yd * yd));
				int row_w = total_w - 2 * inset;
				if (row_w <= 0)
					continue;
				SDL_FillRect(screen, &(SDL_Rect){px + inset, y + dy, row_w, 1}, THEME_COLOR2);
				SDL_FillRect(screen, &(SDL_Rect){px + inset, y + item_h - 1 - dy, row_w, 1}, THEME_COLOR2);
			}
		}

		// Layer 2 (or only layer): THEME_COLOR1 inner capsule for title area
		{
			int pw = pos.pill_width;
			int r = item_h / 3;
			if (r > pw / 2)
				r = pw / 2;
			if (item_h - 2 * r > 0) {
				SDL_FillRect(screen, &(SDL_Rect){px, y + r, pw, item_h - 2 * r}, THEME_COLOR1);
			}
			for (int dy = 0; dy < r; dy++) {
				int yd = r - dy;
				int inset = r - (int)sqrtf((float)(r * r - yd * yd));
				int row_w = pw - 2 * inset;
				if (row_w <= 0)
					continue;
				SDL_FillRect(screen, &(SDL_Rect){px + inset, y + dy, row_w, 1}, THEME_COLOR1);
				SDL_FillRect(screen, &(SDL_Rect){px + inset, y + item_h - 1 - dy, row_w, 1}, THEME_COLOR1);
			}
		}
	}

	// Text positions: two rows vertically centered (like rich)
	int text_start_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
	int medium_h = TTF_FontHeight(font.medium);
	int small_h = TTF_FontHeight(font.small);
	int total_text_h = medium_h + small_h;
	int top_gap = (item_h - total_text_h) / 2;

	pos.text_x = text_start_x;
	pos.text_y = y + top_gap;

	pos.subtitle_x = text_start_x;
	pos.subtitle_y = y + top_gap + medium_h;

	// Badge position (centered vertically in capsule)
	pos.badge_x = SCALE1(PADDING) + pos.pill_width + SCALE1(BUTTON_PADDING);
	pos.badge_y = y + (item_h - TTF_FontHeight(font.tiny)) / 2;

	// Account for right-side capsule radius reducing usable text width
	int r = item_h / 2;
	pos.text_max_width = pos.pill_width - SCALE1(BUTTON_PADDING) - r / 2;

	pos.total_width = pos.pill_width + badge_area_w;

	return pos;
}

// Render a 2-row list item pill with image area on the left
// Height is 1.5x PILL_SIZE, fits 4 items per page
ListItemRichPos render_list_item_pill_rich(SDL_Surface* screen, ListLayout* layout,
										   const char* title, const char* subtitle,
										   char* truncated,
										   int y, bool selected, bool has_image,
										   int extra_subtitle_width) {
	ListItemRichPos pos;

	int item_h = SCALE1(PILL_SIZE) * 3 / 2;
	int img_padding = SCALE1(4);

	// Image area: only reserve space when image is available
	int image_area_w;
	if (has_image) {
		pos.image_size = item_h - img_padding * 2;
		image_area_w = img_padding + pos.image_size + SCALE1(BUTTON_PADDING);
		pos.image_x = SCALE1(PADDING) + img_padding;
		pos.image_y = y + img_padding;
	} else {
		pos.image_size = 0;
		image_area_w = SCALE1(BUTTON_PADDING); // Just left text padding
		pos.image_x = 0;
		pos.image_y = 0;
	}

	// Calculate pill width considering both title and subtitle
	pos.pill_width = Fonts_calcListPillWidth(font.medium, title, truncated, layout->max_width, image_area_w);
	if (subtitle && subtitle[0]) {
		int sub_w;
		TTF_SizeUTF8(font.small, subtitle, &sub_w, NULL);
		int sub_pill_w = MIN(layout->max_width, image_area_w + sub_w + extra_subtitle_width + SCALE1(BUTTON_PADDING * 2));
		if (sub_pill_w > pos.pill_width)
			pos.pill_width = sub_pill_w;
	}

	// Draw background (rounded rectangle with reduced radius)
	if (selected) {
		int px = SCALE1(PADDING);
		int pw = pos.pill_width;
		int r = item_h / 3;
		if (r > pw / 2)
			r = pw / 2;

		// Main body between corner rows
		if (item_h - 2 * r > 0) {
			SDL_FillRect(screen, &(SDL_Rect){px, y + r, pw, item_h - 2 * r}, THEME_COLOR1);
		}
		// Top and bottom corner rows with circular inset
		for (int dy = 0; dy < r; dy++) {
			int yd = r - dy;
			int inset = r - (int)sqrtf((float)(r * r - yd * yd));
			int row_w = pw - 2 * inset;
			if (row_w <= 0)
				continue;
			SDL_FillRect(screen, &(SDL_Rect){px + inset, y + dy, row_w, 1}, THEME_COLOR1);
			SDL_FillRect(screen, &(SDL_Rect){px + inset, y + item_h - 1 - dy, row_w, 1}, THEME_COLOR1);
		}
	}

	// Text positions: two rows vertically centered
	int text_start_x = SCALE1(PADDING) + image_area_w;
	int medium_h = TTF_FontHeight(font.medium);
	int small_h = TTF_FontHeight(font.small);
	int total_text_h = medium_h + small_h;
	int top_gap = (item_h - total_text_h) / 2;

	pos.title_x = text_start_x;
	pos.title_y = y + top_gap;

	pos.subtitle_x = text_start_x;
	pos.subtitle_y = y + top_gap + medium_h;

	pos.text_max_width = pos.pill_width - image_area_w - SCALE1(BUTTON_PADDING);

	return pos;
}

// Render a menu item's pill background and calculate text position
// Menu items have larger spacing (PILL_SIZE + BUTTON_MARGIN) but pill height is just PILL_SIZE
// prefix_width: extra width to account for (e.g., icon)
MenuItemPos render_menu_item_pill(SDL_Surface* screen, ListLayout* layout,
								  const char* text, char* truncated,
								  int index, bool selected, int prefix_width) {
	MenuItemPos pos;

	int item_h = SCALE1(PILL_SIZE);
	pos.item_y = layout->list_y + index * item_h;

	// Calculate text width for pill sizing (include prefix_width for icon)
	pos.pill_width = Fonts_calcListPillWidth(font.large, text, truncated, layout->max_width - prefix_width, prefix_width);

	// Background pill (pill height is PILL_SIZE, not item_h)
	SDL_Rect pill_rect = {SCALE1(PADDING), pos.item_y, pos.pill_width, SCALE1(PILL_SIZE)};
	Fonts_drawListItemBg(screen, &pill_rect, selected);

	// Calculate text position (centered within PILL_SIZE, not item_h)
	pos.text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
	pos.text_y = pos.item_y + (SCALE1(PILL_SIZE) - TTF_FontHeight(font.large)) / 2;

	return pos;
}

// ============================================
// Rounded Rectangle Background
// ============================================

// Render a filled rounded rectangle with smooth circular corners.
// Corner radius is SCALE1(2), clamped to half the width/height.
// Works at any size — unlike pill asset which requires PILL_SIZE height.
void render_rounded_rect_bg(SDL_Surface* screen, int x, int y, int w, int h, uint32_t color) {
	int r = SCALE1(7);
	if (r > h / 2)
		r = h / 2;
	if (r > w / 2)
		r = w / 2;

	// Main body between corner rows (full width)
	if (h - 2 * r > 0) {
		SDL_FillRect(screen, &(SDL_Rect){x, y + r, w, h - 2 * r}, color);
	}

	// Top and bottom corner rows with circular inset
	for (int dy = 0; dy < r; dy++) {
		int yd = r - dy;
		int inset = r - (int)sqrtf((float)(r * r - yd * yd));
		int row_w = w - 2 * inset;
		if (row_w <= 0)
			continue;
		// Top row
		SDL_FillRect(screen, &(SDL_Rect){x + inset, y + dy, row_w, 1}, color);
		// Bottom row (mirrored)
		SDL_FillRect(screen, &(SDL_Rect){x + inset, y + h - 1 - dy, row_w, 1}, color);
	}
}

// ============================================
// Generic Simple Menu Rendering
// ============================================

// Render a simple menu with optional customization callbacks
void render_simple_menu(SDL_Surface* screen, IndicatorType show_setting, int menu_selected,
						const SimpleMenuConfig* config) {
	GFX_clear(screen);
	char truncated[256];
	char label_buffer[256];

	UI_renderMenuBar(screen, config->title);
	ListLayout layout = UI_calcListLayout(screen);

	// Calculate icon size and spacing (scale 24px icons to fit in PILL_SIZE)
	int icon_size = SCALE1(24);
	int icon_spacing = SCALE1(6);

	for (int i = 0; i < config->item_count; i++) {
		bool selected = (i == menu_selected);

		// Get label (use callback if provided)
		const char* label = config->items[i];
		if (config->get_label) {
			const char* custom = config->get_label(i, label, label_buffer, sizeof(label_buffer));
			if (custom)
				label = custom;
		}

		// Check if we have an icon for this item
		SDL_Surface* icon = NULL;
		int icon_offset = 0;
		if (config->get_icon) {
			icon = config->get_icon(i, selected);
			if (icon) {
				icon_offset = icon_size + icon_spacing;
			}
		}

		// Render pill and text (account for icon width in pill calculation)
		MenuItemPos pos = render_menu_item_pill(screen, &layout, label, truncated, i, selected, icon_offset);

		// Render icon if present (scale to display size)
		int text_x = pos.text_x;
		if (icon) {
			int icon_y = pos.item_y + (SCALE1(PILL_SIZE) - icon_size) / 2;
			SDL_Rect src_rect = {0, 0, icon->w, icon->h};
			SDL_Rect dst_rect = {pos.text_x, icon_y, icon_size, icon_size};
			SDL_BlitScaled(icon, &src_rect, screen, &dst_rect);
			text_x += icon_offset;
		}

		// Render text after icon (use custom callback if provided)
		bool custom_rendered = false;
		if (config->render_text) {
			custom_rendered = config->render_text(screen, i, selected,
												  text_x, pos.text_y, layout.max_width - icon_offset);
		}
		if (!custom_rendered) {
			UI_renderListItemText(screen, NULL, truncated, font.large,
								  text_x, pos.text_y, layout.max_width - icon_offset, selected);
		}

		// Render badge if callback provided
		if (config->render_badge) {
			config->render_badge(screen, i, selected, pos.item_y, SCALE1(PILL_SIZE));
		}
	}

	// Button hints
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", (char*)config->btn_b_label, "A", "OPEN", NULL});
}


// ============================================
// Dialog Box
// ============================================

DialogBox render_dialog_box(SDL_Surface* screen, int box_w, int box_h) {
	// Clear scroll text GPU layer so it doesn't show through the dialog
	GFX_clearLayers(LAYER_SCROLLTEXT);

	int hw = screen->w;
	int hh = screen->h;

	DialogBox db;
	db.box_w = box_w;
	db.box_h = box_h;
	db.box_x = (hw - box_w) / 2;
	db.box_y = (hh - box_h) / 2;
	db.content_x = db.box_x + SCALE1(15);
	db.content_w = box_w - SCALE1(30);

	// Dark background around dialog (covers entire screen)
	SDL_Rect top_area = {0, 0, hw, db.box_y};
	SDL_Rect bot_area = {0, db.box_y + box_h, hw, hh - db.box_y - box_h};
	SDL_Rect left_area = {0, db.box_y, db.box_x, box_h};
	SDL_Rect right_area = {db.box_x + box_w, db.box_y, hw - db.box_x - box_w, box_h};
	SDL_FillRect(screen, &top_area, RGB_BLACK);
	SDL_FillRect(screen, &bot_area, RGB_BLACK);
	SDL_FillRect(screen, &left_area, RGB_BLACK);
	SDL_FillRect(screen, &right_area, RGB_BLACK);

	// Box background
	SDL_FillRect(screen, &(SDL_Rect){db.box_x, db.box_y, box_w, box_h}, RGB_BLACK);

	// Box border
	SDL_FillRect(screen, &(SDL_Rect){db.box_x, db.box_y, box_w, SCALE1(2)}, RGB_WHITE);
	SDL_FillRect(screen, &(SDL_Rect){db.box_x, db.box_y + box_h - SCALE1(2), box_w, SCALE1(2)}, RGB_WHITE);
	SDL_FillRect(screen, &(SDL_Rect){db.box_x, db.box_y, SCALE1(2), box_h}, RGB_WHITE);
	SDL_FillRect(screen, &(SDL_Rect){db.box_x + box_w - SCALE1(2), db.box_y, SCALE1(2), box_h}, RGB_WHITE);

	return db;
}
