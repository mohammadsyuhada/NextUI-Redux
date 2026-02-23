#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ui_list.h"
#include "ui_components.h"

// Scroll gap for software scrolling
#define SCROLL_GAP 30

// Delay before scrolling starts (ms) - show static text first
#define SCROLL_START_DELAY 1000

// ============================================
// Scroll Text (marquee animation)
// ============================================

void ScrollText_reset(ScrollTextState* state, const char* text, TTF_Font* font, int max_width, bool use_gpu) {
	GFX_clearLayers(LAYER_SCROLLTEXT);

	if (state->cached_scroll_surface) {
		SDL_FreeSurface(state->cached_scroll_surface);
		state->cached_scroll_surface = NULL;
	}

	strncpy(state->text, text, sizeof(state->text) - 1);
	state->text[sizeof(state->text) - 1] = '\0';
	int text_h = 0;
	TTF_SizeUTF8(font, state->text, &state->text_width, &text_h);
	state->max_width = max_width;
	state->start_time = SDL_GetTicks();
	state->scroll_offset = 0;
	state->use_gpu_scroll = use_gpu;
	state->scroll_active = false;
	state->needs_scroll = false;

	if ((state->text_width > max_width) && use_gpu) {
		int padding = SCALE1(SCROLL_GAP);
		int total_width = state->text_width * 2 + padding;
		int height = TTF_FontHeight(font);

		state->cached_scroll_surface = SDL_CreateRGBSurfaceWithFormat(0,
																	  total_width, height, 32, SDL_PIXELFORMAT_ARGB8888);

		if (state->cached_scroll_surface) {
			SDL_FillRect(state->cached_scroll_surface, NULL, 0);

			SDL_Color white = {255, 255, 255, 255};
			SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font, state->text, white);
			if (text_surf) {
				SDL_SetSurfaceBlendMode(text_surf, SDL_BLENDMODE_NONE);
				SDL_BlitSurface(text_surf, NULL, state->cached_scroll_surface, &(SDL_Rect){0, 0, 0, 0});
				SDL_BlitSurface(text_surf, NULL, state->cached_scroll_surface, &(SDL_Rect){state->text_width + padding, 0, 0, 0});
				SDL_FreeSurface(text_surf);
			}
		}
	}
}

bool ScrollText_isScrolling(ScrollTextState* state) {
	return state->needs_scroll;
}

bool ScrollText_needsRender(ScrollTextState* state) {
	return state->text[0] && state->text_width > state->max_width && !state->needs_scroll;
}

void ScrollText_activateAfterDelay(ScrollTextState* state) {
	if (!state->needs_scroll && state->text_width > state->max_width &&
		SDL_GetTicks() - state->start_time >= SCROLL_START_DELAY) {
		state->needs_scroll = true;
	}
}

void ScrollText_animateOnly(ScrollTextState* state) {
	if (!state->text[0] || !state->needs_scroll || !state->use_gpu_scroll)
		return;
	if (!state->last_font)
		return;

	GFX_clearLayers(LAYER_SCROLLTEXT);
	GFX_scrollTextTexture(
		state->last_font,
		state->text,
		state->last_x, state->last_y,
		state->max_width,
		TTF_FontHeight(state->last_font),
		state->last_color,
		1.0f,
		NULL);
}

void ScrollText_render(ScrollTextState* state, TTF_Font* font, SDL_Color color,
					   SDL_Surface* screen, int x, int y) {
	if (!state->text[0])
		return;

	state->last_x = x;
	state->last_y = y;
	state->last_font = font;
	state->last_color = color;

	if (!state->needs_scroll && state->text_width > state->max_width &&
		SDL_GetTicks() - state->start_time >= SCROLL_START_DELAY) {
		if (state->use_gpu_scroll && !state->scroll_active) {
			GFX_resetScrollText();
			state->scroll_active = true;
		} else {
			state->needs_scroll = true;
		}
	}

	if (!state->needs_scroll) {
		GFX_clearLayers(LAYER_SCROLLTEXT);
		SDL_Surface* surf = TTF_RenderUTF8_Blended(font, state->text, color);
		if (surf) {
			SDL_Rect src = {0, 0, surf->w > state->max_width ? state->max_width : surf->w, surf->h};
			SDL_BlitSurface(surf, &src, screen, &(SDL_Rect){x, y, 0, 0});
			SDL_FreeSurface(surf);
		}
		return;
	}

	if (state->use_gpu_scroll) {
		GFX_clearLayers(LAYER_SCROLLTEXT);
		GFX_scrollTextTexture(
			font,
			state->text,
			x, y,
			state->max_width,
			TTF_FontHeight(font),
			color,
			1.0f,
			NULL);
	} else {
		GFX_clearLayers(LAYER_SCROLLTEXT);

		SDL_Surface* single_surf = TTF_RenderUTF8_Blended(font, state->text, color);
		if (!single_surf)
			return;

		SDL_Surface* full_surf = SDL_CreateRGBSurfaceWithFormat(0,
																state->text_width * 2 + SCROLL_GAP, single_surf->h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (!full_surf) {
			SDL_FreeSurface(single_surf);
			return;
		}

		SDL_FillRect(full_surf, NULL, 0);
		SDL_SetSurfaceBlendMode(single_surf, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(single_surf, NULL, full_surf, &(SDL_Rect){0, 0, 0, 0});
		SDL_BlitSurface(single_surf, NULL, full_surf, &(SDL_Rect){state->text_width + SCROLL_GAP, 0, 0, 0});
		SDL_FreeSurface(single_surf);

		state->scroll_offset += 2;
		if (state->scroll_offset >= state->text_width + SCROLL_GAP) {
			state->scroll_offset = 0;
		}

		SDL_SetSurfaceBlendMode(full_surf, SDL_BLENDMODE_BLEND);
		SDL_Rect src = {state->scroll_offset, 0, state->max_width, full_surf->h};
		SDL_Rect dst = {x, y, 0, 0};
		SDL_BlitSurface(full_surf, &src, screen, &dst);
		SDL_FreeSurface(full_surf);
	}
}

void ScrollText_update(ScrollTextState* state, const char* text, TTF_Font* font,
					   int max_width, SDL_Color color, SDL_Surface* screen, int x, int y, bool use_gpu) {
	if (strcmp(state->text, text) != 0) {
		ScrollText_reset(state, text, font, max_width, use_gpu);
	}
	ScrollText_render(state, font, color, screen, x, y);
}

// ============================================
// List Layout
// ============================================

ListLayout UI_calcListLayout(SDL_Surface* screen) {
	int hw = screen->w;
	int hh = screen->h;

	ListLayout layout;
	layout.list_y = SCALE1(PADDING + PILL_SIZE) + 10;
	layout.list_h = hh - layout.list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
	layout.item_h = SCALE1(PILL_SIZE);
	layout.items_per_page = layout.list_h / layout.item_h;
	layout.max_width = hw - SCALE1(PADDING * 2);

	return layout;
}

// ============================================
// Pill Rendering (stateless)
// ============================================

int UI_calcListPillWidth(TTF_Font* font, const char* text, char* truncated, int max_width, int prefix_width) {
	int available_width = max_width - prefix_width;
	int padding = SCALE1(BUTTON_PADDING * 2);

	int raw_text_w, raw_text_h;
	TTF_SizeUTF8(font, text, &raw_text_w, &raw_text_h);

	if (raw_text_w + padding > available_width) {
		GFX_truncateText(font, text, truncated, available_width, padding);
		return max_width;
	}

	strncpy(truncated, text, 255);
	truncated[255] = '\0';
	return MIN(max_width, prefix_width + raw_text_w + padding);
}

void UI_drawListItemBg(SDL_Surface* dst, SDL_Rect* rect, bool selected) {
	if (selected) {
		GFX_blitPillColor(ASSET_WHITE_PILL, dst, rect, THEME_COLOR1, RGB_WHITE);
	}
}

SDL_Color UI_getListTextColor(bool selected) {
	return selected ? uintToColour(THEME_COLOR5_255) : uintToColour(THEME_COLOR4_255);
}

ListItemPos UI_renderListItemPill(SDL_Surface* screen, ListLayout* layout,
								  TTF_Font* font, const char* text,
								  char* truncated, int y, bool selected,
								  int prefix_width) {
	ListItemPos pos;

	pos.pill_width = UI_calcListPillWidth(font, text, truncated, layout->max_width, prefix_width);

	SDL_Rect pill_rect = {SCALE1(PADDING), y, pos.pill_width, layout->item_h};
	UI_drawListItemBg(screen, &pill_rect, selected);

	pos.text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
	pos.text_y = y + (layout->item_h - TTF_FontHeight(font)) / 2;

	return pos;
}

void UI_renderListItemText(SDL_Surface* screen, ScrollTextState* scroll_state,
						   const char* text, TTF_Font* font,
						   int text_x, int text_y, int max_text_width,
						   bool selected) {
	SDL_Color text_color = UI_getListTextColor(selected);

	SDL_Rect old_clip;
	SDL_GetClipRect(screen, &old_clip);
	SDL_Rect clip = {text_x, text_y, max_text_width, TTF_FontHeight(font)};
	if (old_clip.w > 0 && old_clip.h > 0) {
		int left = clip.x > old_clip.x ? clip.x : old_clip.x;
		int top = clip.y > old_clip.y ? clip.y : old_clip.y;
		int right = (clip.x + clip.w) < (old_clip.x + old_clip.w) ? (clip.x + clip.w) : (old_clip.x + old_clip.w);
		int bottom = (clip.y + clip.h) < (old_clip.y + old_clip.h) ? (clip.y + clip.h) : (old_clip.y + old_clip.h);
		if (right > left && bottom > top) {
			clip = (SDL_Rect){left, top, right - left, bottom - top};
		} else {
			return;
		}
	}
	SDL_SetClipRect(screen, &clip);

	if (selected && scroll_state) {
		ScrollText_update(scroll_state, text, font, max_text_width,
						  text_color, screen, text_x, text_y, true);
	} else {
		SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font, text, text_color);
		if (text_surf) {
			SDL_Rect src = {0, 0, text_surf->w > max_text_width ? max_text_width : text_surf->w, text_surf->h};
			SDL_BlitSurface(text_surf, &src, screen, &(SDL_Rect){text_x, text_y, 0, 0});
			SDL_FreeSurface(text_surf);
		}
	}

	if (old_clip.w > 0 && old_clip.h > 0)
		SDL_SetClipRect(screen, &old_clip);
	else
		SDL_SetClipRect(screen, NULL);
}

// ============================================
// Badged Pill Rendering
// ============================================

ListItemBadgedPos UI_renderListItemPillBadged(
	SDL_Surface* screen, ListLayout* layout,
	TTF_Font* title_font, TTF_Font* subtitle_font, TTF_Font* badge_font,
	const char* text, const char* subtitle, char* truncated,
	int y, bool selected, int badge_width, int extra_subtitle_width) {
	ListItemBadgedPos pos;
	int item_h = SCALE1(PILL_SIZE) * 3 / 2;

	// Badge area: badge content + BUTTON_PADDING on each side
	int badge_area_w = badge_width > 0 ? badge_width + SCALE1(BUTTON_PADDING * 2) : 0;

	// Calculate title pill width (reduced max to leave room for badge area)
	int title_max_width = layout->max_width - badge_area_w;
	pos.pill_width = UI_calcListPillWidth(title_font, text, truncated, title_max_width, 0);

	// Expand pill if subtitle is wider than title
	if (subtitle && subtitle[0]) {
		int sub_w;
		TTF_SizeUTF8(subtitle_font, subtitle, &sub_w, NULL);
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

	// Text positions: two rows vertically centered
	int text_start_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
	int title_h = TTF_FontHeight(title_font);
	int sub_h = TTF_FontHeight(subtitle_font);
	int total_text_h = title_h + sub_h;
	int top_gap = (item_h - total_text_h) / 2;

	pos.text_x = text_start_x;
	pos.text_y = y + top_gap;

	pos.subtitle_x = text_start_x;
	pos.subtitle_y = y + top_gap + title_h;

	// Badge position (centered vertically in capsule)
	pos.badge_x = SCALE1(PADDING) + pos.pill_width + SCALE1(BUTTON_PADDING);
	pos.badge_y = y + (item_h - TTF_FontHeight(badge_font)) / 2;

	// Account for right-side capsule radius reducing usable text width
	int r = item_h / 2;
	pos.text_max_width = pos.pill_width - SCALE1(BUTTON_PADDING) - r / 2;

	pos.total_width = pos.pill_width + badge_area_w;

	return pos;
}

// ============================================
// Settings Page Component
// ============================================

void UI_renderSettingsPage(SDL_Surface* screen, ListLayout* layout,
						   UISettingsItem* items, int count,
						   int selected, int* scroll,
						   const char* status_msg) {
	if (count == 0)
		return;

	int hw = screen->w;

	// Compact rows: 9 rows total (8 items + 1 description)
	int total_rows = 9;
	layout->item_h = layout->list_h / total_rows;
	layout->items_per_page = total_rows - 1;

	UI_adjustListScroll(selected, scroll, layout->items_per_page);

	int start = *scroll;
	int end = start + layout->items_per_page;
	if (end > count)
		end = count;

	for (int vi = start; vi < end; vi++) {
		UISettingsItem* item = &items[vi];
		int sel = (vi == selected);
		int item_y = layout->list_y + (vi - start) * layout->item_h;

		// Custom draw override
		if (item->custom_draw) {
			item->custom_draw(screen, item->custom_draw_ctx, SCALE1(PADDING), item_y,
							  hw - SCALE1(PADDING * 2), layout->item_h, sel);
			continue;
		}

		// Format display value (add arrows for cycleable items when selected)
		char display_val[256];
		const char* display_ptr = NULL;
		if (item->value) {
			if (sel && item->cycleable)
				snprintf(display_val, sizeof(display_val), "< %s >", item->value);
			else
				snprintf(display_val, sizeof(display_val), "%s", item->value);
			display_ptr = display_val;
		}

		UI_renderSettingsRow(screen, layout, item->label, display_ptr,
							 item_y, sel, item->swatch);
	}

	// Scroll indicators
	UI_renderScrollIndicators(screen, *scroll, layout->items_per_page, count);

	// Status message centered below items (e.g. "Scanning for networks...")
	if (status_msg && status_msg[0] && count < layout->items_per_page) {
		int msg_row_y = layout->list_y + count * layout->item_h;
		int empty_h = (layout->items_per_page - count) * layout->item_h;
		int msg_y = msg_row_y + (empty_h - TTF_FontHeight(font.small)) / 2;
		SDL_Surface* msg_surf = TTF_RenderUTF8_Blended(font.small, status_msg, COLOR_GRAY);
		if (msg_surf) {
			int msg_x = (hw - msg_surf->w) / 2;
			SDL_BlitSurface(msg_surf, NULL, screen, &(SDL_Rect){msg_x, msg_y, 0, 0});
			SDL_FreeSurface(msg_surf);
		}
	}

	// Description text in the last row (row 9)
	if (selected >= 0 && selected < count &&
		items[selected].desc && items[selected].desc[0]) {
		int desc_row_y = layout->list_y + layout->items_per_page * layout->item_h;
		int desc_y = desc_row_y + (layout->item_h - TTF_FontHeight(font.tiny)) / 2;
		int desc_max_w = hw - SCALE1(PADDING * 2);

		char truncated_desc[256];
		GFX_truncateText(font.tiny, items[selected].desc, truncated_desc, desc_max_w, 0);

		SDL_Surface* desc_surf = TTF_RenderUTF8_Blended(font.tiny, truncated_desc, COLOR_GRAY);
		if (desc_surf) {
			int desc_x = (hw - desc_surf->w) / 2;
			SDL_BlitSurface(desc_surf, NULL, screen, &(SDL_Rect){desc_x, desc_y, 0, 0});
			SDL_FreeSurface(desc_surf);
		}
	}
}

// ============================================
// Settings Row Rendering
// ============================================

#define SETTINGS_ROW_PADDING 8

int UI_renderSettingsRow(SDL_Surface* screen, ListLayout* layout,
						 const char* label, const char* value,
						 int y, bool selected, int swatch_color) {
	int hw = screen->w;
	TTF_Font* f = font.small;

	// Measure label
	int text_w, text_h;
	TTF_SizeUTF8(f, label, &text_w, &text_h);
	int label_pill_width = text_w + SCALE1(SETTINGS_ROW_PADDING * 2);

	int pill_h = layout->item_h;
	int text_x = SCALE1(PADDING) + SCALE1(SETTINGS_ROW_PADDING);
	int text_y = y + (pill_h - TTF_FontHeight(f)) / 2;

	if (selected) {
		SDL_Color selected_text_color = UI_getListTextColor(1);

		if (value) {
			// 2-layer: full-width THEME_COLOR2 + label-width THEME_COLOR1
			int row_width = hw - SCALE1(PADDING * 2);
			SDL_Rect row_rect = {SCALE1(PADDING), y, row_width, pill_h};
			GFX_blitRectColor(ASSET_BUTTON, screen, &row_rect, THEME_COLOR2);

			SDL_Rect label_pill_rect = {SCALE1(PADDING), y, label_pill_width, pill_h};
			GFX_blitRectColor(ASSET_BUTTON, screen, &label_pill_rect, THEME_COLOR1);

			// Label text
			SDL_Surface* label_surf = TTF_RenderUTF8_Blended(f, label, selected_text_color);
			if (label_surf) {
				SDL_BlitSurface(label_surf, NULL, screen, &(SDL_Rect){text_x, text_y, 0, 0});
				SDL_FreeSurface(label_surf);
			}

			// Value with arrows, right-aligned, white text
			int value_x = hw - SCALE1(PADDING) - SCALE1(SETTINGS_ROW_PADDING);
			int val_text_y = y + (pill_h - TTF_FontHeight(font.tiny)) / 2;

			// Color swatch
			if (swatch_color >= 0) {
				int swatch_size = SCALE1(FONT_TINY);
				int swatch_y = y + (pill_h - swatch_size) / 2;
				SDL_Rect border = {value_x - swatch_size, swatch_y, swatch_size, swatch_size};
				SDL_FillRect(screen, &border, RGB_WHITE);
				SDL_Rect inner = {border.x + 1, border.y + 1, border.w - 2, border.h - 2};
				uint32_t col = (uint32_t)swatch_color;
				uint32_t mapped = SDL_MapRGB(screen->format,
											 (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
				SDL_FillRect(screen, &inner, mapped);
				value_x -= swatch_size + SCALE1(4);
			}

			SDL_Surface* val_surf = TTF_RenderUTF8_Blended(font.tiny, value, COLOR_WHITE);
			if (val_surf) {
				value_x -= val_surf->w;
				SDL_BlitSurface(val_surf, NULL, screen, &(SDL_Rect){value_x, val_text_y, 0, 0});
				SDL_FreeSurface(val_surf);
			}
			return value_x;
		} else {
			// Single label rect only
			SDL_Rect label_pill_rect = {SCALE1(PADDING), y, label_pill_width, pill_h};
			GFX_blitRectColor(ASSET_BUTTON, screen, &label_pill_rect, THEME_COLOR1);

			SDL_Surface* label_surf = TTF_RenderUTF8_Blended(f, label, selected_text_color);
			if (label_surf) {
				SDL_BlitSurface(label_surf, NULL, screen, &(SDL_Rect){text_x, text_y, 0, 0});
				SDL_FreeSurface(label_surf);
			}
			return text_x;
		}
	} else {
		// Unselected: no background
		SDL_Color text_color = UI_getListTextColor(0);

		SDL_Surface* label_surf = TTF_RenderUTF8_Blended(f, label, text_color);
		if (label_surf) {
			SDL_BlitSurface(label_surf, NULL, screen, &(SDL_Rect){text_x, text_y, 0, 0});
			SDL_FreeSurface(label_surf);
		}

		if (value) {
			int value_x = hw - SCALE1(PADDING) - SCALE1(SETTINGS_ROW_PADDING);
			int val_text_y = y + (pill_h - TTF_FontHeight(font.tiny)) / 2;

			// Color swatch
			if (swatch_color >= 0) {
				int swatch_size = SCALE1(FONT_TINY);
				int swatch_y = y + (pill_h - swatch_size) / 2;
				SDL_Rect border = {value_x - swatch_size, swatch_y, swatch_size, swatch_size};
				SDL_FillRect(screen, &border, RGB_WHITE);
				SDL_Rect inner = {border.x + 1, border.y + 1, border.w - 2, border.h - 2};
				uint32_t col = (uint32_t)swatch_color;
				uint32_t mapped = SDL_MapRGB(screen->format,
											 (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
				SDL_FillRect(screen, &inner, mapped);
				value_x -= swatch_size + SCALE1(4);
			}

			SDL_Surface* val_surf = TTF_RenderUTF8_Blended(font.tiny, value, text_color);
			if (val_surf) {
				value_x -= val_surf->w;
				SDL_BlitSurface(val_surf, NULL, screen, &(SDL_Rect){value_x, val_text_y, 0, 0});
				SDL_FreeSurface(val_surf);
			}
			return value_x;
		}
		return text_x;
	}
}

// ============================================
// GPU Scroll Without Background
// ============================================

void ScrollText_renderGPU_NoBg(ScrollTextState* state, TTF_Font* font,
							   SDL_Color color, int x, int y) {
	if (!state->text[0] || !state->needs_scroll || !state->cached_scroll_surface) {
		PLAT_clearLayers(LAYER_SCROLLTEXT);
		return;
	}

	state->last_x = x;
	state->last_y = y;
	state->last_font = font;
	state->last_color = color;

	int padding = SCALE1(SCROLL_GAP);
	int height = state->cached_scroll_surface->h;

	SDL_Surface* clipped = SDL_CreateRGBSurfaceWithFormat(0,
														  state->max_width, height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!clipped)
		return;

	SDL_FillRect(clipped, NULL, 0);
	SDL_SetSurfaceBlendMode(state->cached_scroll_surface, SDL_BLENDMODE_NONE);
	SDL_Rect src = {state->scroll_offset, 0, state->max_width, height};
	SDL_BlitSurface(state->cached_scroll_surface, &src, clipped, NULL);

	PLAT_clearLayers(LAYER_SCROLLTEXT);
	PLAT_drawOnLayer(clipped, x, y, state->max_width, height, 1.0f, false, LAYER_SCROLLTEXT);
	SDL_FreeSurface(clipped);

	state->scroll_offset += 1;
	if (state->scroll_offset >= state->text_width + padding) {
		state->scroll_offset = 0;
	}

	PLAT_GPU_Flip();
}

// ============================================
// Scroll Helpers
// ============================================

void UI_adjustListScroll(int selected, int* scroll, int items_per_page) {
	if (selected < *scroll) {
		*scroll = selected;
	}
	if (selected >= *scroll + items_per_page) {
		*scroll = selected - items_per_page + 1;
	}
}

void UI_renderScrollIndicators(SDL_Surface* screen, int scroll, int items_per_page, int total_count) {
	if (total_count <= items_per_page)
		return;

	int hw = screen->w;
	int hh = screen->h;
	int ox = (hw - SCALE1(24)) / 2;

	if (scroll > 0) {
		GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE - BUTTON_MARGIN)});
	}
	if (scroll + items_per_page < total_count) {
		GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN)});
	}
}

// ============================================
// Pill Animation (non-threaded, for main-loop driven apps)
// ============================================

void UI_pillAnimInit(PillAnimState* state) {
	state->current_y = 0;
	state->target_y = 0;
	state->start_y = 0;
	state->frame = 0;
	state->total_frames = 3;
	state->active = false;
}

void UI_pillAnimSetTarget(PillAnimState* state, int target_y, bool animate) {
	if (target_y == state->current_y && !state->active)
		return;

	state->start_y = state->current_y;
	state->target_y = target_y;
	state->frame = 0;
	state->total_frames = animate ? 3 : 0;
	state->active = true;
}

int UI_pillAnimTick(PillAnimState* state) {
	if (!state->active)
		return state->current_y;

	if (state->frame >= state->total_frames) {
		state->current_y = state->target_y;
		state->active = false;
		return state->current_y;
	}

	state->frame++;
	float t = (state->total_frames > 0) ? ((float)state->frame / state->total_frames) : 1.0f;
	if (t > 1.0f)
		t = 1.0f;

	state->current_y = state->start_y + (int)((state->target_y - state->start_y) * t);
	return state->current_y;
}

bool UI_pillAnimIsActive(PillAnimState* state) {
	return state->active;
}

// ============================================
// Rich Pill Rendering
// ============================================

ListItemRichPos UI_renderListItemPillRich(SDL_Surface* screen, ListLayout* layout,
										  const char* title, const char* subtitle,
										  char* truncated,
										  int y, bool selected, bool has_image,
										  int extra_subtitle_width) {
	ListItemRichPos pos;

	int item_h = SCALE1(PILL_SIZE) * 3 / 2;
	int img_padding = SCALE1(4);

	int image_area_w;
	if (has_image) {
		pos.image_size = item_h - img_padding * 2;
		image_area_w = img_padding + pos.image_size + SCALE1(BUTTON_PADDING);
		pos.image_x = SCALE1(PADDING) + img_padding;
		pos.image_y = y + img_padding;
	} else {
		pos.image_size = 0;
		image_area_w = SCALE1(BUTTON_PADDING);
		pos.image_x = 0;
		pos.image_y = 0;
	}

	pos.pill_width = UI_calcListPillWidth(font.medium, title, truncated, layout->max_width, image_area_w);
	if (subtitle && subtitle[0]) {
		int sub_w;
		TTF_SizeUTF8(font.small, subtitle, &sub_w, NULL);
		int sub_pill_w = MIN(layout->max_width, image_area_w + sub_w + extra_subtitle_width + SCALE1(BUTTON_PADDING * 2));
		if (sub_pill_w > pos.pill_width)
			pos.pill_width = sub_pill_w;
	}

	if (selected) {
		int px = SCALE1(PADDING);
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

// ============================================
// Menu Item Pill Rendering
// ============================================

MenuItemPos UI_renderMenuItemPill(SDL_Surface* screen, ListLayout* layout,
								  const char* text, char* truncated,
								  int index, bool selected, int prefix_width) {
	MenuItemPos pos;

	int item_h = SCALE1(PILL_SIZE);
	pos.item_y = layout->list_y + index * item_h;

	pos.pill_width = UI_calcListPillWidth(font.large, text, truncated, layout->max_width - prefix_width, prefix_width);

	SDL_Rect pill_rect = {SCALE1(PADDING), pos.item_y, pos.pill_width, SCALE1(PILL_SIZE)};
	UI_drawListItemBg(screen, &pill_rect, selected);

	pos.text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
	pos.text_y = pos.item_y + (SCALE1(PILL_SIZE) - TTF_FontHeight(font.large)) / 2;

	return pos;
}

// ============================================
// Rounded Rectangle Background
// ============================================

void UI_renderRoundedRectBg(SDL_Surface* screen, int x, int y, int w, int h, uint32_t color) {
	int r = SCALE1(7);
	if (r > h / 2)
		r = h / 2;
	if (r > w / 2)
		r = w / 2;

	if (h - 2 * r > 0) {
		SDL_FillRect(screen, &(SDL_Rect){x, y + r, w, h - 2 * r}, color);
	}

	for (int dy = 0; dy < r; dy++) {
		int yd = r - dy;
		int inset = r - (int)sqrtf((float)(r * r - yd * yd));
		int row_w = w - 2 * inset;
		if (row_w <= 0)
			continue;
		SDL_FillRect(screen, &(SDL_Rect){x + inset, y + dy, row_w, 1}, color);
		SDL_FillRect(screen, &(SDL_Rect){x + inset, y + h - 1 - dy, row_w, 1}, color);
	}
}

// ============================================
// Generic Simple Menu Rendering
// ============================================

void UI_renderSimpleMenu(SDL_Surface* screen, int menu_selected,
						 const SimpleMenuConfig* config) {
	GFX_clear(screen);
	char truncated[256];
	char label_buffer[256];

	UI_renderMenuBar(screen, config->title);
	ListLayout layout = UI_calcListLayout(screen);

	int icon_size = SCALE1(24);
	int icon_spacing = SCALE1(6);

	for (int i = 0; i < config->item_count; i++) {
		bool selected = (i == menu_selected);

		const char* label = (config->items) ? config->items[i] : "";
		if (config->get_label) {
			const char* custom = config->get_label(i, label, label_buffer, sizeof(label_buffer));
			if (custom)
				label = custom;
		}

		SDL_Surface* icon = NULL;
		int icon_offset = 0;
		if (config->get_icon) {
			icon = config->get_icon(i, selected);
			if (icon) {
				icon_offset = icon_size + icon_spacing;
			}
		}

		MenuItemPos pos = UI_renderMenuItemPill(screen, &layout, label, truncated, i, selected, icon_offset);

		int text_x = pos.text_x;
		if (icon) {
			int icon_y = pos.item_y + (SCALE1(PILL_SIZE) - icon_size) / 2;
			SDL_Rect src_rect = {0, 0, icon->w, icon->h};
			SDL_Rect dst_rect = {pos.text_x, icon_y, icon_size, icon_size};
			SDL_BlitScaled(icon, &src_rect, screen, &dst_rect);
			text_x += icon_offset;
		}

		bool custom_rendered = false;
		if (config->render_text) {
			custom_rendered = config->render_text(screen, i, selected,
												  text_x, pos.text_y, layout.max_width - icon_offset);
		}
		if (!custom_rendered) {
			UI_renderListItemText(screen, NULL, truncated, font.large,
								  text_x, pos.text_y, layout.max_width - icon_offset, selected);
		}

		if (config->render_badge) {
			config->render_badge(screen, i, selected, pos.item_y, SCALE1(PILL_SIZE));
		}
	}

	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", (char*)config->btn_b_label, "A", "OPEN", NULL});
}
