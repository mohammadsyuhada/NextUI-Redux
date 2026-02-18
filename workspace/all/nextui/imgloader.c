#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "config.h"
#include "imgloader.h"

///////////////////////////////////////
// Internal task queue structures

typedef struct {
    char imagePath[MAX_PATH];
    BackgroundLoadedCallback callback;
    void* userData;
} LoadBackgroundTask;

typedef struct TaskNode {
    LoadBackgroundTask* task;
    struct TaskNode* next;
} TaskNode;
typedef struct AnimTaskNode {
	AnimTask* task;
    struct AnimTaskNode* next;
} AnimTaskNode;

///////////////////////////////////////
// Internal state

static TaskNode* taskBGQueueHead = NULL;
static TaskNode* taskBGQueueTail = NULL;
static TaskNode* taskThumbQueueHead = NULL;
static TaskNode* taskThumbQueueTail = NULL;
static AnimTaskNode* animTaskQueueHead = NULL;
static AnimTaskNode* animTtaskQueueTail = NULL;
SDL_mutex* bgqueueMutex = NULL;
SDL_mutex* thumbqueueMutex = NULL;
SDL_mutex* animqueueMutex = NULL;
static SDL_cond* bgqueueCond = NULL;
static SDL_cond* thumbqueueCond = NULL;
static SDL_cond* animqueueCond = NULL;

static SDL_Thread* bgLoadThread = NULL;
static SDL_Thread* thumbLoadThread = NULL;
static SDL_Thread* animWorkerThread = NULL;

static SDL_atomic_t workerThreadsShutdown; // Flag to signal threads to exit (atomic for thread safety)

static SDL_atomic_t animationDrawAtomic;
static SDL_atomic_t needDrawAtomic;

///////////////////////////////////////
// Shared state (non-static, externed in imgloader.h)

SDL_mutex* bgMutex = NULL;
SDL_mutex* thumbMutex = NULL;
SDL_mutex* animMutex = NULL;
SDL_mutex* frameMutex = NULL;
SDL_mutex* fontMutex = NULL;
SDL_cond* flipCond = NULL;

SDL_Surface* folderbgbmp = NULL;
SDL_Surface* thumbbmp = NULL;
SDL_Surface* globalpill = NULL;
SDL_Surface* globalText = NULL;

int folderbgchanged = 0;
int thumbchanged = 0;

SDL_Rect pillRect;
int pilltargetY = 0;
int pilltargetTextY = 0;
bool frameReady = true;
bool pillanimdone = false;

#define MAX_QUEUE_SIZE 1
int currentBGQueueSize = 0;
int currentThumbQueueSize = 0;
int currentAnimQueueSize = 0;

///////////////////////////////////////
// Atomic state accessors

void setAnimationDraw(int v) { SDL_AtomicSet(&animationDrawAtomic, v); }
int getAnimationDraw(void) { return SDL_AtomicGet(&animationDrawAtomic); }
void setNeedDraw(int v) { SDL_AtomicSet(&needDrawAtomic, v); }
int getNeedDraw(void) { return SDL_AtomicGet(&needDrawAtomic); }

///////////////////////////////////////

void updatePillTextSurface(const char* entry_name, int move_w, SDL_Color text_color) {
	int crop_w = move_w - SCALE1(BUTTON_PADDING * 2);
	if (crop_w <= 0) return;

	SDL_LockMutex(fontMutex);
	SDL_Surface* tmp = TTF_RenderUTF8_Blended(font.large, entry_name, text_color);
	SDL_UnlockMutex(fontMutex);
	if (!tmp) return;

	SDL_Surface* converted = SDL_ConvertSurfaceFormat(tmp, screen->format->format, 0);
	SDL_FreeSurface(tmp);
	if (!converted) return;

	SDL_Rect crop_rect = { 0, 0, crop_w, converted->h };
	SDL_Surface* cropped = SDL_CreateRGBSurfaceWithFormat(
		0, crop_rect.w, crop_rect.h, screen->format->BitsPerPixel, screen->format->format
	);
	if (cropped) {
		SDL_SetSurfaceBlendMode(converted, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(converted, &crop_rect, cropped, NULL);
	}
	SDL_FreeSurface(converted);
	if (!cropped) return;

	SDL_LockMutex(animMutex);
	if (globalText) SDL_FreeSurface(globalText);
	globalText = cropped;
	SDL_UnlockMutex(animMutex);
}

///////////////////////////////////////
// Queue management

void enqueueBGTask(LoadBackgroundTask* task) {
	SDL_LockMutex(bgqueueMutex);
    TaskNode* node = (TaskNode*)malloc(sizeof(TaskNode));
    node->task = task;
    node->next = NULL;

    // If queue is full, drop the oldest task (head)
    if (currentBGQueueSize >= MAX_QUEUE_SIZE) {
        TaskNode* oldNode = taskBGQueueHead;
        if (oldNode) {
            taskBGQueueHead = oldNode->next;
            if (!taskBGQueueHead) {
                taskBGQueueTail = NULL;
            }
            if (oldNode->task) {
                free(oldNode->task);  // Only if task was malloc'd
            }
            free(oldNode);
            currentBGQueueSize--;
        }
    }

    // Enqueue the new task
    if (taskBGQueueTail) {
        taskBGQueueTail->next = node;
        taskBGQueueTail = node;
    } else {
        taskBGQueueHead = taskBGQueueTail = node;
    }

    currentBGQueueSize++;
    SDL_CondSignal(bgqueueCond);
    SDL_UnlockMutex(bgqueueMutex);
}
void enqueueThumbTask(LoadBackgroundTask* task) {
	SDL_LockMutex(thumbqueueMutex);
    TaskNode* node = (TaskNode*)malloc(sizeof(TaskNode));
    node->task = task;
    node->next = NULL;

    // If queue is full, drop the oldest task (head)
    if (currentThumbQueueSize >= MAX_QUEUE_SIZE) {
        TaskNode* oldNode = taskThumbQueueHead;
        if (oldNode) {
            taskThumbQueueHead = oldNode->next;
            if (!taskThumbQueueHead) {
                taskThumbQueueTail = NULL;
            }
            if (oldNode->task) {
                free(oldNode->task);  // Only if task was malloc'd
            }
            free(oldNode);
            currentThumbQueueSize--;
        }
    }

    // Enqueue the new task
    if (taskThumbQueueTail) {
        taskThumbQueueTail->next = node;
        taskThumbQueueTail = node;
    } else {
        taskThumbQueueHead = taskThumbQueueTail = node;
    }

    currentThumbQueueSize++;
    SDL_CondSignal(thumbqueueCond);
    SDL_UnlockMutex(thumbqueueMutex);
}

///////////////////////////////////////
// Worker threads

int BGLoadWorker(void* unused) {
    while (!SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_LockMutex(bgqueueMutex);
        while (!taskBGQueueHead && !SDL_AtomicGet(&workerThreadsShutdown)) {
        	SDL_CondWait(bgqueueCond, bgqueueMutex);
        }
        if (SDL_AtomicGet(&workerThreadsShutdown)) {
            SDL_UnlockMutex(bgqueueMutex);
            break;
        }
        TaskNode* node = taskBGQueueHead;
        taskBGQueueHead = node->next;
        if (!taskBGQueueHead) taskBGQueueTail = NULL;
        SDL_UnlockMutex(bgqueueMutex);
		// give processor lil space in between queue items for other shit
		//SDL_Delay(100);
        LoadBackgroundTask* task = node->task;
        free(node);

        SDL_Surface* result = NULL;
        if (access(task->imagePath, F_OK) == 0) {
            SDL_Surface* image = IMG_Load(task->imagePath);
            if (image) {
                SDL_Surface* imageRGBA = SDL_ConvertSurfaceFormat(image, screen->format->format, 0);
                SDL_FreeSurface(image);
                result = imageRGBA;
            }
        }

        if (task->callback) {
			task->callback(result);
		}
        free(task);
		SDL_LockMutex(bgqueueMutex);
		if (!taskBGQueueHead) taskBGQueueTail = NULL;
		currentBGQueueSize--;  // <-- add this
		SDL_UnlockMutex(bgqueueMutex);
    }
    return 0;
}
int ThumbLoadWorker(void* unused) {
    while (!SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_LockMutex(thumbqueueMutex);
        while (!taskThumbQueueHead && !SDL_AtomicGet(&workerThreadsShutdown)) {
        	SDL_CondWait(thumbqueueCond, thumbqueueMutex);
        }
        if (SDL_AtomicGet(&workerThreadsShutdown)) {
            SDL_UnlockMutex(thumbqueueMutex);
            break;
        }
        TaskNode* node = taskThumbQueueHead;
        taskThumbQueueHead = node->next;
        if (!taskThumbQueueHead) taskThumbQueueTail = NULL;
        SDL_UnlockMutex(thumbqueueMutex);
		// give processor lil space in between queue items for other shit
		//SDL_Delay(100);
        LoadBackgroundTask* task = node->task;
        free(node);

        SDL_Surface* result = NULL;
        if (access(task->imagePath, F_OK) == 0) {
            SDL_Surface* image = IMG_Load(task->imagePath);
            if (image) {
                SDL_Surface* imageRGBA = SDL_ConvertSurfaceFormat(image, screen->format->format, 0);
                SDL_FreeSurface(image);
                result = imageRGBA;
            }
        }

        if (task->callback) {
			task->callback(result);
		}
        free(task);
		SDL_LockMutex(thumbqueueMutex);
		if (!taskThumbQueueHead) taskThumbQueueTail = NULL;
		currentThumbQueueSize--;  // <-- add this
		SDL_UnlockMutex(thumbqueueMutex);
    }
    return 0;
}

///////////////////////////////////////
// Public loading functions

void startLoadFolderBackground(const char* imagePath, BackgroundLoadedCallback callback, void* userData) {
    LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
    if (!task) return;

 	snprintf(task->imagePath, sizeof(task->imagePath), "%s", imagePath);
    task->callback = callback;
    task->userData = userData;
    enqueueBGTask(task);
}

void onBackgroundLoaded(SDL_Surface* surface) {
	SDL_LockMutex(bgMutex);
	folderbgchanged = 1;
	if (folderbgbmp) SDL_FreeSurface(folderbgbmp);
    if (!surface) {
		folderbgbmp = NULL;
		setNeedDraw(1);
		SDL_UnlockMutex(bgMutex);
		return;
	}
    folderbgbmp = surface;
	setNeedDraw(1);
	SDL_UnlockMutex(bgMutex);
}

void startLoadThumb(const char* thumbpath, BackgroundLoadedCallback callback, void* userData) {
    LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
    if (!task) return;

    snprintf(task->imagePath, sizeof(task->imagePath), "%s", thumbpath);
    task->callback = callback;
    task->userData = userData;
    enqueueThumbTask(task);
}
void onThumbLoaded(SDL_Surface* surface) {
	SDL_LockMutex(thumbMutex);
	thumbchanged = 1;
	if (thumbbmp) SDL_FreeSurface(thumbbmp);
    if (!surface) {
		thumbbmp = NULL;
		SDL_UnlockMutex(thumbMutex);
		return;
	}


    thumbbmp = surface;
	int img_w = thumbbmp->w;
	int img_h = thumbbmp->h;
	double aspect_ratio = (double)img_h / img_w;
	int max_w = (int)(screen->w * CFG_getGameArtWidth());
	int max_h = (int)(screen->h * 0.6);
	int new_w = max_w;
	int new_h = (int)(new_w * aspect_ratio);

	if (new_h > max_h) {
		new_h = max_h;
		new_w = (int)(new_h / aspect_ratio);
	}

	GFX_ApplyRoundedCorners_8888(
		thumbbmp,
		&(SDL_Rect){0, 0, thumbbmp->w, thumbbmp->h},
		SCALE1((float)CFG_getThumbnailRadius() * ((float)img_w / (float)new_w))
	);
	setNeedDraw(1);
	SDL_UnlockMutex(thumbMutex);
}

///////////////////////////////////////
// Animation

void animcallback(finishedTask *task) {
	SDL_LockMutex(animMutex);
	pillRect = task->dst;
	if(pillRect.w > 0 && pillRect.h > 0) {
		pilltargetY = +screen->w; // move offscreen
		if(task->done) {
			pilltargetY = task->targetY;
			pilltargetTextY = task->targetTextY;
		}
		setNeedDraw(1);
	}
	SDL_UnlockMutex(animMutex);
	setAnimationDraw(1);
}

int animWorker(void* unused) {
	  while (!SDL_AtomicGet(&workerThreadsShutdown)) {
 		SDL_LockMutex(animqueueMutex);
        while (!animTaskQueueHead && !SDL_AtomicGet(&workerThreadsShutdown)) {
            SDL_CondWait(animqueueCond, animqueueMutex);
        }
        if (SDL_AtomicGet(&workerThreadsShutdown)) {
            SDL_UnlockMutex(animqueueMutex);
            break;
        }
        AnimTaskNode* node = animTaskQueueHead;
        animTaskQueueHead = node->next;
        if (!animTaskQueueHead) animTtaskQueueTail = NULL;
		SDL_UnlockMutex(animqueueMutex);

        AnimTask* task = node->task;
		finishedTask* finaltask = (finishedTask*)malloc(sizeof(finishedTask));
		int total_frames = task->frames;
		// This somehow leads to the pill not rendering correctly when wrapping the list (last element to first, or reverse).
		// TODO: Figure out why this is here. Ideally we shouldnt refer to specific platforms in here, but the commit message doesnt
		// help all that much and comparing magic numbers also isnt that descriptive on its own.
		if(strcmp("Desktop", PLAT_getModel()) != 0) {
			if(task->targetY > task->startY + SCALE1(PILL_SIZE) || task->targetY < task->startY - SCALE1(PILL_SIZE)) {
				total_frames = 0;
			}
		}

		for (int frame = 0; frame <= total_frames; frame++) {
			// Check for shutdown at start of each frame
			if (SDL_AtomicGet(&workerThreadsShutdown)) break;

			float t = (float)frame / total_frames;
			if (t > 1.0f) t = 1.0f;

			int current_x = task->startX + (int)((task->targetX - task->startX) * t);
			int current_y = task->startY + (int)(( task->targetY -  task->startY) * t);

			SDL_Rect moveDst = { current_x, current_y, task->move_w, task->move_h };
			finaltask->dst = moveDst;
			finaltask->entry_name = task->entry_name;
			finaltask->move_w = task->move_w;
			finaltask->move_h = task->move_h;
			finaltask->targetY = task->targetY;
			finaltask->targetTextY = task->targetTextY;
			finaltask->move_y = SCALE1(PADDING + task->targetY) + (task->targetTextY - task->targetY);
			finaltask->done = 0;
			if(frame >= total_frames) finaltask->done=1;
			task->callback(finaltask);
			SDL_LockMutex(frameMutex);
			while (!frameReady && !SDL_AtomicGet(&workerThreadsShutdown)) {
				SDL_CondWait(flipCond, frameMutex);
			}
			frameReady = false;
			SDL_UnlockMutex(frameMutex);

		}
		SDL_LockMutex(animqueueMutex);
		if (!animTaskQueueHead) animTtaskQueueTail = NULL;
		currentAnimQueueSize--;  // <-- add this
		SDL_UnlockMutex(animqueueMutex);

		SDL_LockMutex(animMutex);
		pillanimdone = true;
		free(finaltask);
		SDL_UnlockMutex(animMutex);
	}
	return 0;
}

void enqueueanmimtask(AnimTask* task) {
    AnimTaskNode* node = (AnimTaskNode*)malloc(sizeof(AnimTaskNode));
    node->task = task;
    node->next = NULL;

    SDL_LockMutex(animqueueMutex);
	pillanimdone = false;
    // If queue is full, drop the oldest task (head)
    if (currentAnimQueueSize >= 1) {
        AnimTaskNode* oldNode = animTaskQueueHead;
        if (oldNode) {
            animTaskQueueHead = oldNode->next;
            if (!animTaskQueueHead) {
                animTtaskQueueTail = NULL;
            }
            if (oldNode->task) {
                free(oldNode->task);  // Only if task was malloc'd
            }
            free(oldNode);
            currentAnimQueueSize--;
        }
    }

    // Enqueue the new task
    if (animTtaskQueueTail) {
        animTtaskQueueTail->next = node;
        animTtaskQueueTail = node;
    } else {
        animTaskQueueHead = animTtaskQueueTail = node;
    }

    currentAnimQueueSize++;
    SDL_CondSignal(animqueueCond);
    SDL_UnlockMutex(animqueueMutex);
}

void animPill(AnimTask *task) {
	task->callback = animcallback;
	enqueueanmimtask(task);
}

///////////////////////////////////////
// Lifecycle

void initImageLoaderPool(void) {
	// Initialize shutdown flag to 0
	SDL_AtomicSet(&workerThreadsShutdown, 0);
	SDL_AtomicSet(&animationDrawAtomic, 1);
	SDL_AtomicSet(&needDrawAtomic, 0);

    thumbqueueMutex = SDL_CreateMutex();
    bgqueueMutex = SDL_CreateMutex();
    bgqueueCond = SDL_CreateCond();
    thumbqueueCond = SDL_CreateCond();
	bgMutex = SDL_CreateMutex();
	thumbMutex = SDL_CreateMutex();
	animMutex = SDL_CreateMutex();
	animqueueMutex = SDL_CreateMutex();
	animqueueCond = SDL_CreateCond();
	frameMutex = SDL_CreateMutex();
	fontMutex = SDL_CreateMutex();
	flipCond = SDL_CreateCond();

    bgLoadThread = SDL_CreateThread(BGLoadWorker, "BGLoadWorker", NULL);
    thumbLoadThread = SDL_CreateThread(ThumbLoadWorker, "ThumbLoadWorker", NULL);
	animWorkerThread = SDL_CreateThread(animWorker, "animWorker", NULL);
}

void cleanupImageLoaderPool(void) {
	// Signal all worker threads to exit (atomic set for thread safety)
	SDL_AtomicSet(&workerThreadsShutdown, 1);

	// Wake up all waiting threads
	if (bgqueueCond) SDL_CondSignal(bgqueueCond);
	if (thumbqueueCond) SDL_CondSignal(thumbqueueCond);
	if (animqueueCond) SDL_CondSignal(animqueueCond);
	if (flipCond) SDL_CondSignal(flipCond);  // Wake up animWorker if stuck waiting for frame flip

	// Wait for all worker threads to finish
	if (bgLoadThread) {
		SDL_WaitThread(bgLoadThread, NULL);
		bgLoadThread = NULL;
	}
	if (thumbLoadThread) {
		SDL_WaitThread(thumbLoadThread, NULL);
		thumbLoadThread = NULL;
	}
	if (animWorkerThread) {
		SDL_WaitThread(animWorkerThread, NULL);
		animWorkerThread = NULL;
	}

	// Small delay to ensure llvmpipe/OpenGL threads have completed any pending operations
	SDL_Delay(10);

	// Acquire and release each mutex before destroying to ensure no thread is in a critical section
	// This creates a memory barrier and ensures proper synchronization
	if (bgqueueMutex) { SDL_LockMutex(bgqueueMutex); SDL_UnlockMutex(bgqueueMutex); }
	if (thumbqueueMutex) { SDL_LockMutex(thumbqueueMutex); SDL_UnlockMutex(thumbqueueMutex); }
	if (animqueueMutex) { SDL_LockMutex(animqueueMutex); SDL_UnlockMutex(animqueueMutex); }
	if (bgMutex) { SDL_LockMutex(bgMutex); SDL_UnlockMutex(bgMutex); }
	if (thumbMutex) { SDL_LockMutex(thumbMutex); SDL_UnlockMutex(thumbMutex); }
	if (animMutex) { SDL_LockMutex(animMutex); SDL_UnlockMutex(animMutex); }
	if (frameMutex) { SDL_LockMutex(frameMutex); SDL_UnlockMutex(frameMutex); }
	if (fontMutex) { SDL_LockMutex(fontMutex); SDL_UnlockMutex(fontMutex); }

	// Destroy mutexes and condition variables
	if (bgqueueMutex) SDL_DestroyMutex(bgqueueMutex);
	if (thumbqueueMutex) SDL_DestroyMutex(thumbqueueMutex);
	if (animqueueMutex) SDL_DestroyMutex(animqueueMutex);
	if (bgMutex) SDL_DestroyMutex(bgMutex);
	if (thumbMutex) SDL_DestroyMutex(thumbMutex);
	if (animMutex) SDL_DestroyMutex(animMutex);
	if (frameMutex) SDL_DestroyMutex(frameMutex);
	if (fontMutex) SDL_DestroyMutex(fontMutex);

	if (bgqueueCond) SDL_DestroyCond(bgqueueCond);
	if (thumbqueueCond) SDL_DestroyCond(thumbqueueCond);
	if (animqueueCond) SDL_DestroyCond(animqueueCond);
	if (flipCond) SDL_DestroyCond(flipCond);

	// Set pointers to NULL after destruction
	bgqueueMutex = NULL;
	thumbqueueMutex = NULL;
	animqueueMutex = NULL;
	bgMutex = NULL;
	thumbMutex = NULL;
	animMutex = NULL;
	frameMutex = NULL;
	fontMutex = NULL;
	bgqueueCond = NULL;
	thumbqueueCond = NULL;
	animqueueCond = NULL;
	flipCond = NULL;
}
