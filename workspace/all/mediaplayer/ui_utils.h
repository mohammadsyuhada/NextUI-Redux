#ifndef __UI_UTILS_H__
#define __UI_UTILS_H__

#include <stdbool.h>
#include <stdint.h>
#include "vp_defines.h" // Brings in SDL2 via platform.h -> sdl.h
#include "api.h"		// For SDL types and TTF
#include "ui_list.h"

// Format duration as HH:MM:SS or MM:SS
void format_time(char* buf, int seconds);

// Render standard screen header (title pill + hardware status)
void render_screen_header(SDL_Surface* screen, const char* title, IndicatorType show_setting);

// Adjust scroll offset to keep selected item visible
void adjust_list_scroll(int selected, int* scroll, int items_per_page);

// Render scroll up/down indicators for lists
void render_scroll_indicators(SDL_Surface* screen, int scroll, int items_per_page, int total_count);

// Render a list item's pill with optional right-side badge area (settings-style two-layer)
// When badge_width > 0 and selected: THEME_COLOR2 outer pill + THEME_COLOR1 inner title pill
// When badge_width == 0: behaves like render_list_item_pill
ListItemBadgedPos render_list_item_pill_badged(SDL_Surface* screen, ListLayout* layout,
											   const char* text, char* truncated,
											   int y, bool selected, int badge_width);

// Position information returned by render_list_item_pill_rich
typedef struct {
	int pill_width;				// Width of the rendered pill
	int title_x, title_y;		// Row 1 position (medium font)
	int subtitle_x, subtitle_y; // Row 2 position (small font)
	int image_x, image_y;		// Image position (top-left corner)
	int image_size;				// Image width & height (square)
	int text_max_width;			// Max width for text (for scrolling)
} ListItemRichPos;

// Render a 2-row list item pill with image area on the left
// Height is 1.5x PILL_SIZE. Image is square, vertically centered.
// Row 1: title (medium font), Row 2: subtitle (small font)
// Caller renders image at image_x/image_y and text via UI_renderListItemText()
ListItemRichPos render_list_item_pill_rich(SDL_Surface* screen, ListLayout* layout,
										   const char* title, const char* subtitle,
										   char* truncated,
										   int y, bool selected, bool has_image,
										   int extra_subtitle_width);

// Position information returned by render_menu_item_pill
typedef struct {
	int pill_width; // Width of the rendered pill
	int text_x;		// X position for text (after padding)
	int text_y;		// Y position for text (vertically centered in pill)
	int item_y;		// Y position of this menu item
} MenuItemPos;

// Render a menu item's pill background and calculate text position
// Menu items have small spacing (2px) between them (item_h includes margin, pill uses PILL_SIZE)
// index: menu item index (0-based)
// prefix_width: extra width to account for (e.g., icon)
MenuItemPos render_menu_item_pill(SDL_Surface* screen, ListLayout* layout,
								  const char* text, char* truncated,
								  int index, bool selected, int prefix_width);

// ============================================
// Generic Simple Menu Rendering
// ============================================

// Callback to customize item label (e.g., "About" -> "About (Update Available)")
// Returns custom label or NULL to use default
typedef const char* (*MenuItemLabelCallback)(int index, const char* default_label,
											 char* buffer, int buffer_size);

// Callback to render right-side badge (e.g., queue count)
// Called after pill is rendered, can draw additional elements
typedef void (*MenuItemBadgeCallback)(SDL_Surface* screen, int index, bool selected,
									  int item_y, int item_h);

// Callback to get icon for a menu item
// Returns SDL_Surface* icon or NULL if no icon for this item
typedef SDL_Surface* (*MenuItemIconCallback)(int index, bool selected);

// Callback for custom text rendering (e.g., fixed prefix + scrolling suffix)
// Return true if custom rendering was handled, false to use default
typedef bool (*MenuItemCustomTextCallback)(SDL_Surface* screen, int index, bool selected,
										   int text_x, int text_y, int max_text_width);

// Configuration for generic simple menu rendering
typedef struct {
	const char* title;						// Header title
	const char** items;						// Array of menu item labels
	int item_count;							// Number of items
	const char* btn_b_label;				// B button label ("EXIT", "BACK", etc.)
	MenuItemLabelCallback get_label;		// Optional: customize item label
	MenuItemBadgeCallback render_badge;		// Optional: render right-side badge
	MenuItemIconCallback get_icon;			// Optional: get icon for item
	MenuItemCustomTextCallback render_text; // Optional: custom text rendering
} SimpleMenuConfig;

// Render a simple menu with optional customization callbacks
void render_simple_menu(SDL_Surface* screen, IndicatorType show_setting, int menu_selected,
						const SimpleMenuConfig* config);

// ============================================
// Rounded Rectangle Background
// ============================================

// Render a filled rounded rectangle background
// Works at any height (unlike pill asset which requires PILL_SIZE)
// Uses two overlapping rects to create corner inset effect
void render_rounded_rect_bg(SDL_Surface* screen, int x, int y, int w, int h, uint32_t color);


// ============================================
// Dialog Box
// ============================================

// Layout information returned by render_dialog_box
typedef struct {
	int box_x, box_y; // Top-left corner of the box
	int box_w, box_h; // Box dimensions
	int content_x;	  // Left margin for content
	int content_w;	  // Width available for content
} DialogBox;

// Render a dialog box centered on screen with white border
// Clears GPU scroll text layer + fills entire screen black + draws box with border
// Returns box dimensions for the caller to render content inside
DialogBox render_dialog_box(SDL_Surface* screen, int box_w, int box_h);

// ============================================
// Empty State
// ============================================

#endif
