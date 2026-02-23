#ifndef UI_LISTDIALOG_H
#define UI_LISTDIALOG_H

#include "sdl.h" // IWYU pragma: keep
#include <stdbool.h>

#define LISTDIALOG_MAX_ITEMS 128
#define LISTDIALOG_MAX_TEXT 128
#define LISTDIALOG_MAX_ICONS 4

typedef struct {
	char text[LISTDIALOG_MAX_TEXT];			 // title text
	char detail[LISTDIALOG_MAX_TEXT];		 // right-side text (when no append icons)
	int prepend_icons[LISTDIALOG_MAX_ICONS]; // icons before title, -1 terminated
	int append_icons[LISTDIALOG_MAX_ICONS];	 // icons after title (right-aligned), -1 terminated
} ListDialogItem;

typedef enum {
	LISTDIALOG_NONE,
	LISTDIALOG_SELECTED,
	LISTDIALOG_CANCEL,
} ListDialogAction;

typedef struct {
	ListDialogAction action;
	int index;
} ListDialogResult;

void ListDialog_init(const char* title);
void ListDialog_setItems(ListDialogItem* items, int count);
void ListDialog_setStatus(const char* status);
ListDialogResult ListDialog_handleInput(void);
void ListDialog_render(SDL_Surface* screen);
void ListDialog_quit(void);

#endif // UI_LISTDIALOG_H
