#include <stdio.h>
#include <string.h>

#include "vp_defines.h"
#include "api.h"
#include "ui_main.h"
#include "ui_utils.h"
#include "ui_icons.h"
#include "module_common.h"
#include "module_menu.h"
// Base menu items (always present)
static const char* base_menu_items[] = {"Library", "YouTube", "Online TV", "Settings"};
#define BASE_MENU_ITEM_COUNT 4

// Render the main menu
void render_menu(SDL_Surface* screen, IndicatorType show_setting, int menu_selected,
				 char* toast_message, uint32_t toast_time) {
	SimpleMenuConfig config = {
		.title = "Media Player",
		.items = base_menu_items,
		.item_count = BASE_MENU_ITEM_COUNT,
		.btn_b_label = "EXIT",
		.get_label = NULL,
		.render_badge = NULL,
		.get_icon = NULL,
		.render_text = NULL};
	render_simple_menu(screen, show_setting, menu_selected, &config);

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);
}

// Controls help text for each page/state
typedef struct {
	const char* button;
	const char* action;
} ControlHelp;

// Main menu controls (A/B shown in footer)
static const ControlHelp main_menu_controls[] = {
	{"Up/Down", "Navigate"},
	{"Start (hold)", "Exit App"},
	{NULL, NULL}};

// File browser controls (A/B shown in footer)
static const ControlHelp browser_controls[] = {
	{"Up/Down", "Navigate"},
	{"Start (hold)", "Exit App"},
	{NULL, NULL}};

// Settings/About controls
static const ControlHelp settings_controls[] = {
	{"Start (hold)", "Exit App"},
	{NULL, NULL}};

// IPTV user channel list controls
static const ControlHelp iptv_list_controls[] = {
	{"Up/Down", "Navigate"},
	{"Y", "Browse Channels"},
	{"X", "Remove Channel"},
	{"Start (hold)", "Exit App"},
	{NULL, NULL}};

// IPTV curated browse controls
static const ControlHelp iptv_curated_controls[] = {
	{"Up/Down", "Navigate"},
	{"Start (hold)", "Exit App"},
	{NULL, NULL}};

// YouTube search results controls
static const ControlHelp youtube_results_controls[] = {
	{"Left/Right", "Navigate"},
	{"Up", "Channel Info"},
	{"Y", "New Search"},
	{"Start (hold)", "Exit App"},
	{NULL, NULL}};

// YouTube menu controls
static const ControlHelp youtube_menu_controls[] = {
	{"Up/Down", "Navigate"},
	{"Start (hold)", "Exit App"},
	{NULL, NULL}};

// Subscriptions list controls
static const ControlHelp subscriptions_controls[] = {
	{"Up/Down", "Navigate"},
	{"X", "Remove"},
	{"Start (hold)", "Exit App"},
	{NULL, NULL}};

// Generic/default controls
static const ControlHelp default_controls[] = {
	{"Start (hold)", "Exit App"},
	{NULL, NULL}};

// Render controls help dialog overlay
void render_controls_help(SDL_Surface* screen, int app_state) {
	int hw = screen->w;
	int hh = screen->h;
	(void)hw;
	(void)hh;

	// Select controls based on state
	const ControlHelp* controls;
	const char* page_title;

	switch (app_state) {
	case STATE_MENU:
		controls = main_menu_controls;
		page_title = "Main Menu";
		break;
	case STATE_BROWSER:
		controls = browser_controls;
		page_title = "File Browser";
		break;
	case STATE_PLAYING:
		controls = default_controls;
		page_title = "Media Player";
		break;
	case STATE_SETTINGS:
		controls = settings_controls;
		page_title = "Settings";
		break;
	case STATE_IPTV_LIST:
		controls = iptv_list_controls;
		page_title = "IPTV";
		break;
	case STATE_IPTV_CURATED_COUNTRIES:
	case STATE_IPTV_CURATED_CHANNELS:
		controls = iptv_curated_controls;
		page_title = "Browse Channels";
		break;
	case STATE_YOUTUBE_RESULTS:
		controls = youtube_results_controls;
		page_title = "YouTube";
		break;
	case STATE_YOUTUBE_MENU:
		controls = youtube_menu_controls;
		page_title = "YouTube";
		break;
	case STATE_SUBSCRIPTIONS:
		controls = subscriptions_controls;
		page_title = "Subscriptions";
		break;
	default:
		controls = default_controls;
		page_title = "Controls";
		break;
	}

	// Count controls
	int control_count = 0;
	while (controls[control_count].button != NULL) {
		control_count++;
	}

	// Dialog box
	int line_height = SCALE1(18);
	int hint_gap = SCALE1(15);
	int box_h = SCALE1(60) + (control_count * line_height) + hint_gap;
	DialogBox db = render_dialog_box(screen, SCALE1(240), box_h);

	// Title text (left aligned)
	SDL_Surface* title_surf = TTF_RenderUTF8_Blended(font.medium, page_title, COLOR_WHITE);
	if (title_surf) {
		SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){db.content_x, db.box_y + SCALE1(10)});
		SDL_FreeSurface(title_surf);
	}

	int y_offset = db.box_y + SCALE1(35);
	int right_col = db.box_x + SCALE1(90);

	for (int i = 0; i < control_count; i++) {
		// Button name
		SDL_Surface* btn_surf = TTF_RenderUTF8_Blended(font.small, controls[i].button, COLOR_GRAY);
		if (btn_surf) {
			SDL_BlitSurface(btn_surf, NULL, screen, &(SDL_Rect){db.content_x, y_offset});
			SDL_FreeSurface(btn_surf);
		}

		// Action description
		SDL_Surface* action_surf = TTF_RenderUTF8_Blended(font.small, controls[i].action, COLOR_WHITE);
		if (action_surf) {
			SDL_BlitSurface(action_surf, NULL, screen, &(SDL_Rect){right_col, y_offset});
			SDL_FreeSurface(action_surf);
		}

		y_offset += line_height;
	}

	// Button hint at bottom (left aligned, same gap as title from top)
	const char* hint = "Press any button to close";
	SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(font.small, hint, COLOR_GRAY);
	if (hint_surf) {
		int hint_y = db.box_y + db.box_h - SCALE1(10) - hint_surf->h;
		SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){db.content_x, hint_y});
		SDL_FreeSurface(hint_surf);
	}
}

// Render confirmation dialog overlay (title + optional content + "A: Yes  B: No")
void render_confirmation_dialog(SDL_Surface* screen, const char* content, const char* title) {
	bool has_content = content && content[0];
	int box_h = has_content ? SCALE1(110) : SCALE1(90);
	DialogBox db = render_dialog_box(screen, SCALE1(280), box_h);
	int hw = screen->w;

	// Title text
	if (!title)
		title = "Confirm?";
	int title_y = has_content ? db.box_y + SCALE1(15) : db.box_y + SCALE1(20);
	SDL_Surface* title_surf = TTF_RenderUTF8_Blended(font.medium, title, COLOR_WHITE);
	if (title_surf) {
		SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){(hw - title_surf->w) / 2, title_y});
		SDL_FreeSurface(title_surf);
	}

	// Content text (truncated if needed)
	if (has_content) {
		char truncated[64];
		GFX_truncateText(font.small, content, truncated, db.box_w - SCALE1(20), 0);
		SDL_Surface* name_surf = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_GRAY);
		if (name_surf) {
			SDL_BlitSurface(name_surf, NULL, screen, &(SDL_Rect){(hw - name_surf->w) / 2, db.box_y + SCALE1(45)});
			SDL_FreeSurface(name_surf);
		}
	}

	// Button hints
	int hint_y = has_content ? db.box_y + SCALE1(75) : db.box_y + SCALE1(55);
	const char* hint = "A: Yes   B: No";
	SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(font.small, hint, COLOR_GRAY);
	if (hint_surf) {
		SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){(hw - hint_surf->w) / 2, hint_y});
		SDL_FreeSurface(hint_surf);
	}
}

// Check if menu scroll needs continuous redraw (no scrolling menu text in video player)
bool menu_needs_scroll_redraw(void) {
	return false;
}
