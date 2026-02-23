#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_player.h"
#include "player.h"
#include "spectrum.h"
#include "browser.h"
#include "playlist.h"
#include "ui_music.h"
#include "ui_album_art.h"
#include "ui_main.h"
#include "ui_components.h"
#include "lyrics.h"
#include "settings.h"
#include "add_to_playlist.h"
#include "ui_toast.h"
#include "resume.h"
#include "playlist_m3u.h"
#include "background.h"
#include "album_art.h"

// Music folder path
#define MUSIC_PATH SDCARD_PATH "/Music"

// Internal states
typedef enum {
	PLAYER_INTERNAL_BROWSER,
	PLAYER_INTERNAL_PLAYING
} PlayerInternalState;

// Module state
static BrowserContext browser = {0};
static bool shuffle_enabled = false;
static bool repeat_enabled = false;
static PlaylistContext playlist = {0};
static bool playlist_active = false;
static bool initialized = false;

// Delete confirmation state
static bool show_delete_confirm = false;
static char delete_target_path[512] = "";
static char delete_target_name[256] = "";

// Screen off state (module-local)
static bool screen_off = false;

// Resume: M3U playlist path (set by PlaylistModule before runWithPlaylist)
static char resume_playlist_path[512] = "";

// Resume: last save timestamp for periodic updates
static uint32_t last_resume_save = 0;


// Clear all player GPU overlay layers
static void clear_gpu_layers(void) {
	GFX_clearLayers(LAYER_SCROLLTEXT);
	PLAT_clearLayers(LAYER_SPECTRUM);
	PLAT_clearLayers(LAYER_PLAYTIME);
	PLAT_clearLayers(LAYER_LYRICS);
	PLAT_GPU_Flip();
}

// Helper to load directory
static void load_directory(const char* path) {
	Browser_loadDirectory(&browser, path, MUSIC_PATH);
}

// Initialize player module
static void init_player(void) {
	if (initialized)
		return;
	mkdir(MUSIC_PATH, 0755);
	load_directory(MUSIC_PATH);
	initialized = true;
}

// Try to load and play a track, returns true on success
static bool try_load_and_play(const char* path) {
	if (Player_load(path) == 0) {
		Player_play();
		const TrackInfo* info = Player_getTrackInfo();

		// Fetch album art (async) and lyrics after playback starts
		if (info && !Player_getAlbumArt()) {
			const char* artist = info->artist[0] ? info->artist : "";
			const char* title = info->title[0] ? info->title : "";
			if (artist[0] || title[0]) {
				album_art_fetch(artist, title);
			}
		}
		if (Settings_getLyricsEnabled() && info) {
			Lyrics_fetch(info->artist, info->title, info->duration_ms / 1000);
		}

		// Save resume state on every track change
		const char* name = (info && info->title[0]) ? info->title : NULL;
		if (!name) {
			const char* slash = strrchr(path, '/');
			name = slash ? slash + 1 : path;
		}
		if (resume_playlist_path[0] && playlist_active) {
			Resume_savePlaylist(resume_playlist_path, path, name,
								Playlist_getCurrentIndex(&playlist), 0);
		} else {
			int idx = playlist_active ? Playlist_getCurrentIndex(&playlist) : browser.selected;
			Resume_saveFiles(browser.current_path, path, name, idx, 0);
		}
		last_resume_save = SDL_GetTicks();

		return true;
	}
	return false;
}

// Try to play a playlist track by index (-1 means current). Returns true on success.
static bool playlist_try_play(int idx) {
	const PlaylistTrack* track = (idx < 0)
									 ? Playlist_getCurrentTrack(&playlist)
									 : Playlist_getTrack(&playlist, idx);
	return track && try_load_and_play(track->path);
}

// Pick a random audio file from the browser (excluding current). Returns true on success.
static bool browser_pick_random(void) {
	int audio_count = Browser_countAudioFiles(&browser);
	if (audio_count <= 1)
		return false;

	int random_idx = rand() % (audio_count - 1);
	int count = 0;
	for (int i = 0; i < browser.entry_count; i++) {
		if (!browser.entries[i].is_dir && i != browser.selected) {
			if (count == random_idx) {
				browser.selected = i;
				return try_load_and_play(browser.entries[i].path);
			}
			count++;
		}
	}
	return false;
}

// Pick the next audio file in the browser after current. Returns true on success.
static bool browser_pick_next(void) {
	for (int i = browser.selected + 1; i < browser.entry_count; i++) {
		if (!browser.entries[i].is_dir) {
			browser.selected = i;
			return try_load_and_play(browser.entries[i].path);
		}
	}
	return false;
}

// Handle next track logic
static bool handle_track_ended(void) {
	if (repeat_enabled) {
		if (playlist_active)
			return playlist_try_play(-1);
		return try_load_and_play(browser.entries[browser.selected].path);
	}

	if (shuffle_enabled) {
		if (playlist_active)
			return playlist_try_play(Playlist_shuffle(&playlist));
		return browser_pick_random();
	}

	if (playlist_active) {
		int next_idx = Playlist_next(&playlist);
		if (next_idx < 0)
			return false; // End of playlist
		return playlist_try_play(next_idx);
	}
	return browser_pick_next();
}

// Start playback of a track (load + play + init spectrum)
static bool start_playback(const char* path) {
	// Stop any other background player before starting music playback
	if (Background_getActive() != BG_MUSIC) {
		Background_stopAll();
	}
	if (try_load_and_play(path)) {
		Spectrum_init();
		ModuleCommon_recordInputTime();
		ModuleCommon_setAutosleepDisabled(true);
		return true;
	}
	return false;
}

// Clean up playback state
static void cleanup_playback(bool quit_spectrum) {
	clear_gpu_layers();
	PlayTime_clear();
	Lyrics_clearGPU();
	Lyrics_clear();
	if (quit_spectrum) {
		Spectrum_quit();
	}
	Playlist_free(&playlist);
	playlist_active = false;
	ModuleCommon_setAutosleepDisabled(false);
}

// Clean up playback UI only (audio keeps playing in background)
static void cleanup_playback_ui(void) {
	clear_gpu_layers();
	PlayTime_clear();
	Lyrics_clearGPU();
	Lyrics_clear();
	Spectrum_quit();
}

// Build a playlist from a directory and start playing the first track
static bool build_and_start_playlist(const char* dir_path, const char* start_file) {
	Playlist_free(&playlist);
	int track_count = Playlist_buildFromDirectory(&playlist, dir_path, start_file);
	if (track_count > 0) {
		playlist_active = true;
		const PlaylistTrack* track = Playlist_getCurrentTrack(&playlist);
		if (track && start_playback(track->path)) {
			return true;
		}
	}
	return false;
}

// Render delete confirmation dialog
static void render_delete_dialog(SDL_Surface* screen) {
	UI_renderConfirmDialog(screen, "Delete File?", delete_target_name);
	GFX_flip(screen);
}

// Handle USB/Bluetooth media button events
static void handle_hid_events(void) {
	USBHIDEvent hid_event;
	while ((hid_event = Player_pollUSBHID()) != USB_HID_EVENT_NONE) {
		if (hid_event == USB_HID_EVENT_PLAY_PAUSE) {
			Player_togglePause();
		} else if (hid_event == USB_HID_EVENT_NEXT_TRACK) {
			PlayerModule_nextTrack();
		} else if (hid_event == USB_HID_EVENT_PREV_TRACK) {
			PlayerModule_prevTrack();
		} else {
			ModuleCommon_handleHIDVolume(hid_event);
		}
	}
}

// Try to start playback from a browser entry (play-all or single file). Returns true on success.
static bool browser_play_entry(FileEntry* entry) {
	if (entry->is_play_all)
		return build_and_start_playlist(entry->path, "");
	if (build_and_start_playlist(browser.current_path, entry->path))
		return true;
	playlist_active = false;
	return start_playback(entry->path);
}

// Handle input in browser state. Returns true if module should exit to menu.
static bool handle_browser_input(PlayerInternalState* state, bool* dirty) {
	if (PAD_justPressed(BTN_B)) {
		if (strcmp(browser.current_path, MUSIC_PATH) != 0) {
			char* last_slash = strrchr(browser.current_path, '/');
			if (last_slash) {
				*last_slash = '\0';
				load_directory(browser.current_path);
				*dirty = 1;
			}
		} else {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			if (!Background_isPlaying()) {
				Spectrum_quit();
				Browser_freeEntries(&browser);
			}
			return true;
		}
	} else if (browser.entry_count > 0) {
		if (PAD_navigateMenu(&browser.selected, browser.entry_count))
			*dirty = 1;
		else if (PAD_justPressed(BTN_A)) {
			FileEntry* entry = &browser.entries[browser.selected];
			if (entry->is_dir) {
				char path_copy[512];
				snprintf(path_copy, sizeof(path_copy), "%s", entry->path);
				load_directory(path_copy);
				*dirty = 1;
			} else if (browser_play_entry(entry)) {
				*state = PLAYER_INTERNAL_PLAYING;
				*dirty = 1;
			}
		} else if (PAD_justPressed(BTN_X)) {
			FileEntry* entry = &browser.entries[browser.selected];
			if (!entry->is_dir && !entry->is_play_all) {
				snprintf(delete_target_path, sizeof(delete_target_path), "%s", entry->path);
				snprintf(delete_target_name, sizeof(delete_target_name), "%s", entry->name);
				show_delete_confirm = true;
				GFX_clearLayers(LAYER_SCROLLTEXT);
				*dirty = 1;
			}
		} else if (PAD_justPressed(BTN_Y)) {
			FileEntry* entry = &browser.entries[browser.selected];
			if (!entry->is_dir && !entry->is_play_all) {
				AddToPlaylist_open(entry->path, entry->name);
				*dirty = 1;
			}
		}
	}

	// Animate browser scroll
	if (browser_needs_scroll_refresh()) {
		browser_animate_scroll();
	}
	if (browser_scroll_needs_render())
		*dirty = 1;

	return false;
}

// Handle input in playing state. Returns true when main loop should continue (skip render).
static bool handle_playing_input(SDL_Surface* screen, PlayerInternalState* state, bool* dirty) {
	// Handle screen off hint timeout
	if (ModuleCommon_isScreenOffHintActive()) {
		if (ModuleCommon_processScreenOffHintTimeout()) {
			screen_off = true;
			GFX_clear(screen);
			GFX_flip(screen);
		}
		Player_update();
		GFX_sync();
		return true;
	}

	// Handle screen off mode
	if (screen_off) {
		// Wake screen with SELECT+A
		if (PAD_isPressed(BTN_SELECT) && PAD_isPressed(BTN_A)) {
			screen_off = false;
			PLAT_enableBacklight(1);
			ModuleCommon_recordInputTime();
			*dirty = 1;
		}
		// Handle USB/Bluetooth media and volume buttons even with screen off
		handle_hid_events();
		ModuleCommon_handleHardwareVolume();
		Player_update();

		if (Player_getState() == PLAYER_STATE_STOPPED) {
			if (!handle_track_ended() && Player_getState() == PLAYER_STATE_STOPPED) {
				Resume_clear(); // All tracks finished naturally
				screen_off = false;
				PLAT_enableBacklight(1);
				cleanup_playback(false);
				load_directory(MUSIC_PATH);
				*state = PLAYER_INTERNAL_BROWSER;
				*dirty = 1;
			}
		}
		GFX_sync();
		return true;
	}

	// Normal input handling
	if (PAD_anyPressed()) {
		ModuleCommon_recordInputTime();
	}

	if (PAD_justPressed(BTN_A)) {
		Player_togglePause();
		*dirty = 1;
	} else if (PAD_justPressed(BTN_B)) {
		cleanup_album_art_background();
		if (Player_getState() == PLAYER_STATE_PLAYING) {
			cleanup_playback_ui();
			Background_setActive(BG_MUSIC);
		} else {
			Player_stop();
			cleanup_playback(true);
		}
		*state = PLAYER_INTERNAL_BROWSER;
		*dirty = 1;
		return true; // Skip track-ended check to prevent auto-advance
	} else if (PAD_justRepeated(BTN_LEFT)) {
		Player_seek(Player_getPosition() - 5000);
		*dirty = 1;
	} else if (PAD_justRepeated(BTN_RIGHT)) {
		Player_seek(Player_getPosition() + 5000);
		*dirty = 1;
	} else if (PAD_justPressed(BTN_DOWN) || PAD_justPressed(BTN_L1)) {
		PlayerModule_prevTrack();
		*dirty = 1;
	} else if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_R1)) {
		PlayerModule_nextTrack();
		*dirty = 1;
	} else if (PAD_justPressed(BTN_X)) {
		shuffle_enabled = !shuffle_enabled;
		*dirty = 1;
	} else if (PAD_justPressed(BTN_Y)) {
		repeat_enabled = !repeat_enabled;
		*dirty = 1;
	} else if (PAD_justPressed(BTN_L3) || PAD_justPressed(BTN_L2)) {
		Spectrum_cycleNext();
		*dirty = 1;
	} else if (PAD_justPressed(BTN_R3) || PAD_justPressed(BTN_R2)) {
		Settings_toggleLyrics();
		if (!Settings_getLyricsEnabled()) {
			Lyrics_clear();
		} else {
			// Re-fetch lyrics for current track
			const TrackInfo* info = Player_getTrackInfo();
			if (info) {
				Lyrics_fetch(info->artist, info->title, info->duration_ms / 1000);
			}
		}
		*dirty = 1;
	} else if (PAD_tappedSelect(SDL_GetTicks())) {
		ModuleCommon_startScreenOffHint();
		clear_gpu_layers();
		*dirty = 1;
	}

	// Check if track ended
	Player_update();
	if (Player_getState() == PLAYER_STATE_STOPPED) {
		if (!handle_track_ended() && Player_getState() == PLAYER_STATE_STOPPED) {
			Resume_clear(); // All tracks finished naturally
			cleanup_playback(false);
			load_directory(MUSIC_PATH);
			*state = PLAYER_INTERNAL_BROWSER;
		}
		*dirty = 1;
	}

	// Save resume position periodically
	if (Player_getState() == PLAYER_STATE_PLAYING) {
		uint32_t now = SDL_GetTicks();
		if (now - last_resume_save > 5000) {
			Resume_updatePosition(Player_getPosition());
			last_resume_save = now;
		}
	}

	// Auto screen-off after inactivity
	if (Player_getState() == PLAYER_STATE_PLAYING && ModuleCommon_checkAutoScreenOffTimeout()) {
		clear_gpu_layers();
		*dirty = 1;
	}

	// Re-render when async album art fetch completes
	if (album_art_is_fetching())
		*dirty = 1;

	// Animate player GPU layers (skip if screen-off hint just activated)
	if (!ModuleCommon_isScreenOffHintActive()) {
		if (player_needs_scroll_refresh()) {
			player_animate_scroll();
		}
		if (player_title_scroll_needs_render())
			*dirty = 1;
		if (Spectrum_needsRefresh()) {
			Spectrum_renderGPU();
		}
		if (PlayTime_needsRefresh()) {
			PlayTime_renderGPU();
		}
		if (Lyrics_GPUneedsRefresh()) {
			Lyrics_renderGPU();
		}
	}

	return false;
}

ModuleExitReason PlayerModule_run(SDL_Surface* screen) {
	init_player();
	load_directory(browser.current_path[0] ? browser.current_path : MUSIC_PATH);

	PlayerInternalState state = PLAYER_INTERNAL_BROWSER;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	screen_off = false;
	ModuleCommon_resetScreenOffHint();
	ModuleCommon_recordInputTime();

	// Reclaim background music — re-enter playing state
	if (Background_getActive() == BG_MUSIC && PlayerModule_isActive()) {
		Background_setActive(BG_NONE);
		Spectrum_init();
		ModuleCommon_setAutosleepDisabled(true);
		state = PLAYER_INTERNAL_PLAYING;
	}

	while (1) {
		PAD_poll();

		// Handle add-to-playlist dialog overlay
		if (AddToPlaylist_isActive()) {
			if (AddToPlaylist_handleInput()) {
				// Dialog closed — skip rest of input to avoid double-handling buttons
				dirty = 1;
				continue;
			}
			// Still active, render dialog (covers entire screen)
			AddToPlaylist_render(screen);
			GFX_flip(screen);
			GFX_sync();
			continue;
		}

		// Handle delete confirmation dialog (module-specific)
		if (show_delete_confirm) {
			if (PAD_justPressed(BTN_A)) {
				if (unlink(delete_target_path) == 0) {
					load_directory(browser.current_path);
					if (browser.selected >= browser.entry_count) {
						browser.selected = browser.entry_count > 0 ? browser.entry_count - 1 : 0;
					}
				}
			}
			if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B)) {
				delete_target_path[0] = '\0';
				delete_target_name[0] = '\0';
				show_delete_confirm = false;
				dirty = 1;
				continue;
			}
			// Render delete dialog
			render_delete_dialog(screen);
			GFX_sync();
			continue;
		}

		// Handle global input (skip if screen off or hint active)
		if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
			int app_state_for_help = (state == PLAYER_INTERNAL_BROWSER) ? 1 : 2; // STATE_BROWSER=1, STATE_PLAYING=2
			GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, app_state_for_help);
			if (global.should_quit) {
				cleanup_playback(true);
				Browser_freeEntries(&browser);
				return MODULE_EXIT_QUIT;
			}
			if (global.input_consumed) {
				if (global.dirty)
					dirty = 1;
				GFX_sync();
				continue;
			}
		}

		if (state == PLAYER_INTERNAL_BROWSER) {
			if (handle_browser_input(&state, &dirty)) {
				return MODULE_EXIT_TO_MENU;
			}
		} else if (state == PLAYER_INTERNAL_PLAYING) {
			if (handle_playing_input(screen, &state, &dirty)) {
				continue;
			}
		}

		// Handle power management
		if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
			ModuleCommon_PWR_update(&dirty, &show_setting);
		}

		// Auto-clear toast after duration; force re-render while visible
		const char* atp_toast = AddToPlaylist_getToastMessage();
		if (atp_toast && atp_toast[0]) {
			if (SDL_GetTicks() - AddToPlaylist_getToastTime() > TOAST_DURATION) {
				AddToPlaylist_clearToast();
				UI_clearToast();
			}
			dirty = 1;
		}

		// Render
		if (dirty && !screen_off) {
			if (ModuleCommon_isScreenOffHintActive()) {
				GFX_clear(screen);
				render_screen_off_hint(screen);
			} else if (state == PLAYER_INTERNAL_BROWSER) {
				render_browser(screen, show_setting, &browser);
			} else {
				int pl_track = playlist_active ? Playlist_getCurrentIndex(&playlist) + 1 : 0;
				int pl_total = playlist_active ? Playlist_getCount(&playlist) : 0;
				render_playing(screen, show_setting, &browser, shuffle_enabled, repeat_enabled, pl_track, pl_total);
			}

			// Show add-to-playlist toast (if still active after auto-clear check)
			atp_toast = AddToPlaylist_getToastMessage();
			if (atp_toast && atp_toast[0]) {
				UI_renderToast(screen, atp_toast, AddToPlaylist_getToastTime());
			}

			GFX_flip(screen);
			dirty = 0;
		} else if (!screen_off) {
			GFX_sync();
		}
	}
}

// Check if music player module is active
bool PlayerModule_isActive(void) {
	PlayerState state = Player_getState();
	return (state == PLAYER_STATE_PLAYING || state == PLAYER_STATE_PAUSED);
}

// Play next track (for USB HID button support)
void PlayerModule_nextTrack(void) {
	if (playlist_active) {
		int new_idx = Playlist_next(&playlist);
		if (new_idx >= 0) {
			Player_stop();
			playlist_try_play(new_idx);
		}
	} else if (initialized) {
		for (int i = browser.selected + 1; i < browser.entry_count; i++) {
			if (!browser.entries[i].is_dir) {
				Player_stop();
				browser.selected = i;
				try_load_and_play(browser.entries[i].path);
				break;
			}
		}
	}
}

// Play previous track (for USB HID button support)
void PlayerModule_prevTrack(void) {
	if (playlist_active) {
		int new_idx = Playlist_prev(&playlist);
		if (new_idx >= 0) {
			Player_stop();
			playlist_try_play(new_idx);
		}
	} else if (initialized) {
		for (int i = browser.selected - 1; i >= 0; i--) {
			if (!browser.entries[i].is_dir) {
				Player_stop();
				browser.selected = i;
				try_load_and_play(browser.entries[i].path);
				break;
			}
		}
	}
}

// Run the player directly with a pre-built playlist (from PlaylistModule)
ModuleExitReason PlayerModule_runWithPlaylist(SDL_Surface* screen,
											  PlaylistTrack* tracks,
											  int track_count,
											  int start_index) {
	if (!tracks || track_count <= 0)
		return MODULE_EXIT_TO_MENU;

	// Set up the playlist context
	Playlist_free(&playlist);
	Playlist_init(&playlist);
	if (!playlist.tracks)
		return MODULE_EXIT_TO_MENU;
	for (int i = 0; i < track_count && i < PLAYLIST_MAX_TRACKS; i++) {
		playlist.tracks[i] = tracks[i];
	}
	playlist.track_count = track_count;
	playlist.current_index = start_index;
	playlist_active = true;

	// Start playback
	const PlaylistTrack* track = Playlist_getCurrentTrack(&playlist);
	if (!track || !start_playback(track->path)) {
		Playlist_free(&playlist);
		playlist_active = false;
		return MODULE_EXIT_TO_MENU;
	}

	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	screen_off = false;
	ModuleCommon_resetScreenOffHint();
	ModuleCommon_recordInputTime();

	while (1) {
		PAD_poll();

		// Handle add-to-playlist dialog overlay
		if (AddToPlaylist_isActive()) {
			if (AddToPlaylist_handleInput()) {
				// Dialog closed — skip rest of input to avoid double-handling buttons
				dirty = 1;
				continue;
			}
			// Dialog covers entire screen, no need to render underlying content
			AddToPlaylist_render(screen);
			GFX_flip(screen);
			GFX_sync();
			continue;
		}

		// Handle global input (skip if screen off or hint active)
		if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
			GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 2); // STATE_PLAYING=2
			if (global.should_quit) {
				Player_stop();
				cleanup_album_art_background();
				cleanup_playback(true);
				return MODULE_EXIT_QUIT;
			}
			if (global.input_consumed) {
				if (global.dirty)
					dirty = 1;
				GFX_sync();
				continue;
			}
		}

		// Handle screen off hint timeout
		if (ModuleCommon_isScreenOffHintActive()) {
			if (ModuleCommon_processScreenOffHintTimeout()) {
				screen_off = true;
				GFX_clear(screen);
				GFX_flip(screen);
			}
			Player_update();
			GFX_sync();
			continue;
		}

		// Handle screen off mode
		if (screen_off) {
			if (PAD_isPressed(BTN_SELECT) && PAD_isPressed(BTN_A)) {
				screen_off = false;
				PLAT_enableBacklight(1);
				ModuleCommon_recordInputTime();
				dirty = 1;
			}
			handle_hid_events();
			ModuleCommon_handleHardwareVolume();
			Player_update();

			if (Player_getState() == PLAYER_STATE_STOPPED) {
				if (!handle_track_ended() && Player_getState() == PLAYER_STATE_STOPPED) {
					Resume_clear(); // All tracks finished naturally
					screen_off = false;
					PLAT_enableBacklight(1);
					Player_stop();
					cleanup_album_art_background();
					cleanup_playback(true);
					return MODULE_EXIT_TO_MENU;
				}
			}
			GFX_sync();
			continue;
		}

		// Normal input handling
		if (PAD_anyPressed()) {
			ModuleCommon_recordInputTime();
		}

		if (PAD_justPressed(BTN_A)) {
			Player_togglePause();
			dirty = 1;
		} else if (PAD_justPressed(BTN_B)) {
			cleanup_album_art_background();
			if (Player_getState() == PLAYER_STATE_PLAYING) {
				cleanup_playback_ui();
				Background_setActive(BG_MUSIC);
			} else {
				Player_stop();
				cleanup_playback(true);
			}
			return MODULE_EXIT_TO_MENU;
		} else if (PAD_justRepeated(BTN_LEFT)) {
			Player_seek(Player_getPosition() - 5000);
			dirty = 1;
		} else if (PAD_justRepeated(BTN_RIGHT)) {
			Player_seek(Player_getPosition() + 5000);
			dirty = 1;
		} else if (PAD_justPressed(BTN_DOWN) || PAD_justPressed(BTN_L1)) {
			PlayerModule_prevTrack();
			dirty = 1;
		} else if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_R1)) {
			PlayerModule_nextTrack();
			dirty = 1;
		} else if (PAD_justPressed(BTN_X)) {
			shuffle_enabled = !shuffle_enabled;
			dirty = 1;
		} else if (PAD_justPressed(BTN_Y)) {
			repeat_enabled = !repeat_enabled;
			dirty = 1;
		} else if (PAD_justPressed(BTN_L3) || PAD_justPressed(BTN_L2)) {
			Spectrum_cycleNext();
			dirty = 1;
		} else if (PAD_justPressed(BTN_R3) || PAD_justPressed(BTN_R2)) {
			Settings_toggleLyrics();
			if (!Settings_getLyricsEnabled()) {
				Lyrics_clear();
			} else {
				const TrackInfo* info = Player_getTrackInfo();
				if (info) {
					Lyrics_fetch(info->artist, info->title, info->duration_ms / 1000);
				}
			}
			dirty = 1;
		} else if (PAD_tappedSelect(SDL_GetTicks())) {
			ModuleCommon_startScreenOffHint();
			clear_gpu_layers();
			dirty = 1;
		}

		// Check if track ended
		Player_update();
		if (Player_getState() == PLAYER_STATE_STOPPED) {
			if (!handle_track_ended() && Player_getState() == PLAYER_STATE_STOPPED) {
				Resume_clear(); // All tracks finished naturally
				cleanup_album_art_background();
				cleanup_playback(true);
				return MODULE_EXIT_TO_MENU;
			}
			dirty = 1;
		}

		// Save resume position periodically
		if (Player_getState() == PLAYER_STATE_PLAYING) {
			uint32_t now = SDL_GetTicks();
			if (now - last_resume_save > 5000) {
				Resume_updatePosition(Player_getPosition());
				last_resume_save = now;
			}
		}

		// Auto screen-off after inactivity
		if (Player_getState() == PLAYER_STATE_PLAYING && ModuleCommon_checkAutoScreenOffTimeout()) {
			clear_gpu_layers();
			dirty = 1;
		}

		// Animate player GPU layers
		if (!ModuleCommon_isScreenOffHintActive()) {
			if (player_needs_scroll_refresh()) {
				player_animate_scroll();
			}
			if (player_title_scroll_needs_render())
				dirty = 1;
			if (Spectrum_needsRefresh()) {
				Spectrum_renderGPU();
			}
			if (PlayTime_needsRefresh()) {
				PlayTime_renderGPU();
			}
			if (Lyrics_GPUneedsRefresh()) {
				Lyrics_renderGPU();
			}
		}

		// Handle power management
		if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
			ModuleCommon_PWR_update(&dirty, &show_setting);
		}

		// Auto-clear toast after duration; force re-render while visible
		const char* atp_toast = AddToPlaylist_getToastMessage();
		if (atp_toast && atp_toast[0]) {
			if (SDL_GetTicks() - AddToPlaylist_getToastTime() > TOAST_DURATION) {
				AddToPlaylist_clearToast();
				UI_clearToast();
			}
			dirty = 1;
		}

		// Render
		if (dirty && !screen_off) {
			if (ModuleCommon_isScreenOffHintActive()) {
				GFX_clear(screen);
				render_screen_off_hint(screen);
			} else {
				int pl_track = Playlist_getCurrentIndex(&playlist) + 1;
				int pl_total = Playlist_getCount(&playlist);
				render_playing(screen, show_setting, &browser, shuffle_enabled, repeat_enabled, pl_track, pl_total);
			}

			// Show add-to-playlist toast (if still active after auto-clear check)
			atp_toast = AddToPlaylist_getToastMessage();
			if (atp_toast && atp_toast[0]) {
				UI_renderToast(screen, atp_toast, AddToPlaylist_getToastTime());
			}

			GFX_flip(screen);
			dirty = 0;
		} else if (!screen_off) {
			GFX_sync();
		}
	}
}

// Set the M3U playlist path for resume tracking (call before runWithPlaylist)
void PlayerModule_setResumePlaylistPath(const char* m3u_path) {
	snprintf(resume_playlist_path, sizeof(resume_playlist_path), "%s", m3u_path ? m3u_path : "");
}

// Run player restoring a saved resume state
ModuleExitReason PlayerModule_runResume(SDL_Surface* screen, const ResumeState* resume) {
	if (!resume)
		return MODULE_EXIT_TO_MENU;

	if (resume->type == RESUME_TYPE_FILES) {
		// Initialize browser with saved folder
		init_player();
		load_directory(resume->folder_path);

		// Build playlist from directory starting at the saved track
		Playlist_free(&playlist);
		int count = Playlist_buildFromDirectory(&playlist, resume->folder_path, resume->track_path);
		if (count <= 0)
			return MODULE_EXIT_TO_MENU;
		playlist_active = true;

		// Start playback
		const PlaylistTrack* track = Playlist_getCurrentTrack(&playlist);
		if (!track || !start_playback(track->path)) {
			cleanup_playback(false);
			return MODULE_EXIT_TO_MENU;
		}

		// Seek to saved position
		if (resume->position_ms > 0) {
			Player_seek(resume->position_ms);
		}

		// Set browser.selected to match the current track for display
		for (int i = 0; i < browser.entry_count; i++) {
			if (strcmp(browser.entries[i].path, track->path) == 0) {
				browser.selected = i;
				break;
			}
		}

		// Use the shared playing loop via handle_playing_input
		bool dirty = true;
		IndicatorType show_setting = INDICATOR_NONE;
		screen_off = false;
		ModuleCommon_resetScreenOffHint();
		ModuleCommon_recordInputTime();
		PlayerInternalState state = PLAYER_INTERNAL_PLAYING;

		while (1) {
			PAD_poll();

			// Handle add-to-playlist dialog overlay
			if (AddToPlaylist_isActive()) {
				if (AddToPlaylist_handleInput()) {
					dirty = 1;
					continue;
				}
				AddToPlaylist_render(screen);
				GFX_flip(screen);
				GFX_sync();
				continue;
			}

			// Handle global input
			if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
				GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 2);
				if (global.should_quit) {
					Player_stop();
					cleanup_album_art_background();
					cleanup_playback(true);
					return MODULE_EXIT_QUIT;
				}
				if (global.input_consumed) {
					if (global.dirty)
						dirty = 1;
					GFX_sync();
					continue;
				}
			}

			// Delegate to shared playing input handler
			if (handle_playing_input(screen, &state, &dirty)) {
				// If state left playing (BTN_B or all tracks ended), return to menu immediately
				// Don't continue the loop or handle_playing_input will re-trigger track-ended logic
				if (state != PLAYER_INTERNAL_PLAYING) {
					return MODULE_EXIT_TO_MENU;
				}
				continue;
			}

			// If state left playing, return to menu
			if (state != PLAYER_INTERNAL_PLAYING) {
				return MODULE_EXIT_TO_MENU;
			}

			// Handle power management
			if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
				ModuleCommon_PWR_update(&dirty, &show_setting);
			}

			// Auto-clear toast
			const char* atp_toast = AddToPlaylist_getToastMessage();
			if (atp_toast && atp_toast[0]) {
				if (SDL_GetTicks() - AddToPlaylist_getToastTime() > TOAST_DURATION) {
					AddToPlaylist_clearToast();
					UI_clearToast();
				}
				dirty = 1;
			}

			// Render
			if (dirty && !screen_off) {
				if (ModuleCommon_isScreenOffHintActive()) {
					GFX_clear(screen);
					render_screen_off_hint(screen);
				} else {
					int pl_track = Playlist_getCurrentIndex(&playlist) + 1;
					int pl_total = Playlist_getCount(&playlist);
					render_playing(screen, show_setting, &browser, shuffle_enabled, repeat_enabled, pl_track, pl_total);
				}

				atp_toast = AddToPlaylist_getToastMessage();
				if (atp_toast && atp_toast[0]) {
					UI_renderToast(screen, atp_toast, AddToPlaylist_getToastTime());
				}

				GFX_flip(screen);
				dirty = 0;
			} else if (!screen_off) {
				GFX_sync();
			}
		}

	} else if (resume->type == RESUME_TYPE_PLAYLIST) {
		// Load the M3U playlist tracks
		PlaylistTrack m3u_tracks[PLAYLIST_MAX_TRACKS];
		int m3u_count = 0;
		if (M3U_loadTracks(resume->playlist_path, m3u_tracks, PLAYLIST_MAX_TRACKS, &m3u_count) != 0 || m3u_count <= 0) {
			return MODULE_EXIT_TO_MENU;
		}

		// Find the track index in the loaded playlist
		int start_idx = 0;
		for (int i = 0; i < m3u_count; i++) {
			if (strcmp(m3u_tracks[i].path, resume->track_path) == 0) {
				start_idx = i;
				break;
			}
		}

		// Set resume playlist path so the playing loop saves correctly
		PlayerModule_setResumePlaylistPath(resume->playlist_path);

		// Run with the playlist
		ModuleExitReason reason = PlayerModule_runWithPlaylist(screen, m3u_tracks, m3u_count, start_idx);

		resume_playlist_path[0] = '\0';
		return reason;
	}

	return MODULE_EXIT_TO_MENU;
}

// Background tick: handle track advancement and resume saving while in menu
void PlayerModule_backgroundTick(void) {
	Player_update();

	// Handle track ended (auto-advance)
	if (Player_getState() == PLAYER_STATE_STOPPED) {
		if (!handle_track_ended() && Player_getState() == PLAYER_STATE_STOPPED) {
			// All tracks finished
			Resume_clear();
			cleanup_playback(false);
			Background_setActive(BG_NONE);
			ModuleCommon_setAutosleepDisabled(false);
		}
		return;
	}

	// Save resume position periodically
	if (Player_getState() == PLAYER_STATE_PLAYING) {
		uint32_t now = SDL_GetTicks();
		if (now - last_resume_save > 5000) {
			Resume_updatePosition(Player_getPosition());
			last_resume_save = now;
		}
	}
}
