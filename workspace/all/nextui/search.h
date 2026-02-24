#ifndef SEARCH_H
#define SEARCH_H

#include "sdl.h"
#include <stdbool.h>

typedef struct {
	bool dirty;
	bool startgame;
	bool folderbgchanged;
	int screen;
} SearchResult;

void Search_init(void);
void Search_quit(void);

// Opens keyboard, performs search, returns true if user entered a query
bool Search_open(void);

SearchResult Search_handleInput(unsigned long now);
void Search_render(SDL_Surface* screen, SDL_Surface* blackBG, int lastScreen);

#endif // SEARCH_H
