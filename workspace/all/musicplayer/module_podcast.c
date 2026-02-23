#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_podcast.h"
#include "podcast.h"
#include "player.h"
#include "ui_keyboard.h"
#include "ui_components.h"
#include "ui_podcast.h"
#include "ui_radio.h"
#include "ui_main.h"
#include "ui_utils.h"
#include "wifi.h"
#include "background.h"

// Internal states
typedef enum {
	PODCAST_INTERNAL_MENU,
	PODCAST_INTERNAL_MANAGE,
	PODCAST_INTERNAL_TOP_SHOWS,
	PODCAST_INTERNAL_SEARCH_RESULTS,
	PODCAST_INTERNAL_EPISODES,
	PODCAST_INTERNAL_SEEKING,
	PODCAST_INTERNAL_PLAYING,
	PODCAST_INTERNAL_DOWNLOAD_QUEUE
} PodcastInternalState;

// Module state
static int podcast_menu_selected = 0;
static int podcast_menu_scroll = 0;
static int podcast_manage_selected = 0;
static int podcast_top_shows_selected = 0;
static int podcast_top_shows_scroll = 0;
static int podcast_search_selected = 0;
static int podcast_search_scroll = 0;
static char podcast_search_query[256] = "";
static int podcast_episodes_selected = 0;
static int podcast_episodes_scroll = 0;
static int podcast_current_feed_index = -1;
static int podcast_current_episode_index = -1;
static int podcast_queue_selected = 0;
static int podcast_queue_scroll = 0;
static char podcast_toast_message[128] = "";
static uint32_t podcast_toast_time = 0;

// Periodic progress saving
static uint32_t last_progress_save_time = 0;
#define PROGRESS_SAVE_INTERVAL_MS 30000 // 30 seconds

// Confirmation dialog state
static bool show_confirm = false;
static int confirm_target_index = -1;
static char confirm_podcast_name[256] = "";
static int confirm_return_state = 0; // 0 = menu, 1 = top_shows, 2 = search_results

// Screen off state
static bool screen_off = false;

// Handle USB/Bluetooth media button events
static void handle_hid_events(void) {
	USBHIDEvent hid_event;
	while ((hid_event = Player_pollUSBHID()) != USB_HID_EVENT_NONE) {
		if (hid_event == USB_HID_EVENT_PLAY_PAUSE) {
			if (Player_getState() == PLAYER_STATE_PAUSED)
				Player_play();
			else
				Player_pause();
		} else {
			ModuleCommon_handleHIDVolume(hid_event);
		}
	}
}

static void clear_and_show_screen_off_hint(SDL_Surface* screen) {
	GFX_clearLayers(LAYER_SCROLLTEXT);
	PLAT_clearLayers(LAYER_BUFFER);
	PLAT_clearLayers(LAYER_PODCAST_PROGRESS);
	PLAT_GPU_Flip();
	GFX_clear(screen);
	render_screen_off_hint(screen);
	GFX_flip(screen);
}

static void return_to_episodes(PodcastInternalState* state, bool* dirty) {
	Podcast_flushProgress();
	Podcast_clearArtwork();
	GFX_clearLayers(LAYER_SCROLLTEXT);
	PLAT_clearLayers(LAYER_BUFFER);
	PLAT_clearLayers(LAYER_PODCAST_PROGRESS);
	PLAT_GPU_Flip();
	ModuleCommon_setAutosleepDisabled(false);
	podcast_episodes_selected = podcast_current_episode_index;
	*state = PODCAST_INTERNAL_EPISODES;
	*dirty = 1;
}

ModuleExitReason PodcastModule_run(SDL_Surface* screen) {
	Podcast_init();
	UIKeyboard_init();

	// Auto-check for new episodes once per app session
	static bool auto_refreshed = false;
	if (!auto_refreshed && Wifi_isConnected() && Podcast_getSubscriptionCount() > 0) {
		Podcast_startRefreshAll();
		auto_refreshed = true;
	}

	PodcastInternalState state = PODCAST_INTERNAL_MENU;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	screen_off = false;
	ModuleCommon_resetScreenOffHint();
	ModuleCommon_recordInputTime();
	podcast_toast_message[0] = '\0';
	show_confirm = false;
	podcast_menu_selected = 0;
	podcast_menu_scroll = 0;

	// Re-enter playing state if podcast is playing in background
	if (Background_getActive() == BG_PODCAST && Podcast_isActive()) {
		Background_setActive(BG_NONE);
		ModuleCommon_setAutosleepDisabled(true);
		state = PODCAST_INTERNAL_PLAYING;
	}

	while (1) {
		PAD_poll();

		// Handle confirmation dialog
		if (show_confirm) {
			if (PAD_justPressed(BTN_A)) {
				// Confirm unsubscribe
				Podcast_unsubscribe(confirm_target_index);
				if (confirm_return_state == 0) {
					// From main menu — clamp selection to new total
					int cl_count_raw2 = Podcast_getContinueListeningCount();
					int cl_count2 = (cl_count_raw2 > PODCAST_CONTINUE_LISTENING_DISPLAY) ? PODCAST_CONTINUE_LISTENING_DISPLAY : cl_count_raw2;
					int total = cl_count2 + Podcast_getSubscriptionCount();
					if (podcast_menu_selected >= total && total > 0) {
						podcast_menu_selected = total - 1;
					} else if (total == 0) {
						podcast_menu_selected = 0;
					}
				}
				snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Unsubscribed");
				podcast_toast_time = SDL_GetTicks();
				show_confirm = false;
				Podcast_clearTitleScroll();
				dirty = 1;
				GFX_sync();
				continue;
			} else if (PAD_justPressed(BTN_B)) {
				show_confirm = false;
				Podcast_clearTitleScroll();
				dirty = 1;
				GFX_sync();
				continue;
			}
			// Render confirmation dialog (covers entire screen)
			UI_renderConfirmDialog(screen, "Unsubscribe?", confirm_podcast_name);
			GFX_flip(screen);
			GFX_sync();
			continue;
		}

		// Handle global input (skip if screen off or hint active)
		if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
			int app_state_for_help;
			switch (state) {
			case PODCAST_INTERNAL_MENU:
				app_state_for_help = 30;
				break;
			case PODCAST_INTERNAL_MANAGE:
				app_state_for_help = 31;
				break;
			case PODCAST_INTERNAL_TOP_SHOWS:
				app_state_for_help = 33;
				break;
			case PODCAST_INTERNAL_SEARCH_RESULTS:
				app_state_for_help = 34;
				break;
			case PODCAST_INTERNAL_EPISODES:
				app_state_for_help = 35;
				break;
			case PODCAST_INTERNAL_SEEKING:
				app_state_for_help = 37;
				break;
			case PODCAST_INTERNAL_PLAYING:
				app_state_for_help = 37;
				break;
			case PODCAST_INTERNAL_DOWNLOAD_QUEUE:
				app_state_for_help = 35;
				break;
			default:
				app_state_for_help = 30;
				break;
			}

			GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, app_state_for_help);
			if (global.should_quit) {
				Podcast_cleanup();
				return MODULE_EXIT_QUIT;
			}
			if (global.input_consumed) {
				if (global.dirty)
					dirty = 1;
				GFX_sync();
				continue;
			}
		}

		// =========================================
		// PODCAST MENU STATE (unified: continue listening + subscriptions)
		// =========================================
		if (state == PODCAST_INTERNAL_MENU) {
			Podcast_update();

			// Check for background refresh completion
			if (Podcast_checkRefreshCompleted()) {
				Podcast_saveSubscriptions();
				dirty = 1;
			}

			int cl_count_raw = Podcast_getContinueListeningCount();
			int cl_count = (cl_count_raw > PODCAST_CONTINUE_LISTENING_DISPLAY) ? PODCAST_CONTINUE_LISTENING_DISPLAY : cl_count_raw;
			int sub_count = Podcast_getSubscriptionCount();
			// "Downloads" item at the bottom (only visible when there are queued items)
			int dl_queue_count = 0;
			Podcast_getDownloadQueue(&dl_queue_count);
			int has_downloads_item = (dl_queue_count > 0) ? 1 : 0;
			int total = cl_count + sub_count + has_downloads_item;

			// Clamp selection if items changed (e.g. download queue emptied)
			if (podcast_menu_selected >= total && total > 0) {
				podcast_menu_selected = total - 1;
			}

			if (podcast_toast_message[0] && (SDL_GetTicks() - podcast_toast_time < TOAST_DURATION))
				dirty = 1;
			if (has_downloads_item)
				dirty = 1; // Force redraw to update download status
			if (Podcast_isTitleScrolling())
				Podcast_animateTitleScroll();
			if (Podcast_titleScrollNeedsRender())
				dirty = 1;
			if (Podcast_loadPendingThumbnails())
				dirty = 1;

			if (PAD_justRepeated(BTN_UP) && total > 0) {
				podcast_menu_selected = (podcast_menu_selected > 0) ? podcast_menu_selected - 1 : total - 1;
				Podcast_clearTitleScroll();
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN) && total > 0) {
				podcast_menu_selected = (podcast_menu_selected < total - 1) ? podcast_menu_selected + 1 : 0;
				Podcast_clearTitleScroll();
				dirty = 1;
			} else if (PAD_justPressed(BTN_A) && total > 0) {
				if (has_downloads_item && podcast_menu_selected == cl_count + sub_count) {
					// Downloads item — open download queue
					podcast_queue_selected = 0;
					podcast_queue_scroll = 0;
					Podcast_clearTitleScroll();
					podcast_toast_message[0] = '\0';
					UI_clearToast();
					state = PODCAST_INTERNAL_DOWNLOAD_QUEUE;
					dirty = 1;
				} else if (podcast_menu_selected < cl_count) {
					// Continue Listening item — play directly
					ContinueListeningEntry* cl_entry = Podcast_getContinueListening(podcast_menu_selected);
					if (cl_entry) {
						int fi = Podcast_findFeedIndex(cl_entry->feed_url);
						if (fi >= 0) {
							PodcastFeed* feed = Podcast_getSubscription(fi);
							// Find episode index by guid
							int ep_idx = -1;
							if (feed) {
								for (int e = 0; e < feed->episode_count; e++) {
									PodcastEpisode* ep = Podcast_getEpisode(fi, e);
									if (ep && strcmp(ep->guid, cl_entry->episode_guid) == 0) {
										ep_idx = e;
										break;
									}
								}
							}
							if (feed && ep_idx >= 0 && Podcast_episodeFileExists(feed, ep_idx)) {
								Background_stopAll();
								podcast_current_feed_index = fi;
								podcast_current_episode_index = ep_idx;
								int load_result = Podcast_loadAndSeek(feed, ep_idx);
								if (load_result >= 0) {
									Podcast_clearTitleScroll();
									ModuleCommon_recordInputTime();
									last_progress_save_time = SDL_GetTicks();
									if (load_result == 1) {
										state = PODCAST_INTERNAL_SEEKING;
									} else {
										Player_play();
										state = PODCAST_INTERNAL_PLAYING;
									}
									// Update continue listening (move to top)
									Podcast_updateContinueListening(feed->feed_url, feed->feed_id,
																	cl_entry->episode_guid, cl_entry->episode_title,
																	feed->title, feed->artwork_url);
								} else {
									snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Failed to play");
									podcast_toast_time = SDL_GetTicks();
								}
							} else {
								snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Episode not available");
								podcast_toast_time = SDL_GetTicks();
							}
						} else {
							snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Podcast not found");
							podcast_toast_time = SDL_GetTicks();
						}
					}
				} else {
					// Subscription item — go to episodes
					podcast_current_feed_index = podcast_menu_selected - cl_count;
					podcast_episodes_selected = 0;
					podcast_episodes_scroll = 0;
					Podcast_clearTitleScroll();
					podcast_toast_message[0] = '\0';
					UI_clearToast();
					state = PODCAST_INTERNAL_EPISODES;
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_X) && total > 0) {
				// Only allow unsubscribe on subscription items (not Downloads item)
				if (podcast_menu_selected >= cl_count && podcast_menu_selected < cl_count + sub_count) {
					int sub_idx = podcast_menu_selected - cl_count;
					PodcastFeed* feed = Podcast_getSubscription(sub_idx);
					if (feed) {
						strncpy(confirm_podcast_name, feed->title, sizeof(confirm_podcast_name) - 1);
						confirm_podcast_name[sizeof(confirm_podcast_name) - 1] = '\0';
						confirm_target_index = sub_idx;
						confirm_return_state = 0;
						Podcast_clearTitleScroll();
						show_confirm = true;
						dirty = 1;
					}
				}
			} else if (PAD_justPressed(BTN_Y)) {
				podcast_manage_selected = 0;
				podcast_toast_message[0] = '\0';
				Podcast_clearTitleScroll();
				UI_clearToast();
				state = PODCAST_INTERNAL_MANAGE;
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				podcast_toast_message[0] = '\0';
				Podcast_clearTitleScroll();
				UI_clearToast();
				if (Podcast_isActive() || Podcast_isDownloading()) {
					Podcast_saveSubscriptions();
					Podcast_flushProgress();
					if (Podcast_isActive()) {
						Background_setActive(BG_PODCAST);
					}
				} else {
					Podcast_cleanup();
				}
				return MODULE_EXIT_TO_MENU;
			}
		}
		// =========================================
		// MANAGE STATE
		// =========================================
		else if (state == PODCAST_INTERNAL_MANAGE) {
			Podcast_update();

			if (PAD_justRepeated(BTN_UP)) {
				podcast_manage_selected = (podcast_manage_selected > 0) ? podcast_manage_selected - 1 : PODCAST_MANAGE_COUNT - 1;
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN)) {
				podcast_manage_selected = (podcast_manage_selected < PODCAST_MANAGE_COUNT - 1) ? podcast_manage_selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				switch (podcast_manage_selected) {
				case PODCAST_MANAGE_SEARCH: {
					if (!Wifi_ensureConnected(screen, show_setting)) {
						strncpy(podcast_toast_message, "Internet connection required", sizeof(podcast_toast_message) - 1);
						podcast_toast_time = SDL_GetTicks();
						dirty = 1;
						break;
					}
					char* query = UIKeyboard_open("Search podcasts");
					PAD_poll();
					PAD_reset();
					SDL_Delay(100);
					PAD_poll();
					PAD_reset();
					if (query && query[0]) {
						strncpy(podcast_search_query, query, sizeof(podcast_search_query) - 1);
						Podcast_startSearch(podcast_search_query);
						podcast_search_selected = 0;
						podcast_search_scroll = 0;
						podcast_toast_message[0] = '\0';
						state = PODCAST_INTERNAL_SEARCH_RESULTS;
					}
					if (query)
						free(query);
					dirty = 1;
					break;
				}
				case PODCAST_MANAGE_TOP_SHOWS:
					if (!Wifi_ensureConnected(screen, show_setting)) {
						strncpy(podcast_toast_message, "Internet connection required", sizeof(podcast_toast_message) - 1);
						podcast_toast_time = SDL_GetTicks();
						dirty = 1;
						break;
					}
					Podcast_loadCharts(NULL);
					podcast_top_shows_selected = 0;
					podcast_top_shows_scroll = 0;
					podcast_toast_message[0] = '\0';
					state = PODCAST_INTERNAL_TOP_SHOWS;
					dirty = 1;
					break;
				}
			} else if (PAD_justPressed(BTN_B)) {
				state = PODCAST_INTERNAL_MENU;
				dirty = 1;
			}
		}
		// =========================================
		// TOP SHOWS STATE
		// =========================================
		else if (state == PODCAST_INTERNAL_TOP_SHOWS) {
			Podcast_update();
			const PodcastChartsStatus* chart_status = Podcast_getChartsStatus();

			if (chart_status->loading || chart_status->completed)
				dirty = 1;
			if (podcast_toast_message[0] && (SDL_GetTicks() - podcast_toast_time < TOAST_DURATION))
				dirty = 1;
			if (Podcast_isTitleScrolling())
				Podcast_animateTitleScroll();
			if (Podcast_titleScrollNeedsRender())
				dirty = 1;

			if (!chart_status->loading) {
				int count = 0;
				Podcast_getTopShows(&count);

				if (PAD_justRepeated(BTN_UP) && count > 0) {
					podcast_top_shows_selected = (podcast_top_shows_selected > 0) ? podcast_top_shows_selected - 1 : count - 1;
					Podcast_clearTitleScroll();
					dirty = 1;
				} else if (PAD_justRepeated(BTN_DOWN) && count > 0) {
					podcast_top_shows_selected = (podcast_top_shows_selected < count - 1) ? podcast_top_shows_selected + 1 : 0;
					Podcast_clearTitleScroll();
					dirty = 1;
				} else if (PAD_justPressed(BTN_A) && count > 0) {
					PodcastChartItem* items = Podcast_getTopShows(&count);
					if (podcast_top_shows_selected < count) {
						bool already_subscribed = Podcast_isSubscribedByItunesId(items[podcast_top_shows_selected].itunes_id);
						if (already_subscribed) {
							// Find subscription index and show confirm dialog
							int sub_count = 0;
							PodcastFeed* feeds = Podcast_getSubscriptions(&sub_count);
							for (int si = 0; si < sub_count; si++) {
								if (feeds[si].itunes_id[0] && strcmp(feeds[si].itunes_id, items[podcast_top_shows_selected].itunes_id) == 0) {
									strncpy(confirm_podcast_name, items[podcast_top_shows_selected].title, sizeof(confirm_podcast_name) - 1);
									confirm_podcast_name[sizeof(confirm_podcast_name) - 1] = '\0';
									confirm_target_index = si;
									confirm_return_state = 1;
									show_confirm = true;
									break;
								}
							}
						} else {
							Podcast_clearTitleScroll();
							render_podcast_loading(screen, "Subscribing...");
							GFX_flip(screen);
							int sub_result = Podcast_subscribeFromItunes(items[podcast_top_shows_selected].itunes_id);
							if (sub_result == 0) {
								strncpy(podcast_toast_message, "Subscribed!", sizeof(podcast_toast_message) - 1);
							} else {
								const char* err = Podcast_getError();
								strncpy(podcast_toast_message, err && err[0] ? err : "Subscribe failed", sizeof(podcast_toast_message) - 1);
							}
							podcast_toast_time = SDL_GetTicks();
						}
					}
					dirty = 1;
				} else if (PAD_justPressed(BTN_X)) {
					// Refresh charts - clear cache and reload
					if (!Wifi_ensureConnected(screen, show_setting)) {
						strncpy(podcast_toast_message, "Internet connection required", sizeof(podcast_toast_message) - 1);
						podcast_toast_time = SDL_GetTicks();
					} else {
						Podcast_clearChartsCache();
						Podcast_loadCharts(NULL);
						podcast_top_shows_selected = 0;
						podcast_top_shows_scroll = 0;
						strncpy(podcast_toast_message, "Refreshing...", sizeof(podcast_toast_message) - 1);
						podcast_toast_time = SDL_GetTicks();
					}
					dirty = 1;
				}
			}

			if (PAD_justPressed(BTN_B)) {
				Podcast_clearTitleScroll();
				podcast_toast_message[0] = '\0';
				UI_clearToast();
				state = PODCAST_INTERNAL_MANAGE;
				dirty = 1;
			}
		}
		// =========================================
		// SEARCH RESULTS STATE
		// =========================================
		else if (state == PODCAST_INTERNAL_SEARCH_RESULTS) {
			Podcast_update();
			const PodcastSearchStatus* search_status = Podcast_getSearchStatus();

			if (search_status->searching || search_status->completed)
				dirty = 1;
			if (podcast_toast_message[0] && (SDL_GetTicks() - podcast_toast_time < TOAST_DURATION))
				dirty = 1;
			if (Podcast_isTitleScrolling())
				Podcast_animateTitleScroll();
			if (Podcast_titleScrollNeedsRender())
				dirty = 1;

			if (!search_status->searching) {
				int count = 0;
				Podcast_getSearchResults(&count);

				if (PAD_justRepeated(BTN_UP) && count > 0) {
					podcast_search_selected = (podcast_search_selected > 0) ? podcast_search_selected - 1 : count - 1;
					Podcast_clearTitleScroll();
					dirty = 1;
				} else if (PAD_justRepeated(BTN_DOWN) && count > 0) {
					podcast_search_selected = (podcast_search_selected < count - 1) ? podcast_search_selected + 1 : 0;
					Podcast_clearTitleScroll();
					dirty = 1;
				} else if (PAD_justPressed(BTN_A) && count > 0) {
					PodcastSearchResult* results = Podcast_getSearchResults(&count);
					if (podcast_search_selected < count) {
						bool already_subscribed = results[podcast_search_selected].feed_url[0] &&
												  Podcast_isSubscribed(results[podcast_search_selected].feed_url);
						if (already_subscribed) {
							// Find subscription index and show confirm dialog
							int sub_count = 0;
							PodcastFeed* feeds = Podcast_getSubscriptions(&sub_count);
							for (int si = 0; si < sub_count; si++) {
								if (strcmp(feeds[si].feed_url, results[podcast_search_selected].feed_url) == 0) {
									strncpy(confirm_podcast_name, results[podcast_search_selected].title, sizeof(confirm_podcast_name) - 1);
									confirm_podcast_name[sizeof(confirm_podcast_name) - 1] = '\0';
									confirm_target_index = si;
									confirm_return_state = 2;
									show_confirm = true;
									break;
								}
							}
						} else {
							Podcast_clearTitleScroll();
							render_podcast_loading(screen, "Subscribing...");
							GFX_flip(screen);
							int sub_result;
							if (results[podcast_search_selected].feed_url[0]) {
								sub_result = Podcast_subscribe(results[podcast_search_selected].feed_url);
							} else {
								sub_result = Podcast_subscribeFromItunes(results[podcast_search_selected].itunes_id);
							}
							if (sub_result == 0) {
								strncpy(podcast_toast_message, "Subscribed!", sizeof(podcast_toast_message) - 1);
							} else {
								const char* err = Podcast_getError();
								strncpy(podcast_toast_message, err && err[0] ? err : "Subscribe failed", sizeof(podcast_toast_message) - 1);
							}
							podcast_toast_time = SDL_GetTicks();
						}
					}
					dirty = 1;
				}
			}

			if (PAD_justPressed(BTN_B)) {
				Podcast_clearTitleScroll();
				Podcast_cancelSearch();
				podcast_toast_message[0] = '\0';
				UI_clearToast();
				state = PODCAST_INTERNAL_MANAGE;
				dirty = 1;
			}
		}
		// =========================================
		// EPISODES STATE
		// =========================================
		else if (state == PODCAST_INTERNAL_EPISODES) {
			PodcastFeed* feed = Podcast_getSubscription(podcast_current_feed_index);
			int count = feed ? feed->episode_count : 0;

			// Check if refresh just completed
			if (Podcast_checkRefreshCompleted()) {
				feed = Podcast_getSubscription(podcast_current_feed_index);
				count = feed ? feed->episode_count : 0;
				Podcast_invalidateEpisodeCache();
				if (feed && feed->new_episode_count > 0) {
					snprintf(podcast_toast_message, sizeof(podcast_toast_message),
							 "%d new episode%s found!", feed->new_episode_count,
							 feed->new_episode_count > 1 ? "s" : "");
				} else {
					snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Already up to date");
				}
				podcast_toast_time = SDL_GetTicks();
				Podcast_saveSubscriptions();
				dirty = 1;
			}

			// Force redraw when downloads active
			int queue_count = 0;
			PodcastDownloadItem* queue = Podcast_getDownloadQueue(&queue_count);
			for (int i = 0; i < queue_count; i++) {
				if (queue[i].status == PODCAST_DOWNLOAD_DOWNLOADING || queue[i].status == PODCAST_DOWNLOAD_PENDING) {
					dirty = 1;
					break;
				}
			}

			if (Podcast_isTitleScrolling())
				Podcast_animateTitleScroll();
			if (Podcast_titleScrollNeedsRender())
				dirty = 1;
			if (podcast_toast_message[0] && (SDL_GetTicks() - podcast_toast_time < TOAST_DURATION))
				dirty = 1;

			if (PAD_justRepeated(BTN_UP) && count > 0) {
				podcast_episodes_selected = (podcast_episodes_selected > 0) ? podcast_episodes_selected - 1 : count - 1;
				Podcast_clearTitleScroll();
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN) && count > 0) {
				podcast_episodes_selected = (podcast_episodes_selected < count - 1) ? podcast_episodes_selected + 1 : 0;
				Podcast_clearTitleScroll();
				dirty = 1;
			} else if (PAD_justPressed(BTN_A) && count > 0 && feed) {
				podcast_current_episode_index = podcast_episodes_selected;
				PodcastEpisode* ep = Podcast_getEpisode(podcast_current_feed_index, podcast_current_episode_index);

				if (ep) {
					int dl_progress = 0;
					int dl_status = Podcast_getEpisodeDownloadStatus(feed->feed_url, ep->guid, &dl_progress);

					if (dl_status == PODCAST_DOWNLOAD_DOWNLOADING || dl_status == PODCAST_DOWNLOAD_PENDING) {
						// Cancel download
						if (Podcast_cancelEpisodeDownload(feed->feed_url, ep->guid) == 0) {
							snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Download cancelled");
						} else {
							snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Cancel failed");
						}
						podcast_toast_time = SDL_GetTicks();
					} else if (Podcast_episodeFileExists(feed, podcast_current_episode_index)) {
						Background_stopAll();
						int load_result = Podcast_loadAndSeek(feed, podcast_current_episode_index);
						if (load_result >= 0) {
							// Clear new flag only when actually playing
							Podcast_clearNewFlag(podcast_current_feed_index, podcast_current_episode_index);
							Podcast_clearTitleScroll();
							ModuleCommon_recordInputTime();
							last_progress_save_time = SDL_GetTicks();
							// Update continue listening
							Podcast_updateContinueListening(feed->feed_url, feed->feed_id,
															ep->guid, ep->title, feed->title, feed->artwork_url);
							if (load_result == 1) {
								// Has saved progress — seeking, show player UI while waiting
								state = PODCAST_INTERNAL_SEEKING;
							} else {
								// No saved progress — play immediately
								Player_play();
								state = PODCAST_INTERNAL_PLAYING;
							}
						} else {
							snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Failed to play");
							podcast_toast_time = SDL_GetTicks();
						}
					} else {
						if (!Wifi_ensureConnected(screen, show_setting)) {
							snprintf(podcast_toast_message, sizeof(podcast_toast_message), "No network connection");
							podcast_toast_time = SDL_GetTicks();
						} else if (Podcast_queueDownload(feed, podcast_current_episode_index) == 0) {
							snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Downloading...");
							podcast_toast_time = SDL_GetTicks();
						} else {
							snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Download failed");
							podcast_toast_time = SDL_GetTicks();
						}
					}
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_X) && count > 0 && feed) {
				PodcastEpisode* ep = Podcast_getEpisode(podcast_current_feed_index, podcast_episodes_selected);
				if (ep) {
					// Toggle played status
					if (ep->progress_sec == -1) {
						ep->progress_sec = 0;
						Podcast_saveProgress(feed->feed_url, ep->guid, 0);
						snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Marked as unplayed");
					} else {
						ep->progress_sec = -1;
						Podcast_markAsPlayed(feed->feed_url, ep->guid);
						Podcast_removeContinueListening(feed->feed_url, ep->guid);
						snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Marked as played");
					}
					Podcast_flushProgress();
					podcast_toast_time = SDL_GetTicks();
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_Y) && feed) {
				if (Podcast_isRefreshing()) {
					snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Already refreshing...");
					podcast_toast_time = SDL_GetTicks();
				} else if (!Wifi_ensureConnected(screen, show_setting)) {
					snprintf(podcast_toast_message, sizeof(podcast_toast_message), "No network connection");
					podcast_toast_time = SDL_GetTicks();
				} else {
					Podcast_startRefreshFeed(podcast_current_feed_index);
					snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Checking for new episodes...");
					podcast_toast_time = SDL_GetTicks();
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				Podcast_clearTitleScroll();
				podcast_toast_message[0] = '\0';
				UI_clearToast();
				state = PODCAST_INTERNAL_MENU;
				dirty = 1;
			}
		}
		// =========================================
		// DOWNLOAD QUEUE STATE
		// =========================================
		else if (state == PODCAST_INTERNAL_DOWNLOAD_QUEUE) {
			static int prev_queue_count = -1;
			int queue_count = 0;
			Podcast_getDownloadQueue(&queue_count);

			// Detect items removed by background download thread
			if (prev_queue_count >= 0 && queue_count < prev_queue_count) {
				Podcast_clearTitleScroll();
				if (podcast_queue_selected >= queue_count && queue_count > 0) {
					podcast_queue_selected = queue_count - 1;
				}
				dirty = 1;
			}
			prev_queue_count = queue_count;

			// Force redraw when downloads are active
			for (int qi = 0; qi < queue_count; qi++) {
				PodcastDownloadItem* qitem = &Podcast_getDownloadQueue(NULL)[qi];
				if (qitem->status == PODCAST_DOWNLOAD_DOWNLOADING || qitem->status == PODCAST_DOWNLOAD_PENDING) {
					dirty = 1;
					break;
				}
			}

			if (Podcast_isTitleScrolling())
				Podcast_animateTitleScroll();
			if (Podcast_titleScrollNeedsRender())
				dirty = 1;

			if (PAD_justRepeated(BTN_UP) && queue_count > 0) {
				podcast_queue_selected = (podcast_queue_selected > 0) ? podcast_queue_selected - 1 : queue_count - 1;
				Podcast_clearTitleScroll();
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN) && queue_count > 0) {
				podcast_queue_selected = (podcast_queue_selected < queue_count - 1) ? podcast_queue_selected + 1 : 0;
				Podcast_clearTitleScroll();
				dirty = 1;
			} else if (PAD_justPressed(BTN_X) && queue_count > 0) {
				// Cancel/remove selected item
				PodcastDownloadItem* queue = Podcast_getDownloadQueue(NULL);
				if (podcast_queue_selected < queue_count) {
					PodcastDownloadItem* sel = &queue[podcast_queue_selected];
					if (Podcast_cancelEpisodeDownload(sel->feed_url, sel->episode_guid) == 0) {
						snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Download removed");
					} else {
						snprintf(podcast_toast_message, sizeof(podcast_toast_message), "Remove failed");
					}
					podcast_toast_time = SDL_GetTicks();
					// Adjust selection if needed
					int new_count = 0;
					Podcast_getDownloadQueue(&new_count);
					if (podcast_queue_selected >= new_count && new_count > 0) {
						podcast_queue_selected = new_count - 1;
					}
					Podcast_clearTitleScroll();
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				Podcast_clearTitleScroll();
				podcast_toast_message[0] = '\0';
				UI_clearToast();
				state = PODCAST_INTERNAL_MENU;
				dirty = 1;
			}

			if (podcast_toast_message[0] && (SDL_GetTicks() - podcast_toast_time < TOAST_DURATION))
				dirty = 1;
		}
		// =========================================
		// SEEKING STATE (resuming to saved position)
		// =========================================
		else if (state == PODCAST_INTERNAL_SEEKING) {
			ModuleCommon_setAutosleepDisabled(true);

			if (!Player_resume()) {
				// Seek complete — start playback
				Player_play();
				UI_renderToast(screen, "", 0); // Clear the "Resuming..." toast
				ModuleCommon_recordInputTime();
				last_progress_save_time = SDL_GetTicks();
				state = PODCAST_INTERNAL_PLAYING;
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				// Cancel seeking — stop and go back
				Podcast_stop();
				return_to_episodes(&state, &dirty);
				continue;
			}

			dirty = 1; // Keep refreshing to show seeking status
		}
		// =========================================
		// PLAYING STATE
		// =========================================
		else if (state == PODCAST_INTERNAL_PLAYING) {
			ModuleCommon_setAutosleepDisabled(true);

			// Handle screen off hint timeout
			if (ModuleCommon_isScreenOffHintActive()) {
				if (ModuleCommon_processScreenOffHintTimeout()) {
					screen_off = true;
					GFX_clear(screen);
					GFX_flip(screen);
				}
				Podcast_update();
				GFX_sync();
				continue;
			} else if (screen_off) {
				// Wake screen with SELECT+A
				if (PAD_isPressed(BTN_SELECT) && PAD_isPressed(BTN_A)) {
					screen_off = false;
					PLAT_enableBacklight(1);
					ModuleCommon_recordInputTime();
					dirty = 1;
				}
				// Handle USB/Bluetooth media and volume buttons even with screen off
				handle_hid_events();
				ModuleCommon_handleHardwareVolume();
				Podcast_update();
				GFX_sync();
				continue;
			} else {
				if (PAD_justPressed(BTN_A)) {
					if (Player_getState() == PLAYER_STATE_PAUSED)
						Player_play();
					else
						Player_pause();
					ModuleCommon_recordInputTime();
					dirty = 1;
				} else if (PAD_justPressed(BTN_B)) {
					if (Player_getState() == PLAYER_STATE_PLAYING) {
						// Playing — let audio continue in background
						Podcast_flushProgress();
						Podcast_clearArtwork();
						GFX_clearLayers(LAYER_SCROLLTEXT);
						PLAT_clearLayers(LAYER_BUFFER);
						PLAT_clearLayers(LAYER_PODCAST_PROGRESS);
						PLAT_GPU_Flip();
						podcast_episodes_selected = podcast_current_episode_index;
						state = PODCAST_INTERNAL_EPISODES;
						dirty = 1;
					} else {
						// Paused — stop and go back normally
						Podcast_stop();
						return_to_episodes(&state, &dirty);
						continue;
					}
				} else if (PAD_tappedSelect(SDL_GetTicks())) {
					ModuleCommon_startScreenOffHint();
					clear_and_show_screen_off_hint(screen);
					continue;
				} else if (PAD_justRepeated(BTN_LEFT)) {
					int pos_ms = Player_getPosition();
					Player_seek(pos_ms - 10000 < 0 ? 0 : pos_ms - 10000);
					ModuleCommon_recordInputTime();
					dirty = 1;
				} else if (PAD_justRepeated(BTN_RIGHT)) {
					int pos_ms = Player_getPosition();
					int dur_ms = Player_getDuration();
					Player_seek(pos_ms + 30000 > dur_ms ? dur_ms : pos_ms + 30000);
					ModuleCommon_recordInputTime();
					dirty = 1;
				}

				Podcast_update();
				if (Podcast_isTitleScrolling())
					Podcast_animateTitleScroll();
				if (Podcast_titleScrollNeedsRender())
					dirty = 1;

				// Periodic progress saving (every 30 seconds)
				{
					uint32_t now = SDL_GetTicks();
					if (Podcast_isActive() && now - last_progress_save_time >= PROGRESS_SAVE_INTERVAL_MS) {
						PodcastFeed* feed = Podcast_getSubscription(podcast_current_feed_index);
						if (feed) {
							PodcastEpisode* ep = Podcast_getEpisode(podcast_current_feed_index, podcast_current_episode_index);
							if (ep) {
								int position = Player_getPosition();
								if (position > 0) {
									ep->progress_sec = position / 1000;
									Podcast_saveProgress(feed->feed_url, ep->guid, ep->progress_sec);
									Podcast_flushProgress();
								}
							}
						}
						last_progress_save_time = now;
					}
				}

				// Detect episode end (player stopped naturally)
				if (Player_getState() == PLAYER_STATE_STOPPED) {
					PodcastFeed* feed = Podcast_getSubscription(podcast_current_feed_index);
					PodcastEpisode* ep = Podcast_getEpisode(podcast_current_feed_index, podcast_current_episode_index);
					char saved_feed_url[PODCAST_MAX_URL] = "";
					char saved_guid[PODCAST_MAX_GUID] = "";
					if (feed && ep) {
						strncpy(saved_feed_url, feed->feed_url, PODCAST_MAX_URL - 1);
						strncpy(saved_guid, ep->guid, PODCAST_MAX_GUID - 1);
					}

					Podcast_stop();

					if (saved_feed_url[0] && saved_guid[0]) {
						Podcast_markAsPlayed(saved_feed_url, saved_guid);
						Podcast_removeContinueListening(saved_feed_url, saved_guid);
					}
					if (ep)
						ep->progress_sec = -1;

					return_to_episodes(&state, &dirty);
					continue;
				}

				// GPU progress bar update (updates every second without full redraw)
				if (PodcastProgress_needsRefresh()) {
					PodcastProgress_renderGPU();
				}

				// Auto screen-off
				if (Podcast_isActive() && ModuleCommon_checkAutoScreenOffTimeout()) {
					clear_and_show_screen_off_hint(screen);
					continue;
				}
			}
		}

		// Handle power management
		if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
			ModuleCommon_PWR_update(&dirty, &show_setting);
		}

		// Render
		if (dirty && !screen_off) {
			if (ModuleCommon_isScreenOffHintActive()) {
				GFX_clear(screen);
				render_screen_off_hint(screen);
			} else {
				switch (state) {
				case PODCAST_INTERNAL_MENU:
					render_podcast_main_page(screen, show_setting, podcast_menu_selected, &podcast_menu_scroll,
											 podcast_toast_message, podcast_toast_time);
					break;
				case PODCAST_INTERNAL_MANAGE:
					render_podcast_manage(screen, show_setting, podcast_manage_selected, Podcast_getSubscriptionCount());
					break;
				case PODCAST_INTERNAL_TOP_SHOWS:
					render_podcast_top_shows(screen, show_setting, podcast_top_shows_selected, &podcast_top_shows_scroll,
											 podcast_toast_message, podcast_toast_time);
					break;
				case PODCAST_INTERNAL_SEARCH_RESULTS:
					render_podcast_search_results(screen, show_setting, podcast_search_selected, &podcast_search_scroll,
												  podcast_toast_message, podcast_toast_time);
					break;
				case PODCAST_INTERNAL_EPISODES:
					render_podcast_episodes(screen, show_setting, podcast_current_feed_index, podcast_episodes_selected,
											&podcast_episodes_scroll, podcast_toast_message, podcast_toast_time);
					break;
				case PODCAST_INTERNAL_SEEKING:
					render_podcast_playing(screen, show_setting, podcast_current_feed_index, podcast_current_episode_index);
					// Overlay "Resuming..." text
					{
						PodcastEpisode* seek_ep = Podcast_getEpisode(podcast_current_feed_index, podcast_current_episode_index);
						char seek_msg[64];
						if (seek_ep && seek_ep->progress_sec > 0) {
							int m = seek_ep->progress_sec / 60;
							int s = seek_ep->progress_sec % 60;
							snprintf(seek_msg, sizeof(seek_msg), "Resuming at %d:%02d...", m, s);
						} else {
							snprintf(seek_msg, sizeof(seek_msg), "Resuming...");
						}
						UI_renderToast(screen, seek_msg, SDL_GetTicks());
					}
					break;
				case PODCAST_INTERNAL_PLAYING:
					render_podcast_playing(screen, show_setting, podcast_current_feed_index, podcast_current_episode_index);
					break;
				case PODCAST_INTERNAL_DOWNLOAD_QUEUE:
					render_podcast_download_queue(screen, show_setting, podcast_queue_selected, &podcast_queue_scroll,
												  podcast_toast_message, podcast_toast_time);
					break;
				}
			}

			GFX_flip(screen);
			dirty = 0;

			// Toast refresh
			ModuleCommon_tickToast(podcast_toast_message, podcast_toast_time, &dirty);
		} else if (!screen_off) {
			GFX_sync();
		}
	}
}

// Check if podcast module is active (playing)
bool PodcastModule_isActive(void) {
	return Podcast_isActive();
}

// Background tick: detect episode end and save progress while in menu
void PodcastModule_backgroundTick(void) {
	Podcast_update();

	// Periodic progress saving
	uint32_t now = SDL_GetTicks();
	if (Podcast_isActive() && now - last_progress_save_time >= PROGRESS_SAVE_INTERVAL_MS) {
		PodcastFeed* feed = Podcast_getSubscription(podcast_current_feed_index);
		if (feed) {
			PodcastEpisode* ep = Podcast_getEpisode(podcast_current_feed_index, podcast_current_episode_index);
			if (ep) {
				int position = Player_getPosition();
				if (position > 0) {
					ep->progress_sec = position / 1000;
					Podcast_saveProgress(feed->feed_url, ep->guid, ep->progress_sec);
					Podcast_flushProgress();
				}
			}
		}
		last_progress_save_time = now;
	}

	// Detect episode end
	if (Player_getState() == PLAYER_STATE_STOPPED) {
		PodcastFeed* feed = Podcast_getSubscription(podcast_current_feed_index);
		PodcastEpisode* ep = Podcast_getEpisode(podcast_current_feed_index, podcast_current_episode_index);
		char saved_feed_url[PODCAST_MAX_URL] = "";
		char saved_guid[PODCAST_MAX_GUID] = "";
		if (feed && ep) {
			strncpy(saved_feed_url, feed->feed_url, PODCAST_MAX_URL - 1);
			strncpy(saved_guid, ep->guid, PODCAST_MAX_GUID - 1);
		}

		Podcast_stop();

		if (saved_feed_url[0] && saved_guid[0]) {
			Podcast_markAsPlayed(saved_feed_url, saved_guid);
			Podcast_removeContinueListening(saved_feed_url, saved_guid);
		}
		if (ep)
			ep->progress_sec = -1;

		Background_setActive(BG_NONE);
		ModuleCommon_setAutosleepDisabled(false);
	}
}
