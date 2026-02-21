/*
 * updater.c - NextUI System Updater
 *
 * Checks GitHub for the latest release, compares against the installed
 * version, and allows the user to download + install updates.
 */

#include <msettings.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "api.h"
#include "defines.h"
#include "http.h"
#include "ui_components.h"
#include "utils.h"

// ============================================
// Configuration
// ============================================

#define UPDATER_REPO_OWNER "mohammadsyuhada"
#define UPDATER_REPO_NAME "nextui-redux"
#define VERSION_FILE_PATH "/mnt/SDCARD/.system/version.txt"
#define DOWNLOAD_PATH "/tmp/nextui-update.zip"
#define EXTRACT_DEST "/mnt/SDCARD/"

// ============================================
// App states
// ============================================

typedef enum {
	STATE_CHECKING,
	STATE_UP_TO_DATE,
	STATE_UPDATE_AVAIL,
	STATE_CONFIRM,
	STATE_DOWNLOADING,
	STATE_EXTRACTING,
	STATE_DONE,
	STATE_ERROR,
} AppState;

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
// Globals
// ============================================

static AppState app_state = STATE_CHECKING;
static char error_msg[256] = "";
static char current_version[128] = "";
static char current_sha[64] = "";
static char current_tag[128] = "";
static ReleaseInfo latest_release = {0};
static volatile bool async_done = false;
static volatile bool async_success = false;

// ============================================
// JSON helpers (minimal strstr-based parsing)
// ============================================

static const char* find_json_string(const char* json, const char* key, char* out, size_t out_size) {
	if (!json || !key || !out || out_size == 0)
		return NULL;

	// Search for "key":"value" pattern
	char search[128];
	snprintf(search, sizeof(search), "\"%s\":\"", key);

	const char* start = strstr(json, search);
	if (!start) {
		// Try "key": "value" (with space)
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

// Find the browser_download_url of the platform-specific .zip asset in the release JSON
static const char* find_zip_asset_url(const char* json, const char* platform, char* out, size_t out_size) {
	if (!json || !platform || !out || out_size == 0)
		return NULL;

	// Build the suffix we're looking for, e.g. "-tg5040.zip"
	char suffix[64];
	snprintf(suffix, sizeof(suffix), "-%s.zip", platform);

	// Look for "assets" array
	const char* assets = strstr(json, "\"assets\"");
	if (!assets)
		return NULL;

	// Search for .zip download URL within the assets section
	const char* pos = assets;
	while ((pos = strstr(pos, "\"browser_download_url\"")) != NULL) {
		// Skip past the key
		pos = strchr(pos + 21, '"');
		if (!pos)
			break;

		// Try with no space: :"url"
		if (*(pos - 1) == ':') {
			// pos points to opening quote of value
		} else {
			// pos might be key end quote, look for : "
			pos++;
			while (*pos == ' ' || *pos == ':')
				pos++;
			if (*pos != '"')
				continue;
		}

		pos++; // skip opening quote
		const char* end = strchr(pos, '"');
		if (!end)
			break;

		size_t len = end - pos;
		size_t suf_len = strlen(suffix);
		// Check if this URL ends with the platform-specific suffix
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

// Extract first paragraph from release body (up to first double newline)
static void extract_first_paragraph(const char* body, char* out, size_t out_size) {
	if (!body || !out || out_size == 0)
		return;

	out[0] = '\0';
	size_t j = 0;

	for (size_t i = 0; body[i] && j < out_size - 1; i++) {
		// Handle escaped newlines from JSON
		if (body[i] == '\\' && body[i + 1] == 'n') {
			// Check for double newline (paragraph break)
			if (body[i + 2] == '\\' && body[i + 3] == 'n') {
				break; // end of first paragraph
			}
			out[j++] = ' ';
			i++; // skip 'n'
			continue;
		}

		// Handle escaped characters
		if (body[i] == '\\' && body[i + 1] == 'r') {
			i++; // skip \r
			continue;
		}

		// Skip markdown heading markers at start of line
		if (body[i] == '#') {
			while (body[i] == '#')
				i++;
			if (body[i] == ' ')
				i++;
			i--; // loop will increment
			continue;
		}

		out[j++] = body[i];
	}

	out[j] = '\0';

	// Trim trailing whitespace
	while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\n' || out[j - 1] == '\r')) {
		out[--j] = '\0';
	}
}

// ============================================
// Version reading
// ============================================

static void read_current_version(void) {
	FILE* f = fopen(VERSION_FILE_PATH, "r");
	if (!f) {
		snprintf(current_version, sizeof(current_version), "Unknown");
		current_sha[0] = '\0';
		return;
	}

	// Line 1: release name
	if (!fgets(current_version, sizeof(current_version), f)) {
		snprintf(current_version, sizeof(current_version), "Unknown");
		fclose(f);
		return;
	}
	// Strip newline
	current_version[strcspn(current_version, "\r\n")] = '\0';

	// Line 2: commit SHA
	if (!fgets(current_sha, sizeof(current_sha), f)) {
		current_sha[0] = '\0';
	}
	current_sha[strcspn(current_sha, "\r\n")] = '\0';

	// Line 3: tag name
	if (!fgets(current_tag, sizeof(current_tag), f)) {
		current_tag[0] = '\0';
	}
	current_tag[strcspn(current_tag, "\r\n")] = '\0';

	fclose(f);
}

// ============================================
// GitHub API - async check
// ============================================

static void on_release_info(HTTP_Response* response, void* userdata);

static void check_for_updates(void) {
	char url[256];
	snprintf(url, sizeof(url),
			 "https://api.github.com/repos/%s/%s/releases/latest",
			 UPDATER_REPO_OWNER, UPDATER_REPO_NAME);

	HTTP_getAsync(url, on_release_info, NULL);
}

static void on_release_info(HTTP_Response* response, void* userdata) {
	(void)userdata;

	if (!response || response->http_status != 200 || !response->data) {
		snprintf(error_msg, sizeof(error_msg), "Failed to check for updates");
		if (response && response->error)
			snprintf(error_msg, sizeof(error_msg), "%.250s", response->error);
		__sync_synchronize();
		async_success = false;
		async_done = true;
		if (response)
			HTTP_freeResponse(response);
		return;
	}

	// Extract tag_name
	if (!find_json_string(response->data, "tag_name", latest_release.tag_name,
						  sizeof(latest_release.tag_name))) {
		snprintf(error_msg, sizeof(error_msg), "Could not parse release info");
		__sync_synchronize();
		async_success = false;
		async_done = true;
		HTTP_freeResponse(response);
		return;
	}

	// Extract target_commitish (commit SHA from releases API, works for both
	// lightweight and annotated tags)
	if (!find_json_string(response->data, "target_commitish", latest_release.commit_sha,
						  sizeof(latest_release.commit_sha))) {
		snprintf(error_msg, sizeof(error_msg), "Could not determine release commit");
		__sync_synchronize();
		async_success = false;
		async_done = true;
		HTTP_freeResponse(response);
		return;
	}

	// Extract body (release notes)
	char body[4096] = "";
	find_json_string(response->data, "body", body, sizeof(body));
	extract_first_paragraph(body, latest_release.release_notes,
							sizeof(latest_release.release_notes));

	// Extract platform-specific .zip asset URL
	if (!find_zip_asset_url(response->data, PLATFORM, latest_release.download_url,
							sizeof(latest_release.download_url))) {
		snprintf(error_msg, sizeof(error_msg), "No download found for " PLATFORM);
		__sync_synchronize();
		async_success = false;
		async_done = true;
		HTTP_freeResponse(response);
		return;
	}

	HTTP_freeResponse(response);

	__sync_synchronize();
	async_success = true;
	async_done = true;
}

// ============================================
// Download + extract (runs in background thread)
// ============================================

// Run a command with execvp (no shell interpretation, avoids injection)
static int run_command(char* const argv[]) {
	pid_t pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		// Child: redirect stderr to /dev/null
		freopen("/dev/null", "w", stderr);
		execvp(argv[0], argv);
		_exit(127); // exec failed
	}

	// Parent: wait for child
	int status;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return -1;
}

static void* download_thread(void* arg) {
	(void)arg;

	// Download using curl via execvp (avoids shell injection)
	char* argv[] = {"curl", "-L", "-o", DOWNLOAD_PATH,
					latest_release.download_url, NULL};

	int ret = run_command(argv);
	if (ret != 0) {
		snprintf(error_msg, sizeof(error_msg), "Download failed");
		__sync_synchronize();
		async_success = false;
		async_done = true;
		return NULL;
	}

	__sync_synchronize();
	async_success = true;
	async_done = true;
	return NULL;
}

static void* extract_thread(void* arg) {
	(void)arg;

	char* argv[] = {"unzip", "-o", DOWNLOAD_PATH, "-d", EXTRACT_DEST, NULL};

	int ret = run_command(argv);
	if (ret != 0) {
		snprintf(error_msg, sizeof(error_msg), "Extraction failed");
		__sync_synchronize();
		async_success = false;
		async_done = true;
		return NULL;
	}

	unlink(DOWNLOAD_PATH);

	// Derive release name from zip filename (e.g. "NextUI-20260221-tg5040.zip" -> "NextUI-20260221")
	char release_name[128] = "Unknown";
	const char* slash = strrchr(latest_release.download_url, '/');
	if (slash) {
		slash++; // skip '/'
		strncpy(release_name, slash, sizeof(release_name) - 1);
		release_name[sizeof(release_name) - 1] = '\0';
		// Strip platform suffix (e.g. "-tg5040.zip")
		char suffix[64];
		snprintf(suffix, sizeof(suffix), "-%s.zip", PLATFORM);
		size_t name_len = strlen(release_name);
		size_t suf_len = strlen(suffix);
		if (name_len > suf_len && strcmp(release_name + name_len - suf_len, suffix) == 0) {
			release_name[name_len - suf_len] = '\0';
		}
	}

	FILE* vf = fopen(VERSION_FILE_PATH, "w");
	if (vf) {
		fprintf(vf, "%s\n%s\n%s\n", release_name,
				latest_release.commit_sha, latest_release.tag_name);
		fclose(vf);
	}

	__sync_synchronize();
	async_success = true;
	async_done = true;
	return NULL;
}

static void start_download(void) {
	async_done = false;
	async_success = false;
	app_state = STATE_DOWNLOADING;

	pthread_t tid;
	pthread_create(&tid, NULL, download_thread, NULL);
	pthread_detach(tid);
}

static void start_extract(void) {
	async_done = false;
	async_success = false;
	app_state = STATE_EXTRACTING;

	pthread_t tid;
	pthread_create(&tid, NULL, extract_thread, NULL);
	pthread_detach(tid);
}

// ============================================
// Rendering helpers
// ============================================

static void render_text_centered(SDL_Surface* screen, const char* text, TTF_Font* f,
								 SDL_Color color, int y) {
	SDL_Surface* surf = TTF_RenderUTF8_Blended(f, text, color);
	if (surf) {
		SDL_BlitSurface(surf, NULL, screen,
						&(SDL_Rect){(screen->w - surf->w) / 2, y});
		SDL_FreeSurface(surf);
	}
}

static void render_state(SDL_Surface* screen, IndicatorType show_setting) {
	GFX_clear(screen);

	int bar_h = SCALE1(BUTTON_SIZE) + SCALE1(BUTTON_MARGIN * 2);
	int content_y = bar_h + SCALE1(PADDING);
	int center_y = screen->h / 2;

	switch (app_state) {
	case STATE_CHECKING:
		UI_renderMenuBar(screen, "Updater");
		UI_renderButtonHintBar(screen, (char*[]){NULL});
		render_text_centered(screen, "Checking for updates...", font.large, COLOR_WHITE,
							 center_y - TTF_FontHeight(font.large) / 2);
		break;

	case STATE_UP_TO_DATE:
		UI_renderMenuBar(screen, "Updater");
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		render_text_centered(screen, "System is up to date", font.large, COLOR_WHITE,
							 center_y - TTF_FontHeight(font.large));
		render_text_centered(screen, current_version, font.small, COLOR_GRAY,
							 center_y + SCALE1(4));
		break;

	case STATE_UPDATE_AVAIL: {
		UI_renderMenuBar(screen, "Updater");
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", "A", "UPDATE", NULL});

		// Tag name / version
		int y = content_y + SCALE1(PADDING);
		render_text_centered(screen, latest_release.tag_name, font.large, COLOR_WHITE, y);
		y += TTF_FontHeight(font.large) + SCALE1(PADDING);

		// Release notes (word-wrapped)
		if (latest_release.release_notes[0]) {
			int max_w = screen->w - SCALE1(PADDING * 4);
			char notes_copy[2048];
			strncpy(notes_copy, latest_release.release_notes, sizeof(notes_copy) - 1);
			notes_copy[sizeof(notes_copy) - 1] = '\0';

			int max_lines = 8;
			GFX_wrapText(font.small, notes_copy, max_w, max_lines);
			GFX_blitWrappedText(font.small, notes_copy, max_w, max_lines,
								COLOR_GRAY, screen, y);
		}
		break;
	}

	case STATE_CONFIRM:
		UI_renderConfirmDialog(screen, "Install Update?",
							   "The system will reboot after updating.");
		UI_renderButtonHintBar(screen, (char*[]){"B", "CANCEL", "A", "CONFIRM", NULL});
		break;

	case STATE_DOWNLOADING:
		UI_renderMenuBar(screen, "Updater");
		UI_renderButtonHintBar(screen, (char*[]){NULL});
		UI_renderLoadingOverlay(screen, "Downloading update...", NULL);
		break;

	case STATE_EXTRACTING:
		UI_renderMenuBar(screen, "Updater");
		UI_renderButtonHintBar(screen, (char*[]){NULL});
		UI_renderLoadingOverlay(screen, "Installing update...", NULL);
		break;

	case STATE_DONE:
		UI_renderMenuBar(screen, "Updater");
		UI_renderButtonHintBar(screen, (char*[]){NULL});
		UI_renderLoadingOverlay(screen, "Update complete!", "Rebooting...");
		break;

	case STATE_ERROR:
		UI_renderMenuBar(screen, "Updater");
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		render_text_centered(screen, "Update Error", font.large, COLOR_WHITE,
							 center_y - TTF_FontHeight(font.large));
		render_text_centered(screen, error_msg, font.small,
							 (SDL_Color){0xFF, 0x66, 0x66}, center_y + SCALE1(4));
		break;
	}
}

// ============================================
// Main
// ============================================

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	SDL_Surface* screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "Updater");

	InitSettings();
	PWR_init();
	PAD_init();

	setup_signal_handlers();
	read_current_version();

	app_state = STATE_CHECKING;
	async_done = false;
	async_success = false;
	check_for_updates();

	bool quit = 0;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (!quit && !app_quit) {
		GFX_startFrame();
		PAD_poll();

		// Handle async operation completion
		if (async_done) {
			__sync_synchronize(); // ensure all writes from background thread are visible
			dirty = true;

			switch (app_state) {
			case STATE_CHECKING:
				if (async_success) {
					// Compare SHAs - prefix match to handle short vs full SHA
					int is_same = 0;
					if (current_sha[0] && latest_release.commit_sha[0]) {
						size_t cur_len = strlen(current_sha);
						size_t rel_len = strlen(latest_release.commit_sha);
						size_t cmp_len = cur_len < rel_len ? cur_len : rel_len;
						if (cmp_len > 0 && strncmp(current_sha, latest_release.commit_sha, cmp_len) == 0) {
							is_same = 1;
						}
					}

					if (is_same) {
						app_state = STATE_UP_TO_DATE;
					} else {
						app_state = STATE_UPDATE_AVAIL;
					}
				} else {
					app_state = STATE_ERROR;
				}
				async_done = false;
				break;

			case STATE_DOWNLOADING:
				if (async_success) {
					start_extract();
				} else {
					app_state = STATE_ERROR;
				}
				async_done = false;
				break;

			case STATE_EXTRACTING:
				if (async_success) {
					app_state = STATE_DONE;
					dirty = true;
				} else {
					app_state = STATE_ERROR;
				}
				async_done = false;
				break;

			default:
				async_done = false;
				break;
			}
		}

		switch (app_state) {
		case STATE_UP_TO_DATE:
		case STATE_ERROR:
			if (PAD_justPressed(BTN_B)) {
				quit = 1;
			}
			break;

		case STATE_UPDATE_AVAIL:
			if (PAD_justPressed(BTN_B)) {
				quit = 1;
			} else if (PAD_justPressed(BTN_A)) {
				app_state = STATE_CONFIRM;
				dirty = true;
			}
			break;

		case STATE_CONFIRM:
			if (PAD_justPressed(BTN_B)) {
				app_state = STATE_UPDATE_AVAIL;
				dirty = true;
			} else if (PAD_justPressed(BTN_A)) {
				start_download();
				dirty = true;
			}
			break;

		case STATE_DONE:
			// Brief delay then reboot
			render_state(screen, show_setting);
			GFX_flip(screen);
			sleep(2);
			system("reboot");
			quit = 1;
			break;

		default:
			// Non-interactive states (checking, downloading, extracting)
			break;
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (UI_statusBarChanged())
			dirty = true;

		if (dirty) {
			render_state(screen, show_setting);
			GFX_flip(screen);
			dirty = false;
		} else {
			GFX_sync();
		}
	}

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
