#ifndef __UI_YOUTUBE_H__
#define __UI_YOUTUBE_H__

#include <SDL2/SDL.h>
#include "api.h"
#include "youtube.h"
#include "ui_list.h"

// Carousel state for fullscreen thumbnail display
typedef struct {
	SDL_Surface* current_surface;  // Currently loaded thumbnail (owned, freed on change)
	int loaded_index;			   // Index of the loaded thumbnail (-1 = none)
	SDL_Surface* gradient_overlay; // Bottom gradient overlay (owned)
} YouTubeCarouselState;

// Initialize carousel state and create gradient overlay
void YouTubeCarousel_init(YouTubeCarouselState* state, SDL_Surface* screen);

// Free carousel surfaces
void YouTubeCarousel_cleanup(YouTubeCarouselState* state);

// Load thumbnail for given index from disk cache; returns true if loaded
bool YouTubeCarousel_loadThumbnail(YouTubeCarouselState* state, int index,
								   YouTubeSearchResults* results);

// Load thumbnail from per-channel directory; returns true if loaded
bool YouTubeCarousel_loadChannelThumbnail(YouTubeCarouselState* state, int index,
										  YouTubeSearchResults* results,
										  const char* channel_id);

// Render fullscreen carousel: thumbnail + gradient + text overlay + button hints
void render_youtube_carousel(SDL_Surface* screen, IndicatorType show_setting,
							 YouTubeSearchResults* results, int selected,
							 YouTubeCarouselState* carousel,
							 ScrollTextState* scroll_state);

// Render searching animation
void render_youtube_searching(SDL_Surface* screen);

// Render URL resolving animation
void render_youtube_resolving(SDL_Surface* screen);

// Render error screen
void render_youtube_error(SDL_Surface* screen, const char* error_msg);

// Render YouTube sub-menu (Search / Subscriptions)
void render_youtube_submenu(SDL_Surface* screen, IndicatorType show_setting, int selected);

// Render channel details page overlay
void render_youtube_channel_info(SDL_Surface* screen, YouTubeChannelInfo* info,
								 const char* channel_name, bool is_subscribed,
								 bool is_loading);

#endif
