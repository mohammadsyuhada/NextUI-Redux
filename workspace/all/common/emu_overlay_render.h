#ifndef EMU_OVERLAY_RENDER_H
#define EMU_OVERLAY_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#define EMU_OVL_FONT_LARGE 0
#define EMU_OVL_FONT_SMALL 1
#define EMU_OVL_FONT_TINY 2

// Colors (ARGB)
#define EMU_OVL_COLOR_WHITE 0xFFFFFFFF
#define EMU_OVL_COLOR_GRAY 0xFF999999
#define EMU_OVL_COLOR_BLACK 0xFF000000
#define EMU_OVL_COLOR_ACCENT 0xFF00D4AA
#define EMU_OVL_COLOR_BAR_BG 0xB2000000
#define EMU_OVL_COLOR_PILL_DARK 0x80000000
#define EMU_OVL_COLOR_PILL_LIGHT 0x40FFFFFF
#define EMU_OVL_COLOR_SELECTED_BG 0x40FFFFFF
#define EMU_OVL_COLOR_LABEL_BG 0x60FFFFFF

// Settings row colors (matching NextUI theme defaults)
#define EMU_OVL_COLOR_ROW_BG 0xFF002222	   // THEME_COLOR2 default (dark cyan)
#define EMU_OVL_COLOR_ROW_SEL 0xFFFFFFFF   // THEME_COLOR1 default (white)
#define EMU_OVL_COLOR_TEXT_SEL 0xFF000000  // THEME_COLOR5 default (black on white pill)
#define EMU_OVL_COLOR_TEXT_NORM 0xFFFFFFFF // THEME_COLOR4 default (white)

typedef struct EmuOvlRenderBackend {
	int (*init)(int screen_w, int screen_h);
	void (*destroy)(void);
	void (*draw_rect)(int x, int y, int w, int h, uint32_t color);
	void (*draw_text)(const char* text, int x, int y, uint32_t color, int font_id);
	int (*text_width)(const char* text, int font_id);
	int (*text_height)(int font_id);
	void (*begin_frame)(void);
	void (*end_frame)(void);
	void (*capture_frame)(void);
	void (*draw_captured_frame)(float dim);
	// Icon support (PNG images for button hints and screenshots)
	int (*load_icon)(const char* path, int target_height); // returns icon_id (>=0) or -1
	void (*free_icon)(int icon_id);
	void (*draw_icon)(int icon_id, int x, int y);
	int (*icon_width)(int icon_id);
	int (*icon_height)(int icon_id);
	// Save captured frame as PNG
	int (*save_captured_frame)(const char* path);
} EmuOvlRenderBackend;

#endif
