#ifndef PILL_H
#define PILL_H

#include <stdbool.h>

void Pill_init(void);
void Pill_quit(void);

// Called each frame for the selected row to create/animate the pill
void Pill_update(const char* entry_name, int max_width,
				 int previousY, int targetY, int text_offset_y,
				 bool should_animate, bool show_text);

// Text scrolling helpers
bool Pill_shouldScroll(const char* display_name, int max_width);
void Pill_resetScroll(void);

// Render the pill surface onto a layer (dirty redraw path)
void Pill_renderToLayer(bool visible);

// Render pill during animation frames
void Pill_renderAnimFrame(bool visible);

// Render scrolling text on the pill
void Pill_renderScrollText(const char* entry_text, int available_width,
						   int text_offset_y, int row);

// Render pill with text overlay (non-scrolling fallback path)
void Pill_renderFallback(bool visible);

// Animation state queries
bool Pill_isAnimDone(void);
int Pill_getAnimQueueSize(void);
bool Pill_hasAnimationDraw(void);

#endif // PILL_H
