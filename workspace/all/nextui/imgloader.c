#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
} LoadBackgroundTask;

typedef struct TaskNode {
	LoadBackgroundTask* task;
	struct TaskNode* next;
} TaskNode;

///////////////////////////////////////
// Generic task queue

typedef struct TaskQueue {
	TaskNode* head;
	TaskNode* tail;
	int size;
	SDL_mutex* mutex;
	SDL_cond* cond;
} TaskQueue;

///////////////////////////////////////
// Internal state

static TaskQueue bgQueue = {0};
static TaskQueue thumbQueue = {0};
SDL_mutex* bgqueueMutex = NULL;
SDL_mutex* thumbqueueMutex = NULL;

static SDL_Thread* bgLoadThread = NULL;
static SDL_Thread* thumbLoadThread = NULL;

static SDL_atomic_t workerThreadsShutdown; // Flag to signal threads to exit (atomic for thread safety)

static SDL_atomic_t needDrawAtomic;

// Cached screen properties (set once in initImageLoaderPool, safe to read from worker threads)
static Uint32 cachedScreenFormat = 0;
static int cachedScreenBitsPerPixel = 0;
static int cachedScreenW = 0;
static int cachedScreenH = 0;

///////////////////////////////////////
// Shared state (non-static, externed in imgloader.h)

SDL_mutex* bgMutex = NULL;
SDL_mutex* thumbMutex = NULL;
SDL_mutex* frameMutex = NULL;
SDL_mutex* fontMutex = NULL;
SDL_cond* flipCond = NULL;

SDL_Surface* folderbgbmp = NULL;
SDL_Surface* thumbbmp = NULL;

int folderbgchanged = 0;
int thumbchanged = 0;

bool frameReady = true;

///////////////////////////////////////
// Atomic state accessors

void setNeedDraw(int v) {
	SDL_AtomicSet(&needDrawAtomic, v);
}
int getNeedDraw(void) {
	return SDL_AtomicGet(&needDrawAtomic);
}

///////////////////////////////////////
// Queue management

#define MAX_QUEUE_SIZE 1

void enqueueTask(TaskQueue* q, LoadBackgroundTask* task) {
	SDL_LockMutex(q->mutex);
	TaskNode* node = (TaskNode*)malloc(sizeof(TaskNode));
	if (!node) {
		free(task);
		SDL_UnlockMutex(q->mutex);
		return;
	}
	node->task = task;
	node->next = NULL;

	// If queue is full, drop the oldest task (head)
	if (q->size >= MAX_QUEUE_SIZE) {
		TaskNode* oldNode = q->head;
		if (oldNode) {
			q->head = oldNode->next;
			if (!q->head) {
				q->tail = NULL;
			}
			if (oldNode->task) {
				free(oldNode->task);
			}
			free(oldNode);
			q->size--;
		}
	}

	// Enqueue the new task
	if (q->tail) {
		q->tail->next = node;
		q->tail = node;
	} else {
		q->head = q->tail = node;
	}

	q->size++;
	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);
}

///////////////////////////////////////
// Worker thread (shared by BG and Thumb loaders)

int loadWorker(void* arg) {
	TaskQueue* q = (TaskQueue*)arg;
	while (!SDL_AtomicGet(&workerThreadsShutdown)) {
		SDL_LockMutex(q->mutex);
		while (!q->head && !SDL_AtomicGet(&workerThreadsShutdown)) {
			SDL_CondWait(q->cond, q->mutex);
		}
		if (SDL_AtomicGet(&workerThreadsShutdown)) {
			SDL_UnlockMutex(q->mutex);
			break;
		}
		TaskNode* node = q->head;
		q->head = node->next;
		if (!q->head)
			q->tail = NULL;
		q->size--;
		SDL_UnlockMutex(q->mutex);

		LoadBackgroundTask* task = node->task;
		free(node);

		SDL_Surface* result = NULL;
		SDL_Surface* image = IMG_Load(task->imagePath);
		if (image) {
			SDL_Surface* imageRGBA = SDL_ConvertSurfaceFormat(image, cachedScreenFormat, 0);
			SDL_FreeSurface(image);
			result = imageRGBA;
		}

		if (task->callback) {
			task->callback(result);
		}
		free(task);
	}
	return 0;
}

///////////////////////////////////////
// Public loading functions

void startLoadFolderBackground(const char* imagePath, BackgroundLoadedCallback callback) {
	LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
	if (!task)
		return;

	snprintf(task->imagePath, sizeof(task->imagePath), "%s", imagePath);
	task->callback = callback;
	enqueueTask(&bgQueue, task);
}

void onBackgroundLoaded(SDL_Surface* surface) {
	SDL_LockMutex(bgMutex);
	folderbgchanged = 1;
	if (folderbgbmp)
		SDL_FreeSurface(folderbgbmp);
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

void startLoadThumb(const char* thumbpath, BackgroundLoadedCallback callback) {
	LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
	if (!task)
		return;

	snprintf(task->imagePath, sizeof(task->imagePath), "%s", thumbpath);
	task->callback = callback;
	enqueueTask(&thumbQueue, task);
}
void onThumbLoaded(SDL_Surface* surface) {
	SDL_LockMutex(thumbMutex);
	thumbchanged = 1;
	if (thumbbmp)
		SDL_FreeSurface(thumbbmp);
	if (!surface) {
		thumbbmp = NULL;
		SDL_UnlockMutex(thumbMutex);
		return;
	}

	thumbbmp = surface;
	int img_w = surface->w;
	int img_h = surface->h;
	double aspect_ratio = (double)img_h / img_w;
	int max_w = (int)(cachedScreenW * CFG_getGameArtWidth());
	int max_h = (int)(cachedScreenH * 0.6);
	int new_w = max_w;
	int new_h = (int)(new_w * aspect_ratio);

	if (new_h > max_h) {
		new_h = max_h;
		new_w = (int)(new_h / aspect_ratio);
	}

	GFX_ApplyRoundedCorners_8888(
		surface,
		&(SDL_Rect){0, 0, surface->w, surface->h},
		SCALE1((float)CFG_getThumbnailRadius() * ((float)img_w / (float)new_w)));
	setNeedDraw(1);
	SDL_UnlockMutex(thumbMutex);
}

///////////////////////////////////////
// Lifecycle

void initImageLoaderPool(void) {
	// Initialize shutdown flag to 0
	SDL_AtomicSet(&workerThreadsShutdown, 0);
	SDL_AtomicSet(&needDrawAtomic, 0);

	// Cache screen properties for thread-safe access from workers
	cachedScreenFormat = screen->format->format;
	cachedScreenBitsPerPixel = screen->format->BitsPerPixel;
	cachedScreenW = screen->w;
	cachedScreenH = screen->h;

	bgQueue.mutex = SDL_CreateMutex();
	bgQueue.cond = SDL_CreateCond();
	thumbQueue.mutex = SDL_CreateMutex();
	thumbQueue.cond = SDL_CreateCond();
	bgqueueMutex = bgQueue.mutex;
	thumbqueueMutex = thumbQueue.mutex;
	bgMutex = SDL_CreateMutex();
	thumbMutex = SDL_CreateMutex();
	frameMutex = SDL_CreateMutex();
	fontMutex = SDL_CreateMutex();
	flipCond = SDL_CreateCond();

	if (!bgQueue.mutex || !bgQueue.cond || !thumbQueue.mutex || !thumbQueue.cond ||
		!bgMutex || !thumbMutex ||
		!frameMutex || !fontMutex || !flipCond) {
		fprintf(stderr, "imgloader: failed to create SDL sync primitives\n");
		return;
	}

	bgLoadThread = SDL_CreateThread(loadWorker, "BGLoadWorker", &bgQueue);
	thumbLoadThread = SDL_CreateThread(loadWorker, "ThumbLoadWorker", &thumbQueue);
	if (!bgLoadThread || !thumbLoadThread) {
		fprintf(stderr, "imgloader: failed to create worker threads\n");
	}
}

void cleanupImageLoaderPool(void) {
	// Signal all worker threads to exit (atomic set for thread safety)
	SDL_AtomicSet(&workerThreadsShutdown, 1);

	// Wake up all waiting threads (must hold corresponding mutex to avoid lost-wakeup race)
	if (bgQueue.mutex && bgQueue.cond) {
		SDL_LockMutex(bgQueue.mutex);
		SDL_CondSignal(bgQueue.cond);
		SDL_UnlockMutex(bgQueue.mutex);
	}
	if (thumbQueue.mutex && thumbQueue.cond) {
		SDL_LockMutex(thumbQueue.mutex);
		SDL_CondSignal(thumbQueue.cond);
		SDL_UnlockMutex(thumbQueue.mutex);
	}

	// Wait for all worker threads to finish
	if (bgLoadThread) {
		SDL_WaitThread(bgLoadThread, NULL);
		bgLoadThread = NULL;
	}
	if (thumbLoadThread) {
		SDL_WaitThread(thumbLoadThread, NULL);
		thumbLoadThread = NULL;
	}

	// Small delay to ensure llvmpipe/OpenGL threads have completed any pending operations
	SDL_Delay(10);

	// Drain any residual tasks left in queues
	while (bgQueue.head) {
		TaskNode* n = bgQueue.head;
		bgQueue.head = n->next;
		free(n->task);
		free(n);
	}
	bgQueue.tail = NULL;
	bgQueue.size = 0;

	while (thumbQueue.head) {
		TaskNode* n = thumbQueue.head;
		thumbQueue.head = n->next;
		free(n->task);
		free(n);
	}
	thumbQueue.tail = NULL;
	thumbQueue.size = 0;

	// Acquire and release each mutex before destroying to ensure no thread is in a critical section
	// This creates a memory barrier and ensures proper synchronization
	if (bgQueue.mutex) {
		SDL_LockMutex(bgQueue.mutex);
		SDL_UnlockMutex(bgQueue.mutex);
	}
	if (thumbQueue.mutex) {
		SDL_LockMutex(thumbQueue.mutex);
		SDL_UnlockMutex(thumbQueue.mutex);
	}
	if (bgMutex) {
		SDL_LockMutex(bgMutex);
		SDL_UnlockMutex(bgMutex);
	}
	if (thumbMutex) {
		SDL_LockMutex(thumbMutex);
		SDL_UnlockMutex(thumbMutex);
	}
	if (frameMutex) {
		SDL_LockMutex(frameMutex);
		SDL_UnlockMutex(frameMutex);
	}
	if (fontMutex) {
		SDL_LockMutex(fontMutex);
		SDL_UnlockMutex(fontMutex);
	}

	// Destroy mutexes and condition variables
	if (bgQueue.mutex)
		SDL_DestroyMutex(bgQueue.mutex);
	if (thumbQueue.mutex)
		SDL_DestroyMutex(thumbQueue.mutex);
	if (bgMutex)
		SDL_DestroyMutex(bgMutex);
	if (thumbMutex)
		SDL_DestroyMutex(thumbMutex);
	if (frameMutex)
		SDL_DestroyMutex(frameMutex);
	if (fontMutex)
		SDL_DestroyMutex(fontMutex);

	if (bgQueue.cond)
		SDL_DestroyCond(bgQueue.cond);
	if (thumbQueue.cond)
		SDL_DestroyCond(thumbQueue.cond);
	if (flipCond)
		SDL_DestroyCond(flipCond);

	// Set pointers to NULL after destruction
	bgQueue = (TaskQueue){0};
	thumbQueue = (TaskQueue){0};
	bgqueueMutex = NULL;
	thumbqueueMutex = NULL;
	bgMutex = NULL;
	thumbMutex = NULL;
	frameMutex = NULL;
	fontMutex = NULL;
	flipCond = NULL;
}
