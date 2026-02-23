#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "ui_downloader.h"
#include "ui_fonts.h"
#include "ui_list.h"
#include "ui_toast.h"

// Scroll text state for YouTube results (selected item)
static ScrollTextState downloader_results_scroll_text = {0};

// Scroll text state for YouTube download queue (selected item)
static ScrollTextState downloader_queue_scroll_text = {0};

// YouTube sub-menu items
static const char* youtube_menu_items[] = {"Search Music", "Download Queue"};
#define YOUTUBE_MENU_COUNT 2

// Format download speed for display
static void format_download_speed(char* buf, int buf_size, int bytes_per_sec) {
	if (bytes_per_sec <= 0) {
		snprintf(buf, buf_size, "0 B/s");
	} else if (bytes_per_sec < 1024) {
		snprintf(buf, buf_size, "%d B/s", bytes_per_sec);
	} else if (bytes_per_sec < 1024 * 1024) {
		snprintf(buf, buf_size, "%.1f KB/s", bytes_per_sec / 1024.0);
	} else {
		snprintf(buf, buf_size, "%.1f MB/s", bytes_per_sec / (1024.0 * 1024.0));
	}
}

// Format ETA for display
static void format_download_eta(char* buf, int buf_size, int seconds) {
	if (seconds <= 0) {
		buf[0] = '\0';
	} else if (seconds < 60) {
		snprintf(buf, buf_size, "%ds", seconds);
	} else if (seconds < 3600) {
		snprintf(buf, buf_size, "%dm%ds", seconds / 60, seconds % 60);
	} else {
		snprintf(buf, buf_size, "%dh%dm", seconds / 3600, (seconds % 3600) / 60);
	}
}

// Label callback for queue count on Download Queue menu item
static const char* youtube_menu_get_label(int index, const char* default_label,
										  char* buffer, int buffer_size) {
	if (index == 1) { // Download Queue
		int qcount = Downloader_queueCount();
		if (qcount > 0) {
			snprintf(buffer, buffer_size, "Download Queue (%d)", qcount);
			return buffer;
		}
	}
	return NULL; // Use default label
}

// Render YouTube sub-menu
void render_downloader_menu(SDL_Surface* screen, IndicatorType show_setting, int menu_selected,
							char* toast_message, uint32_t toast_time) {
	SimpleMenuConfig config = {
		.title = "Downloader",
		.items = youtube_menu_items,
		.item_count = YOUTUBE_MENU_COUNT,
		.btn_b_label = "BACK",
		.get_label = youtube_menu_get_label,
		.render_badge = NULL,
		.get_icon = NULL};
	UI_renderSimpleMenu(screen, menu_selected, &config);

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);
}

// Render YouTube searching status
void render_downloader_searching(SDL_Surface* screen, IndicatorType show_setting, const char* search_query) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;

	UI_renderMenuBar(screen, "Searching...");

	// Searching message
	char search_msg[300];
	snprintf(search_msg, sizeof(search_msg), "Searching for: %s", search_query);
	SDL_Surface* query_text = TTF_RenderUTF8_Blended(font.medium, search_msg, COLOR_GRAY);
	if (query_text) {
		int qx = (hw - query_text->w) / 2;
		if (qx < SCALE1(PADDING))
			qx = SCALE1(PADDING);
		SDL_BlitSurface(query_text, NULL, screen, &(SDL_Rect){qx, hh / 2 - SCALE1(30)});
		SDL_FreeSurface(query_text);
	}

	// Loading indicator
	const char* loading = "Please wait...";
	SDL_Surface* load_text = TTF_RenderUTF8_Blended(font.medium, loading, COLOR_WHITE);
	if (load_text) {
		SDL_BlitSurface(load_text, NULL, screen, &(SDL_Rect){(hw - load_text->w) / 2, hh / 2 + SCALE1(10)});
		SDL_FreeSurface(load_text);
	}
}

// Render YouTube search results
void render_downloader_results(SDL_Surface* screen, IndicatorType show_setting,
							   const char* search_query,
							   DownloaderResult* results, int result_count,
							   int selected, int* scroll,
							   char* toast_message, uint32_t toast_time, bool searching) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;
	char truncated[256];

	// Title with search query
	char title[128];
	snprintf(title, sizeof(title), "Results: %s", search_query);
	UI_renderMenuBar(screen, title);

	// Use common list layout calculation
	ListLayout layout = UI_calcListLayout(screen);

	// Adjust scroll (only if there's a selection)
	if (selected >= 0) {
		UI_adjustListScroll(selected, scroll, layout.items_per_page);
	}

	// Reserve space for duration on the right (format: "99:59" max)
	int dur_w, dur_h;
	TTF_SizeUTF8(font.tiny, "99:59", &dur_w, &dur_h);
	int duration_reserved = dur_w + SCALE1(PADDING * 2); // Duration width + gap
	int max_width = layout.max_width - duration_reserved;

	for (int i = 0; i < layout.items_per_page && *scroll + i < result_count; i++) {
		int idx = *scroll + i;
		DownloaderResult* result = &results[idx];
		bool is_selected = (idx == selected);
		bool in_queue = Downloader_isInQueue(result->video_id);

		int y = layout.list_y + i * layout.item_h;

		// Calculate indicator width if in queue
		int indicator_width = 0;
		if (in_queue) {
			int ind_w, ind_h;
			TTF_SizeUTF8(font.tiny, "[+]", &ind_w, &ind_h);
			indicator_width = ind_w + SCALE1(4);
		}

		// Calculate text width for pill sizing
		int pill_width = Fonts_calcListPillWidth(font.medium, result->title, truncated, max_width, indicator_width);

		// Background pill (sized to text width)
		SDL_Rect pill_rect = {SCALE1(PADDING), y, pill_width, layout.item_h};
		Fonts_drawListItemBg(screen, &pill_rect, is_selected);

		int title_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
		int text_y = y + (layout.item_h - TTF_FontHeight(font.medium)) / 2;

		// Show indicator if already in queue
		if (in_queue) {
			SDL_Surface* indicator = TTF_RenderUTF8_Blended(font.tiny, "[+]", is_selected ? uintToColour(THEME_COLOR5_255) : COLOR_GRAY);
			if (indicator) {
				SDL_BlitSurface(indicator, NULL, screen, &(SDL_Rect){title_x, y + (layout.item_h - indicator->h) / 2});
				title_x += indicator->w + SCALE1(4);
				SDL_FreeSurface(indicator);
			}
		}

		// Title - use common text rendering with scrolling for selected items
		int title_max_w = pill_width - SCALE1(BUTTON_PADDING * 2) - indicator_width;
		UI_renderListItemText(screen, &downloader_results_scroll_text, result->title, font.medium,
							  title_x, text_y, title_max_w, is_selected);

		// Duration (always on right, outside pill)
		if (result->duration_sec > 0) {
			char dur[16];
			int m = result->duration_sec / 60;
			int s = result->duration_sec % 60;
			snprintf(dur, sizeof(dur), "%d:%02d", m, s);
			SDL_Surface* dur_text = TTF_RenderUTF8_Blended(font.tiny, dur, COLOR_GRAY);
			if (dur_text) {
				SDL_BlitSurface(dur_text, NULL, screen, &(SDL_Rect){hw - dur_text->w - SCALE1(PADDING * 2), y + (layout.item_h - dur_text->h) / 2});
				SDL_FreeSurface(dur_text);
			}
		}
	}

	// Empty results message
	if (result_count == 0) {
		const char* msg = searching ? "Searching..." : "No results found";
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, msg, COLOR_GRAY);
		if (text) {
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, hh / 2 - text->h / 2});
			SDL_FreeSurface(text);
		}
	}

	// Toast notification (rendered to GPU layer above scroll text)
	UI_renderToast(screen, toast_message, toast_time);

	// Button hints
	// Dynamic hint based on queue status (only show A action if item is selected)
	if (selected >= 0 && result_count > 0) {
		const char* action_hint = "DOWNLOAD";
		DownloaderResult* selected_result = &results[selected];
		if (Downloader_isInQueue(selected_result->video_id)) {
			action_hint = "QUEUED";
		}
		UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", (char*)action_hint, NULL});
	} else {
		UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", NULL});
	}
}

// Render YouTube download queue (podcast-style with progress/speed/ETA)
void render_downloader_queue(SDL_Surface* screen, IndicatorType show_setting,
							 int queue_selected, int* queue_scroll) {
	GFX_clear(screen);

	int hw = screen->w;
	char truncated[256];

	int qcount = 0;
	DownloaderQueueItem* queue = Downloader_queueGet(&qcount);
	const DownloaderDownloadStatus* dl_status = Downloader_getDownloadStatus();

	// Title with completion count
	char title[64];
	if (dl_status->total_items > 0) {
		snprintf(title, sizeof(title), "Downloads (%d/%d)",
				 dl_status->completed_count, dl_status->total_items);
	} else {
		snprintf(title, sizeof(title), "Download Queue");
	}
	UI_renderMenuBar(screen, title);

	// Empty queue message
	if (qcount == 0) {
		downloader_queue_clear_scroll();
		UI_renderEmptyState(screen, "Queue is empty", "Search and add songs to download", NULL);
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		return;
	}

	// Two-row pill layout (like podcast download queue)
	ListLayout layout = UI_calcListLayout(screen);
	layout.item_h = SCALE1(PILL_SIZE) * 3 / 2;
	layout.items_per_page = layout.list_h / layout.item_h;
	if (layout.items_per_page > 5)
		layout.items_per_page = 5;
	UI_adjustListScroll(queue_selected, queue_scroll, layout.items_per_page);

	for (int i = 0; i < layout.items_per_page && *queue_scroll + i < qcount; i++) {
		int idx = *queue_scroll + i;
		DownloaderQueueItem* item = &queue[idx];
		bool is_selected = (idx == queue_selected);

		int y = layout.list_y + i * layout.item_h;

		// Build subtitle string for pill sizing (must reflect actual rendered width)
		char subtitle[128] = "";
		switch (item->status) {
		case DOWNLOADER_STATUS_PENDING:
			snprintf(subtitle, sizeof(subtitle), "Queued");
			break;
		case DOWNLOADER_STATUS_DOWNLOADING: {
			// Include speed/ETA in subtitle sizing so pill is wide enough
			char speed_str[32], eta_str[32];
			format_download_speed(speed_str, sizeof(speed_str), item->speed_bps);
			format_download_eta(eta_str, sizeof(eta_str), item->eta_sec);
			if (eta_str[0]) {
				snprintf(subtitle, sizeof(subtitle), "%d%%  %s  ETA %s",
						 item->progress_percent, speed_str, eta_str);
			} else {
				snprintf(subtitle, sizeof(subtitle), "%d%%  %s",
						 item->progress_percent, speed_str);
			}
			break;
		}
		case DOWNLOADER_STATUS_COMPLETE:
			snprintf(subtitle, sizeof(subtitle), "Complete");
			break;
		case DOWNLOADER_STATUS_FAILED:
			snprintf(subtitle, sizeof(subtitle), "Failed");
			break;
		}

		// Two-row pill (title + subtitle)
		int badge_width = 0;
		// For downloading state, subtitle includes progress bar + gap before text
		int extra_sub_w = (item->status == DOWNLOADER_STATUS_DOWNLOADING) ? SCALE1(50) + SCALE1(6) : 0;
		ListItemBadgedPos pos = UI_renderListItemPillBadged(screen, &layout, font.medium, font.small, font.tiny, item->title, subtitle, truncated, y, is_selected, badge_width, extra_sub_w);

		// Title text (row 1)
		UI_renderListItemText(screen, is_selected ? &downloader_queue_scroll_text : NULL,
							  item->title, font.medium,
							  pos.text_x, pos.text_y,
							  pos.text_max_width, is_selected);

		// Subtitle (row 2) — status-dependent
		if (item->status == DOWNLOADER_STATUS_DOWNLOADING) {
			// Progress bar + speed + ETA (like podcast)
			int bar_w = SCALE1(50);
			int bar_h = SCALE1(4);
			int bar_x = pos.subtitle_x;
			int bar_y = pos.subtitle_y + (TTF_FontHeight(font.small) - bar_h) / 2;

			// Bar background
			SDL_Rect bar_bg = {bar_x, bar_y, bar_w, bar_h};
			SDL_FillRect(screen, &bar_bg, SDL_MapRGB(screen->format, 60, 60, 60));

			// Bar fill
			int fill_w = (bar_w * item->progress_percent) / 100;
			if (fill_w > 0) {
				SDL_Rect bar_fill = {bar_x, bar_y, fill_w, bar_h};
				SDL_FillRect(screen, &bar_fill, THEME_COLOR2);
			}

			// Speed and ETA text
			char info_str[64];
			char speed_str[32];
			char eta_str[32];
			format_download_speed(speed_str, sizeof(speed_str), item->speed_bps);
			format_download_eta(eta_str, sizeof(eta_str), item->eta_sec);

			if (eta_str[0]) {
				snprintf(info_str, sizeof(info_str), "%d%%  %s  ETA %s",
						 item->progress_percent, speed_str, eta_str);
			} else {
				snprintf(info_str, sizeof(info_str), "%d%%  %s",
						 item->progress_percent, speed_str);
			}

			SDL_Surface* info_surf = TTF_RenderUTF8_Blended(font.small, info_str, COLOR_GRAY);
			if (info_surf) {
				int info_x = bar_x + bar_w + SCALE1(6);
				int avail_w = pos.text_max_width - bar_w - SCALE1(6);
				SDL_Rect src = {0, 0, info_surf->w > avail_w ? avail_w : info_surf->w, info_surf->h};
				SDL_BlitSurface(info_surf, &src, screen, &(SDL_Rect){info_x, pos.subtitle_y});
				SDL_FreeSurface(info_surf);
			}
		} else if (item->status == DOWNLOADER_STATUS_PENDING) {
			SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, "Queued", COLOR_GRAY);
			if (s) {
				SDL_BlitSurface(s, NULL, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
				SDL_FreeSurface(s);
			}
		} else if (item->status == DOWNLOADER_STATUS_FAILED) {
			SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, "Failed", (SDL_Color){200, 80, 80, 255});
			if (s) {
				SDL_BlitSurface(s, NULL, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
				SDL_FreeSurface(s);
			}
		} else if (item->status == DOWNLOADER_STATUS_COMPLETE) {
			SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, "Complete", (SDL_Color){80, 200, 80, 255});
			if (s) {
				SDL_BlitSurface(s, NULL, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
				SDL_FreeSurface(s);
			}
		}
	}

	// Scroll indicators
	UI_renderScrollIndicators(screen, *queue_scroll, layout.items_per_page, qcount);

	// Button hints
	if (qcount > 0) {
		UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "X", "REMOVE", "B", "BACK", NULL});
	} else {
		UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", NULL});
	}
}

// Check if YouTube results list has active scrolling (for refresh optimization)
bool downloader_results_needs_scroll_refresh(void) {
	return ScrollText_isScrolling(&downloader_results_scroll_text);
}

// Check if results scroll needs a render to transition (delay phase)
bool downloader_results_scroll_needs_render(void) {
	return ScrollText_needsRender(&downloader_results_scroll_text);
}

// Check if YouTube queue list has active scrolling (for refresh optimization)
bool downloader_queue_needs_scroll_refresh(void) {
	return ScrollText_isScrolling(&downloader_queue_scroll_text);
}

// Check if queue scroll needs a render to transition (delay phase)
bool downloader_queue_scroll_needs_render(void) {
	return ScrollText_needsRender(&downloader_queue_scroll_text);
}

// Animate YouTube results scroll only (GPU mode, no screen redraw needed)
void downloader_results_animate_scroll(void) {
	ScrollText_animateOnly(&downloader_results_scroll_text);
}

// Animate YouTube queue scroll only (GPU mode, no screen redraw needed)
void downloader_queue_animate_scroll(void) {
	ScrollText_animateOnly(&downloader_queue_scroll_text);
}

// Clear YouTube queue scroll state (call when queue items are removed)
void downloader_queue_clear_scroll(void) {
	memset(&downloader_queue_scroll_text, 0, sizeof(downloader_queue_scroll_text));
	GFX_clearLayers(LAYER_SCROLLTEXT);
}

// Clear YouTube results scroll state and toast
void downloader_results_clear_scroll(void) {
	memset(&downloader_results_scroll_text, 0, sizeof(downloader_results_scroll_text));
	GFX_clearLayers(LAYER_SCROLLTEXT);
	UI_clearToast();
}
