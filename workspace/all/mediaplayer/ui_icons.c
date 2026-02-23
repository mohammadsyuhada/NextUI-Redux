#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "vp_defines.h"
#include "ui_icons.h"
#include "ui_components.h"

#define ICON_FOLDER RES_PATH "/icon-folder.png"
#define ICON_VIDEO RES_PATH "/icon-video.png"
#define ICON_MP4 RES_PATH "/icon-mp4.png"
#define ICON_MKV RES_PATH "/icon-mkv.png"
#define ICON_AVI RES_PATH "/icon-avi.png"
#define ICON_MOV RES_PATH "/icon-mov.png"
#define ICON_FLV RES_PATH "/icon-flv.png"
#define ICON_M4V RES_PATH "/icon-m4v.png"
#define ICON_WMV RES_PATH "/icon-wmv.png"
#define ICON_MPG RES_PATH "/icon-mpg.png"
#define ICON_MPEG RES_PATH "/icon-mpeg.png"
#define ICON_3GP RES_PATH "/icon-3gp.png"

// Icon storage - original (black) and inverted (white) versions
typedef struct {
	SDL_Surface* folder;
	SDL_Surface* folder_inv;
	SDL_Surface* video;
	SDL_Surface* video_inv;
	SDL_Surface* mp4;
	SDL_Surface* mp4_inv;
	SDL_Surface* mkv;
	SDL_Surface* mkv_inv;
	SDL_Surface* avi;
	SDL_Surface* avi_inv;
	SDL_Surface* mov;
	SDL_Surface* mov_inv;
	SDL_Surface* flv;
	SDL_Surface* flv_inv;
	SDL_Surface* m4v;
	SDL_Surface* m4v_inv;
	SDL_Surface* wmv;
	SDL_Surface* wmv_inv;
	SDL_Surface* mpg;
	SDL_Surface* mpg_inv;
	SDL_Surface* mpeg;
	SDL_Surface* mpeg_inv;
	SDL_Surface* _3gp;
	SDL_Surface* _3gp_inv;
	bool loaded;
} IconSet;

static IconSet icons = {0};

// Invert colors of a surface (black <-> white)
// Creates a new surface with inverted colors, preserving alpha
static SDL_Surface* invert_surface(SDL_Surface* src) {
	if (!src)
		return NULL;

	// Create a new surface with same format
	SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(
		0, src->w, src->h, 32, SDL_PIXELFORMAT_RGBA32);

	if (!dst)
		return NULL;

	// Lock surfaces for direct pixel access
	SDL_LockSurface(src);
	SDL_LockSurface(dst);

	Uint32* src_pixels = (Uint32*)src->pixels;
	Uint32* dst_pixels = (Uint32*)dst->pixels;
	int pixel_count = src->w * src->h;

	for (int i = 0; i < pixel_count; i++) {
		Uint8 r, g, b, a;
		SDL_GetRGBA(src_pixels[i], src->format, &r, &g, &b, &a);

		// Invert RGB, keep alpha
		r = 255 - r;
		g = 255 - g;
		b = 255 - b;

		dst_pixels[i] = SDL_MapRGBA(dst->format, r, g, b, a);
	}

	SDL_UnlockSurface(dst);
	SDL_UnlockSurface(src);

	return dst;
}

// Load an icon and create its inverted version
static void load_icon_pair(const char* path, SDL_Surface** original, SDL_Surface** inverted) {
	*original = IMG_Load(path);
	if (*original) {
		// Convert to RGBA32 for consistent pixel access
		SDL_Surface* converted = SDL_ConvertSurfaceFormat(*original, SDL_PIXELFORMAT_RGBA32, 0);
		if (converted) {
			SDL_FreeSurface(*original);
			*original = converted;
		}
		*inverted = invert_surface(*original);
	} else {
		*inverted = NULL;
	}
}

// Initialize icons
void Icons_init(void) {
	if (icons.loaded)
		return;

	load_icon_pair(ICON_FOLDER, &icons.folder, &icons.folder_inv);
	load_icon_pair(ICON_VIDEO, &icons.video, &icons.video_inv);
	load_icon_pair(ICON_MP4, &icons.mp4, &icons.mp4_inv);
	load_icon_pair(ICON_MKV, &icons.mkv, &icons.mkv_inv);
	load_icon_pair(ICON_AVI, &icons.avi, &icons.avi_inv);
	load_icon_pair(ICON_MOV, &icons.mov, &icons.mov_inv);
	load_icon_pair(ICON_FLV, &icons.flv, &icons.flv_inv);
	load_icon_pair(ICON_M4V, &icons.m4v, &icons.m4v_inv);
	load_icon_pair(ICON_WMV, &icons.wmv, &icons.wmv_inv);
	load_icon_pair(ICON_MPG, &icons.mpg, &icons.mpg_inv);
	load_icon_pair(ICON_MPEG, &icons.mpeg, &icons.mpeg_inv);
	load_icon_pair(ICON_3GP, &icons._3gp, &icons._3gp_inv);
	UI_initEmptyIcon();

	// Consider loaded if at least folder icon exists
	icons.loaded = (icons.folder != NULL);
}

// Cleanup icons
void Icons_quit(void) {
	if (icons.folder) {
		SDL_FreeSurface(icons.folder);
		icons.folder = NULL;
	}
	if (icons.folder_inv) {
		SDL_FreeSurface(icons.folder_inv);
		icons.folder_inv = NULL;
	}
	if (icons.video) {
		SDL_FreeSurface(icons.video);
		icons.video = NULL;
	}
	if (icons.video_inv) {
		SDL_FreeSurface(icons.video_inv);
		icons.video_inv = NULL;
	}
	if (icons.mp4) {
		SDL_FreeSurface(icons.mp4);
		icons.mp4 = NULL;
	}
	if (icons.mp4_inv) {
		SDL_FreeSurface(icons.mp4_inv);
		icons.mp4_inv = NULL;
	}
	if (icons.mkv) {
		SDL_FreeSurface(icons.mkv);
		icons.mkv = NULL;
	}
	if (icons.mkv_inv) {
		SDL_FreeSurface(icons.mkv_inv);
		icons.mkv_inv = NULL;
	}
	if (icons.avi) {
		SDL_FreeSurface(icons.avi);
		icons.avi = NULL;
	}
	if (icons.avi_inv) {
		SDL_FreeSurface(icons.avi_inv);
		icons.avi_inv = NULL;
	}
	if (icons.mov) {
		SDL_FreeSurface(icons.mov);
		icons.mov = NULL;
	}
	if (icons.mov_inv) {
		SDL_FreeSurface(icons.mov_inv);
		icons.mov_inv = NULL;
	}
	if (icons.flv) {
		SDL_FreeSurface(icons.flv);
		icons.flv = NULL;
	}
	if (icons.flv_inv) {
		SDL_FreeSurface(icons.flv_inv);
		icons.flv_inv = NULL;
	}
	if (icons.m4v) {
		SDL_FreeSurface(icons.m4v);
		icons.m4v = NULL;
	}
	if (icons.m4v_inv) {
		SDL_FreeSurface(icons.m4v_inv);
		icons.m4v_inv = NULL;
	}
	if (icons.wmv) {
		SDL_FreeSurface(icons.wmv);
		icons.wmv = NULL;
	}
	if (icons.wmv_inv) {
		SDL_FreeSurface(icons.wmv_inv);
		icons.wmv_inv = NULL;
	}
	if (icons.mpg) {
		SDL_FreeSurface(icons.mpg);
		icons.mpg = NULL;
	}
	if (icons.mpg_inv) {
		SDL_FreeSurface(icons.mpg_inv);
		icons.mpg_inv = NULL;
	}
	if (icons.mpeg) {
		SDL_FreeSurface(icons.mpeg);
		icons.mpeg = NULL;
	}
	if (icons.mpeg_inv) {
		SDL_FreeSurface(icons.mpeg_inv);
		icons.mpeg_inv = NULL;
	}
	if (icons._3gp) {
		SDL_FreeSurface(icons._3gp);
		icons._3gp = NULL;
	}
	if (icons._3gp_inv) {
		SDL_FreeSurface(icons._3gp_inv);
		icons._3gp_inv = NULL;
	}
	UI_quitEmptyIcon();
	icons.loaded = false;
}

// Check if icons are loaded
bool Icons_isLoaded(void) {
	return icons.loaded;
}

// Get folder icon
SDL_Surface* Icons_getFolder(bool selected) {
	if (!icons.loaded)
		return NULL;
	return selected ? icons.folder : icons.folder_inv;
}

// Get generic video icon
SDL_Surface* Icons_getVideo(bool selected) {
	if (!icons.loaded)
		return NULL;
	return selected ? icons.video : icons.video_inv;
}

// Get icon for specific video format
// Falls back to generic video icon if format-specific icon not available
SDL_Surface* Icons_getForFormat(VideoFormat format, bool selected) {
	if (!icons.loaded)
		return NULL;

	SDL_Surface* icon = NULL;
	SDL_Surface* icon_inv = NULL;

	switch (format) {
	case VIDEO_FORMAT_MP4:
		icon = icons.mp4;
		icon_inv = icons.mp4_inv;
		break;
	case VIDEO_FORMAT_MKV:
		icon = icons.mkv;
		icon_inv = icons.mkv_inv;
		break;
	case VIDEO_FORMAT_AVI:
		icon = icons.avi;
		icon_inv = icons.avi_inv;
		break;
	case VIDEO_FORMAT_MOV:
		icon = icons.mov;
		icon_inv = icons.mov_inv;
		break;
	case VIDEO_FORMAT_FLV:
		icon = icons.flv;
		icon_inv = icons.flv_inv;
		break;
	case VIDEO_FORMAT_M4V:
		icon = icons.m4v;
		icon_inv = icons.m4v_inv;
		break;
	case VIDEO_FORMAT_WMV:
		icon = icons.wmv;
		icon_inv = icons.wmv_inv;
		break;
	case VIDEO_FORMAT_MPG:
		icon = icons.mpg;
		icon_inv = icons.mpg_inv;
		break;
	case VIDEO_FORMAT_3GP:
		icon = icons._3gp;
		icon_inv = icons._3gp_inv;
		break;
	default:
		// WEBM, TS, UNKNOWN -> generic video icon
		icon = icons.video;
		icon_inv = icons.video_inv;
		break;
	}

	// If format-specific icon not loaded, fall back to generic
	if (!icon) {
		icon = icons.video;
		icon_inv = icons.video_inv;
	}

	return selected ? icon : icon_inv;
}

// Get empty state icon
