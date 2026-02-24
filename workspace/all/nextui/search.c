#include "search.h"
#include "config.h"
#include "content.h"
#include "defines.h"
#include "imgloader.h"
#include "launcher.h"
#include "types.h"
#include "ui_components.h"
#include "ui_keyboard.h"
#include "ui_list.h"
#include "api.h"

#include <libgen.h>
#include <stdlib.h>
#include <string.h>

static Array* search_results = NULL;
static int search_selected = 0;
static int search_scroll = 0;
static ScrollTextState search_list_scroll = {0};

void Search_init(void) {
	search_results = NULL;
	search_selected = 0;
	search_scroll = 0;
}

void Search_quit(void) {
	if (search_results) {
		EntryArray_free(search_results);
		search_results = NULL;
	}
}

bool Search_open(void) {
	char* query = UIKeyboard_open("Search");
	PAD_poll();
	PAD_reset();

	if (!query || strlen(query) == 0) {
		if (query)
			free(query);
		return false;
	}

	// Free previous results
	if (search_results) {
		EntryArray_free(search_results);
		search_results = NULL;
	}

	search_results = Content_searchRoms(query);
	free(query);

	search_selected = 0;
	search_scroll = 0;
	memset(&search_list_scroll, 0, sizeof(search_list_scroll));

	return true;
}

SearchResult Search_handleInput(unsigned long now) {
	(void)now;
	SearchResult result = {0};
	result.screen = SCREEN_SEARCH;

	int total = search_results ? search_results->count : 0;

	if (PAD_justPressed(BTN_B)) {
		result.screen = SCREEN_GAMELIST;
		result.dirty = true;
		result.folderbgchanged = true;
		GFX_clearLayers(LAYER_SCROLLTEXT);
		if (search_list_scroll.cached_scroll_surface) {
			SDL_FreeSurface(search_list_scroll.cached_scroll_surface);
			search_list_scroll.cached_scroll_surface = NULL;
		}
		search_list_scroll.text[0] = '\0';
		search_list_scroll.needs_scroll = false;
		search_list_scroll.scroll_active = false;
		return result;
	}

	if (total == 0)
		return result;

	int old_selected = search_selected;
	ListLayout layout = UI_calcListLayout(screen);
	int items_per_page = layout.items_per_page;

	if (PAD_justRepeated(BTN_UP)) {
		search_selected--;
		if (search_selected < 0)
			search_selected = total - 1;
	} else if (PAD_justRepeated(BTN_DOWN)) {
		search_selected++;
		if (search_selected >= total)
			search_selected = 0;
	} else if (PAD_justRepeated(BTN_LEFT)) {
		search_selected -= items_per_page;
		if (search_selected < 0)
			search_selected = 0;
	} else if (PAD_justRepeated(BTN_RIGHT)) {
		search_selected += items_per_page;
		if (search_selected >= total)
			search_selected = total - 1;
	}

	UI_adjustListScroll(search_selected, &search_scroll, items_per_page);

	if (search_selected != old_selected)
		result.dirty = true;

	if (PAD_justPressed(BTN_A) && total > 0) {
		Entry* entry = search_results->items[search_selected];
		Entry_open(entry);
		result.startgame = true;
		result.dirty = true;
	}

	return result;
}

void Search_render(SDL_Surface* screen, SDL_Surface* blackBG, int lastScreen) {
	if (lastScreen != SCREEN_SEARCH) {
		onBackgroundLoaded(NULL);
		GFX_clearLayers(LAYER_THUMBNAIL);
	}

	int total = search_results ? search_results->count : 0;

	// Button hints
	{
		char* hints[5] = {NULL};
		int hi = 0;
		hints[hi++] = "B";
		hints[hi++] = "BACK";
		if (total > 0) {
			hints[hi++] = "A";
			hints[hi++] = "OPEN";
		}
		hints[hi] = NULL;
		UI_renderButtonHintBar(screen, hints);
	}

	if (total == 0) {
		UI_renderCenteredMessage(screen, "No results");
		return;
	}

	bool had_thumb = false;
	int ox = screen->w;

	if (CFG_getShowGameArt()) {
		Entry* selected_entry = search_results->items[search_selected];

		char path_copy[1024];
		strncpy(path_copy, selected_entry->path, sizeof(path_copy) - 1);
		path_copy[sizeof(path_copy) - 1] = '\0';
		char* rompath = dirname(path_copy);

		char* res_name = strrchr(selected_entry->path, '/');
		res_name = res_name ? res_name + 1 : (char*)selected_entry->path;
		char res_copy[1024];
		strncpy(res_copy, res_name, sizeof(res_copy) - 1);
		res_copy[sizeof(res_copy) - 1] = '\0';
		char* dot = strrchr(res_copy, '.');
		if (dot)
			*dot = '\0';

		char thumbpath[1024];
		snprintf(thumbpath, sizeof(thumbpath), "%s/.media/%s.png", rompath, res_copy);
		had_thumb = startLoadThumb(thumbpath);
		int max_w = (int)(screen->w - (screen->w * CFG_getGameArtWidth()));
		if (had_thumb)
			ox = (int)(max_w)-SCALE1(BUTTON_MARGIN * 5);
	}

	ListLayout layout = UI_calcListLayout(screen);
	int items_per_page = layout.items_per_page;

	UI_adjustListScroll(search_selected, &search_scroll, items_per_page);

	for (int i = 0; i < items_per_page && (search_scroll + i) < total; i++) {
		int idx = search_scroll + i;
		Entry* entry = search_results->items[idx];
		bool selected = (idx == search_selected);
		char* entry_name = entry->name;

		trimSortingMeta(&entry_name);

		int y = layout.list_y + i * layout.item_h;

		if (had_thumb)
			layout.max_width = MAX(0, ox + SCALE1(BUTTON_MARGIN) - SCALE1(PADDING * 2));

		char truncated[256];
		ListItemPos pos = UI_renderListItemPill(
			screen, &layout, font.large,
			entry_name, truncated, y, selected, 0);
		int text_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2);
		UI_renderListItemText(screen,
							  selected ? &search_list_scroll : NULL,
							  entry_name, font.large,
							  pos.text_x, pos.text_y, text_width, selected);
	}

	UI_renderScrollIndicators(screen, search_scroll, items_per_page, total);
}
