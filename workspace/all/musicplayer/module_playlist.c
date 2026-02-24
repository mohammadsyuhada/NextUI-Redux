#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "ui_toast.h"
#include "module_playlist.h"
#include "module_player.h"
#include "playlist_m3u.h"
#include "playlist.h"
#include "ui_keyboard.h"
#include "display_helper.h"
#include "ui_components.h"
#include "ui_playlist.h"
#include "ui_list.h"

// Internal states
typedef enum {
	PLAYLIST_INTERNAL_LIST,
	PLAYLIST_INTERNAL_DETAIL
} PlaylistInternalState;

// List state
static PlaylistInfo playlists[MAX_PLAYLISTS];
static int playlist_count = 0;
static int list_selected = 0;
static int list_scroll = 0;

// Detail state
static PlaylistTrack detail_tracks[PLAYLIST_MAX_TRACKS];
static int detail_track_count = 0;
static int detail_selected = 0;
static int detail_scroll = 0;
static int current_playlist_index = -1; // Index into playlists[] for current detail view

// Toast
static char playlist_toast_message[128] = "";
static uint32_t playlist_toast_time = 0;

// Confirmation dialog state
static bool show_confirm = false;
static char confirm_name[256] = "";
static int confirm_action = 0; // 0 = delete playlist, 1 = remove track
static int confirm_target = -1;

// Controls help state IDs (for render_controls_help)
#define PLAYLIST_LIST_HELP_STATE 50
#define PLAYLIST_DETAIL_HELP_STATE 51

static void refresh_playlists(void) {
	playlist_count = M3U_listPlaylists(playlists, MAX_PLAYLISTS);
}

static void refresh_detail(void) {
	if (current_playlist_index < 0 || current_playlist_index >= playlist_count)
		return;
	M3U_loadTracks(playlists[current_playlist_index].path, detail_tracks, PLAYLIST_MAX_TRACKS, &detail_track_count);
}

static void show_toast(const char* msg) {
	snprintf(playlist_toast_message, sizeof(playlist_toast_message), "%s", msg);
	playlist_toast_time = SDL_GetTicks();
}

ModuleExitReason PlaylistModule_run(SDL_Surface* screen) {
	M3U_init();
	UIKeyboard_init();
	refresh_playlists();

	PlaylistInternalState state = PLAYLIST_INTERNAL_LIST;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (1) {
		GFX_startFrame();
		PAD_poll();

		// Handle confirmation dialog
		if (show_confirm) {
			if (PAD_justPressed(BTN_A)) {
				if (confirm_action == 0) {
					// Delete playlist
					int idx = confirm_target;
					if (idx >= 0 && idx < playlist_count) {
						M3U_delete(playlists[idx].path);
						refresh_playlists();
						// Adjust selection
						if (list_selected >= playlist_count)
							list_selected = playlist_count - 1;
						if (list_selected < 0)
							list_selected = 0;
						show_toast("Playlist deleted");
					}
				} else if (confirm_action == 1) {
					// Remove track
					int idx = confirm_target;
					if (current_playlist_index >= 0 && current_playlist_index < playlist_count) {
						M3U_removeTrack(playlists[current_playlist_index].path, idx);
						refresh_detail();
						// Update parent count
						playlists[current_playlist_index].track_count = detail_track_count;
						if (detail_selected >= detail_track_count)
							detail_selected = detail_track_count - 1;
						if (detail_selected < 0)
							detail_selected = 0;
						show_toast("Track removed");
					}
				}
				show_confirm = false;
				dirty = 1;
				continue;
			}
			if (PAD_justPressed(BTN_B)) {
				show_confirm = false;
				dirty = 1;
				continue;
			}
			// Render confirmation (dialog covers entire screen)
			const char* confirm_title = (confirm_action == 0) ? "Delete Playlist?" : "Remove Track?";
			UI_renderConfirmDialog(screen, confirm_title, confirm_name);
			GFX_flip(screen);
			GFX_sync();
			continue;
		}

		// Handle global input
		int app_state_for_help = (state == PLAYLIST_INTERNAL_LIST) ? PLAYLIST_LIST_HELP_STATE : PLAYLIST_DETAIL_HELP_STATE;
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, app_state_for_help);
		if (global.should_quit) {
			return MODULE_EXIT_QUIT;
		}
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}

		if (state == PLAYLIST_INTERNAL_LIST) {
			int total_items = playlist_count;

			if (PAD_justPressed(BTN_B)) {
				GFX_clearLayers(LAYER_SCROLLTEXT);
				return MODULE_EXIT_TO_MENU;
			} else if (total_items > 0 && PAD_justRepeated(BTN_UP)) {
				list_selected = (list_selected > 0) ? list_selected - 1 : total_items - 1;
				dirty = 1;
			} else if (total_items > 0 && PAD_justRepeated(BTN_DOWN)) {
				list_selected = (list_selected < total_items - 1) ? list_selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				// Enter playlist detail
				if (list_selected >= 0 && list_selected < playlist_count) {
					current_playlist_index = list_selected;
					refresh_detail();
					detail_selected = 0;
					detail_scroll = 0;
					state = PLAYLIST_INTERNAL_DETAIL;
					GFX_clearLayers(LAYER_SCROLLTEXT);
					dirty = 1;
				}
			} else if (PAD_justPressed(BTN_Y)) {
				// New Playlist
				DisplayHelper_prepareForExternal();
				char* name = UIKeyboard_open("Playlist name");
				PAD_poll();
				PAD_reset();
				DisplayHelper_recoverDisplay();
				{
					SDL_Surface* ns = DisplayHelper_getReinitScreen();
					if (ns)
						screen = ns;
				}
				if (name && name[0]) {
					if (M3U_create(name) == 0) {
						show_toast("Playlist created");
						refresh_playlists();
					} else {
						show_toast("Already exists");
					}
					free(name);
				} else if (name) {
					free(name);
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_X)) {
				// Delete playlist
				if (list_selected >= 0 && list_selected < playlist_count) {
					snprintf(confirm_name, sizeof(confirm_name), "%s", playlists[list_selected].name);
					confirm_action = 0;
					confirm_target = list_selected;
					show_confirm = true;
					GFX_clearLayers(LAYER_SCROLLTEXT);
					dirty = 1;
				}
			}

			// Animate scroll
			if (playlist_list_needs_scroll_refresh()) {
				playlist_list_animate_scroll();
			}
			if (playlist_list_scroll_needs_render())
				dirty = 1;

		} else if (state == PLAYLIST_INTERNAL_DETAIL) {
			int total_items = detail_track_count;

			if (PAD_justPressed(BTN_B)) {
				GFX_clearLayers(LAYER_SCROLLTEXT);
				refresh_playlists(); // Refresh counts
				state = PLAYLIST_INTERNAL_LIST;
				dirty = 1;
			} else if (total_items > 0 && PAD_justRepeated(BTN_UP)) {
				detail_selected = (detail_selected > 0) ? detail_selected - 1 : total_items - 1;
				dirty = 1;
			} else if (total_items > 0 && PAD_justRepeated(BTN_DOWN)) {
				detail_selected = (detail_selected < total_items - 1) ? detail_selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				if (detail_track_count > 0) {
					// Play the playlist starting from selected track
					GFX_clearLayers(LAYER_SCROLLTEXT);
					PlayerModule_setResumePlaylistPath(playlists[current_playlist_index].path);
					PlayerModule_runWithPlaylist(screen, detail_tracks, detail_track_count, detail_selected);
					PlayerModule_setResumePlaylistPath(NULL);
					// On return, refresh and go back to detail
					refresh_detail();
					if (detail_selected >= detail_track_count)
						detail_selected = detail_track_count - 1;
					if (detail_selected < 0)
						detail_selected = 0;
					dirty = 1;
				}
			} else if (PAD_justPressed(BTN_X)) {
				// Remove track
				if (detail_selected >= 0 && detail_selected < detail_track_count) {
					snprintf(confirm_name, sizeof(confirm_name), "%s", detail_tracks[detail_selected].name);
					confirm_action = 1;
					confirm_target = detail_selected;
					show_confirm = true;
					GFX_clearLayers(LAYER_SCROLLTEXT);
					dirty = 1;
				}
			}

			// Animate scroll
			if (playlist_list_needs_scroll_refresh()) {
				playlist_list_animate_scroll();
			}
			if (playlist_list_scroll_needs_render())
				dirty = 1;
		}

		// Power management
		ModuleCommon_PWR_update(&dirty, &show_setting);

		// Render
		if (dirty) {
			// Bounds check: if current playlist was deleted externally, go back to list
			if (state == PLAYLIST_INTERNAL_DETAIL &&
				(current_playlist_index < 0 || current_playlist_index >= playlist_count)) {
				state = PLAYLIST_INTERNAL_LIST;
			}

			if (state == PLAYLIST_INTERNAL_LIST) {
				int items_per_page = UI_calcListLayout(screen).items_per_page;
				UI_adjustListScroll(list_selected, &list_scroll, items_per_page);
				render_playlist_list(screen, show_setting, playlists, playlist_count, list_selected, list_scroll);
			} else {
				int items_per_page = UI_calcListLayout(screen).items_per_page;
				UI_adjustListScroll(detail_selected, &detail_scroll, items_per_page);
				render_playlist_detail(screen, show_setting, playlists[current_playlist_index].name,
									   detail_tracks, detail_track_count, detail_selected, detail_scroll);
			}

			// Toast
			UI_renderToast(screen, playlist_toast_message, playlist_toast_time);

			GFX_flip(screen);
			dirty = 0;

			ModuleCommon_tickToast(playlist_toast_message, playlist_toast_time, &dirty);
		} else {
			GFX_sync();
		}
	}
}
