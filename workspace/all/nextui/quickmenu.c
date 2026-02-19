#include "quickmenu.h"
#include "config.h"
#include "content.h"
#include "defines.h"
#include "imgloader.h"
#include "launcher.h"
#include "types.h"
#include "ui_components.h"
#include <msettings.h>

static Array* quick;		// EntryArray
static Array* quickActions; // EntryArray

typedef enum {
	QM_ROW_ITEMS = 0,
	QM_ROW_TOGGLES = 1,
} QuickMenuRow;

static QuickMenuRow qm_row = QM_ROW_ITEMS;
static int qm_col = 0;
static int qm_slot = 0;
static int qm_shift = 0;
static int qm_slots = 0;

void QuickMenu_init(int simple_mode) {
	quick = getQuickEntries(simple_mode);
	quickActions = getQuickToggles(simple_mode);
	qm_slots =
		QUICK_SWITCHER_COUNT > quick->count ? quick->count : QUICK_SWITCHER_COUNT;
}

void QuickMenu_quit(void) {
	EntryArray_free(quick);
	EntryArray_free(quickActions);
}

void QuickMenu_resetSelection(void) {
	qm_row = QM_ROW_ITEMS;
	qm_col = 0;
	qm_slot = 0;
	qm_shift = 0;
}

QuickMenuResult QuickMenu_handleInput(unsigned long now) {
	QuickMenuResult result = {0};
	result.screen = SCREEN_QUICKMENU;

	int qm_total = qm_row == QM_ROW_ITEMS ? quick->count : quickActions->count;

	if (PAD_justPressed(BTN_B) || PAD_tappedMenu(now)) {
		result.screen = SCREEN_GAMELIST;
		result.folderbgchanged = true;
		result.dirty = true;
	} else if (PAD_justReleased(BTN_A)) {
		Entry* selected =
			qm_row == QM_ROW_ITEMS ? quick->items[qm_col] : quickActions->items[qm_col];
		if (selected->type != ENTRY_DIP) {
			result.screen = SCREEN_GAMELIST;
			// prevent restoring list state, game list screen currently isnt our
			// nav origin
			top->selected = 0;
			top->start = 0;
			int qm_rc = MAIN_ROW_COUNT - 1;
			top->end = top->start + qm_rc;
			restore.depth = -1;
			restore.relative = -1;
			restore.selected = 0;
			restore.start = 0;
			restore.end = 0;
		}
		Entry_open(selected);
		result.dirty = true;
	} else if (PAD_justPressed(BTN_RIGHT)) {
		if (qm_row == QM_ROW_ITEMS && qm_total > qm_slots) {
			qm_col++;
			if (qm_col >= qm_total) {
				qm_col = 0;
				qm_shift = 0;
				qm_slot = 0;
			} else {
				qm_slot++;
				if (qm_slot >= qm_slots) {
					qm_slot = qm_slots - 1;
					qm_shift++;
				}
			}
		} else {
			qm_col += 1;
			if (qm_col >= qm_total) {
				qm_col = 0;
			}
		}
		result.dirty = true;
	} else if (PAD_justPressed(BTN_LEFT)) {
		if (qm_row == QM_ROW_ITEMS && qm_total > qm_slots) {
			qm_col -= 1;
			if (qm_col < 0) {
				qm_col = qm_total - 1;
				qm_shift = qm_total - qm_slots;
				qm_slot = qm_slots - 1;
			} else {
				qm_slot--;
				if (qm_slot < 0) {
					qm_slot = 0;
					qm_shift--;
				}
			}
		} else {
			qm_col -= 1;
			if (qm_col < 0) {
				qm_col = qm_total - 1;
			}
		}
		result.dirty = true;
	} else if (PAD_justPressed(BTN_DOWN)) {
		if (qm_row == QM_ROW_ITEMS) {
			qm_row = QM_ROW_TOGGLES;
			qm_col = 0;
			result.dirty = true;
		}
	} else if (PAD_justPressed(BTN_UP)) {
		if (qm_row == QM_ROW_TOGGLES) {
			qm_row = QM_ROW_ITEMS;
			qm_col = qm_slot + qm_shift;
			result.dirty = true;
		}
	}

	return result;
}

void QuickMenu_render(int lastScreen, int show_setting, int ow,
					  char* folderBgPath, size_t folderBgPathSize) {
	if (lastScreen != SCREEN_QUICKMENU) {
		GFX_clearLayers(LAYER_BACKGROUND);
		GFX_clearLayers(LAYER_THUMBNAIL);
	}

	Entry* current =
		qm_row == QM_ROW_ITEMS ? quick->items[qm_col] : quickActions->items[qm_col];
	char newBgPath[MAX_PATH];
	char fallbackBgPath[MAX_PATH];
	bool show_off =
		(current->quickId == QUICK_WIFI && !CFG_getWifi()) ||
		(current->quickId == QUICK_BLUETOOTH && !CFG_getBluetooth());
	snprintf(newBgPath, sizeof(newBgPath),
			 SDCARD_PATH "/.media/quick_%s%s.png", current->name,
			 show_off ? "_off" : "");
	snprintf(fallbackBgPath, sizeof(fallbackBgPath),
			 SDCARD_PATH "/.media/quick.png");

	if (!exists(newBgPath))
		strncpy(newBgPath, fallbackBgPath, sizeof(newBgPath) - 1);

	if (strcmp(newBgPath, folderBgPath) != 0) {
		strncpy(folderBgPath, newBgPath, folderBgPathSize - 1);
		startLoadFolderBackground(newBgPath, onBackgroundLoaded);
	}

	if (show_setting && !GetHDMI())
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "OPEN", NULL}, GFX_getHardwareHintPairs(show_setting));
	else
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "OPEN", NULL}, NULL);

	if (CFG_getShowQuickswitcherUI()) {
#define MENU_ITEM_SIZE 72	 // item size, top line
#define MENU_MARGIN_Y 32	 // space between main UI elements and quick menu
#define MENU_MARGIN_X 40	 // space between main UI elements and quick menu
#define MENU_ITEM_MARGIN 18	 // space between items, top line
#define MENU_TOGGLE_MARGIN 8 // space between items, bottom line
#define MENU_LINE_MARGIN 8	 // space between top and bottom line

		int item_space_y =
			screen->h -
			SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + // top pill area
				   MENU_MARGIN_Y + MENU_LINE_MARGIN + PILL_SIZE +
				   MENU_MARGIN_Y + // our own area
				   BUTTON_MARGIN + PILL_SIZE + PADDING);
		int item_size = SCALE1(MENU_ITEM_SIZE);
		int item_extra_y = item_space_y - item_size;
		int item_space_x = screen->w - SCALE1(PADDING + MENU_MARGIN_X +
											  MENU_MARGIN_X + PADDING);
		// extra left margin for the first item in order to properly center
		// all of them in the available space
		int item_inset_x =
			(item_space_x - SCALE1(qm_slots * MENU_ITEM_SIZE +
								   (qm_slots - 1) * MENU_ITEM_MARGIN)) /
			2;

		int ox = SCALE1(PADDING + MENU_MARGIN_X) + item_inset_x;
		int oy = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + MENU_MARGIN_Y) +
				 item_extra_y / 2;
		// just to keep selection visible.
		// every display should be able to fit three items, we shift
		// horizontally to accomodate.
		ox -= qm_shift * (item_size + SCALE1(MENU_ITEM_MARGIN));
		for (int c = 0; c < quick->count; c++) {
			SDL_Rect item_rect = {ox, oy, item_size, item_size};
			Entry* item = quick->items[c];

			SDL_Color text_color = uintToColour(THEME_COLOR4_255);
			uint32_t item_color = THEME_COLOR3;
			uint32_t icon_color = THEME_COLOR4;

			if (qm_row == QM_ROW_ITEMS && qm_col == c) {
				text_color = uintToColour(THEME_COLOR5_255);
				item_color = THEME_COLOR1;
				icon_color = THEME_COLOR5;
			}

			GFX_blitRectColor(ASSET_STATE_BG, screen, &item_rect, item_color);

			char icon_path[MAX_PATH];
			snprintf(icon_path, sizeof(icon_path),
					 SDCARD_PATH "/.system/res/%s@%ix.png", item->name,
					 FIXED_SCALE);
			SDL_Surface* bmp = IMG_Load(icon_path);
			if (bmp)
				bmp = UI_convertSurface(bmp, screen);
			if (bmp) {
				int x = (item_rect.w - bmp->w) / 2;
				int y =
					(item_rect.h - SCALE1(FONT_TINY + BUTTON_MARGIN) - bmp->h) /
					2;
				SDL_Rect destRect = {ox + x, oy + y, 0,
									 0}; // width/height not required
				GFX_blitSurfaceColor(bmp, NULL, screen, &destRect, icon_color);
			}
			if (bmp)
				SDL_FreeSurface(bmp);

			int w, h;
			GFX_sizeText(font.tiny, item->name, SCALE1(FONT_TINY), &w, &h);
			SDL_Rect text_rect = {
				item_rect.x + (item_size - w) / 2,
				item_rect.y + item_size - h - SCALE1(BUTTON_MARGIN), w, h};
			GFX_blitText(font.tiny, item->name, SCALE1(FONT_TINY), text_color,
						 screen, &text_rect);

			ox += item_rect.w + SCALE1(MENU_ITEM_MARGIN);
		}

		ox = SCALE1(PADDING + MENU_MARGIN_X);
		ox += (screen->w -
			   SCALE1(PADDING + MENU_MARGIN_X + MENU_MARGIN_X + PADDING) -
			   SCALE1(quickActions->count * PILL_SIZE) -
			   SCALE1((quickActions->count - 1) * MENU_TOGGLE_MARGIN)) /
			  2;
		oy = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + MENU_MARGIN_Y +
					MENU_LINE_MARGIN) +
			 item_size + item_extra_y / 2;
		for (int c = 0; c < quickActions->count; c++) {
			SDL_Rect item_rect = {ox, oy, SCALE1(PILL_SIZE), SCALE1(PILL_SIZE)};
			Entry* item = quickActions->items[c];

			SDL_Color text_color = uintToColour(THEME_COLOR4_255);
			uint32_t item_color = THEME_COLOR3;
			uint32_t icon_color = THEME_COLOR4;

			if (qm_row == QM_ROW_TOGGLES && qm_col == c) {
				text_color = uintToColour(THEME_COLOR5_255);
				item_color = THEME_COLOR1;
				icon_color = THEME_COLOR5;
			}

			GFX_blitPillColor(ASSET_WHITE_PILL, screen, &item_rect, item_color,
							  RGB_WHITE);

			int asset = ASSET_WIFI;
			switch (item->quickId) {
			case QUICK_WIFI:
				asset = CFG_getWifi() ? ASSET_WIFI : ASSET_WIFI_OFF;
				break;
			case QUICK_BLUETOOTH:
				asset = CFG_getBluetooth() ? ASSET_BLUETOOTH : ASSET_BLUETOOTH_OFF;
				break;
			case QUICK_SLEEP:
				asset = ASSET_SUSPEND;
				break;
			case QUICK_REBOOT:
				asset = ASSET_RESTART;
				break;
			case QUICK_POWEROFF:
				asset = ASSET_POWEROFF;
				break;
			case QUICK_SETTINGS:
				asset = ASSET_SETTINGS;
				break;
			case QUICK_PAK_STORE:
				asset = ASSET_STORE;
				break;
			default:
				break;
			}

			SDL_Rect rect;
			GFX_assetRect(asset, &rect);
			int x = item_rect.x;
			int y = item_rect.y;
			x += (SCALE1(PILL_SIZE) - rect.w) / 2;
			y += (SCALE1(PILL_SIZE) - rect.h) / 2;

			GFX_blitAssetColor(asset, NULL, screen, &(SDL_Rect){x, y},
							   icon_color);

			ox += item_rect.w + SCALE1(MENU_TOGGLE_MARGIN);
		}
	}
}
