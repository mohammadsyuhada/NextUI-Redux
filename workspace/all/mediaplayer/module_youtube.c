#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "vp_defines.h"
#include "api.h"
#include "module_common.h"
#include "module_youtube.h"
#include "module_subscriptions.h"
#include "youtube.h"
#include "wifi.h"
#include "ui_keyboard.h"
#include "ffplay_engine.h"
#include "subscriptions.h"
#include "ui_youtube.h"
#include "ui_icons.h"

// YouTube sub-menu selections
#define YT_SUBMENU_SEARCH 0
#define YT_SUBMENU_SUBSCRIPTIONS 1

// YouTube module states
typedef enum {
	YT_STATE_SUBMENU,	   // Sub-menu: Search / Subscriptions
	YT_STATE_IDLE,		   // Showing empty/previous results
	YT_STATE_SEARCHING,	   // yt-dlp search in progress
	YT_STATE_RESULTS,	   // Showing search results
	YT_STATE_RESOLVING,	   // Resolving stream URL for selected video
	YT_STATE_CHANNEL_INFO, // Showing channel details page
	YT_STATE_ERROR		   // Showing error
} YTModuleState;

// Scroll text state for selected result title
static ScrollTextState yt_scroll = {0};

static bool subscribe_fetch_pending = false;
static char subscribe_fetch_channel_id[64] = "";

// Start a new search flow: keyboard -> WiFi -> yt-dlp
static bool start_search(SDL_Surface** screenp, IndicatorType show_setting) {
	// TG5050: release display before keyboard (external binary takes DRM master)
	FfplayEngine_prepareForExternal();

	char* query = UIKeyboard_open("Search YouTube");

	// Flush stale button state from keyboard (B press to exit keyboard
	// would otherwise be detected by our main loop)
	PAD_poll();
	PAD_reset();

	// TG5050: restore display after keyboard exits
	FfplayEngine_recoverDisplay();
	{
		SDL_Surface* ns = FfplayEngine_getReinitScreen();
		if (ns)
			*screenp = ns;
	}

	if (!query)
		return false;

	if (strlen(query) == 0) {
		free(query);
		return false;
	}

	// Ensure WiFi before searching
	// Re-init GFX/PAD after keyboard (it uses external binary)
	Icons_init();

	Wifi_ensureConnected(*screenp, show_setting);

	YouTube_searchAsync(query);
	free(query);
	return true;
}

ModuleExitReason YouTubeModule_run(SDL_Surface* screen) {
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	int selected = 0;
	int submenu_selected = 0;
	YTModuleState state = YT_STATE_SUBMENU;
	char error_msg[256] = {0};

	// Carousel state
	YouTubeCarouselState carousel;
	bool carousel_initialized = false;

	// Reset scroll state
	memset(&yt_scroll, 0, sizeof(yt_scroll));

	while (1) {
		GFX_startFrame();
		PAD_poll();

		// Check if background subscribe fetch completed
		if (subscribe_fetch_pending) {
			YouTubeUploadsOp* u_op = YouTube_getUploadsOp();
			if (u_op->state == YT_OP_DONE) {
				if (subscribe_fetch_channel_id[0]) {
					YouTube_saveVideosCache(subscribe_fetch_channel_id, &u_op->results);
					YouTube_downloadChannelThumbnails(subscribe_fetch_channel_id, &u_op->results);
					const SubscriptionList* sl = Subscriptions_getList();
					for (int si = 0; si < sl->count; si++) {
						if (strcmp(sl->channels[si].channel_id, subscribe_fetch_channel_id) == 0) {
							Subscriptions_updateMeta(si, u_op->results.count, time(NULL));
							break;
						}
					}
				}
				subscribe_fetch_pending = false;
			} else if (u_op->state == YT_OP_ERROR) {
				subscribe_fetch_pending = false;
			}
		}

		// Handle sub-menu state
		if (state == YT_STATE_SUBMENU) {
			GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, STATE_MENU);
			if (global.should_quit) {
				YouTube_cancelChannelInfo();
				if (carousel_initialized) {
					YouTubeCarousel_cleanup(&carousel);
					YouTube_cancelThumbnails();
				}
				return MODULE_EXIT_QUIT;
			}
			if (global.input_consumed) {
				if (global.dirty)
					dirty = 1;
				GFX_sync();
				continue;
			}

			if (PAD_justRepeated(BTN_UP)) {
				submenu_selected = (submenu_selected > 0) ? submenu_selected - 1 : 1;
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN)) {
				submenu_selected = (submenu_selected < 1) ? submenu_selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				if (submenu_selected == YT_SUBMENU_SEARCH) {
					// Enter search flow
					state = YT_STATE_IDLE;
					dirty = 1;
				} else if (submenu_selected == YT_SUBMENU_SUBSCRIPTIONS) {
					// Run subscriptions module inline
					GFX_clearLayers(LAYER_SCROLLTEXT);
					ModuleExitReason reason = SubscriptionsModule_run(screen);
					// TG5050: subscriptions module may have triggered display recovery
					{
						SDL_Surface* ns = FfplayEngine_getReinitScreen();
						if (ns)
							screen = ns;
					}
					if (reason == MODULE_EXIT_QUIT) {
						YouTube_cancelChannelInfo();
						if (carousel_initialized) {
							YouTubeCarousel_cleanup(&carousel);
							YouTube_cancelThumbnails();
						}
						return MODULE_EXIT_QUIT;
					}
					// Returned from subscriptions, back to sub-menu
					dirty = 1;
				}
			} else if (PAD_justPressed(BTN_B)) {
				GFX_clearLayers(LAYER_SCROLLTEXT);
				YouTube_cancelChannelInfo();
				if (carousel_initialized) {
					YouTubeCarousel_cleanup(&carousel);
					YouTube_cancelThumbnails();
				}
				return MODULE_EXIT_TO_MENU;
			}

			ModuleCommon_PWR_update(&dirty, &show_setting);

			if (dirty) {
				render_youtube_submenu(screen, show_setting, submenu_selected);
				GFX_flip(screen);
				dirty = 0;
			} else {
				GFX_sync();
			}
			continue;
		}

		// Handle resolving state (waiting for URL)
		if (state == YT_STATE_RESOLVING) {
			YouTubeAsyncOp* op = YouTube_getResolveOp();
			if (op->state == YT_OP_DONE) {
				// Got stream URL - play it
				FfplayConfig config;
				memset(&config, 0, sizeof(config));
				config.source = FFPLAY_SOURCE_STREAM;
				config.is_stream = true;
				config.start_position_sec = 0;
				config.screen_width = screen->w;
				strncpy(config.path, op->resolved_url, sizeof(config.path) - 1);

				// Set window title to video title
				YouTubeSearchResults* res = &YouTube_getSearchOp()->results;
				if (selected < res->count) {
					strncpy(config.title, res->items[selected].title, sizeof(config.title) - 1);
				}

				// Disable autosleep during playback
				ModuleCommon_setAutosleepDisabled(true);

				FfplayEngine_play(&config);

				// TG5050: display recovery creates a new screen surface
				{
					SDL_Surface* ns = FfplayEngine_getReinitScreen();
					if (ns)
						screen = ns;
				}

				// Re-init after ffplay (icons may need refresh)
				Icons_init();
				memset(&yt_scroll, 0, sizeof(yt_scroll));

				// Force reload thumbnail after playback returns
				if (carousel_initialized) {
					carousel.loaded_index = -1;
				}

				state = YT_STATE_RESULTS;
				dirty = 1;
			} else if (op->state == YT_OP_ERROR) {
				snprintf(error_msg, sizeof(error_msg), "%s", op->error);
				state = YT_STATE_ERROR;
				dirty = 1;
			} else {
				// Still resolving
				if (PAD_justPressed(BTN_B)) {
					YouTube_cancelResolve();
					state = YT_STATE_RESULTS;
					dirty = 1;
				}
				dirty = 1; // Keep refreshing for animation
			}

			ModuleCommon_PWR_update(&dirty, &show_setting);

			if (dirty) {
				if (state == YT_STATE_RESOLVING) {
					render_youtube_resolving(screen);
				} else if (state == YT_STATE_ERROR) {
					render_youtube_error(screen, error_msg);
				} else {
					YouTubeSearchResults* res = &YouTube_getSearchOp()->results;
					if (carousel_initialized && carousel.loaded_index != selected) {
						YouTubeCarousel_loadThumbnail(&carousel, selected, res);
					}
					render_youtube_carousel(screen, show_setting, res,
											selected, &carousel, &yt_scroll);
				}
				GFX_flip(screen);
				dirty = 0;
			} else {
				GFX_sync();
			}
			continue;
		}

		// Handle channel info state
		if (state == YT_STATE_CHANNEL_INFO) {
			YouTubeChannelInfoOp* ch_op = YouTube_getChannelInfoOp();
			YouTubeSearchResults* results = &YouTube_getSearchOp()->results;
			const char* cur_channel = (selected < results->count) ? results->items[selected].channel : "";
			bool is_subscribed = Subscriptions_isSubscribed(cur_channel);

			if (PAD_justRepeated(BTN_DOWN) || PAD_justRepeated(BTN_B)) {
				YouTube_cancelChannelInfo();
				state = YT_STATE_RESULTS;
				// Force reload thumbnail after returning from channel info
				if (carousel_initialized) {
					carousel.loaded_index = -1;
				}
				dirty = 1;
			} else if ((ch_op->state == YT_OP_DONE || ch_op->state == YT_OP_ERROR) &&
					   PAD_justRepeated(BTN_A)) {
				if (cur_channel[0]) {
					if (is_subscribed) {
						// Find channel_id for data cleanup
						const SubscriptionList* sub_list = Subscriptions_getList();
						for (int si = 0; si < sub_list->count; si++) {
							if (strcmp(sub_list->channels[si].channel_name, cur_channel) == 0) {
								Subscriptions_deleteChannelData(sub_list->channels[si].channel_id);
								break;
							}
						}
						Subscriptions_removeByName(cur_channel);
					} else {
						// Subscribe with full metadata
						const char* ch_id = ch_op->info.channel_id_str;
						const char* ch_url = ch_op->info.channel_url;
						if (Subscriptions_addFull(cur_channel, ch_id, ch_url)) {
							// Trigger background fetch of uploads + avatar
							if (ch_id[0]) {
								YouTube_fetchUploadsAsync(ch_url, ch_id);
								subscribe_fetch_pending = true;
								strncpy(subscribe_fetch_channel_id, ch_id, sizeof(subscribe_fetch_channel_id) - 1);
								// Copy temp avatar to permanent location
								if (ch_op->info.avatar_path[0]) {
									char dest[512];
									if (Subscriptions_getAvatarPath(ch_id, dest, sizeof(dest))) {
										char dir[512];
										Subscriptions_getChannelDir(ch_id, dir, sizeof(dir));
										mkdir(APP_DATA_DIR, 0755);
										mkdir(APP_YOUTUBE_DIR, 0755);
										mkdir(dir, 0755);
										char cp_cmd[1024];
										snprintf(cp_cmd, sizeof(cp_cmd), "cp '%s' '%s'",
												 ch_op->info.avatar_path, dest);
										system(cp_cmd);
									}
								}
							}
						}
					}
					dirty = 1;
				}
			}

			// Always redraw while in channel info state (loading animation + state transitions)
			dirty = 1;

			ModuleCommon_PWR_update(&dirty, &show_setting);

			if (dirty) {
				bool loading = (ch_op->state == YT_OP_RUNNING);
				render_youtube_channel_info(screen, &ch_op->info, cur_channel, is_subscribed, loading);
				GFX_flip(screen);
				dirty = 0;
			} else {
				GFX_sync();
			}
			continue;
		}

		// Handle searching state
		if (state == YT_STATE_SEARCHING) {
			YouTubeAsyncOp* op = YouTube_getSearchOp();
			if (op->state == YT_OP_DONE) {
				selected = 0;
				memset(&yt_scroll, 0, sizeof(yt_scroll));

				// Init carousel and start thumbnail downloads
				if (carousel_initialized) {
					YouTubeCarousel_cleanup(&carousel);
					YouTube_cancelThumbnails();
				}
				YouTubeCarousel_init(&carousel, screen);
				carousel_initialized = true;
				YouTube_downloadThumbnails(&op->results);

				// Try to load first thumbnail
				YouTubeCarousel_loadThumbnail(&carousel, 0, &op->results);

				state = YT_STATE_RESULTS;
				dirty = 1;
			} else if (op->state == YT_OP_ERROR) {
				snprintf(error_msg, sizeof(error_msg), "%s", op->error);
				state = YT_STATE_ERROR;
				dirty = 1;
			} else {
				// Still searching
				if (PAD_justPressed(BTN_B)) {
					YouTube_cancelSearch();
					state = YT_STATE_IDLE;
					dirty = 1;
				}
				dirty = 1; // Keep refreshing for animation
			}

			ModuleCommon_PWR_update(&dirty, &show_setting);

			if (dirty) {
				if (state == YT_STATE_SEARCHING) {
					render_youtube_searching(screen);
				} else if (state == YT_STATE_ERROR) {
					render_youtube_error(screen, error_msg);
				} else {
					render_youtube_carousel(screen, show_setting, &YouTube_getSearchOp()->results,
											selected, &carousel, &yt_scroll);
				}
				GFX_flip(screen);
				dirty = 0;
			} else {
				GFX_sync();
			}
			continue;
		}

		// Handle error state
		if (state == YT_STATE_ERROR) {
			if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_A)) {
				// If we have previous results, go back to them
				if (YouTube_getSearchOp()->results.count > 0) {
					state = YT_STATE_RESULTS;
				} else {
					state = YT_STATE_IDLE;
				}
				dirty = 1;
			}

			ModuleCommon_PWR_update(&dirty, &show_setting);

			if (dirty) {
				render_youtube_error(screen, error_msg);
				GFX_flip(screen);
				dirty = 0;
			} else {
				GFX_sync();
			}
			continue;
		}

		// Normal state: IDLE or RESULTS
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting,
																  state == YT_STATE_RESULTS ? STATE_YOUTUBE_RESULTS : STATE_YOUTUBE_MENU);
		if (global.should_quit) {
			YouTube_cancelChannelInfo();
			if (carousel_initialized) {
				YouTubeCarousel_cleanup(&carousel);
				YouTube_cancelThumbnails();
			}
			return MODULE_EXIT_QUIT;
		}
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}

		YouTubeSearchResults* results = &YouTube_getSearchOp()->results;

		if (PAD_justPressed(BTN_B)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			YouTube_cancelChannelInfo();
			if (carousel_initialized) {
				YouTubeCarousel_cleanup(&carousel);
				YouTube_cancelThumbnails();
			}
			carousel_initialized = false;
			state = YT_STATE_SUBMENU;
			dirty = 1;
			continue;
		} else if (PAD_justPressed(BTN_Y)) {
			// Y button: new search
			if (start_search(&screen, show_setting)) {
				state = YT_STATE_SEARCHING;
				dirty = 1;
			} else {
				// Keyboard cancelled, re-init
				Icons_init();
				dirty = 1;
			}
		} else if (state == YT_STATE_RESULTS && results->count > 0) {
			// Carousel navigation: LEFT/RIGHT to navigate, L1/R1 for fast skip
			if (PAD_justRepeated(BTN_RIGHT) || PAD_justRepeated(BTN_R1)) {
				selected = (selected < results->count - 1) ? selected + 1 : 0;
				memset(&yt_scroll, 0, sizeof(yt_scroll));
				if (carousel_initialized) {
					YouTubeCarousel_loadThumbnail(&carousel, selected, results);
				}
				dirty = 1;
			} else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_L1)) {
				selected = (selected > 0) ? selected - 1 : results->count - 1;
				memset(&yt_scroll, 0, sizeof(yt_scroll));
				if (carousel_initialized) {
					YouTubeCarousel_loadThumbnail(&carousel, selected, results);
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_UP)) {
				// Open channel details page
				YouTubeResult* r = &results->items[selected];
				YouTube_fetchChannelInfoAsync(r->id);
				state = YT_STATE_CHANNEL_INFO;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				// Resolve stream URL for selected video
				YouTubeResult* r = &results->items[selected];
				YouTube_resolveUrlAsync(r->id);
				state = YT_STATE_RESOLVING;
				dirty = 1;
			} else if (PAD_justPressed(BTN_X)) {
				// Remove subscription of selected result's channel
				YouTubeResult* r = &results->items[selected];
				if (r->channel[0] && Subscriptions_isSubscribed(r->channel)) {
					Subscriptions_removeByName(r->channel);
					dirty = 1;
				}
			}
		} else if (state == YT_STATE_IDLE) {
			// No results yet, auto-open search on first entry
			if (start_search(&screen, show_setting)) {
				state = YT_STATE_SEARCHING;
				dirty = 1;
			} else {
				// Keyboard cancelled, go straight back to sub-menu
				Icons_init();
				GFX_clearLayers(LAYER_SCROLLTEXT);
				state = YT_STATE_SUBMENU;
				dirty = 1;
				continue;
			}
		}

		// Poll for thumbnail availability (background download may have finished)
		if (state == YT_STATE_RESULTS && carousel_initialized &&
			carousel.loaded_index != selected) {
			if (YouTubeCarousel_loadThumbnail(&carousel, selected, results)) {
				dirty = 1;
			}
		}

		// Animate scroll text
		if (ScrollText_isScrolling(&yt_scroll)) {
			ScrollText_animateOnly(&yt_scroll);
		}
		if (ScrollText_needsRender(&yt_scroll)) {
			dirty = 1;
		}

		ModuleCommon_PWR_update(&dirty, &show_setting);

		if (dirty) {
			if (state == YT_STATE_RESULTS && results->count > 0) {
				render_youtube_carousel(screen, show_setting, results,
										selected, &carousel, &yt_scroll);
			}

			GFX_flip(screen);
			dirty = 0;
		} else {
			GFX_sync();
		}
	}
}
