#ifndef __VP_DEFINES_H__
#define __VP_DEFINES_H__

// Include the common defines.h which includes platform.h and provides
// shared constants (MAX_PATH, SCALE1, PILL_SIZE, COLOR_WHITE, etc.)
#include "defines.h"

// Video root directory
#define VIDEO_ROOT SDCARD_PATH "/Videos"

// App data paths (SHARED_USERDATA_PATH defined in common defines.h)
#define APP_DATA_DIR SHARED_USERDATA_PATH "/media-player"
#define APP_SETTINGS_DIR APP_DATA_DIR
#define APP_THUMBNAILS_DIR SDCARD_PATH "/.cache/youtube-thumbnails"
#define APP_SUBSCRIPTIONS_FILE APP_DATA_DIR "/subscriptions.json"
#define APP_YOUTUBE_DIR APP_DATA_DIR "/youtube"

#define FFPLAY_PATH SHARED_BIN_PATH "/ffplay"

// Supported video file extensions
#define VIDEO_EXT_MP4 "mp4"
#define VIDEO_EXT_MKV "mkv"
#define VIDEO_EXT_AVI "avi"
#define VIDEO_EXT_WEBM "webm"
#define VIDEO_EXT_MOV "mov"
#define VIDEO_EXT_TS "ts"
#define VIDEO_EXT_FLV "flv"
#define VIDEO_EXT_M4V "m4v"
#define VIDEO_EXT_WMV "wmv"
#define VIDEO_EXT_MPG "mpg"
#define VIDEO_EXT_MPEG "mpeg"
#define VIDEO_EXT_3GP "3gp"

// Supported subtitle extensions
#define SUB_EXT_SRT "srt"
#define SUB_EXT_ASS "ass"
#define SUB_EXT_SSA "ssa"
#define SUB_EXT_SUB "sub"

// App state enum for controls help context
typedef enum {
	STATE_MENU = 0,
	STATE_BROWSER = 1,
	STATE_PLAYING = 2,
	STATE_YOUTUBE_MENU = 10,
	STATE_YOUTUBE_SEARCHING = 11,
	STATE_YOUTUBE_RESULTS = 12,
	STATE_YOUTUBE_PLAYING = 13,
	STATE_IPTV_LIST = 20,
	STATE_IPTV_CATEGORIES = 21,
	STATE_IPTV_PLAYING = 22,
	STATE_IPTV_CURATED_COUNTRIES = 23,
	STATE_IPTV_CURATED_CHANNELS = 24,
	STATE_SUBSCRIPTIONS = 30,
	STATE_SETTINGS = 40,
} AppState;

// Video format enum
typedef enum {
	VIDEO_FORMAT_UNKNOWN = 0,
	VIDEO_FORMAT_MP4,
	VIDEO_FORMAT_MKV,
	VIDEO_FORMAT_AVI,
	VIDEO_FORMAT_WEBM,
	VIDEO_FORMAT_MOV,
	VIDEO_FORMAT_TS,
	VIDEO_FORMAT_FLV,
	VIDEO_FORMAT_M4V,
	VIDEO_FORMAT_WMV,
	VIDEO_FORMAT_MPG,
	VIDEO_FORMAT_3GP,
} VideoFormat;

#endif
