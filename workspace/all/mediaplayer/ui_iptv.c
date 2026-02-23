#include <stdio.h>
#include <string.h>

#include "ui_list.h"
#include "vp_defines.h"
#include "api.h"
#include "ui_components.h"
#include "ui_iptv.h"
#include "ui_fonts.h"
#include "ui_utils.h"
#include "ui_toast.h"
#include "iptv.h"
#include "iptv_curated.h"

// Render user's channel list (main screen)
void render_iptv_user_channels(SDL_Surface* screen, IndicatorType show_setting,
							   int selected, int scroll_offset,
							   ScrollTextState* scroll_state) {
	GFX_clear(screen);
	char truncated[256];

	render_screen_header(screen, "Online TV", show_setting);

	int channel_count = IPTV_getUserChannelCount();
	const IPTVChannel* channels = IPTV_getUserChannels();

	ListLayout layout = UI_calcListLayout(screen);
	int scroll = scroll_offset;
	adjust_list_scroll(selected, &scroll, layout.items_per_page);

	for (int i = 0; i < layout.items_per_page && (scroll + i) < channel_count; i++) {
		int idx = scroll + i;
		const IPTVChannel* ch = &channels[idx];
		bool is_selected = (idx == selected);
		int y = layout.list_y + i * layout.item_h;

		ListItemPos pos = UI_renderListItemPill(screen, &layout, font.medium,
												ch->name, truncated,
												y, is_selected, 0);

		UI_renderListItemText(screen, scroll_state, ch->name, font.medium,
							  pos.text_x, pos.text_y,
							  pos.pill_width - SCALE1(BUTTON_PADDING * 2),
							  is_selected);
	}

	render_scroll_indicators(screen, scroll, layout.items_per_page, channel_count);

	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "PLAY", NULL});
}

// Render IPTV empty state (no channels added)
void render_iptv_empty(SDL_Surface* screen, IndicatorType show_setting) {
	GFX_clear(screen);
	render_screen_header(screen, "Online TV", show_setting);
	UI_renderEmptyState(screen, "No channels saved",
						"Press Y to manage channels", NULL);

	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "Y", "MANAGE", "B", "BACK", NULL});
}

// Render curated country list for browsing
void render_iptv_curated_countries(SDL_Surface* screen, IndicatorType show_setting,
								   int selected, int* scroll_offset) {
	GFX_clear(screen);

	int hw = screen->w;
	char truncated[256];

	render_screen_header(screen, "Browse Channels", show_setting);

	int country_count = IPTV_curated_get_country_count();
	const CuratedTVCountry* countries = IPTV_curated_get_countries();

	ListLayout layout = UI_calcListLayout(screen);
	adjust_list_scroll(selected, scroll_offset, layout.items_per_page);

	for (int i = 0; i < layout.items_per_page && *scroll_offset + i < country_count; i++) {
		int idx = *scroll_offset + i;
		const CuratedTVCountry* country = &countries[idx];
		bool is_selected = (idx == selected);

		int y = layout.list_y + i * layout.item_h;

		ListItemPos pos = UI_renderListItemPill(screen, &layout, font.medium,
												country->name, truncated,
												y, is_selected, 0);

		UI_renderListItemText(screen, NULL, country->name, font.medium,
							  pos.text_x, pos.text_y, layout.max_width, is_selected);

		// Channel count on right
		int curated_ch_count = IPTV_curated_get_channel_count(country->code);
		char count_str[32];
		snprintf(count_str, sizeof(count_str), "%d channels", curated_ch_count);
		SDL_Color count_color = is_selected ? COLOR_GRAY : COLOR_DARK_TEXT;
		SDL_Surface* count_text = TTF_RenderUTF8_Blended(font.tiny, count_str, count_color);
		if (count_text) {
			SDL_BlitSurface(count_text, NULL, screen, &(SDL_Rect){hw - count_text->w - SCALE1(PADDING * 2), y + (layout.item_h - count_text->h) / 2});
			SDL_FreeSurface(count_text);
		}
	}

	render_scroll_indicators(screen, *scroll_offset, layout.items_per_page, country_count);

	UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "SELECT", NULL});
}

// Render curated channels for a country
void render_iptv_curated_channels(SDL_Surface* screen, IndicatorType show_setting,
								  const char* country_code,
								  int selected, int* scroll_offset,
								  const int* sorted_indices, int sorted_count,
								  const char* toast_message, uint32_t toast_time) {
	GFX_clear(screen);

	int hw = screen->w;
	char truncated[256];

	// Get country name for title
	const char* country_name = "Channels";
	const CuratedTVCountry* countries = IPTV_curated_get_countries();
	int country_count = IPTV_curated_get_country_count();
	for (int i = 0; i < country_count; i++) {
		if (strcmp(countries[i].code, country_code) == 0) {
			country_name = countries[i].name;
			break;
		}
	}

	render_screen_header(screen, country_name, show_setting);

	int channel_count = 0;
	const CuratedTVChannel* channels = IPTV_curated_get_channels(country_code, &channel_count);

	ListLayout layout = UI_calcListLayout(screen);
	adjust_list_scroll(selected, scroll_offset, layout.items_per_page);

	// Determine if the currently selected channel is already added
	bool selected_exists = false;
	if (sorted_count > 0 && selected < sorted_count) {
		int sel_actual = sorted_indices[selected];
		if (sel_actual < channel_count) {
			selected_exists = IPTV_userChannelExists(channels[sel_actual].url);
		}
	}

	for (int i = 0; i < layout.items_per_page && *scroll_offset + i < sorted_count; i++) {
		int idx = *scroll_offset + i;
		int actual_idx = sorted_indices[idx];
		const CuratedTVChannel* channel = &channels[actual_idx];
		bool is_selected = (idx == selected);
		bool added = IPTV_userChannelExists(channel->url);

		int y = layout.list_y + i * layout.item_h;

		// Calculate prefix width for added indicator
		int prefix_width = 0;
		if (added) {
			int pw, ph;
			TTF_SizeUTF8(font.small, "[+]", &pw, &ph);
			prefix_width = pw + SCALE1(6);
		}

		// Render pill background and get text position
		int name_max_width = layout.max_width - prefix_width - SCALE1(60);
		int text_width = GFX_truncateText(font.medium, channel->name, truncated, name_max_width, SCALE1(BUTTON_PADDING * 2));
		int pill_width = MIN(layout.max_width, prefix_width + text_width + SCALE1(BUTTON_PADDING));

		SDL_Rect pill_rect = {SCALE1(PADDING), y, pill_width, layout.item_h};
		Fonts_drawListItemBg(screen, &pill_rect, is_selected);

		int text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
		int text_y = y + (layout.item_h - TTF_FontHeight(font.medium)) / 2;

		// Added indicator prefix
		if (added) {
			SDL_Color prefix_color = Fonts_getListTextColor(is_selected);
			SDL_Surface* prefix_text = TTF_RenderUTF8_Blended(font.small, "[+]", prefix_color);
			if (prefix_text) {
				SDL_BlitSurface(prefix_text, NULL, screen, &(SDL_Rect){text_x, y + (layout.item_h - prefix_text->h) / 2});
				SDL_FreeSurface(prefix_text);
			}
		}

		// Channel name
		UI_renderListItemText(screen, NULL, channel->name, font.medium,
							  text_x + prefix_width, text_y, name_max_width, is_selected);

		// Category on right
		if (channel->category[0]) {
			SDL_Color cat_color = is_selected ? COLOR_GRAY : COLOR_DARK_TEXT;
			SDL_Surface* cat_text = TTF_RenderUTF8_Blended(font.tiny, channel->category, cat_color);
			if (cat_text) {
				SDL_BlitSurface(cat_text, NULL, screen, &(SDL_Rect){hw - cat_text->w - SCALE1(PADDING * 2), y + (layout.item_h - cat_text->h) / 2});
				SDL_FreeSurface(cat_text);
			}
		}
	}

	render_scroll_indicators(screen, *scroll_offset, layout.items_per_page, sorted_count);

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);

	// Button hints - dynamic based on whether selected channel is already added
	if (selected_exists) {
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "REMOVE", NULL});
	} else {
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "ADD", NULL});
	}
}
