#ifndef IMGLOADER_H
#define IMGLOADER_H

#include "api.h"
#include <stdbool.h>

// Animation direction enum
enum {
	ANIM_NONE = 0,
	SLIDE_LEFT = 1,
	SLIDE_RIGHT = 2,
};

// Task structures (needed by main loop to create AnimTask)
typedef void (*BackgroundLoadedCallback)(SDL_Surface* surface);

typedef struct finishedTask {
	int startX;
	int targetX;
	int startY;
	int targetY;
	int targetTextY;
	int move_y;
	int move_w;
	int move_h;
	int frames;
	int done;
	void* userData;
	char *entry_name;
	SDL_Rect dst;
} finishedTask;

typedef void (*AnimTaskCallback)(finishedTask *task);
typedef struct AnimTask {
	int startX;
	int targetX;
	int startY;
	int targetY;
	int targetTextY;
	int move_w;
	int move_h;
	int frames;
	AnimTaskCallback callback;
	void* userData;
	char *entry_name;
	SDL_Rect dst;
} AnimTask;

// Screen surface (owned by nextui.c)
extern SDL_Surface* screen;

// Shared surfaces (owned by imgloader.c)
extern SDL_Surface* folderbgbmp;
extern SDL_Surface* thumbbmp;
extern SDL_Surface* globalpill;
extern SDL_Surface* globalText;

// Synchronization primitives (owned by imgloader.c)
extern SDL_mutex* bgMutex;
extern SDL_mutex* thumbMutex;
extern SDL_mutex* animMutex;
extern SDL_mutex* frameMutex;
extern SDL_mutex* fontMutex;
extern SDL_cond* flipCond;
extern SDL_mutex* bgqueueMutex;
extern SDL_mutex* thumbqueueMutex;
extern SDL_mutex* animqueueMutex;

// Shared state flags
extern int folderbgchanged;
extern int thumbchanged;
extern SDL_Rect pillRect;
extern int pilltargetY;
extern int pilltargetTextY;
extern bool frameReady;
extern bool pillanimdone;
extern int currentAnimQueueSize;

// Atomic state accessors
void setAnimationDraw(int v);
int getAnimationDraw(void);
void setNeedDraw(int v);
int getNeedDraw(void);

// Lifecycle
void initImageLoaderPool(void);
void cleanupImageLoaderPool(void);

// Background loading
void startLoadFolderBackground(const char* imagePath, BackgroundLoadedCallback callback, void* userData);
void onBackgroundLoaded(SDL_Surface* surface);

// Thumbnail loading
void startLoadThumb(const char* thumbpath, BackgroundLoadedCallback callback, void* userData);
void onThumbLoaded(SDL_Surface* surface);

// Pill animation
void updatePillTextSurface(const char* entry_name, int move_w, SDL_Color text_color);
void animPill(AnimTask *task);

#endif // IMGLOADER_H
