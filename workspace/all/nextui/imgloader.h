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

// Background loading callback
typedef void (*BackgroundLoadedCallback)(SDL_Surface* surface);

// Screen surface (owned by nextui.c)
extern SDL_Surface* screen;

// Shared surfaces (owned by imgloader.c)
extern SDL_Surface* folderbgbmp;
extern SDL_Surface* thumbbmp;

// Synchronization primitives (owned by imgloader.c)
extern SDL_mutex* bgMutex;		   // protects: folderbgbmp, folderbgchanged
extern SDL_mutex* thumbMutex;	   // protects: thumbbmp, thumbchanged
extern SDL_mutex* frameMutex;	   // protects: frameReady; paired with flipCond
extern SDL_mutex* fontMutex;	   // protects: font rendering calls
extern SDL_cond* flipCond;		   // signalled when frameReady becomes true
extern SDL_mutex* bgqueueMutex;	   // alias to internal bgQueue.mutex (used by render loop)
extern SDL_mutex* thumbqueueMutex; // alias to internal thumbQueue.mutex (used by render loop)

// Shared state flags (see mutex comments above for which mutex protects each)
extern int folderbgchanged;
extern int thumbchanged;
extern bool frameReady;

// Atomic state accessors
void setNeedDraw(int v);
int getNeedDraw(void);

// Lifecycle
void initImageLoaderPool(void);
void cleanupImageLoaderPool(void);

// Background loading
void startLoadFolderBackground(const char* imagePath, BackgroundLoadedCallback callback);
void onBackgroundLoaded(SDL_Surface* surface);

// Thumbnail loading
void startLoadThumb(const char* thumbpath, BackgroundLoadedCallback callback);
void onThumbLoaded(SDL_Surface* surface);

#endif // IMGLOADER_H
