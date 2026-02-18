#include "gameswitcher.h"
#include "config.h"
#include "defines.h"
#include "imgloader.h"
#include "launcher.h"
#include "recents.h"
#include "utils.h"

#include <unistd.h>

static int switcher_selected = 0;
static SDL_Surface* switcherSur = NULL;

void GameSwitcher_init(void) {
	switcher_selected = 0;
	switcherSur = NULL;
}

void GameSwitcher_quit(void) {
	if (switcherSur) {
		SDL_FreeSurface(switcherSur);
		switcherSur = NULL;
	}
}

int GameSwitcher_shouldStartInSwitcher(void) {
	if (exists(GAME_SWITCHER_PERSIST_PATH)) {
		// consider this "consumed", dont bring up the switcher next time we
		// regularly exit a game
		unlink(GAME_SWITCHER_PERSIST_PATH);
		return 1;
	}
	return 0;
}

void GameSwitcher_resetSelection(void) {
	switcher_selected = 0;
}

int GameSwitcher_getSelected(void) {
	return switcher_selected;
}

SDL_Surface* GameSwitcher_getSurface(void) {
	return switcherSur;
}

GameSwitcherResult GameSwitcher_handleInput(unsigned long now) {
	GameSwitcherResult result = {0};
	result.screen = SCREEN_GAMESWITCHER;
	result.gsanimdir = ANIM_NONE;

	if (PAD_justPressed(BTN_B) || PAD_tappedSelect(now)) {
		result.screen = SCREEN_GAMELIST;
		switcher_selected = 0;
		result.dirty = 1;
		result.folderbgchanged = 1;
	} else if (Recents_count() > 0 && PAD_justReleased(BTN_A)) {
		// this will drop us back into game switcher after leaving the game
		putFile(GAME_SWITCHER_PERSIST_PATH, "unused");
		result.startgame = 1;
		Entry* selectedEntry =
			Recents_entryFromRecent(Recents_at(switcher_selected));
		resume.should_resume = resume.can_resume;
		Entry_open(selectedEntry);
		result.dirty = 1;
		Entry_free(selectedEntry);
	} else if (Recents_count() > 0 && PAD_justReleased(BTN_Y)) {
		Recents_removeAt(switcher_selected);
		if (switcher_selected >= Recents_count())
			switcher_selected = Recents_count() - 1;
		if (switcher_selected < 0)
			switcher_selected = 0;
		result.dirty = 1;
	} else if (PAD_justPressed(BTN_RIGHT)) {
		switcher_selected++;
		if (switcher_selected == Recents_count())
			switcher_selected = 0; // wrap
		result.dirty = 1;
		result.gsanimdir = SLIDE_LEFT;
	} else if (PAD_justPressed(BTN_LEFT)) {
		switcher_selected--;
		if (switcher_selected < 0)
			switcher_selected = Recents_count() - 1; // wrap
		result.dirty = 1;
		result.gsanimdir = SLIDE_RIGHT;
	}

	return result;
}

void GameSwitcher_render(int lastScreen, SDL_Surface* blackBG,
						 int ow, int gsanimdir, SDL_Surface* tmpOldScreen) {
	GFX_clearLayers(LAYER_ALL);

	// For all recents with resumable state (i.e. has savegame), show game
	// switcher carousel
	if (Recents_count() > 0) {
		Entry* selectedEntry =
			Recents_entryFromRecent(Recents_at(switcher_selected));
		readyResume(selectedEntry);
		// title pill
		{
			int max_width = screen->w - SCALE1(PADDING * 2) - ow;

			char display_name[256];
			int text_width =
				GFX_truncateText(font.large, selectedEntry->name, display_name,
								 max_width, SCALE1(BUTTON_PADDING * 2));
			max_width = MIN(max_width, text_width);

			SDL_Surface* text = NULL;
			SDL_Color textColor = uintToColour(THEME_COLOR6_255);
			SDL_LockMutex(fontMutex);
			text = TTF_RenderUTF8_Blended(font.large, display_name, textColor);
			SDL_UnlockMutex(fontMutex);
			if (text) {
				const int text_offset_y = (SCALE1(PILL_SIZE) - text->h + 1) >> 1;
				GFX_blitPillLight(ASSET_WHITE_PILL, screen,
								  &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING),
											  max_width, SCALE1(PILL_SIZE)});
				SDL_BlitSurface(
					text,
					&(SDL_Rect){0, 0, max_width - SCALE1(BUTTON_PADDING * 2),
								text->h},
					screen,
					&(SDL_Rect){
						SCALE1(PADDING + BUTTON_PADDING),
						SCALE1(PADDING) + text_offset_y,
					});
				SDL_FreeSurface(text);
			}
		}

		if (resume.can_resume)
			GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 0, screen, 0);
		else
			GFX_blitButtonGroup(
				(char*[]){BTN_SLEEP == BTN_POWER ? "POWER" : "MENU", "SLEEP",
						  NULL},
				0, screen, 0);

		GFX_blitButtonGroup((char*[]){"Y", "REMOVE", "A", "RESUME", NULL}, 1,
							screen, 1);

		if (resume.has_preview) {
			// lotta memory churn here

			SDL_Surface* bmp = IMG_Load(resume.preview_path);
			if (bmp) {
				SDL_Surface* raw_preview =
					SDL_ConvertSurfaceFormat(bmp, screen->format->format, 0);
				if (raw_preview) {
					SDL_FreeSurface(bmp);
					bmp = raw_preview;
				}
			}
			if (bmp) {
				int aw = screen->w;
				int ah = screen->h;
				int ax = 0;
				int ay = 0;

				float aspectRatio = (float)bmp->w / (float)bmp->h;
				float screenRatio = (float)screen->w / (float)screen->h;

				if (screenRatio > aspectRatio) {
					aw = (int)(screen->h * aspectRatio);
					ah = screen->h;
				} else {
					aw = screen->w;
					ah = (int)(screen->w / aspectRatio);
				}
				ax = (screen->w - aw) / 2;
				ay = (screen->h - ah) / 2;

				if (lastScreen == SCREEN_GAME) {
					// need to flip once so streaming_texture1 is updated
					GFX_flipHidden();
					GFX_animateSurfaceOpacity(
						bmp, 0, 0, screen->w, screen->h, 0, 255,
						CFG_getMenuTransitions() ? 150 : 20, LAYER_ALL);
				} else if (lastScreen == SCREEN_GAMELIST) {
					GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
									LAYER_BACKGROUND);
					GFX_drawOnLayer(bmp, ax, ay, aw, ah, 1.0f, 0, LAYER_BACKGROUND);
					GFX_flipHidden();
					SDL_Surface* tmpNewScreen = GFX_captureRendererToSurface();
					GFX_clearLayers(LAYER_ALL);
					folderbgchanged = 1;
					GFX_drawOnLayer(tmpOldScreen, 0, 0, screen->w, screen->h, 1.0f,
									0, LAYER_ALL);
					GFX_animateSurface(tmpNewScreen, 0, 0 - screen->h, 0, 0,
									   screen->w, screen->h,
									   CFG_getMenuTransitions() ? 100 : 20, 255,
									   255, LAYER_BACKGROUND);
					SDL_FreeSurface(tmpNewScreen);

				} else if (lastScreen == SCREEN_GAMESWITCHER) {
					GFX_flipHidden();
					GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
									LAYER_BACKGROUND);
					if (gsanimdir == SLIDE_LEFT)
						GFX_animateSurface(bmp, ax + screen->w, ay, ax, ay, aw, ah,
										   CFG_getMenuTransitions() ? 80 : 20, 0, 255,
										   LAYER_ALL);
					else if (gsanimdir == SLIDE_RIGHT)
						GFX_animateSurface(bmp, ax - screen->w, ay, ax, ay, aw, ah,
										   CFG_getMenuTransitions() ? 80 : 20, 0, 255,
										   LAYER_ALL);

					GFX_drawOnLayer(bmp, ax, ay, aw, ah, 1.0f, 0, LAYER_BACKGROUND);
				} else if (lastScreen == SCREEN_QUICKMENU) {
					GFX_flipHidden();
					GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
									LAYER_BACKGROUND);
					GFX_drawOnLayer(bmp, ax, ay, aw, ah, 1.0f, 0, LAYER_BACKGROUND);
				}
				SDL_FreeSurface(bmp); // Free after rendering
			}
		} else if (resume.has_boxart) {
			// Load and display boxart as fallback
			SDL_Surface* boxart = IMG_Load(resume.boxart_path);
			if (boxart) {
				SDL_Surface* converted =
					SDL_ConvertSurfaceFormat(boxart, screen->format->format, 0);
				if (converted) {
					SDL_FreeSurface(boxart);
					boxart = converted;
				}

				// Apply game art settings (sizing)
				int img_w = boxart->w;
				int img_h = boxart->h;
				double aspect_ratio = (double)img_h / img_w;
				int max_w = (int)(screen->w * CFG_getGameArtWidth());
				int max_h = (int)(screen->h * 0.6);
				int new_w = max_w;
				int new_h = (int)(new_w * aspect_ratio);

				if (new_h > max_h) {
					new_h = max_h;
					new_w = (int)(new_h / aspect_ratio);
				}

				// Apply rounded corners
				GFX_ApplyRoundedCorners_8888(
					boxart, &(SDL_Rect){0, 0, boxart->w, boxart->h},
					SCALE1((float)CFG_getThumbnailRadius() *
						   ((float)img_w / (float)new_w)));

				// Center the boxart on screen
				int ax = (screen->w - new_w) / 2;
				int ay = (screen->h - new_h) / 2;

				// Handle animations based on transition direction
				if (lastScreen == SCREEN_GAME) {
					GFX_flipHidden();
					GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
									LAYER_BACKGROUND);
					GFX_animateSurfaceOpacity(boxart, ax, ay, new_w, new_h, 0, 255,
											  CFG_getMenuTransitions() ? 150 : 20,
											  LAYER_ALL);
				} else if (lastScreen == SCREEN_GAMELIST) {
					GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
									LAYER_BACKGROUND);
					GFX_drawOnLayer(boxart, ax, ay, new_w, new_h, 1.0f, 0,
									LAYER_BACKGROUND);
					GFX_flipHidden();
					SDL_Surface* tmpNewScreen = GFX_captureRendererToSurface();
					GFX_clearLayers(LAYER_ALL);
					folderbgchanged = 1;
					GFX_drawOnLayer(tmpOldScreen, 0, 0, screen->w, screen->h, 1.0f,
									0, LAYER_ALL);
					GFX_animateSurface(tmpNewScreen, 0, 0 - screen->h, 0, 0,
									   screen->w, screen->h,
									   CFG_getMenuTransitions() ? 100 : 20, 255,
									   255, LAYER_BACKGROUND);
					SDL_FreeSurface(tmpNewScreen);
				} else if (lastScreen == SCREEN_GAMESWITCHER) {
					GFX_flipHidden();
					GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
									LAYER_BACKGROUND);
					if (gsanimdir == SLIDE_LEFT)
						GFX_animateSurface(boxart, ax + screen->w, ay, ax, ay, new_w,
										   new_h, CFG_getMenuTransitions() ? 80 : 20,
										   0, 255, LAYER_ALL);
					else if (gsanimdir == SLIDE_RIGHT)
						GFX_animateSurface(boxart, ax - screen->w, ay, ax, ay, new_w,
										   new_h, CFG_getMenuTransitions() ? 80 : 20,
										   0, 255, LAYER_ALL);
					GFX_drawOnLayer(boxart, ax, ay, new_w, new_h, 1.0f, 0,
									LAYER_BACKGROUND);
				} else if (lastScreen == SCREEN_QUICKMENU) {
					GFX_flipHidden();
					GFX_drawOnLayer(blackBG, 0, 0, screen->w, screen->h, 1.0f, 0,
									LAYER_BACKGROUND);
					GFX_drawOnLayer(boxart, ax, ay, new_w, new_h, 1.0f, 0,
									LAYER_BACKGROUND);
				}
				SDL_FreeSurface(boxart);
			}
		} else {
			// No savestate preview and no boxart - show "No Preview"
			SDL_Rect preview_rect = {0, 0, screen->w, screen->h};
			SDL_Surface* tmpsur = SDL_CreateRGBSurfaceWithFormat(
				0, screen->w, screen->h, screen->format->BitsPerPixel,
				screen->format->format);
			if (tmpsur) {
				SDL_FillRect(tmpsur, &preview_rect,
							 SDL_MapRGBA(screen->format, 0, 0, 0, 255));
				if (lastScreen == SCREEN_GAME) {
					GFX_animateSurfaceOpacity(
						tmpsur, 0, 0, screen->w, screen->h, 255, 0,
						CFG_getMenuTransitions() ? 150 : 20, LAYER_BACKGROUND);
				} else if (lastScreen == SCREEN_GAMELIST) {
					GFX_animateSurface(
						tmpsur, 0, 0 - screen->h, 0, 0, screen->w, screen->h,
						CFG_getMenuTransitions() ? 100 : 20, 255, 255, LAYER_ALL);
				} else if (lastScreen == SCREEN_GAMESWITCHER) {
					GFX_flipHidden();
					if (gsanimdir == SLIDE_LEFT)
						GFX_animateSurface(
							tmpsur, 0 + screen->w, 0, 0, 0, screen->w, screen->h,
							CFG_getMenuTransitions() ? 80 : 20, 0, 255, LAYER_ALL);
					else if (gsanimdir == SLIDE_RIGHT)
						GFX_animateSurface(
							tmpsur, 0 - screen->w, 0, 0, 0, screen->w, screen->h,
							CFG_getMenuTransitions() ? 80 : 20, 0, 255, LAYER_ALL);
				}
				SDL_FreeSurface(tmpsur);
			}
			GFX_blitMessage(font.large, "No Preview", screen, &preview_rect);
		}
		Entry_free(selectedEntry);
	} else {
		SDL_Rect preview_rect = {0, 0, screen->w, screen->h};
		SDL_FillRect(screen, &preview_rect, 0);
		GFX_blitMessage(font.large, "No Recents", screen, &preview_rect);
		GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
	}

	GFX_flipHidden();

	if (switcherSur)
		SDL_FreeSurface(switcherSur);
	switcherSur = GFX_captureRendererToSurface();
}
