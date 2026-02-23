#ifndef __UI_IPTV_H__
#define __UI_IPTV_H__

#include <SDL2/SDL.h>
#include "api.h"
#include "ui_list.h"

// Render user's channel list (main screen)
void render_iptv_user_channels(SDL_Surface* screen, IndicatorType show_setting,
							   int selected, int scroll_offset,
							   ScrollTextState* scroll_state);

// Render IPTV empty state (no channels added)
void render_iptv_empty(SDL_Surface* screen, IndicatorType show_setting);

// Render curated country list for browsing
void render_iptv_curated_countries(SDL_Surface* screen, IndicatorType show_setting,
								   int selected, int* scroll_offset);

// Render curated channels for a country
void render_iptv_curated_channels(SDL_Surface* screen, IndicatorType show_setting,
								  const char* country_code,
								  int selected, int* scroll_offset,
								  const int* sorted_indices, int sorted_count,
								  const char* toast_message, uint32_t toast_time);

#endif
