#ifndef CONTENT_H
#define CONTENT_H

#include "types.h"
#include "recents.h"

// Set simple_mode for content functions
void Content_setSimpleMode(bool mode);

// Directory construction
Directory* Directory_new(char* path, int selected);
void Directory_index(Directory* self);

// Content query helpers
int getIndexChar(char* str);
void getUniqueName(Entry* entry, char* out_name);
int hasEmu(char* emu_name);
int hasCue(char* dir_path, char* cue_path);
int hasM3u(char* rom_path, char* m3u_path);
int hasCollections(void);
int hasRoms(char* dir_name);
int hasTools(void);
int canPinEntry(Entry* entry);
int isConsoleDir(char* path);

// Content retrieval
Entry* entryFromPakName(char* pak_name);
Array* getRoms(void);
Array* getCollections(void);
Array* getRoot(int simple_mode);
Array* getCollection(char* path);
Array* getDiscs(char* path);
int getFirstDisc(char* m3u_path, char* disc_path);
Array* getEntries(char* path);
void addEntries(Array* entries, char* path);

// Quick menu content
Array* getQuickEntries(int simple_mode);
Array* getQuickToggles(int simple_mode);

#endif // CONTENT_H
