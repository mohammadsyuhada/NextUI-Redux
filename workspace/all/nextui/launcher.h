#ifndef LAUNCHER_H
#define LAUNCHER_H

#include "types.h"
#include "content.h"

// Globals shared between launcher and main loop
// Owned by nextui.c, accessed by launcher.c
extern Directory* top;
extern Array* stack;
extern int quit;
extern int can_resume;
extern int should_resume;
extern int has_preview;
extern int has_boxart;
extern int startgame;
extern char slot_path[256];
extern char preview_path[256];
extern char boxart_path[256];
extern int restore_depth;
extern int restore_relative;
extern int restore_selected;
extern int restore_start;
extern int restore_end;

// Set cleanup function (called by toggleQuick for Reboot/Poweroff)
typedef void (*CleanupPoolFunc)(void);
void Launcher_setCleanupFunc(CleanupPoolFunc func);

// String utilities
int replaceString(char *line, const char *search, const char *replace);
char* escapeSingleQuotes(char* str);

// Navigation
void queueNext(char* cmd);
void openDirectory(char* path, int auto_launch);
void closeDirectory(void);
Array* pathToStack(const char* path);

// Resume
void readyResumePath(char* rom_path, int type);
void readyResume(Entry* entry);
int autoResume(void);

// Game launching
void openPak(char* path);
void openRom(char* path, char* last);
void Entry_open(Entry* self);
void toggleQuick(Entry* self);

// State persistence
void saveLast(char* path);
void loadLast(void);

#endif // LAUNCHER_H
