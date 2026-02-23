#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "vp_defines.h"
#include "api.h"
#include "module_common.h"
#include "module_subscriptions.h"
#include "subscriptions.h"
#include "youtube.h"
#include "wifi.h"
#include "ffplay_engine.h"
#include "ui_components.h"
#include "ui_subscriptions.h"
#include "ui_youtube.h"
#include "ui_icons.h"

// Module states
typedef enum {
	SUB_STATE_LIST,		// Browsing subscription list
	SUB_STATE_LOADING,	// Fetching channel videos
	SUB_STATE_CHANNEL,	// Browsing channel videos
	SUB_STATE_RESOLVING // Resolving stream URL
} SubModuleState;

static ScrollTextState sub_scroll = {0};

// Confirmation dialog state
static bool show_confirm = false;
static int confirm_target_index = -1;
static char confirm_channel_name[SUBS_MAX_NAME] = "";

#define REFRESH_COOLDOWN_SEC 900

static YouTubeSearchResults cached_results;
static char current_channel_id[SUBS_MAX_ID];
static bool has_cached_data = false;
static bool bg_refresh_active = false;

// Sequential background refresh queue
static int refresh_queue[SUBS_MAX_CHANNELS];
static int refresh_queue_count = 0;
static int refresh_queue_pos = 0;
static bool queue_refresh_active = false;

// Load channel videos: cached first, then background refresh
static bool load_channel_videos(const SubscriptionChannel* ch) {
	memset(&cached_results, 0, sizeof(cached_results));
	strncpy(current_channel_id, ch->channel_id, SUBS_MAX_ID - 1);
	current_channel_id[SUBS_MAX_ID - 1] = '\0';
	has_cached_data = false;
	bg_refresh_active = false;

	if (ch->channel_id[0]) {
		int count = YouTube_loadVideosCache(ch->channel_id, &cached_results);
		if (count > 0) {
			has_cached_data = true;
		}
	}

	return has_cached_data;
}

static void start_bg_refresh(const SubscriptionChannel* ch) {
	if (!ch->channel_id[0] && !ch->channel_url[0]) {
		YouTube_searchAsync(ch->channel_name);
	} else {
		YouTube_fetchUploadsAsync(ch->channel_url, ch->channel_id);
	}
	bg_refresh_active = true;
}

// Start fetching the next channel in the queue
static bool queue_start_next(void) {
	if (refresh_queue_pos >= refresh_queue_count) {
		queue_refresh_active = false;
		return false;
	}
	const SubscriptionList* sl = Subscriptions_getList();
	int idx = refresh_queue[refresh_queue_pos];
	if (idx >= sl->count) {
		refresh_queue_pos++;
		return queue_start_next(); // skip invalid, try next
	}
	const SubscriptionChannel* ch = &sl->channels[idx];
	YouTube_fetchUploadsAsync(ch->channel_url, ch->channel_id);
	return true;
}

// Handle completion of current queue item and advance
static void queue_handle_completion(YouTubeSearchResults* fresh) {
	const SubscriptionList* sl = Subscriptions_getList();
	int idx = refresh_queue[refresh_queue_pos];
	if (idx < sl->count && fresh) {
		const SubscriptionChannel* ch = &sl->channels[idx];
		// Save cache and download thumbnails
		YouTube_saveVideosCache(ch->channel_id, fresh);
		YouTube_downloadChannelThumbnails(ch->channel_id, fresh);
		// Update video_count and last_updated
		Subscriptions_updateMeta(idx, fresh->count, time(NULL));
	}
	refresh_queue_pos++;
	if (refresh_queue_pos < refresh_queue_count) {
		queue_start_next();
	} else {
		queue_refresh_active = false;
	}
}

// Cancel the queue refresh
static void queue_cancel(void) {
	if (queue_refresh_active) {
		YouTube_cancelUploads();
		queue_refresh_active = false;
		refresh_queue_count = 0;
		refresh_queue_pos = 0;
	}
}

ModuleExitReason SubscriptionsModule_run(SDL_Surface* screen) {
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	int selected = 0;
	int scroll_offset = 0;
	int channel_selected = 0;
	int channel_scroll = 0;
	SubModuleState state = SUB_STATE_LIST;
	char current_channel[SUBS_MAX_NAME] = {0};

	// Carousel state for channel video browsing
	YouTubeCarouselState carousel;
	bool carousel_initialized = false;

	memset(&sub_scroll, 0, sizeof(sub_scroll));
	show_confirm = false;

	// Build sequential refresh queue for stale channels
	refresh_queue_count = 0;
	refresh_queue_pos = 0;
	queue_refresh_active = false;
	{
		const SubscriptionList* sl = Subscriptions_getList();
		time_t now = time(NULL);
		for (int i = 0; i < sl->count; i++) {
			const SubscriptionChannel* ch = &sl->channels[i];
			if (ch->channel_id[0] && (now - ch->last_updated > REFRESH_COOLDOWN_SEC)) {
				refresh_queue[refresh_queue_count++] = i;
			}
		}
		if (refresh_queue_count > 0) {
			if (Wifi_ensureConnected(screen, show_setting)) {
				queue_refresh_active = true;
				queue_start_next();
			} else {
				refresh_queue_count = 0;
			}
		}
	}

	while (1) {
		PAD_poll();

		const SubscriptionList* subs = Subscriptions_getList();

		// Handle confirmation dialog
		if (show_confirm) {
			if (PAD_justPressed(BTN_A)) {
				const SubscriptionList* sl = Subscriptions_getList();
				if (confirm_target_index < sl->count) {
					Subscriptions_deleteChannelData(sl->channels[confirm_target_index].channel_id);
				}
				Subscriptions_removeAt(confirm_target_index);
				if (selected >= subs->count && selected > 0) {
					selected--;
				}
				show_confirm = false;
				dirty = 1;
				GFX_sync();
				continue;
			} else if (PAD_justPressed(BTN_B)) {
				show_confirm = false;
				dirty = 1;
				GFX_sync();
				continue;
			}
			// Render confirmation dialog (covers entire screen)
			UI_renderConfirmDialog(screen, "Remove Subscription?", confirm_channel_name);
			GFX_flip(screen);
			GFX_sync();
			continue;
		}

		// Handle resolving state
		if (state == SUB_STATE_RESOLVING) {
			YouTubeAsyncOp* op = YouTube_getResolveOp();
			if (op->state == YT_OP_DONE) {
				YouTubeResult* r = &cached_results.items[channel_selected];

				FfplayConfig config;
				memset(&config, 0, sizeof(config));
				config.source = FFPLAY_SOURCE_STREAM;
				config.is_stream = true;
				config.screen_width = screen->w;
				strncpy(config.path, op->resolved_url, sizeof(config.path) - 1);
				strncpy(config.title, r->title, sizeof(config.title) - 1);

				ModuleCommon_setAutosleepDisabled(true);
				FfplayEngine_play(&config);

				Icons_init();
				memset(&sub_scroll, 0, sizeof(sub_scroll));
				if (carousel_initialized)
					carousel.loaded_index = -1;

				state = SUB_STATE_CHANNEL;
				dirty = 1;
			} else if (op->state == YT_OP_ERROR) {
				state = SUB_STATE_CHANNEL;
				dirty = 1;
			} else {
				if (PAD_justPressed(BTN_B)) {
					YouTube_cancelResolve();
					state = SUB_STATE_CHANNEL;
					dirty = 1;
				}
				dirty = 1;
			}

			ModuleCommon_PWR_update(&dirty, &show_setting);
			if (dirty) {
				if (state == SUB_STATE_RESOLVING) {
					render_channel_searching(screen, "Getting stream...");
				} else {
					render_youtube_carousel(screen, show_setting, &cached_results,
											channel_selected, &carousel, &sub_scroll);
				}
				GFX_flip(screen);
				dirty = 0;
			} else {
				GFX_sync();
			}
			continue;
		}

		// Handle loading channel videos
		if (state == SUB_STATE_LOADING) {
			YouTubeAsyncOp* s_op = YouTube_getSearchOp();
			YouTubeUploadsOp* u_op = YouTube_getUploadsOp();

			bool done = false;
			bool error = false;
			YouTubeSearchResults* fresh_results = NULL;

			if (bg_refresh_active && u_op->state != YT_OP_IDLE) {
				if (u_op->state == YT_OP_DONE) {
					fresh_results = &u_op->results;
					done = true;
				} else if (u_op->state == YT_OP_ERROR) {
					error = true;
				}
			} else if (s_op->state != YT_OP_IDLE) {
				if (s_op->state == YT_OP_DONE) {
					fresh_results = &s_op->results;
					done = true;
				} else if (s_op->state == YT_OP_ERROR) {
					error = true;
				}
			}

			if (done && fresh_results) {
				memcpy(&cached_results, fresh_results, sizeof(cached_results));
				has_cached_data = true;

				if (current_channel_id[0]) {
					YouTube_saveVideosCache(current_channel_id, &cached_results);
					YouTube_downloadChannelThumbnails(current_channel_id, &cached_results);

					const SubscriptionList* sl = Subscriptions_getList();
					for (int si = 0; si < sl->count; si++) {
						if (strcmp(sl->channels[si].channel_id, current_channel_id) == 0) {
							Subscriptions_updateMeta(si, cached_results.count, time(NULL));
							Subscriptions_markSeen(si, cached_results.count);
							break;
						}
					}
				}

				channel_selected = 0;
				channel_scroll = 0;
				memset(&sub_scroll, 0, sizeof(sub_scroll));
				state = SUB_STATE_CHANNEL;
				bg_refresh_active = false;
				dirty = 1;
			} else if (error) {
				state = SUB_STATE_LIST;
				bg_refresh_active = false;
				dirty = 1;
			} else {
				if (PAD_justPressed(BTN_B)) {
					YouTube_cancelSearch();
					YouTube_cancelUploads();
					state = SUB_STATE_LIST;
					bg_refresh_active = false;
					dirty = 1;
				}
				dirty = 1;
			}

			ModuleCommon_PWR_update(&dirty, &show_setting);
			if (dirty) {
				if (state == SUB_STATE_LOADING) {
					render_channel_searching(screen, current_channel);
				} else if (state == SUB_STATE_CHANNEL) {
					if (!carousel_initialized) {
						YouTubeCarousel_init(&carousel, screen);
						carousel_initialized = true;
					}
					YouTubeCarousel_loadChannelThumbnail(&carousel, channel_selected,
														 &cached_results, current_channel_id);
					render_youtube_carousel(screen, show_setting, &cached_results,
											channel_selected, &carousel, &sub_scroll);
				} else {
					render_subscriptions_list(screen, show_setting, subs,
											  selected, scroll_offset, &sub_scroll);
				}
				GFX_flip(screen);
				dirty = 0;
			} else {
				GFX_sync();
			}
			continue;
		}

		// Handle channel video browsing
		if (state == SUB_STATE_CHANNEL) {
			GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, STATE_SUBSCRIPTIONS);
			if (global.should_quit) {
				if (carousel_initialized)
					YouTubeCarousel_cleanup(&carousel);
				return MODULE_EXIT_QUIT;
			}
			if (global.input_consumed) {
				if (global.dirty)
					dirty = 1;
				GFX_sync();
				continue;
			}

			// Check if background refresh completed
			if (bg_refresh_active) {
				YouTubeUploadsOp* u_op = YouTube_getUploadsOp();
				YouTubeAsyncOp* s_op = YouTube_getSearchOp();

				YouTubeSearchResults* fresh = NULL;
				bool refresh_done = false;

				if (u_op->state == YT_OP_DONE) {
					fresh = &u_op->results;
					refresh_done = true;
				} else if (s_op->state == YT_OP_DONE) {
					fresh = &s_op->results;
					refresh_done = true;
				} else if (u_op->state == YT_OP_ERROR || s_op->state == YT_OP_ERROR) {
					bg_refresh_active = false;
				}

				if (refresh_done && fresh) {
					memcpy(&cached_results, fresh, sizeof(cached_results));

					if (current_channel_id[0]) {
						YouTube_saveVideosCache(current_channel_id, &cached_results);
						YouTube_downloadChannelThumbnails(current_channel_id, &cached_results);

						const SubscriptionList* sl = Subscriptions_getList();
						for (int si = 0; si < sl->count; si++) {
							if (strcmp(sl->channels[si].channel_id, current_channel_id) == 0) {
								Subscriptions_updateMeta(si, cached_results.count, time(NULL));
								break;
							}
						}
					}

					if (channel_selected >= cached_results.count && cached_results.count > 0) {
						channel_selected = cached_results.count - 1;
					}
					bg_refresh_active = false;
					dirty = 1;
				}
			}

			if (PAD_justPressed(BTN_B)) {
				GFX_clearLayers(LAYER_SCROLLTEXT);
				memset(&sub_scroll, 0, sizeof(sub_scroll));
				YouTube_cancelUploads();
				bg_refresh_active = false;
				if (carousel_initialized) {
					YouTubeCarousel_cleanup(&carousel);
					carousel_initialized = false;
				}
				state = SUB_STATE_LIST;
				// Resume queue refresh if it was paused
				if (refresh_queue_pos < refresh_queue_count) {
					queue_refresh_active = true;
					queue_start_next();
				}
				dirty = 1;
				continue;
			} else if (cached_results.count > 0) {
				if (PAD_justRepeated(BTN_RIGHT) || PAD_justRepeated(BTN_R1)) {
					channel_selected = (channel_selected < cached_results.count - 1) ? channel_selected + 1 : 0;
					memset(&sub_scroll, 0, sizeof(sub_scroll));
					if (carousel_initialized) {
						YouTubeCarousel_loadChannelThumbnail(&carousel, channel_selected,
															 &cached_results, current_channel_id);
					}
					dirty = 1;
				} else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_L1)) {
					channel_selected = (channel_selected > 0) ? channel_selected - 1 : cached_results.count - 1;
					memset(&sub_scroll, 0, sizeof(sub_scroll));
					if (carousel_initialized) {
						YouTubeCarousel_loadChannelThumbnail(&carousel, channel_selected,
															 &cached_results, current_channel_id);
					}
					dirty = 1;
				} else if (PAD_justPressed(BTN_A)) {
					YouTubeResult* r = &cached_results.items[channel_selected];
					YouTube_resolveUrlAsync(r->id);
					state = SUB_STATE_RESOLVING;
					dirty = 1;
				}
			}

			// Poll for thumbnail availability
			if (carousel_initialized && carousel.loaded_index != channel_selected) {
				if (YouTubeCarousel_loadChannelThumbnail(&carousel, channel_selected,
														 &cached_results, current_channel_id)) {
					dirty = 1;
				}
			}

			if (ScrollText_isScrolling(&sub_scroll))
				ScrollText_animateOnly(&sub_scroll);
			if (ScrollText_needsRender(&sub_scroll))
				dirty = 1;

			ModuleCommon_PWR_update(&dirty, &show_setting);
			if (dirty) {
				if (!carousel_initialized) {
					YouTubeCarousel_init(&carousel, screen);
					carousel_initialized = true;
					YouTubeCarousel_loadChannelThumbnail(&carousel, channel_selected,
														 &cached_results, current_channel_id);
				}
				render_youtube_carousel(screen, show_setting, &cached_results,
										channel_selected, &carousel, &sub_scroll);
				GFX_flip(screen);
				dirty = 0;
			} else {
				GFX_sync();
			}
			continue;
		}

		// SUB_STATE_LIST - browsing subscription list

		// Poll sequential background refresh queue
		if (queue_refresh_active) {
			YouTubeUploadsOp* q_op = YouTube_getUploadsOp();
			if (q_op->state == YT_OP_DONE) {
				queue_handle_completion(&q_op->results);
				dirty = 1;
			} else if (q_op->state == YT_OP_ERROR) {
				// Skip failed channel, advance to next
				refresh_queue_pos++;
				if (refresh_queue_pos < refresh_queue_count) {
					queue_start_next();
				} else {
					queue_refresh_active = false;
				}
			}
		}

		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, STATE_SUBSCRIPTIONS);
		if (global.should_quit) {
			queue_cancel();
			if (carousel_initialized)
				YouTubeCarousel_cleanup(&carousel);
			return MODULE_EXIT_QUIT;
		}
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}

		if (PAD_justPressed(BTN_B)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			queue_cancel();
			YouTube_cancelUploads();
			SubUI_clearAvatarCache();
			if (carousel_initialized) {
				YouTubeCarousel_cleanup(&carousel);
				carousel_initialized = false;
			}
			return MODULE_EXIT_TO_MENU;
		} else if (subs->count > 0) {
			if (PAD_justRepeated(BTN_UP)) {
				selected = (selected > 0) ? selected - 1 : subs->count - 1;
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN)) {
				selected = (selected < subs->count - 1) ? selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				const SubscriptionChannel* ch = &subs->channels[selected];
				strncpy(current_channel, ch->channel_name, sizeof(current_channel) - 1);

				// Pause queue refresh - channel-specific refresh takes priority
				bool was_queue_active = queue_refresh_active;
				if (queue_refresh_active) {
					YouTube_cancelUploads();
					queue_refresh_active = false;
				}

				bool has_cache = load_channel_videos(ch);

				// Mark channel as seen to clear "New" badge
				if (has_cache) {
					Subscriptions_markSeen(selected, cached_results.count);
				}

				if (has_cache) {
					channel_selected = 0;
					channel_scroll = 0;
					memset(&sub_scroll, 0, sizeof(sub_scroll));
					state = SUB_STATE_CHANNEL;

					time_t now = time(NULL);
					if (now - ch->last_updated > REFRESH_COOLDOWN_SEC) {
						Wifi_ensureConnected(screen, show_setting);
						start_bg_refresh(ch);
					}

					if (current_channel_id[0]) {
						YouTube_downloadChannelThumbnails(current_channel_id, &cached_results);
					}
				} else {
					Wifi_ensureConnected(screen, show_setting);
					start_bg_refresh(ch);
					state = SUB_STATE_LOADING;
				}
				dirty = 1;
				continue;
			} else if (PAD_justPressed(BTN_X)) {
				// Confirm removal
				strncpy(confirm_channel_name, subs->channels[selected].channel_name, SUBS_MAX_NAME - 1);
				confirm_channel_name[SUBS_MAX_NAME - 1] = '\0';
				confirm_target_index = selected;
				show_confirm = true;
				dirty = 1;
			}
		}

		// Clamp selection if list changed
		if (selected >= subs->count && subs->count > 0) {
			selected = subs->count - 1;
		}

		if (ScrollText_isScrolling(&sub_scroll))
			ScrollText_animateOnly(&sub_scroll);
		if (ScrollText_needsRender(&sub_scroll))
			dirty = 1;

		ModuleCommon_PWR_update(&dirty, &show_setting);
		if (dirty) {
			render_subscriptions_list(screen, show_setting, subs,
									  selected, scroll_offset, &sub_scroll);
			GFX_flip(screen);
			dirty = 0;
		} else {
			GFX_sync();
		}
	}
}
