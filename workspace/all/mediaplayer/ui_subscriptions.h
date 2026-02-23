#ifndef __UI_SUBSCRIPTIONS_H__
#define __UI_SUBSCRIPTIONS_H__

#include <SDL2/SDL.h>
#include "api.h"
#include "subscriptions.h"
#include "youtube.h"
#include "ui_list.h"

// Render subscription list
void render_subscriptions_list(SDL_Surface* screen, IndicatorType show_setting,
							   const SubscriptionList* subs,
							   int selected, int scroll_offset,
							   ScrollTextState* scroll_state);

// Render channel videos (search results for a channel)
void render_channel_videos(SDL_Surface* screen, IndicatorType show_setting,
						   const char* channel_name,
						   YouTubeSearchResults* results,
						   int selected, int scroll_offset,
						   ScrollTextState* scroll_state);

// Render searching channel videos
void render_channel_searching(SDL_Surface* screen, const char* channel_name);

// Render subscriptions empty state
void render_subscriptions_empty(SDL_Surface* screen, IndicatorType show_setting);

// Clear avatar thumbnail cache (call on cleanup/module exit)
void SubUI_clearAvatarCache(void);

#endif
