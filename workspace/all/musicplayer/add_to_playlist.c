#include <stdio.h>
#include <string.h>
#include "api.h"
#include "add_to_playlist.h"
#include "playlist_m3u.h"
#include "ui_keyboard.h"
#include "ui_listdialog.h"

// Internal state
static bool active = false;
static char track_path[512];
static char track_display_name[256];

static PlaylistInfo playlists[MAX_PLAYLISTS];
static int playlist_count = 0;

// Toast state (shown after adding)
static char toast_msg[128] = "";
static uint32_t toast_time = 0;

static void populate_items(void) {
	int total = playlist_count + 1; // +1 for "New Playlist"
	ListDialogItem items[LISTDIALOG_MAX_ITEMS];
	memset(items, 0, sizeof(ListDialogItem) * total);

	// Index 0: New Playlist
	snprintf(items[0].text, LISTDIALOG_MAX_TEXT, "+ New Playlist");
	items[0].prepend_icons[0] = -1;
	items[0].append_icons[0] = -1;

	// Index 1..N: existing playlists
	for (int i = 0; i < playlist_count; i++) {
		snprintf(items[i + 1].text, LISTDIALOG_MAX_TEXT, "%s", playlists[i].name);
		snprintf(items[i + 1].detail, LISTDIALOG_MAX_TEXT, "%d track%s",
				 playlists[i].track_count, playlists[i].track_count == 1 ? "" : "s");
		items[i + 1].prepend_icons[0] = -1;
		items[i + 1].append_icons[0] = -1;
	}

	ListDialog_setItems(items, total);
}

void AddToPlaylist_open(const char* path, const char* display_name) {
	if (!path)
		return;

	M3U_init();
	snprintf(track_path, sizeof(track_path), "%s", path);
	snprintf(track_display_name, sizeof(track_display_name), "%s",
			 display_name ? display_name : "");

	playlist_count = M3U_listPlaylists(playlists, MAX_PLAYLISTS);

	ListDialog_init("Add to Playlist");
	populate_items();

	active = true;
}

bool AddToPlaylist_isActive(void) {
	return active;
}

int AddToPlaylist_handleInput(void) {
	if (!active)
		return 1;

	ListDialogResult result = ListDialog_handleInput();

	if (result.action == LISTDIALOG_CANCEL) {
		ListDialog_quit();
		active = false;
		return 1;
	}

	if (result.action == LISTDIALOG_SELECTED) {
		if (result.index == 0) {
			// New Playlist
			char* name = UIKeyboard_open("Playlist name");
			PAD_poll();
			PAD_reset();
			if (name && name[0]) {
				if (M3U_create(name) == 0) {
					char new_path[512];
					snprintf(new_path, sizeof(new_path), "%s/%s.m3u", PLAYLISTS_DIR, name);
					M3U_addTrack(new_path, track_path, track_display_name);
					snprintf(toast_msg, sizeof(toast_msg), "Added to %s", name);
					toast_time = SDL_GetTicks();
				}
				free(name);
			}
		} else {
			// Existing playlist
			int idx = result.index - 1;
			if (idx >= 0 && idx < playlist_count) {
				if (M3U_containsTrack(playlists[idx].path, track_path)) {
					snprintf(toast_msg, sizeof(toast_msg), "Already in %s", playlists[idx].name);
					toast_time = SDL_GetTicks();
				} else {
					M3U_addTrack(playlists[idx].path, track_path, track_display_name);
					snprintf(toast_msg, sizeof(toast_msg), "Added to %s", playlists[idx].name);
					toast_time = SDL_GetTicks();
				}
			}
		}
		ListDialog_quit();
		active = false;
		return 1;
	}

	return 0;
}

void AddToPlaylist_render(SDL_Surface* screen) {
	if (!active)
		return;

	ListDialog_render(screen);
}

// Get toast message and time (for callers to display after dialog closes)
const char* AddToPlaylist_getToastMessage(void) {
	return toast_msg;
}

uint32_t AddToPlaylist_getToastTime(void) {
	return toast_time;
}

void AddToPlaylist_clearToast(void) {
	toast_msg[0] = '\0';
	toast_time = 0;
}
