#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL2/SDL_image.h>

#include "api.h"
#include "ui_components.h"
#include "ui_subscriptions.h"
#include "ui_fonts.h"
#include "ui_utils.h"
#include "subscriptions.h"

// Avatar thumbnail cache (in-memory)
#define AVATAR_CACHE_SIZE 8

typedef struct {
	char channel_id[SUBS_MAX_ID];
	SDL_Surface* surface;
} AvatarCacheEntry;

static AvatarCacheEntry avatar_cache[AVATAR_CACHE_SIZE];
static int avatar_cache_count = 0;

static SDL_Surface* avatar_cache_find(const char* channel_id) {
	for (int i = 0; i < avatar_cache_count; i++) {
		if (strcmp(avatar_cache[i].channel_id, channel_id) == 0) {
			return avatar_cache[i].surface;
		}
	}
	return NULL;
}

static void avatar_cache_add(const char* channel_id, SDL_Surface* surface) {
	if (avatar_cache_count >= AVATAR_CACHE_SIZE) {
		SDL_FreeSurface(avatar_cache[0].surface);
		memmove(&avatar_cache[0], &avatar_cache[1],
				sizeof(AvatarCacheEntry) * (AVATAR_CACHE_SIZE - 1));
		avatar_cache_count = AVATAR_CACHE_SIZE - 1;
	}
	strncpy(avatar_cache[avatar_cache_count].channel_id, channel_id, SUBS_MAX_ID - 1);
	avatar_cache[avatar_cache_count].channel_id[SUBS_MAX_ID - 1] = '\0';
	avatar_cache[avatar_cache_count].surface = surface;
	avatar_cache_count++;
}

// Scale an avatar surface to target_size x target_size and apply circular mask
static SDL_Surface* scale_avatar(SDL_Surface* src, int target_size) {
	if (!src)
		return NULL;

	SDL_Surface* converted = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
	if (!converted)
		return NULL;

	SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(
		0, target_size, target_size, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!scaled) {
		SDL_FreeSurface(converted);
		return NULL;
	}

	SDL_Rect src_rect = {0, 0, converted->w, converted->h};
	SDL_Rect dst_rect = {0, 0, target_size, target_size};
	SDL_BlitScaled(converted, &src_rect, scaled, &dst_rect);
	SDL_FreeSurface(converted);

	// Apply circular mask
	int radius = target_size / 2;
	uint32_t* pixels = (uint32_t*)scaled->pixels;
	int pitch = scaled->pitch / 4;
	for (int y = 0; y < target_size; y++) {
		for (int x = 0; x < target_size; x++) {
			int dx = x - radius;
			int dy = y - radius;
			if (dx * dx + dy * dy > radius * radius) {
				pixels[y * pitch + x] = 0;
			}
		}
	}

	return scaled;
}

static void avatar_load_visible(const SubscriptionList* subs, int scroll, int items_per_page) {
	for (int i = 0; i < items_per_page && (scroll + i) < subs->count; i++) {
		int idx = scroll + i;
		const SubscriptionChannel* ch = &subs->channels[idx];
		if (!ch->channel_id[0])
			continue;
		if (avatar_cache_find(ch->channel_id))
			continue;

		char path[512];
		if (!Subscriptions_getAvatarPath(ch->channel_id, path, sizeof(path)))
			continue;

		struct stat st;
		if (stat(path, &st) != 0 || st.st_size == 0)
			continue;

		SDL_Surface* raw = IMG_Load(path);
		if (!raw)
			continue;

		int icon_size = SCALE1(PILL_SIZE) * 3 / 2 - SCALE1(8);
		SDL_Surface* scaled = scale_avatar(raw, icon_size);
		SDL_FreeSurface(raw);

		if (scaled) {
			avatar_cache_add(ch->channel_id, scaled);
		}
	}
}

void SubUI_clearAvatarCache(void) {
	for (int i = 0; i < avatar_cache_count; i++) {
		if (avatar_cache[i].surface) {
			SDL_FreeSurface(avatar_cache[i].surface);
		}
	}
	avatar_cache_count = 0;
}

void render_subscriptions_list(SDL_Surface* screen, IndicatorType show_setting,
							   const SubscriptionList* subs,
							   int selected, int scroll_offset,
							   ScrollTextState* scroll_state) {
	GFX_clear(screen);
	char truncated[256];

	UI_renderMenuBar(screen, "Subscriptions");

	if (subs->count == 0) {
		render_subscriptions_empty(screen, show_setting);
		return;
	}

	ListLayout layout = UI_calcListLayout(screen);

	// Rich items (2-row) for avatar display
	int rich_item_h = SCALE1(PILL_SIZE) * 3 / 2 + SCALE1(2);
	int items_per_page = layout.list_h / rich_item_h;
	if (items_per_page < 1)
		items_per_page = 1;

	int scroll = scroll_offset;
	UI_adjustListScroll(selected, &scroll, items_per_page);

	// Lazy-load one avatar per frame
	avatar_load_visible(subs, scroll, items_per_page);

	for (int i = 0; i < items_per_page && (scroll + i) < subs->count; i++) {
		int idx = scroll + i;
		const SubscriptionChannel* ch = &subs->channels[idx];
		bool is_selected = (idx == selected);

		int y = layout.list_y + i * rich_item_h;

		// Subtitle: video count
		char subtitle[64] = "";
		if (ch->video_count > 0) {
			snprintf(subtitle, sizeof(subtitle), "%d videos", ch->video_count);
		}

		// Check if we have an avatar
		bool has_avatar = false;
		SDL_Surface* avatar = NULL;
		if (ch->channel_id[0]) {
			avatar = avatar_cache_find(ch->channel_id);
			if (avatar)
				has_avatar = true;
		}

		ListItemRichPos pos = UI_renderListItemPillRich(screen, &layout,
														ch->channel_name, subtitle, truncated,
														y, is_selected, has_avatar, 0);

		// Blit avatar if available
		if (avatar && has_avatar) {
			SDL_BlitSurface(avatar, NULL, screen,
							&(SDL_Rect){pos.image_x, pos.image_y, pos.image_size, pos.image_size});
		}

		UI_renderListItemText(screen, scroll_state, ch->channel_name, font.medium,
							  pos.title_x, pos.title_y,
							  pos.text_max_width, is_selected);

		if (subtitle[0]) {
			SDL_Color sub_color = is_selected ? Fonts_getListTextColor(true) : COLOR_GRAY;
			SDL_Surface* sub_surf = TTF_RenderUTF8_Blended(font.small, subtitle, sub_color);
			if (sub_surf) {
				SDL_BlitSurface(sub_surf, NULL, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});

				// "New" badge: show when channel has new videos since last view
				if (ch->video_count > ch->seen_video_count && ch->seen_video_count > 0) {
					SDL_Color badge_color = {100, 220, 100, 255};
					SDL_Surface* badge_surf = TTF_RenderUTF8_Blended(font.small, "New", badge_color);
					if (badge_surf) {
						int badge_x = pos.subtitle_x + sub_surf->w + SCALE1(6);
						int badge_bg_w = badge_surf->w + SCALE1(8);
						int badge_bg_h = badge_surf->h + SCALE1(2);
						int badge_bg_y = pos.subtitle_y - SCALE1(1);
						uint32_t green_bg = SDL_MapRGB(screen->format, 30, 80, 30);
						UI_renderRoundedRectBg(screen, badge_x - SCALE1(4), badge_bg_y,
											   badge_bg_w, badge_bg_h, green_bg);
						SDL_BlitSurface(badge_surf, NULL, screen,
										&(SDL_Rect){badge_x, pos.subtitle_y});
						SDL_FreeSurface(badge_surf);
					}
				}

				SDL_FreeSurface(sub_surf);
			}
		}
	}

	UI_renderScrollIndicators(screen, scroll, items_per_page, subs->count);

	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "VIEW", "X", "REMOVE", NULL});
}

void render_channel_videos(SDL_Surface* screen, IndicatorType show_setting,
						   const char* channel_name,
						   YouTubeSearchResults* results,
						   int selected, int scroll_offset,
						   ScrollTextState* scroll_state) {
	GFX_clear(screen);
	char truncated[256];

	UI_renderMenuBar(screen, channel_name);

	if (results->count == 0) {
		UI_renderEmptyState(screen, "No videos found", NULL, NULL);
		return;
	}

	ListLayout layout = UI_calcListLayout(screen);

	// Rich items (2-row)
	int rich_item_h = SCALE1(PILL_SIZE) * 3 / 2 + SCALE1(2);
	int items_per_page = layout.list_h / rich_item_h;
	if (items_per_page < 1)
		items_per_page = 1;

	int scroll = scroll_offset;
	UI_adjustListScroll(selected, &scroll, items_per_page);

	for (int i = 0; i < items_per_page && (scroll + i) < results->count; i++) {
		int idx = scroll + i;
		YouTubeResult* r = &results->items[idx];
		bool is_selected = (idx == selected);

		int y = layout.list_y + i * rich_item_h;

		// Build subtitle with duration
		char subtitle[256];
		if (r->duration_sec > 0) {
			char dur[16];
			format_time(dur, r->duration_sec);
			snprintf(subtitle, sizeof(subtitle), "%s", dur);
		} else {
			subtitle[0] = '\0';
		}

		ListItemRichPos pos = UI_renderListItemPillRich(screen, &layout,
														r->title, subtitle, truncated,
														y, is_selected, false, 0);

		UI_renderListItemText(screen, scroll_state, r->title, font.medium,
							  pos.title_x, pos.title_y, pos.text_max_width, is_selected);

		if (subtitle[0]) {
			SDL_Color sub_color = is_selected ? Fonts_getListTextColor(true) : COLOR_GRAY;
			SDL_Surface* sub_surf = TTF_RenderUTF8_Blended(font.small, subtitle, sub_color);
			if (sub_surf) {
				SDL_BlitSurface(sub_surf, NULL, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
				SDL_FreeSurface(sub_surf);
			}
		}
	}

	UI_renderScrollIndicators(screen, scroll, items_per_page, results->count);

	UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "PLAY", NULL});
}

void render_channel_searching(SDL_Surface* screen, const char* channel_name) {
	GFX_clear(screen);
	int hh = screen->h;
	int center_y = hh / 2;

	char msg[256];
	snprintf(msg, sizeof(msg), "Loading %s...", channel_name);

	SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
	if (text_surf) {
		SDL_BlitSurface(text_surf, NULL, screen,
						&(SDL_Rect){(screen->w - text_surf->w) / 2, center_y - SCALE1(15)});
		SDL_FreeSurface(text_surf);
	}

	SDL_Surface* sub = TTF_RenderUTF8_Blended(font.small, "Fetching channel videos...", COLOR_GRAY);
	if (sub) {
		SDL_BlitSurface(sub, NULL, screen,
						&(SDL_Rect){(screen->w - sub->w) / 2, center_y + SCALE1(10)});
		SDL_FreeSurface(sub);
	}

	UI_renderButtonHintBar(screen, (char*[]){"B", "CANCEL", NULL});
}

void render_subscriptions_empty(SDL_Surface* screen, IndicatorType show_setting) {
	GFX_clear(screen);
	UI_renderMenuBar(screen, "Subscriptions");
	UI_renderEmptyState(screen, "No subscriptions",
						"Subscribe to channels from YouTube search", NULL);
}
