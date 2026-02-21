#ifndef QUICKMENU_H
#define QUICKMENU_H

#include "sdl.h"
#include "api.h"
#include <stdbool.h>

typedef struct {
	bool dirty;
	bool folderbgchanged;
	int screen;
} QuickMenuResult;

void QuickMenu_init(int simple_mode);
void QuickMenu_quit(void);
void QuickMenu_resetSelection(void);
QuickMenuResult QuickMenu_handleInput(unsigned long now);
void QuickMenu_render(int lastScreen, IndicatorType show_setting, int ow,
					  char* folderBgPath, size_t folderBgPathSize);

#endif // QUICKMENU_H
