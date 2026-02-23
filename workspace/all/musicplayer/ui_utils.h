#ifndef __UI_UTILS_H__
#define __UI_UTILS_H__

#include <stdbool.h>
#include <stdint.h>
#include "player.h"
#include "ui_list.h"

// Format duration as MM:SS
void format_time(char* buf, int ms);

// Get format name string
const char* get_format_name(AudioFormat format);

// GPU scroll without background (for player title)
void ScrollText_renderGPU_NoBg(ScrollTextState* state, TTF_Font* font,
							   SDL_Color color, int x, int y);

// Adjust scroll offset to keep selected item visible
void adjust_list_scroll(int selected, int* scroll, int items_per_page);

// Render scroll up/down indicators for lists
void render_scroll_indicators(SDL_Surface* screen, int scroll, int items_per_page, int total_count);

// Render a list item's pill with optional right-side badge area
ListItemBadgedPos render_list_item_pill_badged(SDL_Surface* screen, ListLayout* layout,
											   const char* text, const char* subtitle,
											   char* truncated,
											   int y, bool selected, int badge_width,
											   int extra_subtitle_width);

// Position information returned by render_list_item_pill_rich
typedef struct {
	int pill_width;
	int title_x, title_y;
	int subtitle_x, subtitle_y;
	int image_x, image_y;
	int image_size;
	int text_max_width;
} ListItemRichPos;

// Render a 2-row list item pill with image area on the left
ListItemRichPos render_list_item_pill_rich(SDL_Surface* screen, ListLayout* layout,
										   const char* title, const char* subtitle,
										   char* truncated,
										   int y, bool selected, bool has_image,
										   int extra_subtitle_width);

// Position information returned by render_menu_item_pill
typedef struct {
	int pill_width;
	int text_x;
	int text_y;
	int item_y;
} MenuItemPos;

// Render a menu item's pill background and calculate text position
MenuItemPos render_menu_item_pill(SDL_Surface* screen, ListLayout* layout,
								  const char* text, char* truncated,
								  int index, bool selected, int prefix_width);

// ============================================
// Generic Simple Menu Rendering
// ============================================

typedef const char* (*MenuItemLabelCallback)(int index, const char* default_label,
											 char* buffer, int buffer_size);
typedef void (*MenuItemBadgeCallback)(SDL_Surface* screen, int index, bool selected,
									  int item_y, int item_h);
typedef SDL_Surface* (*MenuItemIconCallback)(int index, bool selected);
typedef bool (*MenuItemCustomTextCallback)(SDL_Surface* screen, int index, bool selected,
										   int text_x, int text_y, int max_text_width);

typedef struct {
	const char* title;
	const char** items;
	int item_count;
	const char* btn_b_label;
	MenuItemLabelCallback get_label;
	MenuItemBadgeCallback render_badge;
	MenuItemIconCallback get_icon;
	MenuItemCustomTextCallback render_text;
} SimpleMenuConfig;

void render_simple_menu(SDL_Surface* screen, IndicatorType show_setting, int menu_selected,
						const SimpleMenuConfig* config);

// ============================================
// Rounded Rectangle Background
// ============================================

void render_rounded_rect_bg(SDL_Surface* screen, int x, int y, int w, int h, uint32_t color);


// ============================================
// Dialog Box
// ============================================

typedef struct {
	int box_x, box_y;
	int box_w, box_h;
	int content_x;
	int content_w;
} DialogBox;

DialogBox render_dialog_box(SDL_Surface* screen, int box_w, int box_h);

// ============================================
#endif
