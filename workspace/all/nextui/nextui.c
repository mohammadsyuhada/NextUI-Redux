#include "api.h"
#include "config.h"
#include "defines.h"
#include "shortcuts.h"
#include "ui_components.h"
#include "utils.h"
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <msettings.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include "content.h"
#include "display_helper.h"
#include "gameswitcher.h"
#include "imgloader.h"
#include "launcher.h"
#include "quickmenu.h"
#include "search.h"
#include "ui_list.h"
#include "recents.h"
#include "types.h"

Directory* top;
Array* stack; // DirectoryArray

bool quit = false;
bool startgame = false;
ResumeState resume = {0};
RestoreState restore = {.depth = -1, .relative = -1};
static bool simple_mode = false;
static int animationdirection = 0;

static void Menu_init(void) {
	stack = Array_new(); // array of open Directories
	Recents_init();
	Recents_setHasEmu(hasEmu);
	Recents_setHasM3u(hasM3u);
	Launcher_setCleanupFunc(cleanupImageLoaderPool);
	Shortcuts_init();

	openDirectory(SDCARD_PATH, 0);
	loadLast(); // restore state when available

	QuickMenu_init(simple_mode);
	Search_init();
}
static void Menu_quit(void) {
	Recents_quit();
	Shortcuts_quit();
	DirectoryArray_free(stack);

	QuickMenu_quit();
	Search_quit();
}

///////////////////////////////////////

static bool dirty = true;
static ScrollTextState list_scroll = {0};
static ShortcutAction confirm_shortcut_action = SHORTCUT_NONE;
static Entry* confirm_shortcut_entry = NULL;

SDL_Surface* screen = NULL;
static SDL_Surface* blackBG = NULL;
static bool had_thumb = false;
static int ox;

static void updateBackgroundLayer(void) {
	SDL_LockMutex(bgMutex);
	if (folderbgchanged) {
		GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
						LAYER_BACKGROUND);
		if (folderbgbmp)
			GFX_drawOnLayer(folderbgbmp, 0, 0, screen->w, screen->h, 1.0f, 0,
							LAYER_BACKGROUND);
		folderbgchanged = 0;
	}
	SDL_UnlockMutex(bgMutex);
}

static void renderThumbnail(int reset_changed) {
	SDL_LockMutex(thumbMutex);
	if (confirm_shortcut_action != SHORTCUT_NONE) {
		GFX_clearLayers(LAYER_THUMBNAIL);
		GFX_clearLayers(LAYER_SCROLLTEXT);
	} else if (thumbbmp && thumbchanged) {
		int max_w = (int)(screen->w * CFG_getGameArtWidth());
		int max_h = (int)(screen->h * 0.6);
		int new_w, new_h;
		UI_calcImageFit(thumbbmp->w, thumbbmp->h, max_w, max_h, &new_w, &new_h);

		int target_x = screen->w - (new_w + SCALE1(BUTTON_MARGIN * 3));
		int target_y = (int)(screen->h * 0.50);
		int center_y = target_y - (new_h / 2);
		GFX_clearLayers(LAYER_THUMBNAIL);
		GFX_drawOnLayer(thumbbmp, target_x, center_y, new_w, new_h, 1.0f, 0,
						LAYER_THUMBNAIL);
		if (reset_changed)
			thumbchanged = 0;
	} else if (thumbchanged) {
		GFX_clearLayers(LAYER_THUMBNAIL);
		if (reset_changed)
			thumbchanged = 0;
	}
	SDL_UnlockMutex(thumbMutex);
}

static void resolveAndLoadBackground(Entry* entry, const char* rompath,
									 char* folderBgPath, size_t pathSize,
									 bool* list_show_entry_names) {
	// Persists across calls to avoid redundant background reloads
	static int lastType = -1;

	char defaultBgPath[512];
	snprintf(defaultBgPath, sizeof(defaultBgPath), SDCARD_PATH "/bg.png");

	// Resolve: what path to compare for changes, and what bg image to load
	const char* cmpPath = NULL;
	char bgPath[512] = {0};

	if ((entry->type == ENTRY_DIR || entry->type == ENTRY_ROM) &&
		Shortcuts_exists(entry->path + strlen(SDCARD_PATH))) {
		cmpPath = entry->path;
	} else if ((entry->type == ENTRY_DIR || entry->type == ENTRY_ROM) &&
			   CFG_getRomsUseFolderBackground()) {
		cmpPath = entry->type == ENTRY_DIR ? entry->path : rompath;
		snprintf(bgPath, sizeof(bgPath), "%s/.media/%s.png", cmpPath,
				 entry->type == ENTRY_DIR ? "bg" : "bglist");
		if (!exists(bgPath))
			strncpy(bgPath, defaultBgPath, sizeof(bgPath) - 1);
	} else if (entry->type == ENTRY_PAK && suffixMatch(".pak", entry->path)) {
		cmpPath = entry->path;
		snprintf(bgPath, sizeof(bgPath), TOOLS_PATH "/.media/%s/bg.png",
				 Shortcuts_getPakBasename(entry->path));
	} else if (exists(defaultBgPath) &&
			   strcmp(defaultBgPath, folderBgPath) != 0) {
		cmpPath = defaultBgPath;
		strncpy(bgPath, defaultBgPath, sizeof(bgPath) - 1);
	} else {
		*list_show_entry_names = true;
		return;
	}

	if (!cmpPath)
		return;

	// Skip if background hasn't changed
	if (strcmp(cmpPath, folderBgPath) == 0 && lastType == entry->type)
		return;

	lastType = entry->type;
	strncpy(folderBgPath, cmpPath, pathSize - 1);

	// Load background, or clear if image doesn't exist
	if (bgPath[0] && exists(bgPath))
		startLoadFolderBackground(bgPath, onBackgroundLoaded);
	else {
		onBackgroundLoaded(NULL);
		*list_show_entry_names = true;
	}
}

static int GameList_handleInput(unsigned long now, int currentScreen,
								IndicatorType show_setting) {
	int selected = top->selected;
	int total = top->entries->count;
	int row_count = MAIN_ROW_COUNT - 1;

	if (PAD_tappedMenu(now)) {
		currentScreen = SCREEN_QUICKMENU;
		animationdirection = SLIDE_DOWN;
		dirty = true;
		folderbgchanged = 1;
		GFX_clearLayers(LAYER_SCROLLTEXT);
		if (list_scroll.cached_scroll_surface) {
			SDL_FreeSurface(list_scroll.cached_scroll_surface);
			list_scroll.cached_scroll_surface = NULL;
		}
		list_scroll.text[0] = '\0';
		list_scroll.needs_scroll = false;
		list_scroll.scroll_active = false;
		if (!HAS_POWER_BUTTON && !simple_mode)
			PWR_enableSleep();
		return currentScreen;
	} else if (PAD_tappedSelect(now) && confirm_shortcut_action == SHORTCUT_NONE) {
		currentScreen = SCREEN_GAMESWITCHER;
		GameSwitcher_resetSelection();
		animationdirection = SLIDE_UP;
		dirty = true;
		GFX_clearLayers(LAYER_SCROLLTEXT);
		if (list_scroll.cached_scroll_surface) {
			SDL_FreeSurface(list_scroll.cached_scroll_surface);
			list_scroll.cached_scroll_surface = NULL;
		}
		list_scroll.text[0] = '\0';
		list_scroll.needs_scroll = false;
		list_scroll.scroll_active = false;
		return currentScreen;
	} else if (total > 0 && confirm_shortcut_action == SHORTCUT_NONE) {
		if (PAD_justRepeated(BTN_UP)) {
			if (selected == 0 && !PAD_justPressed(BTN_UP)) {
			} else {
				selected -= 1;
				if (selected < 0) {
					selected = total - 1;
					int start = total - row_count;
					top->start = (start < 0) ? 0 : start;
					top->end = total;
				} else if (selected < top->start) {
					top->start -= 1;
					top->end -= 1;
				}
			}
		} else if (PAD_justRepeated(BTN_DOWN)) {
			if (selected == total - 1 && !PAD_justPressed(BTN_DOWN)) {
			} else {
				selected += 1;
				if (selected >= total) {
					selected = 0;
					top->start = 0;
					top->end = (total < row_count) ? total : row_count;
				} else if (selected >= top->end) {
					top->start += 1;
					top->end += 1;
				}
			}
		}
		if (PAD_justRepeated(BTN_LEFT)) {
			selected -= row_count;
			if (selected < 0) {
				selected = 0;
				top->start = 0;
				top->end = (total < row_count) ? total : row_count;
			} else if (selected < top->start) {
				top->start -= row_count;
				if (top->start < 0)
					top->start = 0;
				top->end = top->start + row_count;
			}
		} else if (PAD_justRepeated(BTN_RIGHT)) {
			selected += row_count;
			if (selected >= total) {
				selected = total - 1;
				int start = total - row_count;
				top->start = (start < 0) ? 0 : start;
				top->end = total;
			} else if (selected >= top->end) {
				top->end += row_count;
				if (top->end > total)
					top->end = total;
				top->start = top->end - row_count;
			}
		}
	}

	if (confirm_shortcut_action == SHORTCUT_NONE && PAD_justRepeated(BTN_L1) &&
		!PAD_isPressed(BTN_R1) &&
		!PWR_ignoreSettingInput(BTN_L1, show_setting)) { // previous alpha
		Entry* entry = top->entries->items[selected];
		int i = entry->alpha - 1;
		if (i >= 0) {
			selected = top->alphas.items[i];
			if (total > row_count) {
				top->start = selected;
				top->end = top->start + row_count;
				if (top->end > total)
					top->end = total;
				top->start = top->end - row_count;
			}
		}
	} else if (confirm_shortcut_action == SHORTCUT_NONE && PAD_justRepeated(BTN_R1) &&
			   !PAD_isPressed(BTN_L1) &&
			   !PWR_ignoreSettingInput(BTN_R1, show_setting)) { // next alpha
		Entry* entry = top->entries->items[selected];
		int i = entry->alpha + 1;
		if (i < top->alphas.count) {
			selected = top->alphas.items[i];
			if (total > row_count) {
				top->start = selected;
				top->end = top->start + row_count;
				if (top->end > total)
					top->end = total;
				top->start = top->end - row_count;
			}
		}
	}

	if (selected != top->selected) {
		top->selected = selected;
		dirty = true;
	}

	Entry* entry = top->entries->items[top->selected];

	if (dirty && total > 0)
		readyResume(entry);

	// Handle confirmation dialog for shortcuts
	if (confirm_shortcut_action != SHORTCUT_NONE) {
		if (PAD_justPressed(BTN_A)) {
			Shortcuts_confirmAction(confirm_shortcut_action,
									confirm_shortcut_entry);
			confirm_shortcut_action = SHORTCUT_NONE;
			confirm_shortcut_entry = NULL;

			// Refresh root directory to show updated shortcuts
			Directory* root = stack->items[0];
			EntryArray_free(root->entries);
			root->entries = getRoot(simple_mode);
			IntArray_init(&root->alphas);
			Directory_index(root);
			// Keep selected in bounds
			if (root->selected >= root->entries->count) {
				root->selected =
					root->entries->count > 0 ? root->entries->count - 1 : 0;
			}

			dirty = true;
		} else if (PAD_justPressed(BTN_B)) {
			confirm_shortcut_action = SHORTCUT_NONE;
			confirm_shortcut_entry = NULL;
			dirty = true;
		}
	} else if (total > 0 && resume.can_resume && PAD_justReleased(BTN_RESUME) && !PAD_isPressed(BTN_L2) && !PAD_isPressed(BTN_R2)) {
		resume.should_resume = true;
		Entry_open(entry);
		dirty = true;
	}
	// Y to search at root
	else if (stack->count == 1 && PAD_justReleased(BTN_Y)) {
		if (Search_open()) {
			currentScreen = SCREEN_SEARCH;
			animationdirection = SLIDE_LEFT;
			dirty = true;
			GFX_clearLayers(LAYER_SCROLLTEXT);
			if (list_scroll.cached_scroll_surface) {
				SDL_FreeSurface(list_scroll.cached_scroll_surface);
				list_scroll.cached_scroll_surface = NULL;
			}
			list_scroll.text[0] = '\0';
			list_scroll.needs_scroll = false;
			list_scroll.scroll_active = false;
		}
		return currentScreen;
	}
	// Y to add/remove shortcut (only in Tools folder or console directory)
	else if (total > 0 &&
			 (Shortcuts_isInToolsFolder(top->path) ||
			  Shortcuts_isInConsoleDir(top->path)) &&
			 canPinEntry(entry) && PAD_justReleased(BTN_Y)) {
		if (Shortcuts_exists(entry->path + strlen(SDCARD_PATH))) {
			confirm_shortcut_action = SHORTCUT_REMOVE;
		} else {
			confirm_shortcut_action = SHORTCUT_ADD;
		}
		confirm_shortcut_entry = entry;
		dirty = true;
	} else if (total > 0 && PAD_justPressed(BTN_A)) {
		Entry_open(entry);
		if (entry->type == ENTRY_DIR && !startgame) {
			animationdirection = SLIDE_LEFT;
		}
		dirty = true;

		if (top->entries->count > 0)
			readyResume(top->entries->items[top->selected]);
	} else if (PAD_justPressed(BTN_B) && stack->count > 1) {
		closeDirectory();
		animationdirection = SLIDE_RIGHT;
		dirty = true;

		if (top->entries->count > 0)
			readyResume(top->entries->items[top->selected]);
	}

	return currentScreen;
}

int main(int argc, char* argv[]) {
	if (autoResume())
		return 0; // nothing to do

	simple_mode = exists(SIMPLE_MODE_PATH);
	Content_setSimpleMode(simple_mode);

	InitSettings();

	screen = GFX_init(MODE_MAIN);

	PAD_init();
	VIB_init();
	PWR_init();
	if (!HAS_POWER_BUTTON && !simple_mode)
		PWR_disableSleep();

	initImageLoaderPool();
	Menu_init();
	GameSwitcher_init();
	int lastScreen = SCREEN_OFF;
	int currentScreen = CFG_getDefaultView();

	if (GameSwitcher_shouldStartInSwitcher())
		currentScreen = SCREEN_GAMESWITCHER;

	// add a nice fade into the game switcher
	if (currentScreen == SCREEN_GAMESWITCHER)
		lastScreen = SCREEN_GAME;

	// make sure we have no running games logged as active anymore (we might be
	// launching back into the UI here)
	system("gametimectl.elf stop_all");

	GFX_setVsync(VSYNC_STRICT);

	PAD_reset();
	GFX_clearLayers(LAYER_ALL);
	GFX_clear(screen);

	IndicatorType show_setting = INDICATOR_NONE;
	pthread_t cpucheckthread = 0;
	if (pthread_create(&cpucheckthread, NULL, PLAT_cpu_monitor, NULL) == 0) {
		pthread_detach(cpucheckthread);
	}

	int selected_row = top->selected - top->start;
	bool list_show_entry_names = true;

	char folderBgPath[1024] = {0};
	folderbgbmp = NULL;

	blackBG = SDL_CreateRGBSurfaceWithFormat(
		0, screen->w, screen->h, screen->format->BitsPerPixel,
		screen->format->format);
	if (blackBG)
		SDL_FillRect(blackBG, NULL, SDL_MapRGBA(screen->format, 0, 0, 0, 255));

	while (!quit) {
		GFX_startFrame();
		unsigned long now = SDL_GetTicks();

		PAD_poll();

		int total = top->entries->count;

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (UI_statusBarChanged())
			dirty = true;

		// Check if a thumbnail finished loading asynchronously
		if (thumbCheckAsyncLoaded())
			dirty = true;

		int gsanimdir = ANIM_NONE;

		if (currentScreen == SCREEN_QUICKMENU) {
			QuickMenuResult qmr = QuickMenu_handleInput(now);
			if (qmr.dirty)
				dirty = true;
			if (qmr.folderbgchanged)
				folderbgchanged = 1;
			if (qmr.screen != SCREEN_QUICKMENU) {
				currentScreen = qmr.screen;
				animationdirection = SLIDE_UP;
			}
		} else if (currentScreen == SCREEN_GAMESWITCHER) {
			GameSwitcherResult gsr = GameSwitcher_handleInput(now);
			if (gsr.dirty)
				dirty = true;
			if (gsr.folderbgchanged)
				folderbgchanged = 1;
			if (gsr.startgame)
				startgame = true;
			if (gsr.screen != SCREEN_GAMESWITCHER) {
				currentScreen = gsr.screen;
				if (currentScreen == SCREEN_GAMELIST)
					animationdirection = SLIDE_DOWN;
			}
			gsanimdir = gsr.gsanimdir;
		} else if (currentScreen == SCREEN_SEARCH) {
			SearchResult sr = Search_handleInput(now);
			if (sr.dirty)
				dirty = true;
			if (sr.folderbgchanged)
				folderbgchanged = 1;
			if (sr.startgame)
				startgame = true;
			if (sr.screen != SCREEN_SEARCH) {
				currentScreen = sr.screen;
				if (currentScreen == SCREEN_GAMELIST)
					animationdirection = SLIDE_RIGHT;
			}
		} else {
			int prevScreen = currentScreen;
			currentScreen =
				GameList_handleInput(now, currentScreen, show_setting);
			if (currentScreen == SCREEN_QUICKMENU &&
				prevScreen != SCREEN_QUICKMENU) {
				QuickMenu_resetSelection();
			}
			total = top->entries->count;
		}

		// TG5050: search keyboard may have triggered display recovery (new screen surface)
		{
			SDL_Surface* ns = DisplayHelper_getReinitScreen();
			if (ns)
				screen = ns;
		}

		if (dirty) {
			SDL_Surface* tmpOldScreen = NULL;
			if (animationdirection != ANIM_NONE) {
				tmpOldScreen = GFX_captureRendererToSurface();
				if (tmpOldScreen)
					SDL_SetSurfaceBlendMode(tmpOldScreen, SDL_BLENDMODE_BLEND);
			}

			if (lastScreen == SCREEN_GAME || lastScreen == SCREEN_OFF) {
				GFX_clearLayers(LAYER_ALL);
				GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
								LAYER_BACKGROUND);
			} else {
				GFX_clearLayers(LAYER_TRANSITION);
				if (lastScreen != SCREEN_GAMELIST)
					GFX_clearLayers(LAYER_THUMBNAIL);
				GFX_clearLayers(LAYER_SCROLLTEXT);
				GFX_clearLayers(LAYER_IDK2);
			}
			GFX_clear(screen);

			// render top menu bar
			const char* menu_title;
			if (currentScreen == SCREEN_GAMESWITCHER)
				menu_title = GameSwitcher_getSelectedName();
			else if (currentScreen == SCREEN_SEARCH)
				menu_title = "Search";
			else
				menu_title = stack->count > 1 ? top->name : "NextUI Redux";
			int ow = UI_renderMenuBar(screen, menu_title);

			// capture menu bar for fixed overlay during animation
			SDL_Surface* menuBarSurface = NULL;
			if (animationdirection != ANIM_NONE) {
				int bar_h = SCALE1(BUTTON_SIZE) + SCALE1(BUTTON_MARGIN * 2);
				menuBarSurface = SDL_CreateRGBSurfaceWithFormat(
					0, screen->w, bar_h, screen->format->BitsPerPixel,
					screen->format->format);
				if (menuBarSurface) {
					SDL_FillRect(menuBarSurface, NULL,
								 SDL_MapRGBA(menuBarSurface->format, 0, 0, 0, 255));
					SDL_BlitSurface(screen, &(SDL_Rect){0, 0, screen->w, bar_h},
									menuBarSurface, NULL);
				}
			}

			if (currentScreen == SCREEN_QUICKMENU) {
				QuickMenu_render(lastScreen, show_setting, ow,
								 folderBgPath, sizeof(folderBgPath), blackBG);
				lastScreen = SCREEN_QUICKMENU;
			} else if (currentScreen == SCREEN_SEARCH) {
				Search_render(screen, blackBG, lastScreen);
				lastScreen = SCREEN_SEARCH;
			} else if (startgame) {
				GFX_clearLayers(LAYER_ALL);
				GFX_clear(screen);
				GFX_flipHidden();
			} else if (currentScreen == SCREEN_GAMESWITCHER) {
				GameSwitcher_render(lastScreen, blackBG, gsanimdir);
				lastScreen = SCREEN_GAMESWITCHER;
			} else {
				Entry* entry = top->entries->items[top->selected];
				assert(entry);
				char tmp_path[MAX_PATH];
				strncpy(tmp_path, entry->path, sizeof(tmp_path) - 1);
				tmp_path[sizeof(tmp_path) - 1] = '\0';

				char* res_name = strrchr(tmp_path, '/');
				if (res_name)
					res_name++;
				else
					res_name = tmp_path;

				char path_copy[1024];
				strncpy(path_copy, entry->path, sizeof(path_copy) - 1);
				path_copy[sizeof(path_copy) - 1] = '\0';

				char* rompath = dirname(path_copy);

				char res_copy[1024];
				strncpy(res_copy, res_name, sizeof(res_copy) - 1);
				res_copy[sizeof(res_copy) - 1] = '\0';

				char* dot = strrchr(res_copy, '.');
				if (dot)
					*dot = '\0';

				// this is only a choice on the root folder
				list_show_entry_names =
					stack->count > 1 || CFG_getShowFolderNamesAtRoot();

				// load folder background
				resolveAndLoadBackground(entry, rompath, folderBgPath,
										 sizeof(folderBgPath),
										 &list_show_entry_names);

				// load game thumbnails
				if (total > 0) {
					if (CFG_getShowGameArt()) {
						char thumbpath[1024];
						snprintf(thumbpath, sizeof(thumbpath), "%s/.media/%s.png", rompath,
								 res_copy);
						had_thumb = startLoadThumb(thumbpath);
						int max_w = (int)(screen->w - (screen->w * CFG_getGameArtWidth()));
						if (had_thumb)
							ox = (int)(max_w)-SCALE1(BUTTON_MARGIN * 5);
						else
							ox = screen->w;
					}
				}

				// buttons
				{
					char* right_pairs[16] = {NULL};
					int p = 0;

					// pin action (hardware hints override this when volume is pressed)
					if (!(show_setting && !GetHDMI()) && total > 0 &&
						!GetHDMI() &&
						(Shortcuts_isInToolsFolder(top->path) ||
						 Shortcuts_isInConsoleDir(top->path)) &&
						canPinEntry(entry)) {
						right_pairs[p++] = "Y";
						right_pairs[p++] = Shortcuts_exists(entry->path + strlen(SDCARD_PATH))
											   ? "UNPIN"
											   : "PIN";
					}

					// search hint at root
					if (!(show_setting && !GetHDMI()) && !GetHDMI() &&
						stack->count == 1 && total > 0) {
						right_pairs[p++] = "Y";
						right_pairs[p++] = "SEARCH";
					}

					// navigation actions
					if (total == 0) {
						if (stack->count > 1) {
							right_pairs[p++] = "B";
							right_pairs[p++] = "BACK";
						}
					} else if (confirm_shortcut_action == SHORTCUT_NONE) {
						if (resume.can_resume) {
							right_pairs[p++] = "X";
							right_pairs[p++] = "RESUME";
							right_pairs[p++] = "B";
							right_pairs[p++] = "BACK";
							right_pairs[p++] = "A";
							right_pairs[p++] = "OPEN";
						} else if (stack->count > 1) {
							right_pairs[p++] = "B";
							right_pairs[p++] = "BACK";
							right_pairs[p++] = "A";
							right_pairs[p++] = "OPEN";
						} else {
							right_pairs[p++] = "A";
							right_pairs[p++] = "OPEN";
						}
					}

					if (right_pairs[0])
						UI_renderButtonHintBar(screen, right_pairs);
				}

				if (total > 0) {
					selected_row = top->selected - top->start;

					for (int i = top->start, j = 0; i < top->end; i++, j++) {
						Entry* entry = top->entries->items[i];
						char* entry_name = entry->name;
						char* entry_unique = entry->unique;
						bool row_is_selected = (j == selected_row);
						bool row_is_top = (i == top->start);

						// Calculate per-item available width (thumbnail-aware)
						int available_width =
							MAX(0, (had_thumb ? ox + SCALE1(BUTTON_MARGIN)
											  : screen->w - SCALE1(BUTTON_MARGIN)) -
									   SCALE1(PADDING * 2));
						if (row_is_top && !had_thumb)
							available_width -= ow;

						// Prepare display text: prefer unique name, fall back to entry name
						trimSortingMeta(&entry_name);
						if (entry_unique)
							trimSortingMeta(&entry_unique);
						char* display_text = entry_unique ? entry_unique : entry_name;

						int top_offset = PILL_SIZE;
						int y = SCALE1(PADDING + top_offset + j * PILL_SIZE);

						if (list_show_entry_names) {
							char truncated[256];
							ListLayout item_layout = {
								.item_h = SCALE1(PILL_SIZE),
								.max_width = available_width,
							};
							ListItemPos pos = UI_renderListItemPill(
								screen, &item_layout, font.large,
								display_text, truncated, y, row_is_selected, 0);
							int text_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2);
							UI_renderListItemText(screen,
												  row_is_selected ? &list_scroll : NULL,
												  display_text, font.large,
												  pos.text_x, pos.text_y, text_width, row_is_selected);
						}
					}
					if (lastScreen == SCREEN_OFF) {
						GFX_animateSurfaceOpacity(blackBG, 0, 0, screen->w, screen->h, 255,
												  0, CFG_getMenuTransitions() ? 200 : 20,
												  LAYER_THUMBNAIL);
					}

				} else {
					UI_renderCenteredMessage(screen, "Empty folder");
				}

				// Render confirmation dialog for shortcuts
				if (confirm_shortcut_action != SHORTCUT_NONE && confirm_shortcut_entry) {
					char* title =
						confirm_shortcut_action == SHORTCUT_ADD ? "Pin shortcut?" : "Unpin shortcut?";
					UI_renderConfirmDialog(screen, title, confirm_shortcut_entry->name);
				}

				lastScreen = SCREEN_GAMELIST;
			}

			if (animationdirection != ANIM_NONE) {
				if (CFG_getMenuTransitions()) {
					if (lastScreen != SCREEN_GAMESWITCHER) {
						GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
										LAYER_BACKGROUND);
						folderbgchanged = 1;
					}
					GFX_clearLayers(LAYER_TRANSITION);
					GFX_clearLayers(LAYER_THUMBNAIL);
					if (menuBarSurface)
						GFX_drawOnLayer(menuBarSurface, 0, 0, screen->w,
										menuBarSurface->h, 1.0f, 0, LAYER_IDK2);
					GFX_flipHidden();
					SDL_Surface* tmpNewScreen = GFX_captureRendererToSurface();
					SDL_SetSurfaceBlendMode(tmpNewScreen, SDL_BLENDMODE_BLEND);
					GFX_clearLayers(LAYER_THUMBNAIL);
					if (animationdirection == SLIDE_LEFT)
						GFX_animateSlidePages(
							tmpOldScreen, 0, 0, 0 - FIXED_WIDTH, 0,
							tmpNewScreen, FIXED_WIDTH, 0, 0, 0,
							FIXED_WIDTH, FIXED_HEIGHT, 250, LAYER_THUMBNAIL);
					if (animationdirection == SLIDE_RIGHT)
						GFX_animateSlidePages(
							tmpOldScreen, 0, 0, FIXED_WIDTH, 0,
							tmpNewScreen, 0 - FIXED_WIDTH, 0, 0, 0,
							FIXED_WIDTH, FIXED_HEIGHT, 250, LAYER_THUMBNAIL);
					if (animationdirection == SLIDE_DOWN)
						GFX_animateSlidePages(
							tmpOldScreen, 0, 0, 0, FIXED_HEIGHT,
							tmpNewScreen, 0, 0 - FIXED_HEIGHT, 0, 0,
							FIXED_WIDTH, FIXED_HEIGHT, 250, LAYER_THUMBNAIL);
					if (animationdirection == SLIDE_UP)
						GFX_animateSlidePages(
							tmpOldScreen, 0, 0, 0, 0 - FIXED_HEIGHT,
							tmpNewScreen, 0, FIXED_HEIGHT, 0, 0,
							FIXED_WIDTH, FIXED_HEIGHT, 250, LAYER_THUMBNAIL);
					GFX_clearLayers(LAYER_THUMBNAIL);
					GFX_clearLayers(LAYER_IDK2);
					SDL_FreeSurface(tmpNewScreen);
				}
				// animation done
				animationdirection = ANIM_NONE;
			}
			if (menuBarSurface)
				SDL_FreeSurface(menuBarSurface);

			if (lastScreen == SCREEN_QUICKMENU) {
				updateBackgroundLayer();
			} else if (lastScreen == SCREEN_SEARCH) {
				updateBackgroundLayer();
				renderThumbnail(1);
			} else if (lastScreen == SCREEN_GAMELIST) {
				updateBackgroundLayer();
				renderThumbnail(1);

				GFX_clearLayers(LAYER_TRANSITION);
				GFX_clearLayers(LAYER_SCROLLTEXT);
			}
			if (!startgame) // dont flip if game gonna start
				GFX_flip(screen);

			if (tmpOldScreen)
				SDL_FreeSurface(tmpOldScreen);

			dirty = false;
		} else if (folderbgchanged || thumbchanged ||
				   ScrollText_isScrolling(&list_scroll) || ScrollText_needsRender(&list_scroll)) {
			updateBackgroundLayer();
			renderThumbnail(1);
			if (currentScreen != SCREEN_GAMESWITCHER &&
				currentScreen != SCREEN_QUICKMENU &&
				currentScreen != SCREEN_SEARCH) {
				if (confirm_shortcut_action != SHORTCUT_NONE) {
					GFX_clearLayers(LAYER_SCROLLTEXT);
				} else {
					ScrollText_activateAfterDelay(&list_scroll);
					if (ScrollText_isScrolling(&list_scroll)) {
						ScrollText_animateOnly(&list_scroll);
					}
				}
			} else {
				SDL_Delay(16);
			}
			// Flush layer changes (e.g. new thumbnail) to screen
			if (getNeedDraw()) {
				PLAT_GPU_Flip();
				setNeedDraw(0);
			}
			dirty = false;
		} else {
			// want to draw only if needed
			SDL_LockMutex(bgqueueMutex);
			SDL_LockMutex(thumbqueueMutex);
			if (getNeedDraw()) {
				PLAT_GPU_Flip();
				setNeedDraw(0);
			} else {
				unsigned long elapsed = SDL_GetTicks() - now;
				if (elapsed < 16)
					SDL_Delay(16 - elapsed);
			}
			SDL_UnlockMutex(thumbqueueMutex);
			SDL_UnlockMutex(bgqueueMutex);
		}

		SDL_LockMutex(frameMutex);
		frameReady = true;
		SDL_CondSignal(flipCond);
		SDL_UnlockMutex(frameMutex);

		// animation does not carry over between loops, this should only ever be set
		// by input handling and directly consumed by the following render pass
		assert(animationdirection == ANIM_NONE);

		// handle HDMI change
		static int had_hdmi = -1;
		int has_hdmi = GetHDMI();
		if (had_hdmi == -1)
			had_hdmi = has_hdmi;
		if (has_hdmi != had_hdmi) {
			had_hdmi = has_hdmi;

			Entry* entry = top->entries->items[top->selected];
			LOG_info("restarting after HDMI change... (%s)\n", entry->path);
			saveLast(entry->path); // NOTE: doesn't work in Recents (by design)
			sleep(4);
			quit = true;
		}
	}

	// Fast exit when launching a game — skip full cleanup to minimize
	// delay. The OS reclaims all memory/FDs on process exit. The parent
	// shell script reads /tmp/next only after nextui.elf exits.
	if (startgame) {
		GFX_quit();
		_exit(0);
	}

	Menu_quit();
	PWR_quit();
	PAD_quit();

	// Cleanup scroll text state
	if (list_scroll.cached_scroll_surface) {
		SDL_FreeSurface(list_scroll.cached_scroll_surface);
		list_scroll.cached_scroll_surface = NULL;
	}

	// Cleanup worker threads and their synchronization primitives
	cleanupImageLoaderPool();

	GFX_quit(); // Cleanup video subsystem first to stop GPU threads

	// Now safe to free surfaces after GPU threads are stopped
	if (blackBG)
		SDL_FreeSurface(blackBG);
	if (folderbgbmp)
		SDL_FreeSurface(folderbgbmp);
	if (thumbbmp)
		SDL_FreeSurface(thumbbmp);

	QuitSettings();
}