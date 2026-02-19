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
#include "gameswitcher.h"
#include "imgloader.h"
#include "launcher.h"
#include "pill.h"
#include "quickmenu.h"
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
}
static void Menu_quit(void) {
	Recents_quit();
	Shortcuts_quit();
	DirectoryArray_free(stack);

	QuickMenu_quit();
}

///////////////////////////////////////

static bool dirty = true;
static int previous_row = 0;
static int previous_depth = 0;
static ShortcutAction confirm_shortcut_action = SHORTCUT_NONE;
static Entry* confirm_shortcut_entry = NULL;

SDL_Surface* screen = NULL;
static bool had_thumb = false;
static int ox;

static void updateBackgroundLayer(void) {
	SDL_LockMutex(bgMutex);
	if (folderbgchanged) {
		if (folderbgbmp)
			GFX_drawOnLayer(folderbgbmp, 0, 0, screen->w, screen->h, 1.0f, 0,
							LAYER_BACKGROUND);
		else
			GFX_clearLayers(LAYER_BACKGROUND);
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
								int show_setting) {
	int selected = top->selected;
	int total = top->entries->count;

	if (PAD_tappedMenu(now)) {
		currentScreen = SCREEN_QUICKMENU;
		dirty = true;
		folderbgchanged = 1;
		if (!HAS_POWER_BUTTON && !simple_mode)
			PWR_enableSleep();
		return currentScreen;
	} else if (PAD_tappedSelect(now) && confirm_shortcut_action == SHORTCUT_NONE) {
		currentScreen = SCREEN_GAMESWITCHER;
		GameSwitcher_resetSelection();
		dirty = true;
		return currentScreen;
	} else if (total > 0 && confirm_shortcut_action == SHORTCUT_NONE) {
		if (PAD_justRepeated(BTN_UP)) {
			if (selected == 0 && !PAD_justPressed(BTN_UP)) {
			} else {
				selected -= 1;
				if (selected < 0) {
					selected = total - 1;
					int start = total - MAIN_ROW_COUNT;
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
					top->end = (total < MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
				} else if (selected >= top->end) {
					top->start += 1;
					top->end += 1;
				}
			}
		}
		if (PAD_justRepeated(BTN_LEFT)) {
			selected -= MAIN_ROW_COUNT;
			if (selected < 0) {
				selected = 0;
				top->start = 0;
				top->end = (total < MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
			} else if (selected < top->start) {
				top->start -= MAIN_ROW_COUNT;
				if (top->start < 0)
					top->start = 0;
				top->end = top->start + MAIN_ROW_COUNT;
			}
		} else if (PAD_justRepeated(BTN_RIGHT)) {
			selected += MAIN_ROW_COUNT;
			if (selected >= total) {
				selected = total - 1;
				int start = total - MAIN_ROW_COUNT;
				top->start = (start < 0) ? 0 : start;
				top->end = total;
			} else if (selected >= top->end) {
				top->end += MAIN_ROW_COUNT;
				if (top->end > total)
					top->end = total;
				top->start = top->end - MAIN_ROW_COUNT;
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
			if (total > MAIN_ROW_COUNT) {
				top->start = selected;
				top->end = top->start + MAIN_ROW_COUNT;
				if (top->end > total)
					top->end = total;
				top->start = top->end - MAIN_ROW_COUNT;
			}
		}
	} else if (confirm_shortcut_action == SHORTCUT_NONE && PAD_justRepeated(BTN_R1) &&
			   !PAD_isPressed(BTN_L1) &&
			   !PWR_ignoreSettingInput(BTN_R1, show_setting)) { // next alpha
		Entry* entry = top->entries->items[selected];
		int i = entry->alpha + 1;
		if (i < top->alphas.count) {
			selected = top->alphas.items[i];
			if (total > MAIN_ROW_COUNT) {
				top->start = selected;
				top->end = top->start + MAIN_ROW_COUNT;
				if (top->end > total)
					top->end = total;
				top->start = top->end - MAIN_ROW_COUNT;
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
	} else if (total > 0 && resume.can_resume && PAD_justReleased(BTN_RESUME)) {
		resume.should_resume = true;
		Entry_open(entry);
		dirty = true;
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

	int show_setting = INDICATOR_NONE;
	int was_online = PWR_isOnline();
	int had_bt = PLAT_btIsConnected();

	pthread_t cpucheckthread = 0;
	if (pthread_create(&cpucheckthread, NULL, PLAT_cpu_monitor, NULL) == 0) {
		pthread_detach(cpucheckthread);
	}

	int selected_row = top->selected - top->start;
	float targetY;
	float previousY;
	bool is_scrolling = false;
	bool list_show_entry_names = true;

	char folderBgPath[1024] = {0};
	folderbgbmp = NULL;

	SDL_Surface* blackBG = SDL_CreateRGBSurfaceWithFormat(
		0, screen->w, screen->h, screen->format->BitsPerPixel,
		screen->format->format);
	if (blackBG)
		SDL_FillRect(blackBG, NULL, SDL_MapRGBA(screen->format, 0, 0, 0, 255));

	Pill_init();

	while (!quit) {
		GFX_startFrame();
		unsigned long now = SDL_GetTicks();

		PAD_poll();

		int total = top->entries->count;

		PWR_update(&dirty, &show_setting, NULL, NULL);

		int is_online = PWR_isOnline();
		if (was_online != is_online)
			dirty = true;
		was_online = is_online;

		int has_bt = PLAT_btIsConnected();
		if (had_bt != has_bt)
			dirty = true;
		had_bt = has_bt;

		int gsanimdir = ANIM_NONE;

		if (currentScreen == SCREEN_QUICKMENU) {
			QuickMenuResult qmr = QuickMenu_handleInput(now);
			if (qmr.dirty)
				dirty = true;
			if (qmr.folderbgchanged)
				folderbgchanged = 1;
			if (qmr.screen != SCREEN_QUICKMENU)
				currentScreen = qmr.screen;
		} else if (currentScreen == SCREEN_GAMESWITCHER) {
			GameSwitcherResult gsr = GameSwitcher_handleInput(now);
			if (gsr.dirty)
				dirty = true;
			if (gsr.folderbgchanged)
				folderbgchanged = 1;
			if (gsr.startgame)
				startgame = true;
			if (gsr.screen != SCREEN_GAMESWITCHER)
				currentScreen = gsr.screen;
			gsanimdir = gsr.gsanimdir;
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

		if (dirty) {
			SDL_Surface* tmpOldScreen = NULL;
			// NOTE:22 This causes slowdown when CFG_getMenuTransitions is set to
			// false because animationdirection turns > 0 somewhere but is never set
			// back to 0 and so this code runs on every action, will fix later
			if (animationdirection != ANIM_NONE ||
				(lastScreen == SCREEN_GAMELIST &&
				 currentScreen == SCREEN_GAMESWITCHER)) {
				if (tmpOldScreen)
					SDL_FreeSurface(tmpOldScreen);
				tmpOldScreen = GFX_captureRendererToSurface();
				if (tmpOldScreen)
					SDL_SetSurfaceBlendMode(tmpOldScreen, SDL_BLENDMODE_BLEND);
			}

			if (lastScreen == SCREEN_GAME || lastScreen == SCREEN_OFF) {
				GFX_clearLayers(LAYER_ALL);
			} else {
				GFX_clearLayers(LAYER_TRANSITION);
				if (lastScreen != SCREEN_GAMELIST)
					GFX_clearLayers(LAYER_THUMBNAIL);
				GFX_clearLayers(LAYER_SCROLLTEXT);
				GFX_clearLayers(LAYER_IDK2);
			}
			GFX_clear(screen);

			int ow = GFX_blitHardwareGroup(screen, show_setting);
			if (currentScreen == SCREEN_QUICKMENU) {
				QuickMenu_render(lastScreen, show_setting, ow,
								 folderBgPath, sizeof(folderBgPath));
				lastScreen = SCREEN_QUICKMENU;
			} else if (startgame) {
				GFX_clearLayers(LAYER_ALL);
				GFX_clear(screen);
				GFX_flipHidden();
				if (tmpOldScreen)
					GFX_animateSurfaceOpacity(tmpOldScreen, 0, 0, screen->w, screen->h,
											  255, 0, CFG_getMenuTransitions() ? 150 : 20,
											  LAYER_BACKGROUND);
			} else if (currentScreen == SCREEN_GAMESWITCHER) {
				GameSwitcher_render(lastScreen, blackBG, ow, gsanimdir, tmpOldScreen);
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
						had_thumb = false;
						startLoadThumb(thumbpath, onThumbLoaded);
						int max_w = (int)(screen->w - (screen->w * CFG_getGameArtWidth()));
						if (exists(thumbpath)) {
							ox = (int)(max_w)-SCALE1(BUTTON_MARGIN * 5);
							had_thumb = true;
						} else
							ox = screen->w;
					}
				}

				// buttons
				if (show_setting && !GetHDMI())
					GFX_blitHardwareHints(screen, show_setting);
				else if (resume.can_resume)
					GFX_blitButtonGroup((char*[]){"X", "RESUME", NULL}, 0, screen, 0);
				else if (total > 0 &&
						 (Shortcuts_isInToolsFolder(top->path) ||
						  Shortcuts_isInConsoleDir(top->path)) &&
						 canPinEntry(entry)) {
					char* label = Shortcuts_exists(entry->path + strlen(SDCARD_PATH))
									  ? "UNPIN"
									  : "PIN";
					GFX_blitButtonGroup((char*[]){"Y", label, NULL}, 0, screen, 0);
				} else
					GFX_blitButtonGroup(
						(char*[]){BTN_SLEEP == BTN_POWER ? "POWER" : "MENU",
								  BTN_SLEEP == BTN_POWER || simple_mode ? "SLEEP"
																		: "INFO",
								  NULL},
						0, screen, 0);

				if (total == 0) {
					if (stack->count > 1) {
						GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 0, screen, 1);
					}
				} else if (confirm_shortcut_action == SHORTCUT_NONE) {
					if (stack->count > 1) {
						GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "OPEN", NULL}, 1,
											screen, 1);
					} else {
						GFX_blitButtonGroup((char*[]){"A", "OPEN", NULL}, 0, screen, 1);
					}
				}

				if (total > 0) {
					selected_row = top->selected - top->start;
					previousY = previous_row * PILL_SIZE;
					targetY = selected_row * PILL_SIZE;
					for (int i = top->start, j = 0; i < top->end; i++, j++) {
						Entry* entry = top->entries->items[i];
						char* entry_name = entry->name;
						char* entry_unique = entry->unique;
						int available_width =
							MAX(0, (had_thumb ? ox + SCALE1(BUTTON_MARGIN)
											  : screen->w - SCALE1(BUTTON_MARGIN)) -
									   SCALE1(PADDING * 2));
						bool row_is_selected = (j == selected_row);
						bool row_is_top = (i == top->start);
						bool row_has_moved = (previous_row != selected_row ||
											  previous_depth != stack->count);
						if (row_is_top && !(had_thumb))
							available_width -= ow;

						trimSortingMeta(&entry_name);
						if (entry_unique) // Only render if a unique name exists
							trimSortingMeta(&entry_unique);

						char display_name[256];
						int text_width = GFX_getTextWidth(
							font.large, entry_unique ? entry_unique : entry_name,
							display_name, available_width, SCALE1(BUTTON_PADDING * 2));
						int max_width = MIN(available_width, text_width);

						// This spaghetti is preventing white text on white pill when
						// volume/color temp is shown, dont ask me why. This all needs to
						// get tossed out and redone properly later.
						SDL_Color text_color = uintToColour(THEME_COLOR4_255);
						int notext = 0;
						if (!row_has_moved && row_is_selected) {
							text_color = uintToColour(THEME_COLOR5_255);
							notext = 1;
						}

						SDL_LockMutex(fontMutex);
						SDL_Surface* text =
							TTF_RenderUTF8_Blended(font.large, entry_name, text_color);
						SDL_Surface* text_unique = TTF_RenderUTF8_Blended(
							font.large, display_name, COLOR_DARK_TEXT);
						SDL_UnlockMutex(fontMutex);
						if (!text || !text_unique) {
							if (text)
								SDL_FreeSurface(text);
							if (text_unique)
								SDL_FreeSurface(text_unique);
							continue;
						}
						// TODO: Use actual font metrics to center, this only works in
						// simple cases
						const int text_offset_y = (SCALE1(PILL_SIZE) - text->h + 1) >> 1;
						if (row_is_selected) {
							is_scrolling =
								list_show_entry_names &&
								Pill_shouldScroll(display_name, max_width);
							Pill_resetScroll();
							bool should_animate = previous_depth == stack->count;
							Pill_update(entry_name, max_width,
										previousY, targetY, text_offset_y,
										should_animate, !notext);
						}
						SDL_Rect text_rect = {0, 0, max_width - SCALE1(BUTTON_PADDING * 2),
											  text->h};
						SDL_Rect dest_rect = {SCALE1(BUTTON_MARGIN + BUTTON_PADDING),
											  SCALE1(PADDING + (j * PILL_SIZE)) +
												  text_offset_y};

						if (list_show_entry_names) {
							SDL_BlitSurface(text_unique, &text_rect, screen, &dest_rect);
							SDL_BlitSurface(text, &text_rect, screen, &dest_rect);
						}
						SDL_FreeSurface(text_unique); // Free after use
						SDL_FreeSurface(text);		  // Free after use
					}
					if (lastScreen == SCREEN_GAMESWITCHER) {
						SDL_Surface* gsSur = GameSwitcher_getSurface();
						if (gsSur) {
							// update cpu surface here first
							GFX_clearLayers(LAYER_ALL);
							folderbgchanged = 1;

							GFX_flipHidden();
							GFX_animateSurface(gsSur, 0, 0, 0, 0 - screen->h, screen->w,
											   screen->h, CFG_getMenuTransitions() ? 100 : 20,
											   255, 255, LAYER_BACKGROUND);
							animationdirection = ANIM_NONE;
						}
					}
					if (lastScreen == SCREEN_OFF) {
						GFX_animateSurfaceOpacity(blackBG, 0, 0, screen->w, screen->h, 255,
												  0, CFG_getMenuTransitions() ? 200 : 20,
												  LAYER_THUMBNAIL);
					}

					previous_row = selected_row;
					previous_depth = stack->count;
				} else {
					// TODO: for some reason screen's dimensions end up being 0x0 in
					// GFX_blitMessage...
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
					GFX_clearLayers(LAYER_BACKGROUND);
					folderbgchanged = 1;
					GFX_clearLayers(LAYER_TRANSITION);
					GFX_flipHidden();
					SDL_Surface* tmpNewScreen = GFX_captureRendererToSurface();
					SDL_SetSurfaceBlendMode(tmpNewScreen, SDL_BLENDMODE_BLEND);
					GFX_clearLayers(LAYER_THUMBNAIL);
					if (animationdirection == SLIDE_LEFT)
						GFX_animateAndFadeSurface(tmpOldScreen, 0, 0, 0 - FIXED_WIDTH, 0,
												  FIXED_WIDTH, FIXED_HEIGHT, 200,
												  tmpNewScreen, 1, 0, FIXED_WIDTH,
												  FIXED_HEIGHT, 0, 255, LAYER_THUMBNAIL);
					if (animationdirection == SLIDE_RIGHT)
						GFX_animateAndFadeSurface(tmpOldScreen, 0, 0, 0 + FIXED_WIDTH, 0,
												  FIXED_WIDTH, FIXED_HEIGHT, 200,
												  tmpNewScreen, 1, 0, FIXED_WIDTH,
												  FIXED_HEIGHT, 0, 255, LAYER_THUMBNAIL);
					GFX_clearLayers(LAYER_THUMBNAIL);
					SDL_FreeSurface(tmpNewScreen);
				}
				// animation done
				animationdirection = ANIM_NONE;
			}

			if (lastScreen == SCREEN_QUICKMENU) {
				updateBackgroundLayer();
			} else if (lastScreen == SCREEN_GAMELIST) {
				updateBackgroundLayer();
				renderThumbnail(0);

				GFX_clearLayers(LAYER_TRANSITION);
				GFX_clearLayers(LAYER_SCROLLTEXT);

				if (confirm_shortcut_action == SHORTCUT_NONE)
					Pill_renderToLayer(list_show_entry_names);
			}
			if (!startgame) // dont flip if game gonna start
				GFX_flip(screen);

			if (tmpOldScreen)
				SDL_FreeSurface(tmpOldScreen);

			dirty = false;
		} else if (Pill_hasAnimationDraw() || folderbgchanged || thumbchanged ||
				   is_scrolling) {
			// honestly this whole thing is here only for the scrolling text, I set it
			// now to run this at 30fps which is enough for scrolling text, should
			// move this to seperate animation function eventually
			updateBackgroundLayer();
			renderThumbnail(1);
			if (confirm_shortcut_action == SHORTCUT_NONE) {
				Pill_renderAnimFrame(list_show_entry_names);
			}
			if (currentScreen != SCREEN_GAMESWITCHER &&
				currentScreen != SCREEN_QUICKMENU) {
				// Skip pill text rendering when confirmation dialog is shown
				if (confirm_shortcut_action != SHORTCUT_NONE) {
					GFX_clearLayers(LAYER_TRANSITION);
					GFX_clearLayers(LAYER_SCROLLTEXT);
				} else if (is_scrolling && Pill_isAnimDone() && Pill_getAnimQueueSize() < 1) {
					int ow = GFX_blitHardwareGroup(screen, show_setting);
					Entry* entry = top->entries->items[top->selected];
					trimSortingMeta(&entry->name);
					char* entry_text = entry->name;
					if (entry->unique) {
						trimSortingMeta(&entry->unique);
						entry_text = entry->unique;
					}

					int available_width =
						(had_thumb ? ox + SCALE1(BUTTON_MARGIN)
								   : screen->w - SCALE1(BUTTON_MARGIN)) -
						SCALE1(PADDING * 2);
					if (top->selected == top->start && !had_thumb)
						available_width -= ow;

					int text_offset_y =
						(SCALE1(PILL_SIZE) - TTF_FontHeight(font.large) + 1) >> 1;

					if (list_show_entry_names) {
						Pill_renderScrollText(entry_text, available_width,
											  text_offset_y, previous_row);
					}
				} else {
					Pill_renderFallback(list_show_entry_names);
				}
			} else {
				SDL_Delay(16);
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

	Menu_quit();
	PWR_quit();
	PAD_quit();

	// Cleanup pill animation thread first (uses frameMutex/flipCond from imgloader)
	Pill_quit();

	// Cleanup worker threads and their synchronization primitives
	cleanupImageLoaderPool();

	GFX_quit(); // Cleanup video subsystem first to stop GPU threads

	// Now safe to free surfaces after GPU threads are stopped
	GameSwitcher_quit();
	if (blackBG)
		SDL_FreeSurface(blackBG);
	if (folderbgbmp)
		SDL_FreeSurface(folderbgbmp);
	if (thumbbmp)
		SDL_FreeSurface(thumbbmp);

	QuitSettings();
}