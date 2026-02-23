#include <stdio.h>
#include <string.h>
#include <math.h>

#include <SDL2/SDL_image.h>

#include "api.h"
#include "ui_components.h"
#include "ui_youtube.h"
#include "ui_utils.h"

// ============================================
// Carousel: gradient overlay + fullscreen thumbnail
// ============================================

// Create gradient overlay surface (bottom third of screen, alpha ramps 0 -> 200)
static SDL_Surface* create_gradient_overlay(int w, int h) {
	int grad_h = h / 3;
	SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, grad_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surf)
		return NULL;

	SDL_LockSurface(surf);
	Uint32* pixels = (Uint32*)surf->pixels;
	for (int y = 0; y < grad_h; y++) {
		// Alpha ramps from 0 (top) to 200 (bottom)
		Uint8 alpha = (Uint8)(200 * y / (grad_h - 1));
		Uint32 color = SDL_MapRGBA(surf->format, 0, 0, 0, alpha);
		for (int x = 0; x < w; x++) {
			pixels[y * (surf->pitch / 4) + x] = color;
		}
	}
	SDL_UnlockSurface(surf);

	return surf;
}

void YouTubeCarousel_init(YouTubeCarouselState* state, SDL_Surface* screen) {
	memset(state, 0, sizeof(*state));
	state->loaded_index = -1;
	state->gradient_overlay = create_gradient_overlay(screen->w, screen->h);
}

void YouTubeCarousel_cleanup(YouTubeCarouselState* state) {
	if (state->current_surface) {
		SDL_FreeSurface(state->current_surface);
		state->current_surface = NULL;
	}
	if (state->gradient_overlay) {
		SDL_FreeSurface(state->gradient_overlay);
		state->gradient_overlay = NULL;
	}
	state->loaded_index = -1;
}

bool YouTubeCarousel_loadThumbnail(YouTubeCarouselState* state, int index,
								   YouTubeSearchResults* results) {
	if (index < 0 || index >= results->count)
		return false;

	char path[512];
	if (!YouTube_getThumbnailPath(results->items[index].id, path, sizeof(path))) {
		// Thumbnail file doesn't exist yet; check if downloader already tried it
		YouTubeThumbDownloader* dl = YouTube_getThumbDownloader();
		if (dl->downloaded_count > index || !dl->running) {
			// Batch download finished but no file - start a background retry
			YouTube_retryThumbnail(results->items[index].id);
		}
		return false;
	}

	// Free previous surface
	if (state->current_surface) {
		SDL_FreeSurface(state->current_surface);
		state->current_surface = NULL;
	}

	state->current_surface = IMG_Load(path);
	if (state->current_surface) {
		state->loaded_index = index;
		return true;
	}
	return false;
}

bool YouTubeCarousel_loadChannelThumbnail(YouTubeCarouselState* state, int index,
										  YouTubeSearchResults* results,
										  const char* channel_id) {
	if (index < 0 || index >= results->count)
		return false;

	char path[512];
	if (!YouTube_getChannelThumbnailPath(channel_id, results->items[index].id, path, sizeof(path))) {
		return false;
	}

	if (state->current_surface) {
		SDL_FreeSurface(state->current_surface);
		state->current_surface = NULL;
	}

	state->current_surface = IMG_Load(path);
	if (state->current_surface) {
		state->loaded_index = index;
		return true;
	}
	return false;
}

// Render fullscreen carousel
void render_youtube_carousel(SDL_Surface* screen, IndicatorType show_setting,
							 YouTubeSearchResults* results, int selected,
							 YouTubeCarouselState* carousel,
							 ScrollTextState* scroll_state) {
	int hw = screen->w;
	int hh = screen->h;

	GFX_clear(screen);

	if (results->count == 0) {
		UI_renderEmptyState(screen, "No results", "Try a different search", "SEARCH");
		return;
	}

	YouTubeResult* r = &results->items[selected];

	// Background: thumbnail scaled to fill, or dark placeholder
	if (carousel->current_surface && carousel->loaded_index == selected) {
		GFX_blitScaleToFill(carousel->current_surface, screen);
	} else {
		// Dark placeholder
		SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 20, 20, 20));
		// Only show "Loading" if we haven't finished attempting this thumbnail
		if (carousel->loaded_index != selected) {
			SDL_Surface* loading = TTF_RenderUTF8_Blended(font.small, "Loading thumbnail...", COLOR_GRAY);
			if (loading) {
				SDL_BlitSurface(loading, NULL, screen,
								&(SDL_Rect){(hw - loading->w) / 2, hh / 2 - loading->h / 2});
				SDL_FreeSurface(loading);
			}
		}
	}

	// Gradient overlay at bottom
	if (carousel->gradient_overlay) {
		int grad_y = hh - carousel->gradient_overlay->h;
		SDL_SetSurfaceBlendMode(carousel->gradient_overlay, SDL_BLENDMODE_BLEND);
		SDL_BlitSurface(carousel->gradient_overlay, NULL, screen,
						&(SDL_Rect){0, grad_y, 0, 0});
	}

	// Counter "3 / 20" at top-right
	{
		char counter[32];
		snprintf(counter, sizeof(counter), "%d / %d", selected + 1, results->count);
		SDL_Surface* counter_surf = TTF_RenderUTF8_Blended(font.small, counter, COLOR_WHITE);
		if (counter_surf) {
			int cx = hw - counter_surf->w - SCALE1(PADDING) - SCALE1(8);
			int cy = SCALE1(PADDING);
			int bg_w = counter_surf->w + SCALE1(12);
			int bg_h = counter_surf->h + SCALE1(6);
			int bg_x = cx - SCALE1(6);
			int bg_y = cy - SCALE1(3);
			UI_renderRoundedRectBg(screen, bg_x, bg_y, bg_w, bg_h,
								   SDL_MapRGB(screen->format, 0, 0, 0));
			SDL_BlitSurface(counter_surf, NULL, screen, &(SDL_Rect){cx, cy});
			SDL_FreeSurface(counter_surf);
		}
	}

	// Text overlay area - positioned above button hints
	int btn_area_h = SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
	int text_margin = SCALE1(PADDING);

	// Channel + duration line (small font, light gray)
	{
		char subtitle[256];
		bool has_channel = r->channel[0] && strcmp(r->channel, "NA") != 0;
		if (has_channel && r->duration_sec > 0) {
			char dur[16];
			format_time(dur, r->duration_sec);
			snprintf(subtitle, sizeof(subtitle), "%s  |  %s", r->channel, dur);
		} else if (r->duration_sec > 0) {
			char dur[16];
			format_time(dur, r->duration_sec);
			snprintf(subtitle, sizeof(subtitle), "%s", dur);
		} else if (has_channel) {
			snprintf(subtitle, sizeof(subtitle), "%s", r->channel);
		} else {
			subtitle[0] = '\0';
		}

		int max_w = hw - text_margin * 2;
		SDL_Color sub_color = {180, 180, 180, 255};
		SDL_Surface* sub_surf = subtitle[0] ? TTF_RenderUTF8_Blended(font.small, subtitle, sub_color) : NULL;

		{
			int sub_y = sub_surf ? hh - btn_area_h - sub_surf->h - SCALE1(16)
								 : hh - btn_area_h - SCALE1(16);
			int title_y = sub_surf ? sub_y - TTF_FontHeight(font.medium) - SCALE1(2)
								   : hh - btn_area_h - TTF_FontHeight(font.medium) - SCALE1(16);

			// Semi-transparent black background behind text
			int bg_pad_x = SCALE1(8);
			int bg_pad_y = SCALE1(6);
			int bg_x = text_margin - bg_pad_x;
			int bg_y = title_y - bg_pad_y;
			int bg_w = max_w + bg_pad_x * 2;
			int bg_bottom = sub_surf ? sub_y + sub_surf->h : title_y + TTF_FontHeight(font.medium);
			int bg_h = bg_bottom - title_y + bg_pad_y * 2;

			SDL_Surface* bg = SDL_CreateRGBSurfaceWithFormat(0, bg_w, bg_h, 32, SDL_PIXELFORMAT_ARGB8888);
			if (bg) {
				SDL_FillRect(bg, NULL, SDL_MapRGBA(bg->format, 0, 0, 0, 192));
				SDL_SetSurfaceBlendMode(bg, SDL_BLENDMODE_BLEND);
				SDL_BlitSurface(bg, NULL, screen, &(SDL_Rect){bg_x, bg_y, 0, 0});
				SDL_FreeSurface(bg);
			}

			// Subtitle
			if (sub_surf) {
				if (sub_surf->w > max_w) {
					SDL_Rect clip = {0, 0, max_w, sub_surf->h};
					SDL_BlitSurface(sub_surf, &clip, screen, &(SDL_Rect){text_margin, sub_y});
				} else {
					SDL_BlitSurface(sub_surf, NULL, screen, &(SDL_Rect){text_margin, sub_y});
				}
			}

			// Title (medium font, white) - with scroll for long titles
			ScrollText_update(scroll_state, r->title, font.medium,
							  max_w, COLOR_WHITE, screen, text_margin, title_y, false);

			if (sub_surf)
				SDL_FreeSurface(sub_surf);
		}
	}

	// Button hints
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "PLAY", NULL});
}

// ============================================
// Status screens (unchanged)
// ============================================

// Render centered status text
static void render_centered_status(SDL_Surface* screen, const char* text, const char* subtitle) {
	GFX_clear(screen);
	int hh = screen->h;
	int center_y = hh / 2;

	SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font.medium, text, COLOR_WHITE);
	if (text_surf) {
		SDL_BlitSurface(text_surf, NULL, screen,
						&(SDL_Rect){(screen->w - text_surf->w) / 2, center_y - SCALE1(15)});
		SDL_FreeSurface(text_surf);
	}

	if (subtitle) {
		SDL_Surface* sub_surf = TTF_RenderUTF8_Blended(font.small, subtitle, COLOR_GRAY);
		if (sub_surf) {
			SDL_BlitSurface(sub_surf, NULL, screen,
							&(SDL_Rect){(screen->w - sub_surf->w) / 2, center_y + SCALE1(10)});
			SDL_FreeSurface(sub_surf);
		}
	}

	UI_renderButtonHintBar(screen, (char*[]){"B", "CANCEL", NULL});
}

void render_youtube_searching(SDL_Surface* screen) {
	render_centered_status(screen, "Searching YouTube...", "This may take a moment");
}

void render_youtube_resolving(SDL_Surface* screen) {
	render_centered_status(screen, "Getting stream URL...", "Connecting to video");
}

void render_youtube_error(SDL_Surface* screen, const char* error_msg) {
	GFX_clear(screen);
	int hh = screen->h;
	int center_y = hh / 2;

	SDL_Surface* title = TTF_RenderUTF8_Blended(font.medium, "Error", COLOR_WHITE);
	if (title) {
		SDL_BlitSurface(title, NULL, screen,
						&(SDL_Rect){(screen->w - title->w) / 2, center_y - SCALE1(15)});
		SDL_FreeSurface(title);
	}

	if (error_msg && error_msg[0]) {
		SDL_Surface* msg = TTF_RenderUTF8_Blended(font.small, error_msg, COLOR_GRAY);
		if (msg) {
			SDL_BlitSurface(msg, NULL, screen,
							&(SDL_Rect){(screen->w - msg->w) / 2, center_y + SCALE1(10)});
			SDL_FreeSurface(msg);
		}
	}

	UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
}

// ============================================
// Channel Details Page
// ============================================

void render_youtube_channel_info(SDL_Surface* screen, YouTubeChannelInfo* info,
								 const char* channel_name, bool is_subscribed,
								 bool is_loading) {
	int hw = screen->w;
	int hh = screen->h;

	// Dark background
	SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 15, 15, 15));

	if (is_loading) {
		// Show channel name immediately (from search results) + loading indicator
		int center_y = hh / 2;

		if (channel_name && channel_name[0]) {
			SDL_Surface* name_surf = TTF_RenderUTF8_Blended(font.medium, channel_name, COLOR_WHITE);
			if (name_surf) {
				int nx = (hw - name_surf->w) / 2;
				if (name_surf->w > hw - SCALE1(PADDING * 2)) {
					SDL_Rect clip = {0, 0, hw - SCALE1(PADDING * 2), name_surf->h};
					nx = SCALE1(PADDING);
					SDL_BlitSurface(name_surf, &clip, screen, &(SDL_Rect){nx, center_y - SCALE1(25)});
				} else {
					SDL_BlitSurface(name_surf, NULL, screen, &(SDL_Rect){nx, center_y - SCALE1(25)});
				}
				SDL_FreeSurface(name_surf);
			}
		}

		SDL_Surface* loading = TTF_RenderUTF8_Blended(font.small, "Loading channel info...", COLOR_GRAY);
		if (loading) {
			SDL_BlitSurface(loading, NULL, screen,
							&(SDL_Rect){(hw - loading->w) / 2, center_y + SCALE1(5)});
			SDL_FreeSurface(loading);
		}

		UI_renderButtonHintBar(screen, (char*[]){"B", "CANCEL", NULL});
		return;
	}

	// Loaded state: show full channel details
	if (!info || !info->loaded)
		return;

	// Calculate total content height for vertical centering
	int avatar_size = SCALE1(64);
	int total_h = avatar_size + SCALE1(10);				// avatar + gap
	total_h += TTF_FontHeight(font.large) + SCALE1(6);	// channel name + gap
	total_h += TTF_FontHeight(font.small) + SCALE1(12); // subscriber count + gap
	total_h += TTF_FontHeight(font.small) + SCALE1(8);	// status pill

	// Account for button hints area at bottom
	int btn_area_h = SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
	int avail_h = hh - btn_area_h;
	int content_y = (avail_h - total_h) / 2;
	if (info->avatar_path[0]) {
		SDL_Surface* avatar = IMG_Load(info->avatar_path);
		if (avatar) {
			SDL_Rect src_rect = {0, 0, avatar->w, avatar->h};
			SDL_Rect dst_rect = {(hw - avatar_size) / 2, content_y, avatar_size, avatar_size};
			SDL_BlitScaled(avatar, &src_rect, screen, &dst_rect);
			SDL_FreeSurface(avatar);
			content_y += avatar_size + SCALE1(10);
		} else {
			content_y += SCALE1(10);
		}
	} else {
		// Placeholder circle
		int cx = hw / 2;
		int cy = content_y + avatar_size / 2;
		int r = avatar_size / 2;
		for (int dy = -r; dy <= r; dy++) {
			int dx = (int)sqrtf((float)(r * r - dy * dy));
			SDL_FillRect(screen, &(SDL_Rect){cx - dx, cy + dy, dx * 2, 1},
						 SDL_MapRGB(screen->format, 60, 60, 60));
		}
		// "?" in center
		SDL_Surface* q = TTF_RenderUTF8_Blended(font.large, "?", COLOR_GRAY);
		if (q) {
			SDL_BlitSurface(q, NULL, screen, &(SDL_Rect){cx - q->w / 2, cy - q->h / 2});
			SDL_FreeSurface(q);
		}
		content_y += avatar_size + SCALE1(10);
	}

	// Channel name (large, centered)
	{
		const char* display_name = info->name[0] ? info->name : channel_name;
		if (display_name && display_name[0]) {
			SDL_Surface* name_surf = TTF_RenderUTF8_Blended(font.large, display_name, COLOR_WHITE);
			if (name_surf) {
				int max_w = hw - SCALE1(PADDING * 2);
				if (name_surf->w > max_w) {
					SDL_Rect clip = {0, 0, max_w, name_surf->h};
					SDL_BlitSurface(name_surf, &clip, screen, &(SDL_Rect){SCALE1(PADDING), content_y});
				} else {
					SDL_BlitSurface(name_surf, NULL, screen, &(SDL_Rect){(hw - name_surf->w) / 2, content_y});
				}
				content_y += name_surf->h + SCALE1(6);
				SDL_FreeSurface(name_surf);
			}
		}
	}

	// Subscriber count (centered, gray)
	if (info->subscriber_count[0]) {
		SDL_Surface* sub_surf = TTF_RenderUTF8_Blended(font.small, info->subscriber_count, COLOR_GRAY);
		if (sub_surf) {
			SDL_BlitSurface(sub_surf, NULL, screen, &(SDL_Rect){(hw - sub_surf->w) / 2, content_y});
			content_y += sub_surf->h + SCALE1(12);
			SDL_FreeSurface(sub_surf);
		}
	}

	// Subscribe/Unsubscribe status indicator
	{
		const char* status_text = is_subscribed ? "SUBSCRIBED" : "NOT SUBSCRIBED";
		SDL_Color status_color = is_subscribed ? (SDL_Color){100, 200, 100, 255} : COLOR_GRAY;
		SDL_Surface* status_surf = TTF_RenderUTF8_Blended(font.small, status_text, status_color);
		if (status_surf) {
			// Background pill for status
			int pill_w = status_surf->w + SCALE1(16);
			int pill_h = status_surf->h + SCALE1(8);
			int pill_x = (hw - pill_w) / 2;
			int pill_y = content_y;

			UI_renderRoundedRectBg(screen, pill_x, pill_y, pill_w, pill_h,
								   SDL_MapRGB(screen->format, 40, 40, 40));

			SDL_BlitSurface(status_surf, NULL, screen,
							&(SDL_Rect){(hw - status_surf->w) / 2, pill_y + SCALE1(4)});
			SDL_FreeSurface(status_surf);
		}
	}

	// Button hints
	const char* a_label = is_subscribed ? "UNSUBSCRIBE" : "SUBSCRIBE";
	UI_renderButtonHintBar(screen, (char*[]){"A", (char*)a_label, "DOWN", "BACK", NULL});
}

// YouTube sub-menu items
static const char* yt_submenu_items[] = {"Search", "Subscriptions"};
#define YT_SUBMENU_ITEM_COUNT 2

void render_youtube_submenu(SDL_Surface* screen, IndicatorType show_setting, int selected) {
	SimpleMenuConfig config = {
		.title = "YouTube",
		.items = yt_submenu_items,
		.item_count = YT_SUBMENU_ITEM_COUNT,
		.btn_b_label = "BACK",
		.get_label = NULL,
		.render_badge = NULL,
		.get_icon = NULL,
		.render_text = NULL};
	UI_renderSimpleMenu(screen, selected, &config);
}
