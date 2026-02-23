#ifndef UI_LIST_H
#define UI_LIST_H

#include <stdbool.h>
#include <stdint.h>
#include "defines.h"
#include "api.h"

// Scrolling text state for marquee animation
typedef struct {
	char text[512];						// Text to display
	int text_width;						// Full text width in pixels
	int max_width;						// Maximum display width
	uint32_t start_time;				// Animation start time
	bool needs_scroll;					// True if text is wider than max_width
	int scroll_offset;					// Current pixel offset for smooth scrolling
	bool use_gpu_scroll;				// True = use GPU layer (for lists), False = software (for player)
	int last_x, last_y;					// Last render position (for animate-only mode)
	TTF_Font* last_font;				// Last font used (for animate-only mode)
	SDL_Color last_color;				// Last color used (for animate-only mode)
	SDL_Surface* cached_scroll_surface; // Cached surface for GPU scroll (no bg)
	bool scroll_active;					// True once GPU scroll has actually started (after delay)
} ScrollTextState;

void ScrollText_reset(ScrollTextState* state, const char* text,
					  TTF_Font* font, int max_width, bool use_gpu);
bool ScrollText_isScrolling(ScrollTextState* state);
bool ScrollText_needsRender(ScrollTextState* state);
void ScrollText_activateAfterDelay(ScrollTextState* state);
void ScrollText_animateOnly(ScrollTextState* state);
void ScrollText_render(ScrollTextState* state, TTF_Font* font,
					   SDL_Color color, SDL_Surface* screen, int x, int y);

// Unified update: checks for text change, resets if needed, and renders
// use_gpu: true for lists (GPU layer with pill bg), false for player (software, no bg)
void ScrollText_update(ScrollTextState* state, const char* text, TTF_Font* font,
					   int max_width, SDL_Color color, SDL_Surface* screen, int x, int y, bool use_gpu);

// GPU scroll without background (for player title)
// Uses PLAT_drawOnLayer to render to GPU layer without pill background
void ScrollText_renderGPU_NoBg(ScrollTextState* state, TTF_Font* font,
							   SDL_Color color, int x, int y);

// ---- List Layout ----

typedef struct {
	int list_y;			// Y where list starts
	int list_h;			// Height available for list
	int item_h;			// Height per item
	int items_per_page; // Visible item count
	int max_width;		// Max content width
} ListLayout;

ListLayout UI_calcListLayout(SDL_Surface* screen);

// ---- Pill Rendering (stateless) ----

typedef struct {
	int pill_width;
	int text_x;
	int text_y;
} ListItemPos;

int UI_calcListPillWidth(TTF_Font* font, const char* text, char* truncated,
						 int max_width, int prefix_width);
void UI_drawListItemBg(SDL_Surface* dst, SDL_Rect* rect, bool selected);
SDL_Color UI_getListTextColor(bool selected);

// Render a list item's pill background and calculate text position
// Combines: Fonts_calcListPillWidth + Fonts_drawListItemBg + text position calculation
// prefix_width: extra width to account for (e.g., checkbox, indicator)
ListItemPos UI_renderListItemPill(SDL_Surface* screen, ListLayout* layout,
								  TTF_Font* font, const char* text,
								  char* truncated, int y, bool selected,
								  int prefix_width);

void UI_renderListItemText(SDL_Surface* screen, ScrollTextState* scroll_state,
						   const char* text, TTF_Font* font,
						   int text_x, int text_y, int max_text_width,
						   bool selected);

// ---- Badged Pill Rendering ----

// Position information returned by render_list_item_pill_badged
typedef struct {
	int pill_width;		// Width of the title (inner) pill
	int text_x;			// X position for title text
	int text_y;			// Y position for title text (row 1)
	int subtitle_x;		// X position for subtitle text (row 2)
	int subtitle_y;		// Y position for subtitle text (row 2)
	int badge_x;		// X position for badge content start
	int badge_y;		// Y position for badge content (centered)
	int total_width;	// Total width of title pill + badge area
	int text_max_width; // Max width for text content
} ListItemBadgedPos;

// Render a two-row list item pill with optional right-side badge area.
// Item height is 1.5x PILL_SIZE. Title (title_font) + subtitle (subtitle_font).
// When badge_width > 0 and selected: THEME_COLOR2 outer capsule + THEME_COLOR1 inner.
// When badge_width == 0: single THEME_COLOR1 capsule.
// Caller renders badge content at badge_x, badge_y.
ListItemBadgedPos UI_renderListItemPillBadged(
	SDL_Surface* screen, ListLayout* layout,
	TTF_Font* title_font, TTF_Font* subtitle_font, TTF_Font* badge_font,
	const char* text, const char* subtitle, char* truncated,
	int y, bool selected, int badge_width, int extra_subtitle_width);

// ---- Settings Page Component ----

typedef struct {
	const char* label; // Left-side text
	const char* value; // Right-side text (NULL for none)
	int swatch;		   // Color swatch (-1 for none)
	int cycleable;	   // Show "< >" arrows when selected
	const char* desc;  // Description shown when item is selected
	void (*custom_draw)(SDL_Surface* screen, void* ctx,
						int x, int y, int w, int h, int selected);
	void* custom_draw_ctx;
} UISettingsItem;

// Render a compact settings page (9 rows: 8 items + 1 description)
// Handles layout calculation, scrolling, item rendering, scroll indicators,
// status message, and description text.
void UI_renderSettingsPage(SDL_Surface* screen, ListLayout* layout,
						   UISettingsItem* items, int count,
						   int selected, int* scroll,
						   const char* status_msg);

// ---- Settings Row Rendering ----

// Render a settings-style row: label (left) + value (right)
// - selected: 2-layer pill (THEME_COLOR2 full-width + THEME_COLOR1 label-width),
//   value with "< >" arrows
// - unselected: no background, label + value text only
// - swatch_color: if non-negative, draws a color swatch square next to the value
// Returns the x position where value rendering ended (left edge of value area)
int UI_renderSettingsRow(SDL_Surface* screen, ListLayout* layout,
						 const char* label, const char* value,
						 int y, bool selected, int swatch_color);

// ---- Scroll Helpers ----

void UI_adjustListScroll(int selected, int* scroll, int items_per_page);
void UI_renderScrollIndicators(SDL_Surface* screen, int scroll,
							   int items_per_page, int total_count);

// ---- Pill Animation (non-threaded, for main-loop driven apps) ----

typedef struct {
	int current_y;
	int target_y;
	int start_y;
	int frame;
	int total_frames;
	bool active;
} PillAnimState;

void UI_pillAnimInit(PillAnimState* state);
void UI_pillAnimSetTarget(PillAnimState* state, int target_y, bool animate);
int UI_pillAnimTick(PillAnimState* state);
bool UI_pillAnimIsActive(PillAnimState* state);

// ---- Rich Pill Rendering ----

// Position information returned by UI_renderListItemPillRich
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
ListItemRichPos UI_renderListItemPillRich(SDL_Surface* screen, ListLayout* layout,
										  const char* title, const char* subtitle,
										  char* truncated,
										  int y, bool selected, bool has_image,
										  int extra_subtitle_width);

// ---- Menu Item Pill Rendering ----

// Position information returned by UI_renderMenuItemPill
typedef struct {
	int pill_width; // Width of the rendered pill
	int text_x;		// X position for text (after padding)
	int text_y;		// Y position for text (vertically centered in pill)
	int item_y;		// Y position of this menu item
} MenuItemPos;

// Render a menu item's pill background and calculate text position
// Menu items use PILL_SIZE height. index: menu item index (0-based)
// prefix_width: extra width to account for (e.g., icon)
MenuItemPos UI_renderMenuItemPill(SDL_Surface* screen, ListLayout* layout,
								  const char* text, char* truncated,
								  int index, bool selected, int prefix_width);

// ---- Rounded Rectangle Background ----

// Render a filled rounded rectangle background
// Works at any height (unlike pill asset which requires PILL_SIZE)
void UI_renderRoundedRectBg(SDL_Surface* screen, int x, int y, int w, int h, uint32_t color);

// ---- Generic Simple Menu Rendering ----

// Callback to customize item label
typedef const char* (*MenuItemLabelCallback)(int index, const char* default_label,
											 char* buffer, int buffer_size);

// Callback to render right-side badge
typedef void (*MenuItemBadgeCallback)(SDL_Surface* screen, int index, bool selected,
									  int item_y, int item_h);

// Callback to get icon for a menu item
typedef SDL_Surface* (*MenuItemIconCallback)(int index, bool selected);

// Callback for custom text rendering
// Return true if custom rendering was handled, false to use default
typedef bool (*MenuItemCustomTextCallback)(SDL_Surface* screen, int index, bool selected,
										   int text_x, int text_y, int max_text_width);

// Configuration for generic simple menu rendering
typedef struct {
	const char* title;						// Header title
	const char** items;						// Array of menu item labels (can be NULL if get_label provided)
	int item_count;							// Number of items
	const char* btn_b_label;				// B button label ("EXIT", "BACK", etc.)
	MenuItemLabelCallback get_label;		// Optional: customize item label
	MenuItemBadgeCallback render_badge;		// Optional: render right-side badge
	MenuItemIconCallback get_icon;			// Optional: get icon for item
	MenuItemCustomTextCallback render_text; // Optional: custom text rendering
} SimpleMenuConfig;

// Render a simple menu with optional customization callbacks
void UI_renderSimpleMenu(SDL_Surface* screen, int menu_selected,
						 const SimpleMenuConfig* config);

#endif // UI_LIST_H
