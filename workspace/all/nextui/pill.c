#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "defines.h"
#include "api.h"
#include "config.h"
#include "imgloader.h"
#include "pill.h"

///////////////////////////////////////
// Types

typedef struct PillFinishedTask {
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
	char* entry_name;
	SDL_Rect dst;
} PillFinishedTask;

typedef void (*PillAnimCallback)(PillFinishedTask* task);

typedef struct PillAnimTask {
	int startX;
	int targetX;
	int startY;
	int targetY;
	int targetTextY;
	int move_w;
	int move_h;
	int frames;
	PillAnimCallback callback;
	char* entry_name;
	SDL_Rect dst;
} PillAnimTask;

typedef struct PillAnimTaskNode {
	PillAnimTask* task;
	struct PillAnimTaskNode* next;
} PillAnimTaskNode;

///////////////////////////////////////
// Internal state

static SDL_Surface* pillSurface = NULL;
static SDL_Surface* pillText = NULL;
static int pillW = 0;

static SDL_Rect pillRect;
static int pilltargetY = 0;
static int pilltargetTextY = 0;
static bool pillanimdone = false;

static int currentAnimQueueSize = 0;

static PillAnimTaskNode* animTaskQueueHead = NULL;
static PillAnimTaskNode* animTaskQueueTail = NULL;

static SDL_mutex* pillAnimMutex = NULL;	 // protects: pillRect, pilltargetY, pilltargetTextY, pillanimdone, pillSurface, pillText
static SDL_mutex* pillQueueMutex = NULL; // protects: anim task queue, currentAnimQueueSize
static SDL_cond* pillQueueCond = NULL;

static SDL_Thread* pillWorkerThread = NULL;
static SDL_atomic_t pillShutdown;

static SDL_atomic_t pillAnimationDrawAtomic;

// Cached screen properties (set once in Pill_init)
static Uint32 cachedFormat = 0;
static int cachedBitsPerPixel = 0;
static int cachedScreenW = 0;
static int cachedScreenH = 0;

///////////////////////////////////////
// Atomic accessors

static void setAnimationDraw(int v) {
	SDL_AtomicSet(&pillAnimationDrawAtomic, v);
}

static int getAnimationDraw(void) {
	return SDL_AtomicGet(&pillAnimationDrawAtomic);
}

///////////////////////////////////////
// Internal: update pill text surface

static void updatePillTextSurface(const char* entry_name, int move_w, SDL_Color text_color) {
	int crop_w = move_w - SCALE1(BUTTON_PADDING * 2);
	if (crop_w <= 0)
		return;

	SDL_LockMutex(fontMutex);
	SDL_Surface* tmp = TTF_RenderUTF8_Blended(font.large, entry_name, text_color);
	SDL_UnlockMutex(fontMutex);
	if (!tmp)
		return;

	SDL_Surface* converted = SDL_ConvertSurfaceFormat(tmp, cachedFormat, 0);
	SDL_FreeSurface(tmp);
	if (!converted)
		return;

	SDL_Rect crop_rect = {0, 0, crop_w, converted->h};
	SDL_Surface* cropped = SDL_CreateRGBSurfaceWithFormat(
		0, crop_rect.w, crop_rect.h, cachedBitsPerPixel, cachedFormat);
	if (cropped) {
		SDL_SetSurfaceBlendMode(converted, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(converted, &crop_rect, cropped, NULL);
	}
	SDL_FreeSurface(converted);
	if (!cropped)
		return;

	SDL_LockMutex(pillAnimMutex);
	if (pillText)
		SDL_FreeSurface(pillText);
	pillText = cropped;
	SDL_UnlockMutex(pillAnimMutex);
}

///////////////////////////////////////
// Animation callback

static void pillAnimCallback(PillFinishedTask* task) {
	SDL_LockMutex(pillAnimMutex);
	pillRect = task->dst;
	if (pillRect.w > 0 && pillRect.h > 0) {
		pilltargetY = cachedScreenH; // move offscreen below
		if (task->done) {
			pilltargetY = task->targetY;
			pilltargetTextY = task->targetTextY;
		}
		setNeedDraw(1);
	}
	setAnimationDraw(1);
	SDL_UnlockMutex(pillAnimMutex);
}

///////////////////////////////////////
// Animation worker thread

static int pillAnimWorker(void* unused) {
	(void)unused;
	while (!SDL_AtomicGet(&pillShutdown)) {
		SDL_LockMutex(pillQueueMutex);
		while (!animTaskQueueHead && !SDL_AtomicGet(&pillShutdown)) {
			SDL_CondWait(pillQueueCond, pillQueueMutex);
		}
		if (SDL_AtomicGet(&pillShutdown)) {
			SDL_UnlockMutex(pillQueueMutex);
			break;
		}
		PillAnimTaskNode* node = animTaskQueueHead;
		animTaskQueueHead = node->next;
		if (!animTaskQueueHead)
			animTaskQueueTail = NULL;
		currentAnimQueueSize--;
		SDL_UnlockMutex(pillQueueMutex);

		PillAnimTask* task = node->task;
		free(node);
		PillFinishedTask* finaltask = (PillFinishedTask*)malloc(sizeof(PillFinishedTask));
		if (!finaltask) {
			if (task->entry_name)
				free(task->entry_name);
			free(task);
			continue;
		}
		int total_frames = task->frames;
		// This somehow leads to the pill not rendering correctly when wrapping the list (last element to first, or reverse).
		// TODO: Figure out why this is here. Ideally we shouldnt refer to specific platforms in here, but the commit message doesnt
		// help all that much and comparing magic numbers also isnt that descriptive on its own.
		if (strcmp("Desktop", PLAT_getModel()) != 0) {
			if (task->targetY > task->startY + SCALE1(PILL_SIZE) || task->targetY < task->startY - SCALE1(PILL_SIZE)) {
				total_frames = 0;
			}
		}

		for (int frame = 0; frame <= total_frames; frame++) {
			if (SDL_AtomicGet(&pillShutdown))
				break;

			float t = (total_frames > 0) ? ((float)frame / total_frames) : 1.0f;
			if (t > 1.0f)
				t = 1.0f;

			int current_x = task->startX + (int)((task->targetX - task->startX) * t);
			int current_y = task->startY + (int)((task->targetY - task->startY) * t);

			SDL_Rect moveDst = {current_x, current_y, task->move_w, task->move_h};
			finaltask->dst = moveDst;
			finaltask->entry_name = task->entry_name;
			finaltask->move_w = task->move_w;
			finaltask->move_h = task->move_h;
			finaltask->targetY = task->targetY;
			finaltask->targetTextY = task->targetTextY;
			finaltask->move_y = SCALE1(PADDING + task->targetY) + (task->targetTextY - task->targetY);
			finaltask->done = 0;
			if (frame >= total_frames)
				finaltask->done = 1;
			task->callback(finaltask);
			SDL_LockMutex(frameMutex);
			while (!frameReady && !SDL_AtomicGet(&pillShutdown)) {
				SDL_CondWait(flipCond, frameMutex);
			}
			frameReady = false;
			SDL_UnlockMutex(frameMutex);
		}
		SDL_LockMutex(pillAnimMutex);
		pillanimdone = true;
		free(finaltask);
		SDL_UnlockMutex(pillAnimMutex);

		if (task->entry_name)
			free(task->entry_name);
		free(task);
	}
	return 0;
}

///////////////////////////////////////
// Internal: enqueue animation task

static void enqueuePillAnimTask(PillAnimTask* task) {
	PillAnimTaskNode* node = (PillAnimTaskNode*)malloc(sizeof(PillAnimTaskNode));
	if (!node) {
		if (task->entry_name)
			free(task->entry_name);
		free(task);
		return;
	}
	node->task = task;
	node->next = NULL;

	SDL_LockMutex(pillQueueMutex);
	pillanimdone = false;
	// If queue is full, drop the oldest task (head)
	if (currentAnimQueueSize >= 1) {
		PillAnimTaskNode* oldNode = animTaskQueueHead;
		if (oldNode) {
			animTaskQueueHead = oldNode->next;
			if (!animTaskQueueHead) {
				animTaskQueueTail = NULL;
			}
			if (oldNode->task) {
				if (oldNode->task->entry_name)
					free(oldNode->task->entry_name);
				free(oldNode->task);
			}
			free(oldNode);
			currentAnimQueueSize--;
		}
	}

	// Enqueue the new task
	if (animTaskQueueTail) {
		animTaskQueueTail->next = node;
		animTaskQueueTail = node;
	} else {
		animTaskQueueHead = animTaskQueueTail = node;
	}

	currentAnimQueueSize++;
	SDL_CondSignal(pillQueueCond);
	SDL_UnlockMutex(pillQueueMutex);
}

///////////////////////////////////////
// Public API

void Pill_init(void) {
	SDL_AtomicSet(&pillShutdown, 0);
	SDL_AtomicSet(&pillAnimationDrawAtomic, 1);

	cachedFormat = screen->format->format;
	cachedBitsPerPixel = screen->format->BitsPerPixel;
	cachedScreenW = screen->w;
	cachedScreenH = screen->h;

	pillAnimMutex = SDL_CreateMutex();
	pillQueueMutex = SDL_CreateMutex();
	pillQueueCond = SDL_CreateCond();

	if (!pillAnimMutex || !pillQueueMutex || !pillQueueCond) {
		fprintf(stderr, "pill: failed to create SDL sync primitives\n");
		return;
	}

	SDL_LockMutex(pillAnimMutex);
	pillSurface = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE, screen->w,
												 SCALE1(PILL_SIZE), FIXED_DEPTH,
												 screen->format->format);
	pillText = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE, screen->w,
											  SCALE1(PILL_SIZE), FIXED_DEPTH,
											  screen->format->format);
	pillW = 0;
	SDL_UnlockMutex(pillAnimMutex);

	pillWorkerThread = SDL_CreateThread(pillAnimWorker, "pillAnimWorker", NULL);
	if (!pillWorkerThread) {
		fprintf(stderr, "pill: failed to create animation worker thread\n");
	}
}

void Pill_quit(void) {
	SDL_AtomicSet(&pillShutdown, 1);

	// Wake up the worker thread
	if (pillQueueMutex && pillQueueCond) {
		SDL_LockMutex(pillQueueMutex);
		SDL_CondSignal(pillQueueCond);
		SDL_UnlockMutex(pillQueueMutex);
	}
	// Also wake via frame sync in case worker is waiting there
	if (frameMutex && flipCond) {
		SDL_LockMutex(frameMutex);
		frameReady = true;
		SDL_CondSignal(flipCond);
		SDL_UnlockMutex(frameMutex);
	}

	if (pillWorkerThread) {
		SDL_WaitThread(pillWorkerThread, NULL);
		pillWorkerThread = NULL;
	}

	// Drain residual tasks
	while (animTaskQueueHead) {
		PillAnimTaskNode* n = animTaskQueueHead;
		animTaskQueueHead = n->next;
		if (n->task) {
			if (n->task->entry_name)
				free(n->task->entry_name);
			free(n->task);
		}
		free(n);
	}
	animTaskQueueTail = NULL;
	currentAnimQueueSize = 0;

	// Acquire and release each mutex before destroying
	if (pillQueueMutex) {
		SDL_LockMutex(pillQueueMutex);
		SDL_UnlockMutex(pillQueueMutex);
	}
	if (pillAnimMutex) {
		SDL_LockMutex(pillAnimMutex);
		SDL_UnlockMutex(pillAnimMutex);
	}

	// Free surfaces
	if (pillSurface) {
		SDL_FreeSurface(pillSurface);
		pillSurface = NULL;
	}
	if (pillText) {
		SDL_FreeSurface(pillText);
		pillText = NULL;
	}

	// Destroy sync primitives
	if (pillQueueMutex)
		SDL_DestroyMutex(pillQueueMutex);
	if (pillAnimMutex)
		SDL_DestroyMutex(pillAnimMutex);
	if (pillQueueCond)
		SDL_DestroyCond(pillQueueCond);

	pillQueueMutex = NULL;
	pillAnimMutex = NULL;
	pillQueueCond = NULL;
}

void Pill_update(const char* entry_name, int max_width,
				 int previousY, int targetY, int text_offset_y,
				 bool should_animate, bool show_text) {
	SDL_LockMutex(pillAnimMutex);
	if (pillSurface) {
		SDL_FreeSurface(pillSurface);
		pillSurface = NULL;
	}
	pillSurface = SDL_CreateRGBSurfaceWithFormat(
		SDL_SWSURFACE, max_width, SCALE1(PILL_SIZE), FIXED_DEPTH,
		screen->format->format);
	GFX_blitPillDark(ASSET_WHITE_PILL, pillSurface,
					 &(SDL_Rect){0, 0, max_width, SCALE1(PILL_SIZE)});
	pillW = max_width;
	SDL_UnlockMutex(pillAnimMutex);

	updatePillTextSurface(entry_name, max_width, uintToColour(THEME_COLOR5_255));

	PillAnimTask* task = malloc(sizeof(PillAnimTask));
	if (task) {
		task->startX = SCALE1(BUTTON_MARGIN);
		task->startY = SCALE1(previousY + PADDING);
		task->targetX = SCALE1(BUTTON_MARGIN);
		task->targetY = SCALE1(targetY + PADDING);
		task->targetTextY = SCALE1(PADDING + targetY) + text_offset_y;
		pilltargetTextY = cachedScreenH;
		task->move_w = max_width;
		task->move_h = SCALE1(PILL_SIZE);
		task->frames = should_animate && CFG_getMenuAnimations() ? 3 : 1;
		task->entry_name = strdup(show_text ? entry_name : " ");
		task->callback = pillAnimCallback;
		enqueuePillAnimTask(task);
	}
}

bool Pill_shouldScroll(const char* display_name, int max_width) {
	return GFX_textShouldScroll(font.large, display_name,
								max_width - SCALE1(BUTTON_PADDING * 2),
								fontMutex);
}

void Pill_resetScroll(void) {
	GFX_resetScrollText();
}

void Pill_renderToLayer(bool visible) {
	SDL_LockMutex(pillAnimMutex);
	if (visible && pillSurface) {
		GFX_drawOnLayer(pillSurface, pillRect.x, pillRect.y, pillW,
						pillSurface->h, 1.0f, 0, LAYER_TRANSITION);
	}
	SDL_UnlockMutex(pillAnimMutex);
}

void Pill_renderAnimFrame(bool visible) {
	SDL_LockMutex(pillAnimMutex);
	if (getAnimationDraw()) {
		GFX_clearLayers(LAYER_TRANSITION);
		if (visible && pillSurface)
			GFX_drawOnLayer(pillSurface, pillRect.x, pillRect.y, pillW,
							pillSurface->h, 1.0f, 0, LAYER_TRANSITION);
		setAnimationDraw(0);
	}
	SDL_UnlockMutex(pillAnimMutex);
}

void Pill_renderScrollText(const char* entry_text, int available_width,
						   int text_offset_y, int row) {
	SDL_Color text_color = uintToColour(THEME_COLOR5_255);
	char cached_display_name[256];
	int text_width =
		GFX_getTextWidth(font.large, entry_text, cached_display_name,
						 available_width, SCALE1(BUTTON_PADDING * 2));
	int max_width = MIN(available_width, text_width);

	GFX_clearLayers(LAYER_SCROLLTEXT);
	GFX_scrollTextTexture(
		font.large, entry_text, SCALE1(BUTTON_MARGIN + BUTTON_PADDING),
		SCALE1(PADDING + row * PILL_SIZE) + text_offset_y,
		max_width - SCALE1(BUTTON_PADDING * 2), 0, text_color, 1,
		fontMutex);
}

bool Pill_isAnimDone(void) {
	return pillanimdone;
}

int Pill_getAnimQueueSize(void) {
	return currentAnimQueueSize;
}

bool Pill_hasAnimationDraw(void) {
	return getAnimationDraw() != 0;
}

void Pill_renderFallback(bool visible) {
	GFX_clearLayers(LAYER_TRANSITION);
	GFX_clearLayers(LAYER_SCROLLTEXT);
	SDL_LockMutex(pillAnimMutex);
	if (visible && pillSurface) {
		GFX_drawOnLayer(pillSurface, pillRect.x, pillRect.y, pillW,
						pillSurface->h, 1.0f, 0, LAYER_TRANSITION);
		if (pillText)
			GFX_drawOnLayer(pillText,
							SCALE1(BUTTON_MARGIN + BUTTON_PADDING),
							pilltargetTextY, pillText->w, pillText->h,
							1.0f, 0, LAYER_SCROLLTEXT);
	}
	SDL_UnlockMutex(pillAnimMutex);
	PLAT_GPU_Flip();
}
