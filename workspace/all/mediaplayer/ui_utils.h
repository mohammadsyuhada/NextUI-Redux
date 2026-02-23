#ifndef __UI_UTILS_H__
#define __UI_UTILS_H__

#include <stdbool.h>
#include <stdint.h>

// Format duration as HH:MM:SS or MM:SS
void format_time(char* buf, int seconds);

// ============================================
// Dialog Box
// ============================================

// Layout information returned by render_dialog_box
typedef struct {
	int box_x, box_y; // Top-left corner of the box
	int box_w, box_h; // Box dimensions
	int content_x;	  // Left margin for content
	int content_w;	  // Width available for content
} DialogBox;

#endif
