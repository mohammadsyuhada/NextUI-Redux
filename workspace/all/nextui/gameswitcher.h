#ifndef GAMESWITCHER_H
#define GAMESWITCHER_H

#include "sdl.h"

typedef struct {
	int dirty;
	int folderbgchanged;
	int startgame;
	int screen;
	int gsanimdir;
} GameSwitcherResult;

void GameSwitcher_init(void);
void GameSwitcher_quit(void);
int GameSwitcher_shouldStartInSwitcher(void);
void GameSwitcher_resetSelection(void);
int GameSwitcher_getSelected(void);
SDL_Surface* GameSwitcher_getSurface(void);
GameSwitcherResult GameSwitcher_handleInput(unsigned long now);
void GameSwitcher_render(int lastScreen, SDL_Surface* blackBG,
						 int ow, int gsanimdir, SDL_Surface* tmpOldScreen);

#endif // GAMESWITCHER_H
