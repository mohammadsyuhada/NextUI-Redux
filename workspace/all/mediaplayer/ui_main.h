#ifndef __UI_MAIN_H__
#define __UI_MAIN_H__

#include "api.h"
#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>

// Render the main menu
void render_menu(SDL_Surface* screen, IndicatorType show_setting, int menu_selected,
				 char* toast_message, uint32_t toast_time);

// Render controls help dialog overlay
void render_controls_help(SDL_Surface* screen, int app_state);

#endif
