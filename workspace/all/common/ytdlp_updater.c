#define _GNU_SOURCE
#include "ytdlp_updater.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "defines.h"
#include "api.h"
#include "ui_components.h"

// Paths
static const char* ytdlp_path = SHARED_BIN_PATH "/yt-dlp";
static const char* version_file = SHARED_BIN_PATH "/yt-dlp_version.txt";

// Update status
static YtdlpUpdateStatus update_status = {0};
static pthread_t update_thread;
static volatile bool update_running = false;
static volatile bool update_should_stop = false;

// Current yt-dlp version
static char current_version[32] = "unknown";

static void* update_thread_func(void* arg) {
	(void)arg;

	update_status.updating = true;
	update_status.progress_percent = 0;

	// Check connectivity
	int conn = system("ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1");
	if (conn != 0) {
		conn = system("ping -c 1 -W 2 1.1.1.1 >/dev/null 2>&1");
	}

	if (conn != 0) {
		strncpy(update_status.error_message, "No internet connection", sizeof(update_status.error_message) - 1);
		update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	// Check for cancellation
	if (update_should_stop) {
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	update_status.progress_percent = 10;

	// Fetch latest version from GitHub API
	char temp_dir[512];
	snprintf(temp_dir, sizeof(temp_dir), "/tmp/ytdlp_update_%d", getpid());
	mkdir(temp_dir, 0755);

	char latest_file[600];
	snprintf(latest_file, sizeof(latest_file), "%s/latest.json", temp_dir);

	char cmd[1024];
	char error_file[600];
	char wget_bin[600];
	snprintf(error_file, sizeof(error_file), "%s/wget_error.txt", temp_dir);
	snprintf(wget_bin, sizeof(wget_bin), SHARED_BIN_PATH "/wget");

	update_status.progress_percent = 15;

	// Use timeout to prevent indefinite blocking on slow/unstable WiFi
	snprintf(cmd, sizeof(cmd),
			 "%s -q -T 30 -t 2 -U \"NextUI-Music-Player\" -O \"%s\" \"https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest\" 2>\"%s\"",
			 wget_bin, latest_file, error_file);

	int wget_result = system(cmd);
	if (wget_result != 0 || access(latest_file, F_OK) != 0) {
		// Try to read the actual error
		FILE* ef = fopen(error_file, "r");
		if (ef) {
			char err_line[128];
			if (fgets(err_line, sizeof(err_line), ef)) {
				char* nl = strchr(err_line, '\n');
				if (nl)
					*nl = '\0';
				strncpy(update_status.error_message, err_line, sizeof(update_status.error_message) - 1);
			} else {
				snprintf(update_status.error_message, sizeof(update_status.error_message),
						 "wget error %d", WEXITSTATUS(wget_result));
			}
			fclose(ef);
		} else {
			strncpy(update_status.error_message, "Failed to check GitHub", sizeof(update_status.error_message) - 1);
			update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
		}
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	// Check for cancellation after wget
	if (update_should_stop) {
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
		system(cmd);
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	update_status.progress_percent = 30;

	// Parse version from JSON (simple grep approach)
	char version_cmd[1024];
	snprintf(version_cmd, sizeof(version_cmd),
			 "grep -o '\"tag_name\": *\"[^\"]*' \"%s\" | cut -d'\"' -f4",
			 latest_file);

	char latest_version[32] = "";
	FILE* pipe = popen(version_cmd, "r");
	if (pipe) {
		if (fgets(latest_version, sizeof(latest_version), pipe)) {
			char* nl = strchr(latest_version, '\n');
			if (nl)
				*nl = '\0';
		}
		pclose(pipe);
	}

	if (strlen(latest_version) == 0) {
		strncpy(update_status.error_message, "Could not parse version", sizeof(update_status.error_message) - 1);
		update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	strncpy(update_status.latest_version, latest_version, sizeof(update_status.latest_version));
	strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));

	// Compare versions
	if (strcmp(latest_version, current_version) == 0) {
		update_status.update_available = false;
		update_status.progress_percent = 100; // Signal completion for UI
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	// Check for cancellation before download
	if (update_should_stop) {
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
		system(cmd);
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	update_status.update_available = true;
	update_status.progress_percent = 40;

	// Get download URL for aarch64
	char url_cmd[1024];
	snprintf(url_cmd, sizeof(url_cmd),
			 "grep -o '\"browser_download_url\": *\"[^\"]*yt-dlp_linux_aarch64\"' \"%s\" | cut -d'\"' -f4",
			 latest_file);

	char download_url[512] = "";
	pipe = popen(url_cmd, "r");
	if (pipe) {
		if (fgets(download_url, sizeof(download_url), pipe)) {
			char* nl = strchr(download_url, '\n');
			if (nl)
				*nl = '\0';
		}
		pclose(pipe);
	}

	if (strlen(download_url) == 0) {
		strncpy(update_status.error_message, "No ARM64 binary found", sizeof(update_status.error_message) - 1);
		update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
		system(cmd);
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	// Check for cancellation before large download
	if (update_should_stop) {
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
		system(cmd);
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	update_status.progress_percent = 50;

	// Download new binary with real-time progress via file size monitoring
	char new_binary[600];
	snprintf(new_binary, sizeof(new_binary), "%s/yt-dlp.new", temp_dir);

	update_status.progress_percent = 50;
	update_status.download_bytes = 0;
	update_status.download_total = 0;
	strncpy(update_status.status_detail, "Getting file info...", sizeof(update_status.status_detail) - 1);

	// First, get the actual file size from server using wget --spider
	// GitHub releases redirect to CDN, so we need to follow redirects and get the final Content-Length
	char size_file[600];
	snprintf(size_file, sizeof(size_file), "%s/size.txt", temp_dir);

	// Use --max-redirect to follow GitHub's redirects, get last Content-Length (from final destination)
	snprintf(cmd, sizeof(cmd),
			 "%s --spider -S --max-redirect=10 -T 30 -U \"NextUI-Music-Player\" \"%s\" 2>&1 | grep -i 'Content-Length' | tail -1 | awk '{print $2}' | tr -d '\\r' > \"%s\"",
			 wget_bin, download_url, size_file);
	system(cmd);

	// Read the file size
	FILE* size_f = fopen(size_file, "r");
	if (size_f) {
		long file_size = 0;
		if (fscanf(size_f, "%ld", &file_size) == 1 && file_size > 1000000) {
			update_status.download_total = file_size;
		}
		fclose(size_f);
	}

	// Fallback to approximate size if we couldn't get it (yt-dlp is ~34MB as of 2024+)
	if (update_status.download_total == 0) {
		update_status.download_total = 35000000; // ~35MB fallback
	}

	// Check for cancellation
	if (update_should_stop) {
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
		system(cmd);
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	strncpy(update_status.status_detail, "Starting download...", sizeof(update_status.status_detail) - 1);

	// Create a marker file to track wget completion
	char done_marker[600];
	snprintf(done_marker, sizeof(done_marker), "%s/wget.done", temp_dir);

	// Start wget in background, create marker file when done
	// Using shell to chain commands: wget && touch done_marker
	snprintf(cmd, sizeof(cmd),
			 "(%s -T 120 -t 3 -q -U \"NextUI-Music-Player\" -O \"%s\" \"%s\"; echo $? > \"%s\") &",
			 wget_bin, new_binary, download_url, done_marker);
	system(cmd);

	// Monitor download progress
	int timeout_seconds = 0;
	const int max_timeout = 180; // 3 minutes
	long last_size = 0;
	int stable_count = 0;

	while (timeout_seconds < max_timeout) {
		// Check for cancellation
		if (update_should_stop) {
			system("pkill -f 'wget.*yt-dlp' 2>/dev/null");
			snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
			system(cmd);
			update_status.updating = false;
			update_running = false;
			return NULL;
		}

		// Check if wget finished (marker file exists)
		if (access(done_marker, F_OK) == 0) {
			break;
		}

		// Check current file size
		struct stat st;
		if (stat(new_binary, &st) == 0 && st.st_size > 0) {
			update_status.download_bytes = st.st_size;

			// Calculate progress (map to 50-78% range, save 78-80 for verification)
			int dl_percent = (int)((st.st_size * 100) / update_status.download_total);
			if (dl_percent > 100)
				dl_percent = 100;
			update_status.progress_percent = 50 + (dl_percent * 28 / 100);

			// Format MB display
			float mb_done = st.st_size / (1024.0f * 1024.0f);
			float mb_total = update_status.download_total / (1024.0f * 1024.0f);
			snprintf(update_status.status_detail, sizeof(update_status.status_detail),
					 "%.1fMB / %.1fMB", mb_done, mb_total);

			// Track if file stopped growing (might be done or stalled)
			if (st.st_size == last_size) {
				stable_count++;
			} else {
				stable_count = 0;
			}
			last_size = st.st_size;
		} else {
			// File not created yet
			snprintf(update_status.status_detail, sizeof(update_status.status_detail),
					 "Connecting...");
		}

		usleep(500000); // Check every 0.5 seconds for smoother updates
		timeout_seconds++;
	}

	// Wait a moment for wget to fully finish writing
	usleep(500000);

	// Kill any remaining wget just in case
	system("pkill -f 'wget.*yt-dlp' 2>/dev/null");

	// Check wget exit status from marker file
	int wget_exit = -1;
	FILE* marker_f = fopen(done_marker, "r");
	if (marker_f) {
		fscanf(marker_f, "%d", &wget_exit);
		fclose(marker_f);
	}

	update_status.progress_percent = 78;

	// Verify download completed
	struct stat final_st;
	if (stat(new_binary, &final_st) != 0 || final_st.st_size < 1000000) {
		// File doesn't exist or too small (< 1MB)
		if (wget_exit != 0 && wget_exit != -1) {
			snprintf(update_status.error_message, sizeof(update_status.error_message),
					 "Download failed (error %d)", wget_exit);
		} else if (timeout_seconds >= max_timeout) {
			strncpy(update_status.error_message, "Download timed out", sizeof(update_status.error_message) - 1);
		} else {
			snprintf(update_status.error_message, sizeof(update_status.error_message),
					 "Incomplete (%ld bytes)", (long)(stat(new_binary, &final_st) == 0 ? final_st.st_size : 0));
		}
		update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
		system(cmd);
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	// Update final size for display
	update_status.download_bytes = final_st.st_size;
	update_status.download_total = final_st.st_size;
	float final_mb = final_st.st_size / (1024.0f * 1024.0f);
	snprintf(update_status.status_detail, sizeof(update_status.status_detail),
			 "%.1f MB downloaded", final_mb);

	// Check for cancellation after download
	if (update_should_stop) {
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
		system(cmd);
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	update_status.progress_percent = 80;

	// Make executable
	chmod(new_binary, 0755);

	// Backup old binary
	char backup_path[600];
	snprintf(backup_path, sizeof(backup_path), "%s.old", ytdlp_path);
	rename(ytdlp_path, backup_path);

	// Move new binary
	snprintf(cmd, sizeof(cmd), "mv \"%s\" \"%s\"", new_binary, ytdlp_path);
	if (system(cmd) != 0) {
		// Restore backup
		rename(backup_path, ytdlp_path);
		strncpy(update_status.error_message, "Failed to install update", sizeof(update_status.error_message) - 1);
		update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
		system(cmd);
		update_status.updating = false;
		update_running = false;
		return NULL;
	}

	// Update version file (shared location)
	FILE* vf = fopen(version_file, "w");
	if (vf) {
		fprintf(vf, "%s\n", latest_version);
		fclose(vf);
	}

	strncpy(current_version, latest_version, sizeof(current_version));

	// Cleanup
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
	system(cmd);

	update_status.progress_percent = 100;
	update_status.updating = false;
	update_running = false;

	return NULL;
}

void YtdlpUpdater_init(void) {
	// Load current version from shared version file first
	FILE* f = fopen(version_file, "r");
	if (f) {
		if (fgets(current_version, sizeof(current_version), f)) {
			// Remove newline
			char* nl = strchr(current_version, '\n');
			if (nl)
				*nl = '\0';
		}
		fclose(f);
	}

	// If version is still unknown, try to get it from yt-dlp --version
	if (strcmp(current_version, "unknown") == 0) {
		char cmd[600];
		snprintf(cmd, sizeof(cmd), "%s --version 2>/dev/null", ytdlp_path);
		FILE* pipe = popen(cmd, "r");
		if (pipe) {
			if (fgets(current_version, sizeof(current_version), pipe)) {
				char* nl = strchr(current_version, '\n');
				if (nl)
					*nl = '\0';
				// Save to shared version file for future
				FILE* vf = fopen(version_file, "w");
				if (vf) {
					fprintf(vf, "%s\n", current_version);
					fclose(vf);
				}
			}
			pclose(pipe);
		}
	}
}

void YtdlpUpdater_cleanup(void) {
	YtdlpUpdater_cancelUpdate();
}

const char* YtdlpUpdater_getVersion(void) {
	return current_version;
}

int YtdlpUpdater_startUpdate(void) {
	if (update_running)
		return 0;

	memset(&update_status, 0, sizeof(update_status));
	strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));

	update_running = true;
	update_should_stop = false;

	if (pthread_create(&update_thread, NULL, update_thread_func, NULL) != 0) {
		update_running = false;
		LOG_error("Failed to create yt-dlp update thread\n");
		return -1;
	}

	pthread_detach(update_thread);
	return 0;
}

void YtdlpUpdater_cancelUpdate(void) {
	if (update_running) {
		update_should_stop = true;
	}
}

const YtdlpUpdateStatus* YtdlpUpdater_getUpdateStatus(void) {
	return &update_status;
}

bool YtdlpUpdater_isUpdating(void) {
	return update_running;
}

void render_ytdlp_updating(SDL_Surface* screen, IndicatorType show_setting) {
	GFX_clear(screen);
	int hw = screen->w;
	int hh = screen->h;

	UI_renderMenuBar(screen, "Updating yt-dlp");

	const YtdlpUpdateStatus* status = YtdlpUpdater_getUpdateStatus();

	// Current version
	char ver_str[128];
	snprintf(ver_str, sizeof(ver_str), "Current: %s", status->current_version);
	SDL_Surface* ver_text = TTF_RenderUTF8_Blended(font.medium, ver_str, COLOR_GRAY);
	if (ver_text) {
		SDL_BlitSurface(ver_text, NULL, screen, &(SDL_Rect){(hw - ver_text->w) / 2, hh / 2 - SCALE1(50)});
		SDL_FreeSurface(ver_text);
	}

	// Status message
	const char* status_msg = "Checking connection...";
	if (status->progress_percent >= 15 && status->progress_percent < 30) {
		status_msg = "Fetching version info...";
	} else if (status->progress_percent >= 30 && status->progress_percent < 50) {
		status_msg = "Checking for updates...";
	} else if (status->progress_percent >= 50 && status->progress_percent < 80) {
		status_msg = "Downloading yt-dlp...";
	} else if (status->progress_percent >= 80 && status->progress_percent < 100) {
		status_msg = "Installing update...";
	} else if (!status->updating && !status->update_available && status->progress_percent >= 100) {
		status_msg = "Already up to date!";
	} else if (status->progress_percent >= 100) {
		status_msg = "Update complete!";
	} else if (!status->updating && strlen(status->error_message) > 0) {
		status_msg = status->error_message;
	}

	SDL_Surface* status_text = TTF_RenderUTF8_Blended(font.medium, status_msg, COLOR_WHITE);
	if (status_text) {
		SDL_BlitSurface(status_text, NULL, screen, &(SDL_Rect){(hw - status_text->w) / 2, hh / 2});
		SDL_FreeSurface(status_text);
	}

	// Latest version (if known)
	if (strlen(status->latest_version) > 0) {
		snprintf(ver_str, sizeof(ver_str), "Latest: %s", status->latest_version);
		SDL_Surface* latest_text = TTF_RenderUTF8_Blended(font.small, ver_str, COLOR_GRAY);
		if (latest_text) {
			SDL_BlitSurface(latest_text, NULL, screen, &(SDL_Rect){(hw - latest_text->w) / 2, hh / 2 + SCALE1(30)});
			SDL_FreeSurface(latest_text);
		}
	}

	// Progress bar
	if (status->updating) {
		int bar_w = hw - SCALE1(PADDING * 8);
		int bar_h = SCALE1(12);
		int bar_x = SCALE1(PADDING * 4);
		int bar_y = hh / 2 + SCALE1(55);

		// Background
		SDL_Rect bg_rect = {bar_x, bar_y, bar_w, bar_h};
		SDL_FillRect(screen, &bg_rect, SDL_MapRGB(screen->format, 64, 64, 64));

		// Progress fill
		int prog_w = (bar_w * status->progress_percent) / 100;
		if (prog_w > 0) {
			SDL_Rect prog_rect = {bar_x, bar_y, prog_w, bar_h};
			SDL_FillRect(screen, &prog_rect, SDL_MapRGB(screen->format, 100, 200, 100));
		}

		// Download detail text
		if (strlen(status->status_detail) > 0) {
			SDL_Surface* detail_text = TTF_RenderUTF8_Blended(font.small, status->status_detail, COLOR_GRAY);
			if (detail_text) {
				SDL_BlitSurface(detail_text, NULL, screen, &(SDL_Rect){(hw - detail_text->w) / 2, bar_y + bar_h + SCALE1(6)});
				SDL_FreeSurface(detail_text);
			}
		}

		// Percentage text
		char pct_str[16];
		snprintf(pct_str, sizeof(pct_str), "%d%%", status->progress_percent);
		SDL_Surface* pct_text = TTF_RenderUTF8_Blended(font.tiny, pct_str, COLOR_WHITE);
		if (pct_text) {
			int pct_x = bar_x + (bar_w - pct_text->w) / 2;
			int pct_y = bar_y + (bar_h - pct_text->h) / 2;
			SDL_BlitSurface(pct_text, NULL, screen, &(SDL_Rect){pct_x, pct_y});
			SDL_FreeSurface(pct_text);
		}
	}

	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", status->updating ? "CANCEL" : "BACK", NULL});
}
