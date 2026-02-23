#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "podcast.h"
#include "player.h"
#include "ui_podcast.h"
#include "ui_list.h"
#include "ui_toast.h"
#include "ui_icons.h"
#include "ui_album_art.h"
#include "wget_fetch.h"

// Max artwork size (1MB to match radio album art buffer)
#define PODCAST_ARTWORK_MAX_SIZE (1024 * 1024)

// Scroll state for selected item title in lists
static ScrollTextState podcast_title_scroll = {0};

// Scroll state for playing screen episode title
static ScrollTextState podcast_playing_title_scroll = {0};

// Podcast artwork state
static SDL_Surface* podcast_artwork = NULL;
static char podcast_artwork_url[512] = {0};

// Podcast progress GPU state
static int progress_bar_x = 0, progress_bar_y = 0;
static int progress_bar_w = 0, progress_bar_h = 0;
static int progress_time_y = 0;
static int progress_screen_w = 0;
static int progress_duration_ms = 0;
static int progress_last_position_sec = -1;
static bool progress_position_set = false;

// Helper to convert surface to ARGB8888 for proper scaling
static SDL_Surface* convert_to_argb8888(SDL_Surface* src) {
	if (!src)
		return NULL;

	SDL_Surface* converted = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(src);
	return converted;
}

// Check if downloaded image data is complete (not truncated)
// JPEG: ends with FF D9, PNG: ends with IEND chunk
static bool is_image_complete(const uint8_t* data, int size) {
	if (size < 4)
		return false;
	// JPEG: starts with FF D8, ends with FF D9
	if (data[0] == 0xFF && data[1] == 0xD8) {
		return (data[size - 2] == 0xFF && data[size - 1] == 0xD9);
	}
	// PNG: starts with 89 50 4E 47, ends with IEND chunk (AE 42 60 82)
	if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
		return (size >= 8 &&
				data[size - 4] == 0xAE && data[size - 3] == 0x42 &&
				data[size - 2] == 0x60 && data[size - 1] == 0x82);
	}
	// Unknown format — assume complete
	return true;
}

// Fetch podcast artwork from URL (cached in podcast folder)
// feed_id: the podcast's feed_id for storing artwork in its folder
static void podcast_fetch_artwork(const char* artwork_url, const char* feed_id) {
	if (!artwork_url || !artwork_url[0] || !feed_id || !feed_id[0])
		return;

	// Already have this artwork
	if (strcmp(podcast_artwork_url, artwork_url) == 0 && podcast_artwork)
		return;

	// Clear old artwork and invalidate album art background cache
	if (podcast_artwork) {
		SDL_FreeSurface(podcast_artwork);
		podcast_artwork = NULL;
		cleanup_album_art_background();
	}
	strncpy(podcast_artwork_url, artwork_url, sizeof(podcast_artwork_url) - 1);

	// Build cache path: <podcast_data_dir>/<feed_id>/artwork.jpg
	char feed_dir[512];
	Podcast_getFeedDataPath(feed_id, feed_dir, sizeof(feed_dir));

	char cache_path[768];
	snprintf(cache_path, sizeof(cache_path), "%s/artwork.jpg", feed_dir);

	// Try to load from cache first
	FILE* f = fopen(cache_path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);

		if (size > 0 && size < PODCAST_ARTWORK_MAX_SIZE) {
			uint8_t* data = (uint8_t*)malloc(size);
			if (data && fread(data, 1, size, f) == (size_t)size) {
				if (is_image_complete(data, size)) {
					SDL_RWops* rw = SDL_RWFromConstMem(data, size);
					if (rw) {
						SDL_Surface* loaded = IMG_Load_RW(rw, 1);
						podcast_artwork = convert_to_argb8888(loaded);
					}
				}
			}
			free(data);
		}
		fclose(f);
		if (podcast_artwork)
			return;
		// Cached file is corrupt/incomplete — delete it so we re-fetch
		remove(cache_path);
	}

	// Fetch from network using static buffer
	static uint8_t artwork_buffer[PODCAST_ARTWORK_MAX_SIZE];
	int size = wget_fetch(artwork_url, artwork_buffer, PODCAST_ARTWORK_MAX_SIZE);

	if (size > 0 && is_image_complete(artwork_buffer, size)) {
		// Save to podcast folder (directory should already exist from subscription)
		f = fopen(cache_path, "wb");
		if (f) {
			fwrite(artwork_buffer, 1, size, f);
			fclose(f);
		}

		// Load as SDL surface and convert to ARGB8888 for proper scaling
		SDL_RWops* rw = SDL_RWFromConstMem(artwork_buffer, size);
		if (rw) {
			SDL_Surface* loaded = IMG_Load_RW(rw, 1);
			podcast_artwork = convert_to_argb8888(loaded);
		}
	}
}

// Clear podcast artwork (call when leaving playing screen)
void Podcast_clearArtwork(void) {
	if (podcast_artwork) {
		SDL_FreeSurface(podcast_artwork);
		podcast_artwork = NULL;
	}
	podcast_artwork_url[0] = '\0';
	memset(&podcast_playing_title_scroll, 0, sizeof(podcast_playing_title_scroll));
	PodcastProgress_clear(); // Clear GPU progress layer
}

// Thumbnail cache for subscription artwork on main page
#define THUMBNAIL_CACHE_SIZE 8
typedef struct {
	char feed_id[17];
	SDL_Surface* thumbnail;
} ThumbnailCacheEntry;
static ThumbnailCacheEntry thumbnail_cache[THUMBNAIL_CACHE_SIZE];
static int thumbnail_cache_count = 0;

// Scale surface to size x size and apply circular mask
static SDL_Surface* load_circular_thumbnail_from_surface(SDL_Surface* raw, int size) {
	if (!raw)
		return NULL;

	// Convert to ARGB8888 for proper scaling
	SDL_Surface* converted = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
	if (!converted)
		return NULL;

	// Scale to size x size
	SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!scaled) {
		SDL_FreeSurface(converted);
		return NULL;
	}
	SDL_Rect src = {0, 0, converted->w, converted->h};
	SDL_Rect dst = {0, 0, size, size};
	SDL_BlitScaled(converted, &src, scaled, &dst);
	SDL_FreeSurface(converted);

	// Apply circular mask
	int radius = size / 2;
	uint32_t* pixels = (uint32_t*)scaled->pixels;
	int pitch = scaled->pitch / 4;
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			int dx = x - radius;
			int dy = y - radius;
			if (dx * dx + dy * dy > radius * radius) {
				pixels[y * pitch + x] = 0; // Fully transparent
			}
		}
	}

	return scaled;
}

// Load image file from disk path, scale to size x size, apply circular mask
// Deletes corrupt/incomplete files so they get re-fetched
static SDL_Surface* load_circular_thumbnail(const char* path, int size) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize <= 0 || fsize > 1024 * 1024) {
		fclose(f);
		return NULL;
	}

	uint8_t* data = (uint8_t*)malloc(fsize);
	if (!data) {
		fclose(f);
		return NULL;
	}
	if ((long)fread(data, 1, fsize, f) != fsize) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);

	// Validate image completeness
	if (!is_image_complete(data, fsize)) {
		free(data);
		remove(path); // Delete corrupt file so it gets re-fetched
		return NULL;
	}

	SDL_RWops* rw = SDL_RWFromConstMem(data, fsize);
	SDL_Surface* raw = NULL;
	if (rw)
		raw = IMG_Load_RW(rw, 1);
	free(data);
	if (!raw) {
		remove(path);
		return NULL;
	}

	SDL_Surface* result = load_circular_thumbnail_from_surface(raw, size);
	SDL_FreeSurface(raw);
	return result;
}

// Add thumbnail to in-memory cache (FIFO eviction)
static void cache_thumbnail(const char* cache_key, SDL_Surface* surface) {
	if (thumbnail_cache_count >= THUMBNAIL_CACHE_SIZE) {
		SDL_FreeSurface(thumbnail_cache[0].thumbnail);
		for (int i = 0; i < THUMBNAIL_CACHE_SIZE - 1; i++) {
			thumbnail_cache[i] = thumbnail_cache[i + 1];
		}
		thumbnail_cache_count = THUMBNAIL_CACHE_SIZE - 1;
	}
	ThumbnailCacheEntry* entry = &thumbnail_cache[thumbnail_cache_count];
	strncpy(entry->feed_id, cache_key, sizeof(entry->feed_id) - 1);
	entry->feed_id[sizeof(entry->feed_id) - 1] = '\0';
	entry->thumbnail = surface;
	thumbnail_cache_count++;
}

// Look up thumbnail in memory cache
static SDL_Surface* find_cached_thumbnail(const char* cache_key) {
	for (int i = 0; i < thumbnail_cache_count; i++) {
		if (strcmp(thumbnail_cache[i].feed_id, cache_key) == 0 && thumbnail_cache[i].thumbnail) {
			return thumbnail_cache[i].thumbnail;
		}
	}
	return NULL;
}

// Lazy load one subscription thumbnail from disk (call once per frame)
// Returns true if a thumbnail was loaded (caller should redraw)
static bool subscription_thumb_load_one(const char* feed_id, int size) {
	if (!feed_id || !feed_id[0] || size <= 0)
		return false;

	// Already in memory cache
	if (find_cached_thumbnail(feed_id))
		return false;

	// Load from disk: <feed_data_dir>/artwork.jpg
	char feed_dir[512];
	Podcast_getFeedDataPath(feed_id, feed_dir, sizeof(feed_dir));
	char art_path[768];
	snprintf(art_path, sizeof(art_path), "%s/artwork.jpg", feed_dir);

	SDL_Surface* thumb = load_circular_thumbnail(art_path, size);
	if (thumb) {
		cache_thumbnail(feed_id, thumb);
		return true;
	}
	return false;
}

// Disk cache path for artwork by itunes_id
#define PODCAST_CACHE_PARENT SDCARD_PATH "/.cache"
#define PODCAST_CACHE_DIR SDCARD_PATH "/.cache/podcast"

static void get_artwork_cache_path(const char* itunes_id, char* path, int path_size) {
	snprintf(path, path_size, PODCAST_CACHE_DIR "/%s.jpg", itunes_id);
}

// Get artwork thumbnail: memory cache -> disk cache -> NULL (non-blocking)
static SDL_Surface* get_artwork_thumbnail(const char* itunes_id, int size) {
	if (!itunes_id || !itunes_id[0] || size <= 0)
		return NULL;

	// Check memory cache
	SDL_Surface* cached = find_cached_thumbnail(itunes_id);
	if (cached)
		return cached;

	// Check disk cache
	char cache_path[768];
	get_artwork_cache_path(itunes_id, cache_path, sizeof(cache_path));
	SDL_Surface* thumb = load_circular_thumbnail(cache_path, size);
	if (thumb) {
		cache_thumbnail(itunes_id, thumb);
		return thumb;
	}

	return NULL;
}

// Lazy fetch: download one artwork from network, save to disk, cache in memory
// Returns true if an image was fetched (caller should break to limit one per frame)
static bool artwork_fetch_one(const char* itunes_id, const char* artwork_url, int size) {
	if (!itunes_id || !itunes_id[0] || !artwork_url || !artwork_url[0] || size <= 0)
		return false;

	// Already in memory
	if (find_cached_thumbnail(itunes_id))
		return false;

	// Already on disk
	char cache_path[768];
	get_artwork_cache_path(itunes_id, cache_path, sizeof(cache_path));
	SDL_Surface* thumb = load_circular_thumbnail(cache_path, size);
	if (thumb) {
		cache_thumbnail(itunes_id, thumb);
		return true;
	}

	// Fetch from network
	static uint8_t art_buf[PODCAST_ARTWORK_MAX_SIZE];
	int dl_size = wget_fetch(artwork_url, art_buf, PODCAST_ARTWORK_MAX_SIZE);
	if (dl_size <= 0 || !is_image_complete(art_buf, dl_size))
		return false;

	// Save to disk cache
	mkdir(PODCAST_CACHE_PARENT, 0755);
	mkdir(PODCAST_CACHE_DIR, 0755);
	FILE* f = fopen(cache_path, "wb");
	if (f) {
		fwrite(art_buf, 1, dl_size, f);
		fclose(f);
	}

	// Load into surface from memory
	SDL_RWops* rw = SDL_RWFromConstMem(art_buf, dl_size);
	if (!rw)
		return false;
	SDL_Surface* raw = IMG_Load_RW(rw, 1);
	if (!raw)
		return false;

	thumb = load_circular_thumbnail_from_surface(raw, size);
	SDL_FreeSurface(raw);
	if (!thumb)
		return false;

	cache_thumbnail(itunes_id, thumb);
	return true;
}

void Podcast_clearThumbnailCache(void) {
	for (int i = 0; i < thumbnail_cache_count; i++) {
		if (thumbnail_cache[i].thumbnail) {
			SDL_FreeSurface(thumbnail_cache[i].thumbnail);
			thumbnail_cache[i].thumbnail = NULL;
		}
		thumbnail_cache[i].feed_id[0] = '\0';
	}
	thumbnail_cache_count = 0;
}

bool Podcast_loadPendingThumbnails(void) {
	int sub_count = Podcast_getSubscriptionCount();
	if (sub_count == 0)
		return false;

	PodcastFeed* feeds = Podcast_getSubscriptions(NULL);
	int thumb_size = SCALE1(PILL_SIZE) * 3 / 2 - SCALE1(4) * 2;

	for (int i = 0; i < sub_count; i++) {
		if (subscription_thumb_load_one(feeds[i].feed_id, thumb_size))
			return true;
	}
	return false;
}

// Episode header artwork (square with rounded corners, cached per feed)
static SDL_Surface* episode_header_art = NULL;
static char episode_header_feed_id[17] = {0};
static int episode_header_art_size = 0;

// Load image file, scale to size x size, apply rounded corner mask
// Deletes corrupt/incomplete files so they get re-fetched
static SDL_Surface* load_rounded_thumbnail(const char* path, int size, int radius) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize <= 0 || fsize > 1024 * 1024) {
		fclose(f);
		return NULL;
	}

	uint8_t* data = (uint8_t*)malloc(fsize);
	if (!data) {
		fclose(f);
		return NULL;
	}
	if ((long)fread(data, 1, fsize, f) != fsize) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);

	// Validate image completeness
	if (!is_image_complete(data, fsize)) {
		free(data);
		remove(path);
		return NULL;
	}

	SDL_RWops* rw = SDL_RWFromConstMem(data, fsize);
	SDL_Surface* raw = NULL;
	if (rw)
		raw = IMG_Load_RW(rw, 1);
	free(data);
	if (!raw) {
		remove(path);
		return NULL;
	}

	SDL_Surface* converted = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(raw);
	if (!converted)
		return NULL;

	SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!scaled) {
		SDL_FreeSurface(converted);
		return NULL;
	}
	SDL_Rect src_r = {0, 0, converted->w, converted->h};
	SDL_Rect dst_r = {0, 0, size, size};
	SDL_BlitScaled(converted, &src_r, scaled, &dst_r);
	SDL_FreeSurface(converted);

	if (radius > 0) {
		uint32_t* pixels = (uint32_t*)scaled->pixels;
		int pitch_px = scaled->pitch / 4;
		for (int py = 0; py < size; py++) {
			for (int px = 0; px < size; px++) {
				int cx = -1, cy = -1;
				if (px < radius && py < radius) {
					cx = radius;
					cy = radius;
				} else if (px >= size - radius && py < radius) {
					cx = size - 1 - radius;
					cy = radius;
				} else if (px < radius && py >= size - radius) {
					cx = radius;
					cy = size - 1 - radius;
				} else if (px >= size - radius && py >= size - radius) {
					cx = size - 1 - radius;
					cy = size - 1 - radius;
				}
				if (cx >= 0 && (px - cx) * (px - cx) + (py - cy) * (py - cy) > radius * radius) {
					pixels[py * pitch_px + px] = 0;
				}
			}
		}
	}

	return scaled;
}

// Get or load episode header artwork (cached by feed_id and size)
static SDL_Surface* get_episode_header_art(const char* feed_id, int size) {
	if (episode_header_art && strcmp(episode_header_feed_id, feed_id) == 0 && episode_header_art_size == size)
		return episode_header_art;

	if (episode_header_art) {
		SDL_FreeSurface(episode_header_art);
		episode_header_art = NULL;
	}
	episode_header_art_size = 0;
	episode_header_feed_id[0] = '\0';

	char feed_dir[512];
	Podcast_getFeedDataPath(feed_id, feed_dir, sizeof(feed_dir));
	char art_path[768];
	snprintf(art_path, sizeof(art_path), "%s/artwork.jpg", feed_dir);

	episode_header_art = load_rounded_thumbnail(art_path, size, SCALE1(8));
	if (episode_header_art) {
		strncpy(episode_header_feed_id, feed_id, sizeof(episode_header_feed_id) - 1);
		episode_header_feed_id[sizeof(episode_header_feed_id) - 1] = '\0';
		episode_header_art_size = size;
	}
	return episode_header_art;
}

// Management menu item labels (Y button menu)
static const char* podcast_manage_items[] = {
	"Search",
	"Top Shows"};

// Format duration as HH:MM:SS or MM:SS
static void format_duration(char* buf, int seconds) {
	if (seconds <= 0) {
		strcpy(buf, "--:--");
		return;
	}
	int h = seconds / 3600;
	int m = (seconds % 3600) / 60;
	int s = seconds % 60;
	if (h > 0) {
		sprintf(buf, "%d:%02d:%02d", h, m, s);
	} else {
		sprintf(buf, "%02d:%02d", m, s);
	}
}

// Format progress/duration pair as "MM:SS/MM:SS" or "H:MM:SS/H:MM:SS"
// When hours is 0, omits hours prefix (e.g., "01:52" not "0:01:52")
static void format_duration_pair(char* buf, int progress_sec, int duration_sec) {
	int p_h = progress_sec / 3600, p_m = (progress_sec % 3600) / 60, p_s = progress_sec % 60;
	int d_h = duration_sec / 3600, d_m = (duration_sec % 3600) / 60, d_s = duration_sec % 60;
	char p_buf[16], d_buf[16];
	if (p_h > 0)
		snprintf(p_buf, 16, "%d:%02d:%02d", p_h, p_m, p_s);
	else
		snprintf(p_buf, 16, "%02d:%02d", p_m, p_s);
	if (d_h > 0)
		snprintf(d_buf, 16, "%d:%02d:%02d", d_h, d_m, d_s);
	else
		snprintf(d_buf, 16, "%02d:%02d", d_m, d_s);
	snprintf(buf, 32, "%s/%s", p_buf, d_buf);
}

// Format date as relative time or date string
static void format_date(char* buf, uint32_t timestamp) {
	if (timestamp == 0) {
		strcpy(buf, "");
		return;
	}

	time_t now = time(NULL);
	time_t pub = (time_t)timestamp;
	int days = (now - pub) / (24 * 3600);

	if (days == 0) {
		strcpy(buf, "Today");
	} else if (days == 1) {
		strcpy(buf, "Yesterday");
	} else if (days < 7) {
		sprintf(buf, "%d days ago", days);
	} else if (days < 30) {
		sprintf(buf, "%d weeks ago", days / 7);
	} else {
		struct tm* tm = localtime(&pub);
		strftime(buf, 32, "%b %d", tm);
	}
}

// --- Section header helper ---
static void render_section_header(SDL_Surface* screen, const char* text, int y) {
	SDL_Surface* surf = TTF_RenderUTF8_Blended(font.small, text, COLOR_GRAY);
	if (surf) {
		SDL_BlitSurface(surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y});
		SDL_FreeSurface(surf);
	}
}

// --- Rich list item renderer (artwork + title + subtitle using rich pill) ---
// Generic: works for subscriptions, search results, top shows
// Thumbnails are memory-cache only (non-blocking). Lazy loading done by caller.
static ListItemRichPos render_rich_list_item(SDL_Surface* screen, ListLayout* layout,
											 const char* title, const char* subtitle,
											 const char* feed_id, const char* itunes_id,
											 int y, bool selected, int extra_subtitle_width) {
	char truncated[256];

	// Check memory cache only (non-blocking)
	const char* cache_key = (feed_id && feed_id[0]) ? feed_id : (itunes_id && itunes_id[0]) ? itunes_id
																							: NULL;
	SDL_Surface* thumb = cache_key ? find_cached_thumbnail(cache_key) : NULL;
	bool has_image = (thumb != NULL);

	ListItemRichPos pos = UI_renderListItemPillRich(screen, layout, title, subtitle, truncated, y, selected, has_image, extra_subtitle_width);

	if (thumb) {
		SDL_Rect dst = {pos.image_x, pos.image_y, pos.image_size, pos.image_size};
		SDL_BlitScaled(thumb, NULL, screen, &dst);
	}

	// Title (row 1, scrollable when selected) — pass full title, not truncated
	UI_renderListItemText(screen, selected ? &podcast_title_scroll : NULL,
						  title, font.medium,
						  pos.title_x, pos.title_y, pos.text_max_width, selected);

	// Subtitle (row 2)
	if (subtitle && subtitle[0]) {
		SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, subtitle, COLOR_GRAY);
		if (s) {
			SDL_Rect src = {0, 0, s->w > pos.text_max_width ? pos.text_max_width : s->w, s->h};
			SDL_BlitSurface(s, &src, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
			SDL_FreeSurface(s);
		}
	}

	return pos;
}

// Render redesigned podcast main page (continue listening + subscriptions with artwork)
void render_podcast_main_page(SDL_Surface* screen, IndicatorType show_setting,
							  int selected, int* scroll,
							  const char* toast_message, uint32_t toast_time) {
	GFX_clear(screen);

	int hw = screen->w;

	UI_renderMenuBar(screen, "Podcasts");

	int cl_count_raw = Podcast_getContinueListeningCount();
	int cl_count = (cl_count_raw > PODCAST_CONTINUE_LISTENING_DISPLAY) ? PODCAST_CONTINUE_LISTENING_DISPLAY : cl_count_raw;
	int sub_count = Podcast_getSubscriptionCount();
	// "Downloads" menu item at the bottom (only when queue is non-empty)
	int dl_queue_count = 0;
	Podcast_getDownloadQueue(&dl_queue_count);
	int has_downloads_item = (dl_queue_count > 0) ? 1 : 0;
	int total = cl_count + sub_count + has_downloads_item;

	// Empty state
	if (total == 0) {
		UI_renderEmptyState(screen, "No podcasts subscribed", "Press Y to manage podcasts", "MANAGE");
		return;
	}

	// Item dimensions (both sections use rich pill height)
	int sub_item_h = SCALE1(PILL_SIZE) * 3 / 2;
	int cl_item_h = sub_item_h;
	int section_header_h = SCALE1(16);
	int section_gap = SCALE1(4); // Gap between section header and first item

	// Calculate total content height and per-item Y positions
	// We'll compute item positions in a flat array
	int item_y[PODCAST_MAX_CONTINUE_LISTENING + PODCAST_MAX_SUBSCRIPTIONS + 4]; // generous
	int content_y = 0;

	ListLayout layout = UI_calcListLayout(screen);
	ListLayout pill_layout = layout; // Keep original PILL_SIZE item_h for pill rendering
	// Start content right after page title pill (no extra margin)
	int base_y = SCALE1(PADDING + PILL_SIZE + 1);
	int hh = screen->h;
	int viewport_h = hh - base_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN + 8);

	content_y = 0;

	// Continue Listening section
	if (cl_count > 0) {
		content_y += section_header_h + section_gap;
		for (int i = 0; i < cl_count; i++) {
			item_y[i] = content_y;
			content_y += cl_item_h;
		}
	}

	// Subscriptions section
	if (sub_count > 0) {
		if (cl_count > 0)
			content_y += SCALE1(18); // Gap between sections
		content_y += section_header_h + section_gap;
		for (int i = 0; i < sub_count; i++) {
			item_y[cl_count + i] = content_y;
			content_y += sub_item_h;
		}
	}

	// Downloads item (at the bottom)
	if (has_downloads_item) {
		if (cl_count > 0 || sub_count > 0)
			content_y += SCALE1(18); // Gap
		item_y[cl_count + sub_count] = content_y;
		content_y += sub_item_h;
	}

	int total_content_h = content_y;

	// Adjust scroll to keep selected item visible
	if (selected >= 0 && selected < total) {
		int sel_y = item_y[selected];
		int sel_h = (selected < cl_count) ? cl_item_h : sub_item_h;

		// For first item in a section, include the section header
		int sel_top = sel_y;
		if (selected == 0 && cl_count > 0) {
			sel_top = 0; // include "Continue Listening" header
		} else if (selected == cl_count && sub_count > 0) {
			sel_top = sel_y - section_header_h - section_gap;
			if (cl_count > 0)
				sel_top -= SCALE1(18); // include gap
		}

		if (sel_top - *scroll < 0) {
			*scroll = sel_top;
		} else if (sel_y + sel_h - *scroll > viewport_h) {
			*scroll = sel_y + sel_h - viewport_h;
		}
	}

	// Clamp scroll
	if (*scroll < 0)
		*scroll = 0;
	if (total_content_h > viewport_h) {
		if (*scroll > total_content_h - viewport_h)
			*scroll = total_content_h - viewport_h;
	} else {
		*scroll = 0;
	}

	// Set clip rect for list area
	SDL_Rect clip = {0, base_y, hw, viewport_h};
	SDL_SetClipRect(screen, &clip);

	int draw_offset = base_y - *scroll;

	// --- Continue Listening section ---
	int cy = 0;
	if (cl_count > 0) {
		int header_screen_y = draw_offset + cy;
		if (header_screen_y + section_header_h > base_y && header_screen_y < base_y + viewport_h) {
			render_section_header(screen, "Continue Listening", header_screen_y);
		}
		cy += section_header_h + section_gap;

		for (int i = 0; i < cl_count; i++) {
			bool is_selected = (i == selected);
			int y = draw_offset + cy;

			if (y + cl_item_h > base_y && y < base_y + viewport_h) {
				ContinueListeningEntry* entry = Podcast_getContinueListening(i);
				if (entry) {
					render_rich_list_item(screen, &pill_layout, entry->episode_title, entry->feed_title,
										  NULL, NULL, y, is_selected, 0);
				}
			}
			cy += cl_item_h;
		}
	}

	// --- Subscriptions section ---
	if (sub_count > 0) {
		if (cl_count > 0)
			cy += SCALE1(18); // Gap between sections
		int header_screen_y = draw_offset + cy;
		if (header_screen_y + section_header_h > base_y && header_screen_y < base_y + viewport_h) {
			render_section_header(screen, "Subscriptions", header_screen_y);
		}
		cy += section_header_h + section_gap;

		PodcastFeed* feeds = Podcast_getSubscriptions(NULL);
		for (int i = 0; i < sub_count; i++) {
			bool is_selected = (cl_count + i == selected);
			int y = draw_offset + cy;

			if (y + sub_item_h > base_y && y < base_y + viewport_h) {
				char ep_str[64];
				snprintf(ep_str, sizeof(ep_str), "%d Episodes", feeds[i].episode_count);

				// Pre-calculate badge width so pill can account for it
				char new_label[16];
				int badge_extra = 0;
				if (feeds[i].new_episode_count > 0) {
					snprintf(new_label, sizeof(new_label), "%d New", feeds[i].new_episode_count);
					int label_w = 0;
					TTF_SizeUTF8(font.tiny, new_label, &label_w, NULL);
					badge_extra = SCALE1(4) + label_w + SCALE1(6); // gap + text + pill padding
				}

				ListItemRichPos rpos = render_rich_list_item(screen, &pill_layout, feeds[i].title, ep_str, feeds[i].feed_id, NULL, y, is_selected, badge_extra);

				// Render "N New" badge after subtitle text
				if (feeds[i].new_episode_count > 0) {
					int sub_tw = 0;
					TTF_SizeUTF8(font.small, ep_str, &sub_tw, NULL);
					int small_h = TTF_FontHeight(font.small);

					SDL_Surface* new_surf = TTF_RenderUTF8_Blended(font.tiny, new_label, COLOR_WHITE);
					if (new_surf) {
						int badge_h = new_surf->h + SCALE1(2);
						int badge_w = new_surf->w + SCALE1(6);
						int badge_x = rpos.subtitle_x + sub_tw + SCALE1(4);
						int badge_y = rpos.subtitle_y + (small_h - badge_h) / 2;
						SDL_Rect bg = {badge_x, badge_y, badge_w, badge_h};
						SDL_FillRect(screen, &bg, THEME_COLOR2);
						SDL_BlitSurface(new_surf, NULL, screen,
										&(SDL_Rect){badge_x + SCALE1(3), badge_y + SCALE1(1)});
						SDL_FreeSurface(new_surf);
					}
				}
			}
			cy += sub_item_h;
		}
	}

	// --- Downloads item ---
	if (has_downloads_item) {
		if (cl_count > 0 || sub_count > 0)
			cy += SCALE1(18); // Gap
		int dl_idx = cl_count + sub_count;
		bool dl_selected = (dl_idx == selected);
		int y = draw_offset + cy;

		if (y + sub_item_h > base_y && y < base_y + viewport_h) {
			char dl_subtitle[64];
			snprintf(dl_subtitle, sizeof(dl_subtitle), "%d Episode%s", dl_queue_count, dl_queue_count != 1 ? "s" : "");

			char truncated_dl[256];
			ListItemBadgedPos pos = UI_renderListItemPillBadged(screen, &pill_layout, font.medium, font.small, font.tiny, "Downloads", dl_subtitle, truncated_dl, y, dl_selected, 0, 0);

			UI_renderListItemText(screen, dl_selected ? &podcast_title_scroll : NULL,
								  "Downloads", font.medium,
								  pos.text_x, pos.text_y, pos.text_max_width, dl_selected);

			SDL_Surface* sub_surf = TTF_RenderUTF8_Blended(font.small, dl_subtitle, COLOR_GRAY);
			if (sub_surf) {
				int avail_w = pos.text_max_width;
				SDL_Rect src = {0, 0, sub_surf->w > avail_w ? avail_w : sub_surf->w, sub_surf->h};
				SDL_BlitSurface(sub_surf, &src, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
				SDL_FreeSurface(sub_surf);
			}
		}
		cy += sub_item_h;
	}

	SDL_SetClipRect(screen, NULL);

	// Button hints — context dependent
	if (has_downloads_item && selected == cl_count + sub_count) {
		// Downloads item selected
		UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "OPEN", "Y", "MANAGE", NULL});
	} else if (selected < cl_count) {
		// Continue listening item selected
		UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "PLAY", "Y", "MANAGE", NULL});
	} else {
		// Subscription item selected
		UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "SELECT", "Y", "MANAGE", NULL});
	}

	// Toast
	UI_renderToast(screen, toast_message, toast_time);
}

// Render the podcast management menu (Y button opens this)
void render_podcast_manage(SDL_Surface* screen, IndicatorType show_setting,
						   int menu_selected, int subscription_count) {
	GFX_clear(screen);

	char truncated[256];
	char label_buf[128];

	UI_renderMenuBar(screen, "Manage Podcasts");

	// Use common list layout
	ListLayout layout = UI_calcListLayout(screen);

	for (int i = 0; i < PODCAST_MANAGE_COUNT; i++) {
		bool selected = (i == menu_selected);
		const char* item_label = podcast_manage_items[i];

		// Render menu item pill
		MenuItemPos pos = UI_renderMenuItemPill(screen, &layout, item_label, truncated, i, selected, 0);

		// Render text using standard list item text (consistent colors and font)
		UI_renderListItemText(screen, NULL, truncated, font.large,
							  pos.text_x, pos.text_y, layout.max_width, selected);
	}

	// Button hints
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "SELECT", NULL});
}

// Render Top Shows list
void render_podcast_top_shows(SDL_Surface* screen, IndicatorType show_setting,
							  int selected, int* scroll,
							  const char* toast_message, uint32_t toast_time) {
	GFX_clear(screen);

	int hw = screen->w;

	UI_renderMenuBar(screen, "Top Shows");

	const PodcastChartsStatus* status = Podcast_getChartsStatus();

	// Loading state
	if (status->loading) {
		int center_y = screen->h / 2;
		const char* msg = "Loading...";
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
		if (text) {
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, center_y});
			SDL_FreeSurface(text);
		}
		return;
	}

	int count = 0;
	PodcastChartItem* items = Podcast_getTopShows(&count);

	// Empty state
	if (count == 0) {
		int center_y = screen->h / 2 - SCALE1(15);
		const char* msg = status->error_message[0] ? status->error_message : "No shows available";
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
		if (text) {
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, center_y});
			SDL_FreeSurface(text);
		}
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		return;
	}

	// List layout (rich 2-row items)
	ListLayout layout = UI_calcListLayout(screen);
	layout.item_h = SCALE1(PILL_SIZE) * 3 / 2;
	layout.items_per_page = layout.list_h / layout.item_h;
	UI_adjustListScroll(selected, scroll, layout.items_per_page);

	int thumb_size = SCALE1(PILL_SIZE) * 3 / 2 - SCALE1(4) * 2; // same as image_size in pill_rich
	for (int i = 0; i < layout.items_per_page && *scroll + i < count; i++) {
		int idx = *scroll + i;
		PodcastChartItem* item = &items[idx];
		bool is_selected = (idx == selected);

		int y = layout.list_y + i * layout.item_h;

		render_rich_list_item(screen, &layout, item->title, item->author,
							  NULL, item->itunes_id, y, is_selected, 0);
	}

	// Lazy fetch: fetch one uncached artwork per frame for visible items
	for (int i = 0; i < layout.items_per_page && *scroll + i < count; i++) {
		PodcastChartItem* item = &items[*scroll + i];
		if (artwork_fetch_one(item->itunes_id, item->artwork_url, thumb_size))
			break;
	}

	UI_renderScrollIndicators(screen, *scroll, layout.items_per_page, count);

	// Check if selected item is already subscribed (by iTunes ID)
	bool selected_is_subscribed = false;
	if (selected < count && items[selected].itunes_id[0]) {
		selected_is_subscribed = Podcast_isSubscribedByItunesId(items[selected].itunes_id);
	}

	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", selected_is_subscribed ? "UNSUBSCRIBE" : "SUBSCRIBE", NULL});

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);
}

// Render search results
void render_podcast_search_results(SDL_Surface* screen, IndicatorType show_setting,
								   int selected, int* scroll,
								   const char* toast_message, uint32_t toast_time) {
	GFX_clear(screen);

	int hw = screen->w;

	UI_renderMenuBar(screen, "Search Results");

	const PodcastSearchStatus* status = Podcast_getSearchStatus();

	// Searching state
	if (status->searching) {
		int center_y = screen->h / 2;
		const char* msg = "Searching...";
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
		if (text) {
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, center_y});
			SDL_FreeSurface(text);
		}
		return;
	}

	int count = 0;
	PodcastSearchResult* results = Podcast_getSearchResults(&count);

	// Empty/error state
	if (count == 0) {
		int center_y = screen->h / 2 - SCALE1(15);
		const char* msg = status->error_message[0] ? status->error_message : "No results found";
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
		if (text) {
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, center_y});
			SDL_FreeSurface(text);
		}
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		return;
	}

	// List layout (rich 2-row items)
	ListLayout layout = UI_calcListLayout(screen);
	layout.item_h = SCALE1(PILL_SIZE) * 3 / 2;
	layout.items_per_page = layout.list_h / layout.item_h;
	UI_adjustListScroll(selected, scroll, layout.items_per_page);

	// Check if selected item is already subscribed
	bool selected_is_subscribed = false;
	if (selected < count && results[selected].feed_url[0]) {
		selected_is_subscribed = Podcast_isSubscribed(results[selected].feed_url);
	}

	int thumb_size = SCALE1(PILL_SIZE) * 3 / 2 - SCALE1(4) * 2;
	for (int i = 0; i < layout.items_per_page && *scroll + i < count; i++) {
		int idx = *scroll + i;
		PodcastSearchResult* result = &results[idx];
		bool is_selected = (idx == selected);

		int y = layout.list_y + i * layout.item_h;

		render_rich_list_item(screen, &layout, result->title, result->author,
							  NULL, result->itunes_id, y, is_selected, 0);
	}

	// Lazy fetch: fetch one uncached artwork per frame for visible items
	for (int i = 0; i < layout.items_per_page && *scroll + i < count; i++) {
		PodcastSearchResult* result = &results[*scroll + i];
		if (artwork_fetch_one(result->itunes_id, result->artwork_url, thumb_size))
			break;
	}

	UI_renderScrollIndicators(screen, *scroll, layout.items_per_page, count);

	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", selected_is_subscribed ? "UNSUBSCRIBE" : "SUBSCRIBE", NULL});

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);
}

// Render episode list for a feed
void render_podcast_episodes(SDL_Surface* screen, IndicatorType show_setting,
							 int feed_index, int selected, int* scroll,
							 const char* toast_message, uint32_t toast_time) {
	GFX_clear(screen);

	int hw = screen->w;
	char truncated[256];

	PodcastFeed* feed = Podcast_getSubscription(feed_index);
	if (!feed) {
		UI_renderMenuBar(screen, "Episodes");
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		return;
	}

	int count = feed->episode_count;

	UI_renderMenuBar(screen, "Episodes");

	// Viewport (below fixed header, above buttons)
	int base_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
	int viewport_h = screen->h - base_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN + 8);
	int pad = SCALE1(PADDING);

	// Content dimensions (info area + episodes — all scrollable together)
	int info_area_h = SCALE1(PILL_SIZE) * 9 / 2 - base_y;
	int item_h = SCALE1(PILL_SIZE) * 3 / 2;
	// First episode sits at bottom of viewport when scroll=0
	int episodes_start = viewport_h - item_h;
	int total_content_h = episodes_start + count * item_h;

	// Empty state — show info without scrolling
	if (count == 0) {
		// Render info area at fixed position
		int img_pad = SCALE1(2);
		int img_size = info_area_h - img_pad * 2;
		SDL_Surface* header_art = get_episode_header_art(feed->feed_id, img_size);
		bool has_art = (header_art != NULL);
		if (has_art) {
			SDL_Rect art_dst = {pad, base_y + img_pad, img_size, img_size};
			SDL_BlitScaled(header_art, NULL, screen, &art_dst);
		}
		int text_x = has_art ? (pad + img_size + SCALE1(8)) : pad;
		int text_max_w = hw - text_x - pad;
		int ty = base_y + img_pad;
		GFX_truncateText(font.medium, feed->title, truncated, text_max_w, 0);
		SDL_Surface* t = TTF_RenderUTF8_Blended(font.medium, truncated, COLOR_WHITE);
		if (t) {
			SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){text_x, ty});
			ty += t->h + SCALE1(1);
			SDL_FreeSurface(t);
		}
		if (feed->author[0]) {
			GFX_truncateText(font.small, feed->author, truncated, text_max_w, 0);
			SDL_Surface* a = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_GRAY);
			if (a) {
				SDL_BlitSurface(a, NULL, screen, &(SDL_Rect){text_x, ty});
				SDL_FreeSurface(a);
			}
		}
		int center_y = base_y + info_area_h + (viewport_h - info_area_h) / 2;
		const char* msg = "No episodes available";
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
		if (text) {
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, center_y});
			SDL_FreeSurface(text);
		}
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		return;
	}

	// Scroll adjustment: keep selected episode visible
	// When selected == 0 (first item or wrapped to top), show info area
	{
		int sel_y = episodes_start + selected * item_h;
		int sel_bottom = sel_y + item_h;

		if (selected == 0) {
			*scroll = 0;
		} else {
			if (sel_bottom - *scroll > viewport_h)
				*scroll = sel_bottom - viewport_h;
			if (sel_y < *scroll)
				*scroll = sel_y;
		}
	}

	// Clamp scroll
	if (*scroll < 0)
		*scroll = 0;
	if (total_content_h > viewport_h) {
		if (*scroll > total_content_h - viewport_h)
			*scroll = total_content_h - viewport_h;
	} else {
		*scroll = 0;
	}

	int draw_offset = base_y - *scroll;

	// Set clip rect for scrollable area
	SDL_Rect clip = {0, base_y, hw, viewport_h};
	SDL_SetClipRect(screen, &clip);

	// === Info Area (content_y = 0, scrolls with episodes) ===
	{
		int info_sy = draw_offset; // screen y of info area top
		if (info_sy + info_area_h > base_y && info_sy < base_y + viewport_h) {
			int img_pad = SCALE1(2);
			int img_size = info_area_h - img_pad * 2;
			SDL_Surface* header_art = get_episode_header_art(feed->feed_id, img_size);
			bool has_art = (header_art != NULL);

			if (has_art) {
				SDL_Rect art_dst = {pad, info_sy + img_pad, img_size, img_size};
				SDL_BlitScaled(header_art, NULL, screen, &art_dst);
			}

			int text_x = has_art ? (pad + img_size + SCALE1(8)) : pad;
			int text_max_w = hw - text_x - pad;
			int ty = info_sy + img_pad;
			int info_bottom = info_sy + info_area_h;

			// Title (medium font, white)
			{
				GFX_truncateText(font.medium, feed->title, truncated, text_max_w, 0);
				SDL_Surface* t = TTF_RenderUTF8_Blended(font.medium, truncated, COLOR_WHITE);
				if (t) {
					SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){text_x, ty});
					ty += t->h + SCALE1(1);
					SDL_FreeSurface(t);
				}
			}

			// Author (small font, gray)
			if (feed->author[0]) {
				GFX_truncateText(font.small, feed->author, truncated, text_max_w, 0);
				SDL_Surface* a = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_GRAY);
				if (a) {
					SDL_BlitSurface(a, NULL, screen, &(SDL_Rect){text_x, ty});
					ty += a->h + SCALE1(2);
					SDL_FreeSurface(a);
				}
			}

			// Description (tiny font, gray, word-wrapped up to 3 lines)
			if (feed->description[0]) {
				TTF_Font* desc_font = font.tiny;
				char desc_buf[512];
				int di;
				for (di = 0; di < 511 && feed->description[di] && feed->description[di] != '\n' && feed->description[di] != '\r'; di++)
					desc_buf[di] = feed->description[di];
				desc_buf[di] = '\0';

				int desc_line_h = TTF_FontHeight(desc_font);
				const char* remaining = desc_buf;
				int max_lines = 3;

				for (int line = 0; line < max_lines && *remaining; line++) {
					if (ty + desc_line_h > info_bottom)
						break;

					int tw;
					TTF_SizeUTF8(desc_font, remaining, &tw, NULL);

					if (tw <= text_max_w || line == max_lines - 1) {
						GFX_truncateText(desc_font, remaining, truncated, text_max_w, 0);
						SDL_Surface* d = TTF_RenderUTF8_Blended(desc_font, truncated, COLOR_GRAY);
						if (d) {
							SDL_BlitSurface(d, NULL, screen, &(SDL_Rect){text_x, ty});
							ty += d->h;
							SDL_FreeSurface(d);
						}
						break;
					}

					const char* p = remaining;
					const char* last_break = remaining;
					while (*p) {
						while (*p && *p != ' ')
							p++;
						int seg_len = p - remaining;
						char measure[512];
						if (seg_len >= 512)
							seg_len = 511;
						memcpy(measure, remaining, seg_len);
						measure[seg_len] = '\0';
						TTF_SizeUTF8(desc_font, measure, &tw, NULL);
						if (tw > text_max_w)
							break;
						last_break = p;
						while (*p == ' ')
							p++;
					}

					if (last_break == remaining)
						break;

					int line_len = last_break - remaining;
					char line_buf[512];
					if (line_len >= 512)
						line_len = 511;
					memcpy(line_buf, remaining, line_len);
					line_buf[line_len] = '\0';

					SDL_Surface* d = TTF_RenderUTF8_Blended(desc_font, line_buf, COLOR_GRAY);
					if (d) {
						SDL_BlitSurface(d, NULL, screen, &(SDL_Rect){text_x, ty});
						ty += d->h;
						SDL_FreeSurface(d);
					}

					remaining = last_break;
					while (*remaining == ' ')
						remaining++;
				}
			}
		}
	}

	// === Episodes (pixel-based positioning) ===
	ListLayout layout;
	layout.item_h = item_h;
	layout.max_width = hw - SCALE1(PADDING * 2);

	// Check download status of selected episode for button hints
	int selected_download_status = -1;
	int selected_progress = 0;
	bool selected_is_downloaded = false;
	bool selected_is_resumable = false;
	if (selected < count) {
		PodcastEpisode* sel_ep = Podcast_getEpisode(feed_index, selected);
		if (sel_ep) {
			selected_download_status = Podcast_getEpisodeDownloadStatus(feed->feed_url, sel_ep->guid, &selected_progress);
			selected_is_downloaded = Podcast_episodeFileExists(feed, selected);
			selected_is_resumable = (sel_ep->progress_sec > 0);
		}
	}

	for (int i = 0; i < count; i++) {
		int ep_cy = episodes_start + i * item_h; // content y
		int y = draw_offset + ep_cy;			 // screen y

		// Skip if entirely outside viewport
		if (y + item_h <= base_y || y >= base_y + viewport_h)
			continue;

		PodcastEpisode* ep = Podcast_getEpisode(feed_index, i);
		if (!ep)
			continue;

		bool is_selected = (i == selected);

		// Check episode download status
		int dl_progress = 0;
		int dl_status = Podcast_getEpisodeDownloadStatus(feed->feed_url, ep->guid, &dl_progress);

		// Determine badge info
		bool is_downloaded = Podcast_episodeFileExists(feed, i);
		bool is_played = (ep->progress_sec == -1);
		bool has_progress = (ep->progress_sec > 0);

		// Badge icons: complete icon if played, download icon if not downloaded
		int badge_icon_size = SCALE1(14);
		int num_badges = 0;
		if (is_played)
			num_badges++;
		if (!is_downloaded)
			num_badges++;
		int badge_width = num_badges > 0 ? num_badges * badge_icon_size : 0;

		// Two-layer capsule pill with subtitle inside
		ListItemBadgedPos pos = UI_renderListItemPillBadged(screen, &layout, font.medium, font.small, font.tiny, ep->title, NULL, truncated, y, is_selected, badge_width, 0);

		// Title text (row 1)
		UI_renderListItemText(screen, is_selected ? &podcast_title_scroll : NULL,
							  ep->title, font.medium,
							  pos.text_x, pos.text_y,
							  pos.text_max_width, is_selected);

		// Render badge icons
		if (num_badges > 0) {
			int bx = pos.badge_x;
			int by = y + (layout.item_h - badge_icon_size) / 2;
			if (is_played) {
				SDL_Surface* icon = Icons_getComplete(false);
				if (icon) {
					SDL_Rect src = {0, 0, icon->w, icon->h};
					SDL_Rect dst = {bx, by, badge_icon_size, badge_icon_size};
					SDL_BlitScaled(icon, &src, screen, &dst);
					bx += badge_icon_size + SCALE1(2);
				}
			}
			if (!is_downloaded) {
				SDL_Surface* icon = Icons_getDownload(false);
				if (icon) {
					SDL_Rect src = {0, 0, icon->w, icon->h};
					SDL_Rect dst = {bx, by, badge_icon_size, badge_icon_size};
					SDL_BlitScaled(icon, &src, screen, &dst);
				}
			}
		}

		// Subtitle (row 2)
		int small_h = TTF_FontHeight(font.small);
		int subtitle_x_offset = 0;

		// Render "New" badge pill if episode is new
		if (ep->is_new) {
			SDL_Surface* new_surf = TTF_RenderUTF8_Blended(font.tiny, "New", COLOR_WHITE);
			if (new_surf) {
				int badge_h = new_surf->h + SCALE1(2);
				int badge_w = new_surf->w + SCALE1(6);
				int badge_y = pos.subtitle_y + (small_h - badge_h) / 2;
				// Draw theme-colored pill background
				SDL_Rect bg = {pos.subtitle_x, badge_y, badge_w, badge_h};
				SDL_FillRect(screen, &bg, THEME_COLOR2);
				// Draw text centered in pill
				SDL_BlitSurface(new_surf, NULL, screen,
								&(SDL_Rect){pos.subtitle_x + SCALE1(3), badge_y + SCALE1(1)});
				SDL_FreeSurface(new_surf);
				subtitle_x_offset = badge_w + SCALE1(4);
			}
		}

		if (dl_status == PODCAST_DOWNLOAD_DOWNLOADING) {
			int bar_w = SCALE1(50);
			int bar_h = SCALE1(4);
			int bar_x = pos.subtitle_x + subtitle_x_offset;
			int bar_y = pos.subtitle_y + (small_h - bar_h) / 2;
			SDL_Rect bar_bg = {bar_x, bar_y, bar_w, bar_h};
			SDL_FillRect(screen, &bar_bg, SDL_MapRGB(screen->format, 60, 60, 60));
			int fill_w = (bar_w * dl_progress) / 100;
			if (fill_w > 0) {
				SDL_Rect bar_fill = {bar_x, bar_y, fill_w, bar_h};
				SDL_FillRect(screen, &bar_fill, THEME_COLOR2);
			}
		} else if (dl_status == PODCAST_DOWNLOAD_PENDING) {
			SDL_Surface* queued_surf = TTF_RenderUTF8_Blended(font.small, "Queued", COLOR_GRAY);
			if (queued_surf) {
				int avail_w = pos.text_max_width - subtitle_x_offset;
				SDL_Rect src = {0, 0, queued_surf->w > avail_w ? avail_w : queued_surf->w, queued_surf->h};
				SDL_BlitSurface(queued_surf, &src, screen,
								&(SDL_Rect){pos.subtitle_x + subtitle_x_offset, pos.subtitle_y});
				SDL_FreeSurface(queued_surf);
			}
		} else if (has_progress && ep->duration_sec > 0) {
			char progress_str[64];
			format_duration_pair(progress_str, ep->progress_sec, ep->duration_sec);
			char date_str[32];
			format_date(date_str, ep->pub_date);
			if (date_str[0]) {
				char combined[96];
				snprintf(combined, sizeof(combined), "%s | %s", progress_str, date_str);
				strcpy(progress_str, combined);
			}
			SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, progress_str, COLOR_GRAY);
			if (s) {
				int avail_w = pos.text_max_width - subtitle_x_offset;
				SDL_Rect src = {0, 0, s->w > avail_w ? avail_w : s->w, s->h};
				SDL_BlitSurface(s, &src, screen,
								&(SDL_Rect){pos.subtitle_x + subtitle_x_offset, pos.subtitle_y});
				SDL_FreeSurface(s);
			}
		} else {
			char subtitle_str[64] = {0};
			if (ep->duration_sec > 0) {
				format_duration(subtitle_str, ep->duration_sec);
			}
			char date_str[32];
			format_date(date_str, ep->pub_date);
			if (date_str[0]) {
				if (subtitle_str[0]) {
					char combined[96];
					snprintf(combined, sizeof(combined), "%s | %s", subtitle_str, date_str);
					strcpy(subtitle_str, combined);
				} else {
					strcpy(subtitle_str, date_str);
				}
			}
			if (subtitle_str[0]) {
				SDL_Surface* sub_surf = TTF_RenderUTF8_Blended(font.small, subtitle_str, COLOR_GRAY);
				if (sub_surf) {
					int avail_w = pos.text_max_width - subtitle_x_offset;
					SDL_Rect src = {0, 0, sub_surf->w > avail_w ? avail_w : sub_surf->w, sub_surf->h};
					SDL_BlitSurface(sub_surf, &src, screen,
									&(SDL_Rect){pos.subtitle_x + subtitle_x_offset, pos.subtitle_y});
					SDL_FreeSurface(sub_surf);
				}
			}
		}
	}

	SDL_SetClipRect(screen, NULL);

	const char* action_label =
		(selected_download_status == PODCAST_DOWNLOAD_DOWNLOADING ||
		 selected_download_status == PODCAST_DOWNLOAD_PENDING)
			? "CANCEL"
		: selected_is_downloaded ? (selected_is_resumable ? "RESUME" : "PLAY")
								 : "DOWNLOAD";
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", (char*)action_label, "Y", "REFRESH", NULL});

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);
}

// Format download speed for display
static void format_speed(char* buf, int buf_size, int bytes_per_sec) {
	if (bytes_per_sec <= 0) {
		snprintf(buf, buf_size, "0 B/s");
	} else if (bytes_per_sec < 1024) {
		snprintf(buf, buf_size, "%d B/s", bytes_per_sec);
	} else if (bytes_per_sec < 1024 * 1024) {
		snprintf(buf, buf_size, "%.1f KB/s", bytes_per_sec / 1024.0);
	} else {
		snprintf(buf, buf_size, "%.1f MB/s", bytes_per_sec / (1024.0 * 1024.0));
	}
}

// Format ETA for display
static void format_eta(char* buf, int buf_size, int seconds) {
	if (seconds <= 0) {
		buf[0] = '\0';
	} else if (seconds < 60) {
		snprintf(buf, buf_size, "%ds", seconds);
	} else if (seconds < 3600) {
		snprintf(buf, buf_size, "%dm%ds", seconds / 60, seconds % 60);
	} else {
		snprintf(buf, buf_size, "%dh%dm", seconds / 3600, (seconds % 3600) / 60);
	}
}

// Render download queue view
void render_podcast_download_queue(SDL_Surface* screen, IndicatorType show_setting,
								   int selected, int* scroll,
								   const char* toast_message, uint32_t toast_time) {
	GFX_clear(screen);

	int hw = screen->w;
	char truncated[256];

	int queue_count = 0;
	PodcastDownloadItem* queue = Podcast_getDownloadQueue(&queue_count);
	const PodcastDownloadProgress* progress = Podcast_getDownloadProgress();

	// Title with completion count
	char title[64];
	if (queue_count > 0) {
		snprintf(title, sizeof(title), "Downloads (%d/%d)",
				 progress->completed_count, progress->total_items);
	} else {
		snprintf(title, sizeof(title), "Downloads");
	}
	UI_renderMenuBar(screen, title);

	// Empty state
	if (queue_count == 0) {
		int center_y = screen->h / 2;
		const char* msg = "No downloads";
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
		if (text) {
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, center_y - text->h / 2});
			SDL_FreeSurface(text);
		}
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		UI_renderToast(screen, toast_message, toast_time);
		return;
	}

	// List layout — use taller item height for two-row pills (title + subtitle)
	ListLayout layout = UI_calcListLayout(screen);
	layout.item_h = SCALE1(PILL_SIZE) * 3 / 2;
	layout.items_per_page = layout.list_h / layout.item_h;
	if (layout.items_per_page > 5)
		layout.items_per_page = 5;
	UI_adjustListScroll(selected, scroll, layout.items_per_page);

	for (int i = 0; i < layout.items_per_page && *scroll + i < queue_count; i++) {
		int idx = *scroll + i;
		PodcastDownloadItem* item = &queue[idx];
		bool is_selected = (idx == selected);

		int y = layout.list_y + i * layout.item_h;

		// Two-layer pill with subtitle
		int badge_width = 0;
		ListItemBadgedPos pos = UI_renderListItemPillBadged(screen, &layout, font.medium, font.small, font.tiny, item->episode_title, item->feed_title, truncated, y, is_selected, badge_width, 0);

		// Title text (row 1)
		UI_renderListItemText(screen, is_selected ? &podcast_title_scroll : NULL,
							  item->episode_title, font.medium,
							  pos.text_x, pos.text_y,
							  pos.text_max_width, is_selected);

		// Subtitle (row 2) — status-dependent
		int small_h = TTF_FontHeight(font.small);
		(void)small_h;

		if (item->status == PODCAST_DOWNLOAD_DOWNLOADING) {
			// Progress bar + speed + ETA
			int bar_w = SCALE1(50);
			int bar_h = SCALE1(4);
			int bar_x = pos.subtitle_x;
			int bar_y = pos.subtitle_y + (TTF_FontHeight(font.small) - bar_h) / 2;

			// Bar background
			SDL_Rect bar_bg = {bar_x, bar_y, bar_w, bar_h};
			SDL_FillRect(screen, &bar_bg, SDL_MapRGB(screen->format, 60, 60, 60));

			// Bar fill
			int fill_w = (bar_w * item->progress_percent) / 100;
			if (fill_w > 0) {
				SDL_Rect bar_fill = {bar_x, bar_y, fill_w, bar_h};
				SDL_FillRect(screen, &bar_fill, THEME_COLOR2);
			}

			// Speed and ETA text after bar
			char info_str[64];
			char speed_str[32];
			char eta_str[32];
			format_speed(speed_str, sizeof(speed_str), progress->speed_bps);
			format_eta(eta_str, sizeof(eta_str), progress->eta_sec);

			if (eta_str[0]) {
				snprintf(info_str, sizeof(info_str), "%d%%  %s  ETA %s",
						 item->progress_percent, speed_str, eta_str);
			} else {
				snprintf(info_str, sizeof(info_str), "%d%%  %s",
						 item->progress_percent, speed_str);
			}

			SDL_Surface* info_surf = TTF_RenderUTF8_Blended(font.small, info_str, COLOR_GRAY);
			if (info_surf) {
				int info_x = bar_x + bar_w + SCALE1(6);
				int avail_w = pos.text_max_width - bar_w - SCALE1(6);
				SDL_Rect src = {0, 0, info_surf->w > avail_w ? avail_w : info_surf->w, info_surf->h};
				SDL_BlitSurface(info_surf, &src, screen,
								&(SDL_Rect){info_x, pos.subtitle_y});
				SDL_FreeSurface(info_surf);
			}
		} else if (item->status == PODCAST_DOWNLOAD_PENDING) {
			const char* label = "Queued";
			SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, label, COLOR_GRAY);
			if (s) {
				SDL_BlitSurface(s, NULL, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
				SDL_FreeSurface(s);
			}
		} else if (item->status == PODCAST_DOWNLOAD_FAILED) {
			const char* label = "[Failed]";
			if (item->retry_count > 0) {
				char fail_str[64];
				snprintf(fail_str, sizeof(fail_str), "[Failed after %d retries]", item->retry_count);
				SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, fail_str, (SDL_Color){200, 80, 80, 255});
				if (s) {
					int avail_w = pos.text_max_width;
					SDL_Rect src = {0, 0, s->w > avail_w ? avail_w : s->w, s->h};
					SDL_BlitSurface(s, &src, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
					SDL_FreeSurface(s);
				}
			} else {
				SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, label, (SDL_Color){200, 80, 80, 255});
				if (s) {
					SDL_BlitSurface(s, NULL, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
					SDL_FreeSurface(s);
				}
			}
		} else if (item->status == PODCAST_DOWNLOAD_COMPLETE) {
			const char* label = "Complete";
			SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, label, (SDL_Color){80, 200, 80, 255});
			if (s) {
				SDL_BlitSurface(s, NULL, screen, &(SDL_Rect){pos.subtitle_x, pos.subtitle_y});
				SDL_FreeSurface(s);
			}
		}

		// Feed title as secondary info (tiny font, below subtitle)
		if (item->feed_title[0]) {
			GFX_truncateText(font.tiny, item->feed_title, truncated, pos.text_max_width, 0);
			// Render as part of subtitle line if space allows
		}
	}

	// Scroll indicators
	UI_renderScrollIndicators(screen, *scroll, layout.items_per_page, queue_count);

	char* hint_pairs[16];
	int h = 0;
	if (queue_count > 0) {
		hint_pairs[h++] = "X";
		hint_pairs[h++] = "REMOVE";
	}
	hint_pairs[h++] = "B";
	hint_pairs[h++] = "BACK";
	hint_pairs[h++] = "START";
	hint_pairs[h++] = "CONTROLS";
	hint_pairs[h] = NULL;
	UI_renderButtonHintBar(screen, hint_pairs);

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);
}

// Render now playing screen for podcast (matches radio/music player style)
void render_podcast_playing(SDL_Surface* screen, IndicatorType show_setting,
							int feed_index, int episode_index) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;
	char truncated[256];

	PodcastFeed* feed = Podcast_getSubscription(feed_index);
	PodcastEpisode* ep = Podcast_getEpisode(feed_index, episode_index);

	if (!feed || !ep) {
		UI_renderMenuBar(screen, "Now Playing");
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		return;
	}

	// Fetch and render album art background (if available)
	if (feed->artwork_url[0] && feed->feed_id[0]) {
		podcast_fetch_artwork(feed->artwork_url, feed->feed_id);
		if (podcast_artwork && podcast_artwork->w > 0 && podcast_artwork->h > 0) {
			render_album_art_background(screen, podcast_artwork);
		}
	}

	// === TOP BAR ===
	int top_y = SCALE1(PADDING);

	// Badge
	const char* badge_text = "PODCAST";
	SDL_Surface* badge_surf = TTF_RenderUTF8_Blended(font.tiny, badge_text, COLOR_GRAY);
	int badge_h = badge_surf ? badge_surf->h + SCALE1(4) : SCALE1(16);
	int badge_x = SCALE1(PADDING);
	int badge_w = 0;

	if (badge_surf) {
		badge_w = badge_surf->w + SCALE1(10);
		// Draw border (gray)
		SDL_Rect border = {badge_x, top_y, badge_w, badge_h};
		SDL_FillRect(screen, &border, RGB_GRAY);
		SDL_Rect inner = {badge_x + 1, top_y + 1, badge_w - 2, badge_h - 2};
		SDL_FillRect(screen, &inner, RGB_BLACK);
		SDL_BlitSurface(badge_surf, NULL, screen, &(SDL_Rect){badge_x + SCALE1(5), top_y + SCALE1(2)});
		SDL_FreeSurface(badge_surf);
	}

	// Episode counter "01 / 67" (like track counter in music player)
	// Show position among downloaded episodes, not total episodes
	int downloaded_total = Podcast_countDownloadedEpisodes(feed_index);
	int downloaded_idx = Podcast_getDownloadedEpisodeIndex(feed_index, episode_index);
	char ep_counter[32];
	if (downloaded_idx >= 0 && downloaded_total > 0) {
		snprintf(ep_counter, sizeof(ep_counter), "%02d / %02d", downloaded_idx + 1, downloaded_total);
	} else {
		// Fallback if episode is not downloaded (shouldn't happen in playing state)
		snprintf(ep_counter, sizeof(ep_counter), "%02d / %02d", episode_index + 1, feed->episode_count);
	}
	SDL_Surface* counter_surf = TTF_RenderUTF8_Blended(font.tiny, ep_counter, COLOR_GRAY);
	if (counter_surf) {
		int counter_x = badge_x + badge_w + SCALE1(8);
		int counter_y = top_y + (badge_h - counter_surf->h) / 2;
		SDL_BlitSurface(counter_surf, NULL, screen, &(SDL_Rect){counter_x, counter_y});
		SDL_FreeSurface(counter_surf);
	}

	// Hardware status (clock, battery) on right
	GFX_blitHardwareGroup(screen, show_setting);

	// === PODCAST INFO SECTION (like music player artist/title/album) ===
	int info_y = SCALE1(PADDING + 45);
	int max_w_text = hw - SCALE1(PADDING * 2);

	// Podcast name (like Artist in music player) - gray, artist font
	GFX_truncateText(font.medium, feed->title, truncated, max_w_text, 0);
	SDL_Surface* podcast_surf = TTF_RenderUTF8_Blended(font.medium, truncated, COLOR_GRAY);
	if (podcast_surf) {
		SDL_BlitSurface(podcast_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
		info_y += podcast_surf->h + SCALE1(2);
		SDL_FreeSurface(podcast_surf);
	} else {
		info_y += SCALE1(18);
	}

	// Episode title (like Title in music player) - white, title font, with scrolling
	const char* title = ep->title[0] ? ep->title : "Unknown Episode";
	int title_y = info_y;

	// Check if text changed and reset scroll state
	if (strcmp(podcast_playing_title_scroll.text, title) != 0) {
		ScrollText_reset(&podcast_playing_title_scroll, title, font.title, max_w_text, true);
	}

	// Activate scroll after delay (this render path bypasses ScrollText_render)
	ScrollText_activateAfterDelay(&podcast_playing_title_scroll);

	// If text needs scrolling, use GPU layer
	if (podcast_playing_title_scroll.needs_scroll) {
		ScrollText_renderGPU_NoBg(&podcast_playing_title_scroll, font.title, COLOR_WHITE, SCALE1(PADDING), title_y);
	} else {
		// Static text - render to screen surface
		PLAT_clearLayers(LAYER_SCROLLTEXT);
		SDL_Surface* title_surf = TTF_RenderUTF8_Blended(font.title, title, COLOR_WHITE);
		if (title_surf) {
			SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), title_y, 0, 0});
			SDL_FreeSurface(title_surf);
		}
	}
	info_y += TTF_FontHeight(font.title) + SCALE1(2);

	// Episode description (word-wrapped, up to 4 lines)
	if (ep->description[0]) {
		TTF_Font* desc_font = font.small;
		int desc_line_h = TTF_FontHeight(desc_font);
		int max_lines = 4;

		// Strip HTML tags and newlines from description
		char desc_buf[512];
		int di = 0;
		bool in_tag = false;
		for (const char* sp = ep->description; *sp && di < 511; sp++) {
			if (*sp == '<') {
				in_tag = true;
				continue;
			}
			if (*sp == '>') {
				in_tag = false;
				continue;
			}
			if (in_tag)
				continue;
			if (*sp == '\n' || *sp == '\r') {
				desc_buf[di++] = ' ';
				continue;
			}
			if (*sp == '&') {
				if (strncmp(sp, "&amp;", 5) == 0) {
					desc_buf[di++] = '&';
					sp += 4;
				} else if (strncmp(sp, "&lt;", 4) == 0) {
					desc_buf[di++] = '<';
					sp += 3;
				} else if (strncmp(sp, "&gt;", 4) == 0) {
					desc_buf[di++] = '>';
					sp += 3;
				} else if (strncmp(sp, "&quot;", 6) == 0) {
					desc_buf[di++] = '"';
					sp += 5;
				} else if (strncmp(sp, "&apos;", 6) == 0) {
					desc_buf[di++] = '\'';
					sp += 5;
				} else if (strncmp(sp, "&#39;", 5) == 0) {
					desc_buf[di++] = '\'';
					sp += 4;
				} else if (strncmp(sp, "&nbsp;", 6) == 0) {
					desc_buf[di++] = ' ';
					sp += 5;
				} else
					desc_buf[di++] = '&';
				continue;
			}
			desc_buf[di++] = *sp;
		}
		desc_buf[di] = '\0';

		const char* remaining = desc_buf;
		for (int line = 0; line < max_lines && *remaining; line++) {
			int tw;
			TTF_SizeUTF8(desc_font, remaining, &tw, NULL);

			if (tw <= max_w_text || line == max_lines - 1) {
				GFX_truncateText(desc_font, remaining, truncated, max_w_text, 0);
				SDL_Surface* d = TTF_RenderUTF8_Blended(desc_font, truncated, COLOR_GRAY);
				if (d) {
					SDL_BlitSurface(d, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
					info_y += d->h;
					SDL_FreeSurface(d);
				}
				break;
			}

			const char* p = remaining;
			const char* last_break = remaining;
			while (*p) {
				while (*p && *p != ' ')
					p++;
				int seg_len = p - remaining;
				char measure[512];
				if (seg_len >= 512)
					seg_len = 511;
				memcpy(measure, remaining, seg_len);
				measure[seg_len] = '\0';
				TTF_SizeUTF8(desc_font, measure, &tw, NULL);
				if (tw > max_w_text)
					break;
				last_break = p;
				while (*p == ' ')
					p++;
			}

			if (last_break == remaining)
				break;

			int line_len = last_break - remaining;
			char line_buf[512];
			if (line_len >= 512)
				line_len = 511;
			memcpy(line_buf, remaining, line_len);
			line_buf[line_len] = '\0';

			SDL_Surface* d = TTF_RenderUTF8_Blended(desc_font, line_buf, COLOR_GRAY);
			if (d) {
				SDL_BlitSurface(d, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
				info_y += d->h;
				SDL_FreeSurface(d);
			}

			remaining = last_break;
			while (*remaining == ' ')
				remaining++;
		}
	}

	// === PROGRESS BAR SECTION (GPU rendered) ===
	int bar_y = hh - SCALE1(35);
	int bar_h = SCALE1(4);
	int bar_margin = SCALE1(PADDING);
	int bar_w = hw - bar_margin * 2;
	int time_y = bar_y + SCALE1(8);

	// Get duration for GPU rendering
	int duration = Podcast_getDuration(); // Uses episode metadata duration

	// Set position for GPU rendering (actual rendering happens in main loop)
	PodcastProgress_setPosition(bar_margin, bar_y, bar_w, bar_h, time_y, hw, duration);
}

// Render loading screen
void render_podcast_loading(SDL_Surface* screen, const char* message) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;

	const char* msg = message ? message : "Loading...";
	SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
	if (text) {
		SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, hh / 2});
		SDL_FreeSurface(text);
	}
}

// Check if podcast title is currently scrolling (list or playing screen)
bool Podcast_isTitleScrolling(void) {
	if (ScrollText_isScrolling(&podcast_title_scroll))
		return true;
	// Only scroll playing title when playing, not when paused
	if (Player_getState() != PLAYER_STATE_PLAYING)
		return false;
	return ScrollText_isScrolling(&podcast_playing_title_scroll);
}

// Check if title scroll needs a render to transition from delay to active scrolling
// Returns true during the delay phase when text is wider than max_width but scrolling hasn't started
bool Podcast_titleScrollNeedsRender(void) {
	if (ScrollText_needsRender(&podcast_title_scroll))
		return true;
	if (ScrollText_needsRender(&podcast_playing_title_scroll))
		return true;
	return false;
}

// Animate podcast title scroll only (GPU mode, no screen redraw needed)
void Podcast_animateTitleScroll(void) {
	if (ScrollText_isScrolling(&podcast_title_scroll)) {
		ScrollText_animateOnly(&podcast_title_scroll);
	}
	// Only scroll playing title when playing, not when paused
	if (Player_getState() != PLAYER_STATE_PLAYING)
		return;
	if (ScrollText_isScrolling(&podcast_playing_title_scroll)) {
		ScrollText_renderGPU_NoBg(&podcast_playing_title_scroll,
								  podcast_playing_title_scroll.last_font,
								  podcast_playing_title_scroll.last_color,
								  podcast_playing_title_scroll.last_x,
								  podcast_playing_title_scroll.last_y);
	}
}

// Clear podcast title scroll state (call when selection changes or leaving page)
void Podcast_clearTitleScroll(void) {
	memset(&podcast_title_scroll, 0, sizeof(podcast_title_scroll));
	GFX_clearLayers(LAYER_SCROLLTEXT);
	GFX_resetScrollText(); // Also reset NextUI's internal scroll state
	PLAT_GPU_Flip();	   // Commit the layer clearing to the display
}

// === PODCAST PROGRESS GPU FUNCTIONS ===

void PodcastProgress_setPosition(int bar_x, int bar_y, int bar_w, int bar_h,
								 int time_y, int screen_w, int duration_ms) {
	progress_bar_x = bar_x;
	progress_bar_y = bar_y;
	progress_bar_w = bar_w;
	progress_bar_h = bar_h;
	progress_time_y = time_y;
	progress_screen_w = screen_w;
	progress_duration_ms = duration_ms;
	progress_position_set = true;
}

void PodcastProgress_clear(void) {
	progress_position_set = false;
	progress_last_position_sec = -1;
	PLAT_clearLayers(LAYER_PODCAST_PROGRESS);
}

bool PodcastProgress_needsRefresh(void) {
	if (!progress_position_set)
		return false;
	// Only update when playing, not when paused
	if (Player_getState() != PLAYER_STATE_PLAYING)
		return false;

	int position_ms = Player_getPosition();
	int position_sec = position_ms / 1000;

	// Only refresh if second changed
	return (position_sec != progress_last_position_sec);
}

void PodcastProgress_renderGPU(void) {
	if (!progress_position_set)
		return;

	int position_ms = Player_getPosition();
	int position_sec = position_ms / 1000;

	// Skip if nothing changed
	if (position_sec == progress_last_position_sec)
		return;

	progress_last_position_sec = position_sec;

	int duration_ms = progress_duration_ms > 0 ? progress_duration_ms : Podcast_getDuration();
	int bar_margin = progress_bar_x;

	// Calculate progress bar fill width
	int fill_w = 0;
	if (duration_ms > 0) {
		fill_w = (progress_bar_w * position_ms) / duration_ms;
		if (fill_w > progress_bar_w)
			fill_w = progress_bar_w;
	}

	// Create surface for progress bar + time text
	// Height: bar + gap + time text
	int time_gap = SCALE1(8);
	int time_h = TTF_FontHeight(font.tiny);
	int total_h = progress_bar_h + time_gap + time_h;

	SDL_Surface* combined = SDL_CreateRGBSurfaceWithFormat(0, progress_screen_w, total_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!combined)
		return;

	SDL_FillRect(combined, NULL, 0); // Transparent background

	// Draw progress bar background
	SDL_Rect bar_bg = {bar_margin, 0, progress_bar_w, progress_bar_h};
	SDL_FillRect(combined, &bar_bg, SDL_MapRGBA(combined->format, 60, 60, 60, 255));

	// Draw progress bar fill
	if (fill_w > 0) {
		SDL_Rect bar_fill = {bar_margin, 0, fill_w, progress_bar_h};
		SDL_FillRect(combined, &bar_fill, SDL_MapRGBA(combined->format, 255, 255, 255, 255));
	}

	// Render time texts
	char time_cur[16], time_dur[16];
	format_duration(time_cur, position_sec);
	format_duration(time_dur, duration_ms / 1000);

	SDL_Surface* cur_surf = TTF_RenderUTF8_Blended(font.tiny, time_cur, COLOR_GRAY);
	if (cur_surf) {
		SDL_BlitSurface(cur_surf, NULL, combined, &(SDL_Rect){bar_margin, progress_bar_h + time_gap});
		SDL_FreeSurface(cur_surf);
	}

	SDL_Surface* dur_surf = TTF_RenderUTF8_Blended(font.tiny, time_dur, COLOR_GRAY);
	if (dur_surf) {
		SDL_BlitSurface(dur_surf, NULL, combined, &(SDL_Rect){progress_screen_w - bar_margin - dur_surf->w, progress_bar_h + time_gap});
		SDL_FreeSurface(dur_surf);
	}

	// Clear previous and draw new
	PLAT_clearLayers(LAYER_PODCAST_PROGRESS);
	PLAT_drawOnLayer(combined, 0, progress_bar_y, progress_screen_w, total_h, 1.0f, false, LAYER_PODCAST_PROGRESS);
	SDL_FreeSurface(combined);

	PLAT_GPU_Flip();
}
