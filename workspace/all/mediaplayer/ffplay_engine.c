#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#include "vp_defines.h"
#include "api.h"
#include "msettings.h"
#include "ffplay_engine.h"

// PID of the currently running ffplay child process (0 = none)
static pid_t ffplay_pid = 0;

// Check if Bluetooth audio is active (via msettings or .asoundrc)
static int is_bluetooth_audio(void) {
	if (GetAudioSink() == AUDIO_SINK_BLUETOOTH)
		return 1;
	const char* home = getenv("HOME");
	if (home) {
		char path[512];
		snprintf(path, sizeof(path), "%s/.asoundrc", home);
		FILE* f = fopen(path, "r");
		if (f) {
			char buf[256];
			while (fgets(buf, sizeof(buf), f)) {
				if (strstr(buf, "bluealsa")) {
					fclose(f);
					return 1;
				}
			}
			fclose(f);
		}
	}
	return 0;
}

// Build argv for ffplay and exec in a forked child. Returns exit status.
static int ffplay_exec(FfplayConfig* config, int use_subs) {
	char* argv[64];
	int argc = 0;

	argv[argc++] = FFPLAY_PATH;
	argv[argc++] = "-fs";		// Fullscreen
	argv[argc++] = "-autoexit"; // Exit when video ends
	argv[argc++] = "-loglevel";
	argv[argc++] = "error";

	// Seek position
	char seek_str[32];
	if (config->start_position_sec > 0) {
		snprintf(seek_str, sizeof(seek_str), "%d", config->start_position_sec);
		argv[argc++] = "-ss";
		argv[argc++] = seek_str;
	}

	// Downscale decoded frames to screen width — reduces renderer workload by
	// avoiding pushing more pixels than the display can show.
	// Note: the decoder still works at full resolution; this is a post-decode scale.
	// min(w,iw) is a no-op for content already <= screen width.
	char scale_filter[128] = "";
	if (config->screen_width > 0) {
		snprintf(scale_filter, sizeof(scale_filter),
				 "scale='min(%d,iw)':-2:flags=fast_bilinear", config->screen_width);
	}

	// Subtitle filters
	// When multiple external subs are available, each becomes a separate -vf entry
	// plus one empty -vf for "subtitles off". D-pad DOWN cycles through them in ffplay.
	char vf_strs[MAX_SUBTITLE_FILES + 1][1024];
	char vf_str[1024];

	if (use_subs && config->subtitle_count > 0) {
		// Disable embedded subtitle streams — external vfilters handle subtitles instead.
		// Without this, embedded subs render on top and hide external subtitle changes.
		argv[argc++] = "-sn";
		// Multiple external subtitle files — one -vf per file + empty for "off"
		for (int i = 0; i < config->subtitle_count; i++) {
			if (scale_filter[0]) {
				snprintf(vf_strs[i], sizeof(vf_strs[0]),
						 "%s,subtitles='%s':fontsdir='%s':force_style='Fontname=Rounded Mplus 1c Bold,FontSize=32'",
						 scale_filter, config->subtitle_paths[i], RES_PATH);
			} else {
				snprintf(vf_strs[i], sizeof(vf_strs[0]),
						 "subtitles='%s':fontsdir='%s':force_style='Fontname=Rounded Mplus 1c Bold,FontSize=32'",
						 config->subtitle_paths[i], RES_PATH);
			}
			argv[argc++] = "-vf";
			argv[argc++] = vf_strs[i];
		}
		// "Subtitles off" entry — still apply scale filter if set
		if (scale_filter[0]) {
			snprintf(vf_strs[config->subtitle_count], sizeof(vf_strs[0]), "%s", scale_filter);
			argv[argc++] = "-vf";
			argv[argc++] = vf_strs[config->subtitle_count];
		} else {
			vf_strs[config->subtitle_count][0] = '\0';
			argv[argc++] = "-vf";
			argv[argc++] = vf_strs[config->subtitle_count];
		}
	} else if (use_subs && config->subtitle_path[0] != '\0') {
		// Single subtitle (legacy path: embedded or single external)
		// fontsdir: system fontconfig has no fonts, so point to our bundled font
		// force_style: only for external subs (SRT has no styling); skip for embedded
		//              ASS/SSA which have their own fonts and positioning
		if (config->subtitle_is_external) {
			snprintf(vf_str, sizeof(vf_str),
					 "%s%ssubtitles='%s':fontsdir='%s':force_style='Fontname=Rounded Mplus 1c Bold,FontSize=32'",
					 scale_filter, scale_filter[0] ? "," : "",
					 config->subtitle_path, RES_PATH);
		} else {
			snprintf(vf_str, sizeof(vf_str),
					 "%s%ssubtitles='%s':fontsdir='%s'",
					 scale_filter, scale_filter[0] ? "," : "",
					 config->subtitle_path, RES_PATH);
		}
		argv[argc++] = "-vf";
		argv[argc++] = vf_str;
	} else {
		// No subtitle filters — disable ffplay's built-in subtitle stream decoder
		// so it doesn't auto-render embedded subs (saves CPU, especially for HEVC)
		argv[argc++] = "-sn";
		if (scale_filter[0]) {
			snprintf(vf_str, sizeof(vf_str), "%s", scale_filter);
			argv[argc++] = "-vf";
			argv[argc++] = vf_str;
		}
	}

	// Window title
	if (config->title[0] != '\0') {
		argv[argc++] = "-window_title";
		argv[argc++] = config->title;
	}

	// Common playback options for all sources
	argv[argc++] = "-framedrop"; // Drop frames if decoding too slow
	argv[argc++] = "-fast";		 // Enable speed-optimized decoding
	// Skip expensive decode steps — critical for HEVC on this ARM chip,
	// negligible quality impact for H.264 on a small screen.
	argv[argc++] = "-skip_loop_filter"; // Skip deblocking/SAO filter
	argv[argc++] = "all";
	argv[argc++] = "-skip_idct"; // Skip IDCT on non-reference frames
	argv[argc++] = "noref";		 // note: "noref" not "nonref"

	// Stream-specific buffering options
	if (config->is_stream) {
		argv[argc++] = "-infbuf"; // Disable buffer size limit for live streams
		argv[argc++] = "-probesize";
		argv[argc++] = "5000000"; // 5MB probe size
		argv[argc++] = "-analyzeduration";
		argv[argc++] = "5000000";	  // 5 seconds analysis
		argv[argc++] = "-user_agent"; // YouTube CDN requires a browser User-Agent
		argv[argc++] = "Mozilla/5.0";
		argv[argc++] = "-reconnect";
		argv[argc++] = "1";
		argv[argc++] = "-reconnect_streamed";
		argv[argc++] = "1";
		argv[argc++] = "-reconnect_delay_max";
		argv[argc++] = "5"; // Retry up to 5s before giving up
	}

	// ClearKey decryption for DASH DRM streams (CENC)
	if (config->decryption_key[0] != '\0') {
		argv[argc++] = "-cenc_decryption_key";
		argv[argc++] = config->decryption_key;
	}

	// Input file (must be last)
	argv[argc++] = "-i";
	argv[argc++] = config->path;
	argv[argc] = NULL;


	// Set up Bluetooth audio routing before fork
	int bt_audio = is_bluetooth_audio();
	if (bt_audio) {
		// Set BlueALSA mixer to 100% so audio is audible
		system("amixer scontrols 2>/dev/null | grep -i 'A2DP' | "
			   "sed \"s/.*'\\([^']*\\)'.*/\\1/\" | "
			   "while read ctrl; do amixer sset \"$ctrl\" 127 2>/dev/null; done");
	}

	// Fork and exec ffplay
	ffplay_pid = fork();
	if (ffplay_pid < 0) {
		LOG_error("fork() failed: %s\n", strerror(errno));
		return -1;
	}

	if (ffplay_pid == 0) {
		// Child process: configure environment before exec
		if (bt_audio)
			setenv("AUDIODEV", "bluealsa", 1);
		// Generate a minimal fontconfig pointing to system fonts directory
		// so fontconfig doesn't scan the entire filesystem (~13s startup delay)
		if (use_subs) {
			FILE* fc = fopen("/tmp/ffplay-fonts.conf", "w");
			if (fc) {
				fprintf(fc,
						"<?xml version=\"1.0\"?>\n"
						"<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
						"<fontconfig>\n"
						"\t<dir>" RES_PATH "</dir>\n"
						"\t<cachedir>/tmp/fontconfig-cache</cachedir>\n"
						"</fontconfig>\n");
				fclose(fc);
				setenv("FONTCONFIG_FILE", "/tmp/ffplay-fonts.conf", 1);
			}
		}
		execv(FFPLAY_PATH, argv);
		_exit(127);
	}

	// Parent process: wait for ffplay to exit
	int status = 0;
	int result;
	do {
		result = waitpid(ffplay_pid, &status, 0);
	} while (result == -1 && errno == EINTR);

	ffplay_pid = 0;

	if (WIFEXITED(status)) {
		int code = WEXITSTATUS(status);
		if (code != 0) {
			LOG_error("ffplay exited with code %d, url: %s\n", code, config->path);
		}
		return code;
	}
	return -1;
}

int FfplayEngine_play(FfplayConfig* config) {
	if (!config || config->path[0] == '\0') {
		return -1;
	}

	// Check if ffplay binary exists before doing anything
	if (access(FFPLAY_PATH, X_OK) != 0) {
		LOG_error("ffplay binary not found: %s\n", FFPLAY_PATH);
		return -1;
	}

	LOG_info("ffplay: playing %s\n", config->path);

	// Release joysticks so ffplay can use them for input.
	// GFX is NOT released — ffplay opens its own SDL2 display independently.
	PAD_quit();

	int has_subs = (config->subtitle_path[0] != '\0') || (config->subtitle_count > 0);
	int exit_code = ffplay_exec(config, has_subs);

	// Re-initialize input and clear stale button states
	PAD_init();
	PAD_reset();

	return exit_code;
}

void FfplayEngine_stop(void) {
	if (ffplay_pid > 0) {
		kill(ffplay_pid, SIGTERM);
		// Give it a moment to clean up
		usleep(100000);
		// Force kill if still running
		kill(ffplay_pid, SIGKILL);
		waitpid(ffplay_pid, NULL, WNOHANG);
		ffplay_pid = 0;
	}
}
