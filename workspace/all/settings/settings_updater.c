/*
 * settings_updater.c - System updater for NextUI Settings
 *
 * Checks GitHub for the latest release, compares against the installed
 * version, and allows the user to download + install updates.
 *
 * Single "Updater" item in the About page:
 *   - "Fetching update.."           while checking in background
 *   - "Install Update"  (desc=tag)  when update available, A to install
 *   - "You already have latest version"  when up to date
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

#include "settings_updater.h"
#include "settings_menu.h"
#include "defines.h"
#include "api.h"
#include "http.h"
#include "ui_components.h"

// ============================================
// Configuration
// ============================================

#define UPDATER_REPO_OWNER "mohammadsyuhada"
#define UPDATER_REPO_NAME "nextui-redux"
#define VERSION_FILE_PATH "/mnt/SDCARD/.system/version.txt"
#define DOWNLOAD_PATH "/tmp/nextui-update.zip"
#define EXTRACT_DEST "/mnt/SDCARD/"

// ============================================
// Release info
// ============================================

typedef struct {
	char tag_name[128];
	char commit_sha[64];
	char download_url[512];
	char release_notes[2048];
} ReleaseInfo;

// ============================================
// State
// ============================================

typedef enum {
	UPDATE_IDLE,
	UPDATE_CHECKING,
	UPDATE_UP_TO_DATE,
	UPDATE_AVAILABLE,
	UPDATE_ERROR,
} UpdateCheckState;

static UpdateCheckState auto_state = UPDATE_IDLE;
static ReleaseInfo cached_release = {0};
static char item_label[160] = "Updater";
static char item_desc[256] = "";
static char current_sha_cache[64] = "";

// Background check thread
static pthread_t auto_tid;
static volatile int auto_done = 0;
static volatile int auto_success = 0;
static HTTP_Response* auto_response = NULL;
static char auto_error[256] = "";

// ============================================
// JSON helpers
// ============================================

static const char* find_json_string(const char* json, const char* key, char* out, size_t out_size) {
	if (!json || !key || !out || out_size == 0)
		return NULL;

	char search[128];
	snprintf(search, sizeof(search), "\"%s\":\"", key);

	const char* start = strstr(json, search);
	if (!start) {
		snprintf(search, sizeof(search), "\"%s\": \"", key);
		start = strstr(json, search);
		if (!start)
			return NULL;
	}

	start += strlen(search);
	const char* end = strchr(start, '"');
	if (!end)
		return NULL;

	size_t len = end - start;
	if (len >= out_size)
		len = out_size - 1;

	strncpy(out, start, len);
	out[len] = '\0';

	return out;
}

static const char* find_zip_asset_url(const char* json, const char* platform, char* out, size_t out_size) {
	if (!json || !platform || !out || out_size == 0)
		return NULL;

	char suffix[64];
	snprintf(suffix, sizeof(suffix), "-%s.zip", platform);

	const char* assets = strstr(json, "\"assets\"");
	if (!assets)
		return NULL;

	const char* pos = assets;
	while ((pos = strstr(pos, "\"browser_download_url\"")) != NULL) {
		pos = strchr(pos + 21, '"');
		if (!pos)
			break;

		if (*(pos - 1) == ':') {
			// pos points to opening quote of value
		} else {
			pos++;
			while (*pos == ' ' || *pos == ':')
				pos++;
			if (*pos != '"')
				continue;
		}

		pos++;
		const char* end = strchr(pos, '"');
		if (!end)
			break;

		size_t len = end - pos;
		size_t suf_len = strlen(suffix);
		if (len > suf_len && strncmp(end - suf_len, suffix, suf_len) == 0) {
			if (len >= out_size)
				len = out_size - 1;
			strncpy(out, pos, len);
			out[len] = '\0';
			return out;
		}

		pos = end + 1;
	}

	return NULL;
}

static void extract_first_paragraph(const char* body, char* out, size_t out_size) {
	if (!body || !out || out_size == 0)
		return;

	out[0] = '\0';
	size_t j = 0;

	for (size_t i = 0; body[i] && j < out_size - 1; i++) {
		if (body[i] == '\\' && body[i + 1] == 'n') {
			if (body[i + 2] == '\\' && body[i + 3] == 'n')
				break;
			out[j++] = ' ';
			i++;
			continue;
		}
		if (body[i] == '\\' && body[i + 1] == 'r') {
			i++;
			continue;
		}
		if (body[i] == '#') {
			while (body[i] == '#')
				i++;
			if (body[i] == ' ')
				i++;
			i--;
			continue;
		}
		out[j++] = body[i];
	}

	out[j] = '\0';
	while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\n' || out[j - 1] == '\r'))
		out[--j] = '\0';
}

// ============================================
// Version reading
// ============================================

static void read_current_version(char* version, size_t ver_size,
								 char* sha, size_t sha_size,
								 char* tag, size_t tag_size) {
	version[0] = '\0';
	sha[0] = '\0';
	tag[0] = '\0';

	FILE* f = fopen(VERSION_FILE_PATH, "r");
	if (!f) {
		snprintf(version, ver_size, "Unknown");
		return;
	}

	if (!fgets(version, ver_size, f)) {
		snprintf(version, ver_size, "Unknown");
		fclose(f);
		return;
	}
	version[strcspn(version, "\r\n")] = '\0';

	if (!fgets(sha, sha_size, f))
		sha[0] = '\0';
	sha[strcspn(sha, "\r\n")] = '\0';

	if (!fgets(tag, tag_size, f))
		tag[0] = '\0';
	tag[strcspn(tag, "\r\n")] = '\0';

	fclose(f);
}

// ============================================
// Command execution (no shell, avoids injection)
// ============================================

static int run_command(char* const argv[]) {
	pid_t pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		freopen("/dev/null", "w", stderr);
		execvp(argv[0], argv);
		_exit(127);
	}

	int status;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return -1;
}

// ============================================
// Async context (for download/extract)
// ============================================

typedef struct {
	volatile int done;
	volatile int success;
	char error[256];
	ReleaseInfo* release;
} AsyncContext;

// ============================================
// Background auto-check thread
// ============================================

static void* auto_check_thread(void* arg) {
	(void)arg;

	char url[256];
	snprintf(url, sizeof(url),
			 "https://api.github.com/repos/%s/%s/releases/latest",
			 UPDATER_REPO_OWNER, UPDATER_REPO_NAME);

	auto_response = HTTP_get(url);

	if (!auto_response || auto_response->http_status != 200 || !auto_response->data) {
		if (auto_response && auto_response->error)
			snprintf(auto_error, sizeof(auto_error), "%.250s", auto_response->error);
		else
			snprintf(auto_error, sizeof(auto_error), "Failed to check for updates");
		auto_success = 0;
		__sync_synchronize();
		auto_done = 1;
		return NULL;
	}

	auto_success = 1;
	__sync_synchronize();
	auto_done = 1;
	return NULL;
}

// Parse the response and update state. Called from main thread.
static void process_auto_check_result(void) {
	pthread_join(auto_tid, NULL);

	if (!auto_success) {
		if (auto_response)
			HTTP_freeResponse(auto_response);
		auto_response = NULL;
		auto_state = UPDATE_ERROR;
		snprintf(item_label, sizeof(item_label), "Updater");
		snprintf(item_desc, sizeof(item_desc), "%s", auto_error);
		return;
	}

	HTTP_Response* response = auto_response;
	auto_response = NULL;
	ReleaseInfo release = {0};

	if (!find_json_string(response->data, "tag_name", release.tag_name,
						  sizeof(release.tag_name)) ||
		!find_json_string(response->data, "target_commitish", release.commit_sha,
						  sizeof(release.commit_sha)) ||
		!find_zip_asset_url(response->data, PLATFORM, release.download_url,
							sizeof(release.download_url))) {
		HTTP_freeResponse(response);
		auto_state = UPDATE_ERROR;
		snprintf(item_label, sizeof(item_label), "Updater");
		snprintf(item_desc, sizeof(item_desc), "Could not parse release info");
		return;
	}

	char body[4096] = "";
	find_json_string(response->data, "body", body, sizeof(body));
	extract_first_paragraph(body, release.release_notes, sizeof(release.release_notes));
	HTTP_freeResponse(response);

	// Compare SHAs
	int is_same = 0;
	if (current_sha_cache[0] && release.commit_sha[0]) {
		size_t cur_len = strlen(current_sha_cache);
		size_t rel_len = strlen(release.commit_sha);
		size_t cmp_len = cur_len < rel_len ? cur_len : rel_len;
		if (cmp_len > 0 && strncmp(current_sha_cache, release.commit_sha, cmp_len) == 0)
			is_same = 1;
	}

	if (is_same) {
		auto_state = UPDATE_UP_TO_DATE;
		snprintf(item_label, sizeof(item_label), "You already have latest version");
		item_desc[0] = '\0';
	} else {
		cached_release = release;
		auto_state = UPDATE_AVAILABLE;
		snprintf(item_label, sizeof(item_label), "Install Update");
		snprintf(item_desc, sizeof(item_desc), "%s", release.tag_name);
	}
}

// ============================================
// Download thread
// ============================================

static void* download_thread(void* arg) {
	AsyncContext* ctx = (AsyncContext*)arg;

	char* argv[] = {"curl", "-L", "-o", DOWNLOAD_PATH,
					ctx->release->download_url, NULL};

	int ret = run_command(argv);
	if (ret != 0) {
		snprintf(ctx->error, sizeof(ctx->error), "Download failed");
		ctx->success = 0;
		__sync_synchronize();
		ctx->done = 1;
		return NULL;
	}

	ctx->success = 1;
	__sync_synchronize();
	ctx->done = 1;
	return NULL;
}

// ============================================
// Extract thread
// ============================================

static void* extract_thread(void* arg) {
	AsyncContext* ctx = (AsyncContext*)arg;

	char* argv[] = {"unzip", "-o", DOWNLOAD_PATH, "-d", EXTRACT_DEST, NULL};

	int ret = run_command(argv);
	if (ret != 0) {
		snprintf(ctx->error, sizeof(ctx->error), "Extraction failed");
		ctx->success = 0;
		__sync_synchronize();
		ctx->done = 1;
		return NULL;
	}

	unlink(DOWNLOAD_PATH);

	char release_name[128] = "Unknown";
	const char* slash = strrchr(ctx->release->download_url, '/');
	if (slash) {
		slash++;
		strncpy(release_name, slash, sizeof(release_name) - 1);
		release_name[sizeof(release_name) - 1] = '\0';
		char suffix[64];
		snprintf(suffix, sizeof(suffix), "-%s.zip", PLATFORM);
		size_t name_len = strlen(release_name);
		size_t suf_len = strlen(suffix);
		if (name_len > suf_len && strcmp(release_name + name_len - suf_len, suffix) == 0)
			release_name[name_len - suf_len] = '\0';
	}

	FILE* vf = fopen(VERSION_FILE_PATH, "w");
	if (vf) {
		fprintf(vf, "%s\n%s\n%s\n", release_name,
				ctx->release->commit_sha, ctx->release->tag_name);
		fclose(vf);
	}

	ctx->success = 1;
	__sync_synchronize();
	ctx->done = 1;
	return NULL;
}

// ============================================
// Overlay helpers
// ============================================

static void render_overlay(SDL_Surface* screen, const char* title, const char* subtitle) {
	GFX_clear(screen);
	settings_menu_render(screen, 0);
	UI_renderLoadingOverlay(screen, title, subtitle);
	GFX_flip(screen);
}

static int wait_for_async(SDL_Surface* screen, AsyncContext* ctx,
						  const char* title, const char* subtitle) {
	while (!ctx->done) {
		GFX_startFrame();
		PAD_poll();
		render_overlay(screen, title, subtitle);
	}
	__sync_synchronize();
	return ctx->success;
}

static void show_message(SDL_Surface* screen, const char* title, const char* subtitle) {
	unsigned long start = SDL_GetTicks();
	while (SDL_GetTicks() - start < 2000) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B))
			break;
		render_overlay(screen, title, subtitle);
	}
}

// Show update info (version + release notes) with A to confirm, B to cancel.
static int show_update_info(SDL_Surface* screen, ReleaseInfo* release) {
	while (1) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justPressed(BTN_B))
			return 0;
		if (PAD_justPressed(BTN_A))
			return 1;

		GFX_clear(screen);
		UI_renderMenuBar(screen, "Update Available");
		UI_renderButtonHintBar(screen, (char*[]){"B", "CANCEL", "A", "INSTALL", NULL});

		int bar_h = SCALE1(BUTTON_SIZE) + SCALE1(BUTTON_MARGIN * 2);
		int y = bar_h + SCALE1(PADDING * 2);

		SDL_Surface* tag_surf = TTF_RenderUTF8_Blended(font.large, release->tag_name, COLOR_WHITE);
		if (tag_surf) {
			SDL_BlitSurface(tag_surf, NULL, screen,
							&(SDL_Rect){(screen->w - tag_surf->w) / 2, y});
			y += tag_surf->h + SCALE1(PADDING);
			SDL_FreeSurface(tag_surf);
		}

		if (release->release_notes[0]) {
			int max_w = screen->w - SCALE1(PADDING * 4);
			char notes_copy[2048];
			strncpy(notes_copy, release->release_notes, sizeof(notes_copy) - 1);
			notes_copy[sizeof(notes_copy) - 1] = '\0';

			int max_lines = 8;
			GFX_wrapText(font.small, notes_copy, max_w, max_lines);
			GFX_blitWrappedText(font.small, notes_copy, max_w, max_lines,
								COLOR_GRAY, screen, y);
		}

		GFX_flip(screen);
	}
}

// Run download + extract + reboot.
static void do_install(SDL_Surface* screen, ReleaseInfo* release) {
	AsyncContext ctx = {0};
	ctx.release = release;
	pthread_t tid;

	if (pthread_create(&tid, NULL, download_thread, &ctx) != 0) {
		show_message(screen, "Update Error", "Failed to start download");
		return;
	}
	if (!wait_for_async(screen, &ctx, "Downloading update...", NULL)) {
		pthread_join(tid, NULL);
		show_message(screen, "Update Error", ctx.error);
		return;
	}
	pthread_join(tid, NULL);

	memset(&ctx, 0, sizeof(ctx));
	ctx.release = release;

	if (pthread_create(&tid, NULL, extract_thread, &ctx) != 0) {
		show_message(screen, "Update Error", "Failed to start installation");
		return;
	}
	if (!wait_for_async(screen, &ctx, "Installing update...", NULL)) {
		pthread_join(tid, NULL);
		show_message(screen, "Update Error", ctx.error);
		return;
	}
	pthread_join(tid, NULL);

	render_overlay(screen, "Update complete!", "Rebooting...");
	sleep(2);
	system("reboot");
}

// ============================================
// Find the updater item in the page
// ============================================

static SettingItem* find_updater_item(SettingsPage* page) {
	for (int i = 0; i < page->item_count; i++) {
		if (page->items[i].on_press == updater_check_for_updates)
			return &page->items[i];
	}
	return NULL;
}

// ============================================
// Public API
// ============================================

void updater_about_on_show(SettingsPage* page) {
	(void)page;

	char version[128], tag[128];
	read_current_version(version, sizeof(version),
						 current_sha_cache, sizeof(current_sha_cache),
						 tag, sizeof(tag));

	// Only auto-check once per session (retry allowed on error)
	if (auto_state == UPDATE_CHECKING || auto_state == UPDATE_UP_TO_DATE ||
		auto_state == UPDATE_AVAILABLE)
		return;

	auto_done = 0;
	auto_success = 0;
	auto_response = NULL;
	auto_error[0] = '\0';
	auto_state = UPDATE_CHECKING;
	snprintf(item_label, sizeof(item_label), "Fetching update..");
	item_desc[0] = '\0';

	if (pthread_create(&auto_tid, NULL, auto_check_thread, NULL) != 0) {
		auto_state = UPDATE_ERROR;
		snprintf(item_label, sizeof(item_label), "Updater");
		snprintf(item_desc, sizeof(item_desc), "Failed to start update check");
	}
}

void updater_about_on_tick(SettingsPage* page) {
	if (auto_state != UPDATE_CHECKING || !auto_done)
		return;

	__sync_synchronize();
	process_auto_check_result();

	SettingItem* item = find_updater_item(page);
	if (item) {
		item->name = item_label;
		item->desc = item_desc;
	}
}

const char* updater_get_status(void) {
	return item_label;
}

void updater_check_for_updates(void) {
	// Do nothing while background check is in progress
	if (auto_state == UPDATE_CHECKING)
		return;

	// Do nothing if already up to date
	if (auto_state == UPDATE_UP_TO_DATE)
		return;

	// If update available, show info and install
	if (auto_state == UPDATE_AVAILABLE) {
		SettingsPage* page = settings_menu_current();
		if (!page || !page->screen)
			return;

		SDL_Surface* screen = page->screen;

		if (!show_update_info(screen, &cached_release))
			return;

		do_install(screen, &cached_release);
		return;
	}

	// Error or idle: retry the check (non-blocking, will update via on_tick)
	auto_state = UPDATE_IDLE;
	SettingsPage* page = settings_menu_current();
	if (page)
		updater_about_on_show(page);

	SettingItem* item = find_updater_item(page);
	if (item) {
		item->name = item_label;
		item->desc = item_desc;
	}
}
