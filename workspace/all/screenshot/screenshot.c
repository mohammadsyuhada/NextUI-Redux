#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <linux/input.h>

#define PID_FILE "/tmp/screenshot.pid"
#define SCREENSHOT_DIR "/mnt/SDCARD/Images/Screenshots"
#define FFMPEG_PATH "/usr/bin/ffmpeg"
#define INPUT_COUNT 5

// evdev codes for L2/R2 analog triggers and X button
#define ABS_Z_CODE 2		// L2 trigger axis
#define ABS_RZ_CODE 5		// R2 trigger axis
#define BTN_WEST_CODE 0x134 // X button (BTN_WEST)

#define COOLDOWN_MS 1000 // minimum ms between screenshots

static int inputs[INPUT_COUNT] = {};
static volatile int quit = 0;

static void on_term(int sig) {
	quit = 1;
}

static void cleanup(void) {
	remove(PID_FILE);
	for (int i = 0; i < INPUT_COUNT; i++) {
		if (inputs[i] >= 0)
			close(inputs[i]);
	}
}

static void mkdir_p(const char* path) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char* p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static void capture_screenshot(void) {
	mkdir_p(SCREENSHOT_DIR);

	time_t now = time(NULL);
	struct tm* t = localtime(&now);
	char output[512];
	snprintf(output, sizeof(output),
			 SCREENSHOT_DIR "/SCR_%04d%02d%02d_%02d%02d%02d.jpg",
			 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
			 t->tm_hour, t->tm_min, t->tm_sec);

	pid_t pid = fork();
	if (pid < 0)
		return;

	if (pid == 0) {
		setsid();
		freopen("/dev/null", "r", stdin);
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execl(FFMPEG_PATH, "ffmpeg", "-nostdin",
			  "-f", "fbdev", "-i", "/dev/fb0",
			  "-frames:v", "1", "-c:v", "mjpeg", "-q:v", "2",
			  "-y", output,
			  (char*)NULL);
		_exit(1);
	}

	// Wait for ffmpeg to finish (single frame capture is fast)
	waitpid(pid, NULL, 0);
}

int main(int argc, char* argv[]) {
	struct sigaction sa = {0};
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	// Write PID file
	FILE* f = fopen(PID_FILE, "w");
	if (f) {
		fprintf(f, "%d", getpid());
		fclose(f);
	}

	// Open input devices
	char path[32];
	for (int i = 0; i < INPUT_COUNT; i++) {
		sprintf(path, "/dev/input/event%i", i);
		inputs[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}

	int l2_pressed = 0;
	int r2_pressed = 0;
	uint32_t last_capture_ms = 0;
	struct input_event ev;
	struct timeval tod;

	while (!quit) {
		gettimeofday(&tod, NULL);
		uint32_t now_ms = tod.tv_sec * 1000 + tod.tv_usec / 1000;

		for (int i = 0; i < INPUT_COUNT; i++) {
			if (inputs[i] < 0)
				continue;
			while (read(inputs[i], &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type == EV_ABS) {
					if (ev.code == ABS_Z_CODE)
						l2_pressed = ev.value > 0;
					else if (ev.code == ABS_RZ_CODE)
						r2_pressed = ev.value > 0;
				} else if (ev.type == EV_KEY && ev.value == 1) {
					if (ev.code == BTN_WEST_CODE &&
						l2_pressed && r2_pressed &&
						(now_ms - last_capture_ms) > COOLDOWN_MS) {
						capture_screenshot();
						last_capture_ms = now_ms;
					}
				}
			}
		}

		usleep(16666); // ~60fps polling
	}

	cleanup();
	return 0;
}
