# System-Wide Screenshot Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a system-wide screenshot hotkey (L2+R2+X) that captures the framebuffer to PNG, works inside games/emulators, and is toggled on/off via the Quick Menu.

**Architecture:** A background C daemon (`screenshot.elf`) reads raw Linux input events from `/dev/input/event0-4` (like `keymon`), detects the L2+R2+X combo, and captures `/dev/fb0` via ffmpeg. The Quick Menu gets a new "Screenshot" toggle to start/stop the daemon, following the existing screen recorder pattern.

**Tech Stack:** C, Linux evdev (`linux/input.h`), ffmpeg (fbdev capture), PID file for state tracking.

---

### Task 1: Create the screenshot daemon binary

**Files:**
- Create: `workspace/all/screenshot/screenshot.c`

**Step 1: Write `screenshot.c`**

This daemon follows the exact `keymon.c` pattern (see `workspace/tg5040/keymon/keymon.c`):
- Opens `/dev/input/event0-4` with `O_RDONLY | O_NONBLOCK | O_CLOEXEC`
- Polls at 60fps with `usleep(16666)`
- Tracks L2/R2 analog trigger state via `EV_ABS` events (codes `ABS_Z=2`, `ABS_RZ=5`)
- Detects X button press via `EV_KEY` event (code `BTN_WEST=0x133`, value=1)
- When L2+R2+X detected, forks ffmpeg to capture one frame from `/dev/fb0` as PNG
- Writes own PID to `/tmp/screenshot.pid` on startup
- Removes PID file on SIGTERM/exit
- Includes a cooldown (~1 second) to prevent rapid-fire captures

```c
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
#include <sys/wait.h>
#include <linux/input.h>

#define PID_FILE       "/tmp/screenshot.pid"
#define SCREENSHOT_DIR "/mnt/SDCARD/Images/Screenshots"
#define FFMPEG_PATH    "/usr/bin/ffmpeg"
#define INPUT_COUNT    5

// evdev codes for L2/R2 analog triggers and X button
#define ABS_Z_CODE   2    // L2 trigger axis
#define ABS_RZ_CODE  5    // R2 trigger axis
#define BTN_WEST_CODE 0x133 // X button (BTN_WEST / BTN_Y in some mappings)

// Also support alternate X button codes in case BTN_WEST doesn't match
#define BTN_NORTH_CODE 0x132

#define COOLDOWN_MS  1000  // minimum ms between screenshots

static int inputs[INPUT_COUNT] = {};
static volatile int quit = 0;

static void on_term(int sig) {
	quit = 1;
}

static void cleanup(void) {
	remove(PID_FILE);
	for (int i = 0; i < INPUT_COUNT; i++) {
		if (inputs[i] >= 0) close(inputs[i]);
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
		SCREENSHOT_DIR "/SCR_%04d%02d%02d_%02d%02d%02d.png",
		t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
		t->tm_hour, t->tm_min, t->tm_sec);

	pid_t pid = fork();
	if (pid < 0) return;

	if (pid == 0) {
		// Child: capture single frame from framebuffer
		setsid();
		freopen("/dev/null", "r", stdin);
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execl(FFMPEG_PATH, "ffmpeg", "-nostdin",
			"-f", "fbdev", "-frames:v", "1",
			"-i", "/dev/fb0",
			"-y", output,
			(char*)NULL);
		_exit(1);
	}

	// Parent: wait for ffmpeg to finish (single frame capture is fast)
	waitpid(pid, NULL, 0);
}

int main(int argc, char* argv[]) {
	// Signal handling
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
			if (inputs[i] < 0) continue;
			while (read(inputs[i], &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type == EV_ABS) {
					if (ev.code == ABS_Z_CODE) {
						l2_pressed = ev.value > 0;
					} else if (ev.code == ABS_RZ_CODE) {
						r2_pressed = ev.value > 0;
					}
				} else if (ev.type == EV_KEY && ev.value == 1) {
					// value 1 = pressed
					if (ev.code == BTN_WEST_CODE || ev.code == BTN_NORTH_CODE) {
						if (l2_pressed && r2_pressed &&
							(now_ms - last_capture_ms) > COOLDOWN_MS) {
							capture_screenshot();
							last_capture_ms = now_ms;
						}
					}
				}
			}
		}

		usleep(16666); // ~60fps polling
	}

	cleanup();
	return 0;
}
```

**Step 2: Verify the file compiles (locally or review)**

This is a cross-compiled binary, so just review the code for correctness.

**Step 3: Commit**

```bash
git add workspace/all/screenshot/screenshot.c
git commit -m "feat: add screenshot daemon binary"
```

---

### Task 2: Create the screenshot daemon makefile

**Files:**
- Create: `workspace/all/screenshot/makefile`

**Step 1: Write the makefile**

Follow the `keymon` makefile pattern (see `workspace/tg5040/keymon/makefile`). The screenshot daemon is minimal - it only needs `linux/input.h` (kernel headers) and standard libc. No SDL, no msettings.

```makefile
ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

TARGET = screenshot
PRODUCT = build/$(PLATFORM)/$(TARGET).elf

CC = $(CROSS_COMPILE)gcc
CFLAGS = -Os -s -Wl,--gc-sections

all:
	mkdir -p build/$(PLATFORM)
	$(CC) $(TARGET).c -o $(PRODUCT) $(CFLAGS)
clean:
	rm -f $(PRODUCT)
```

**Step 2: Commit**

```bash
git add workspace/all/screenshot/makefile
git commit -m "feat: add screenshot daemon makefile"
```

---

### Task 3: Add QUICK_SCREENSHOT to the QuickAction enum

**Files:**
- Modify: `workspace/all/nextui/types.h:54-64`

**Step 1: Add `QUICK_SCREENSHOT` to the enum**

Add it after `QUICK_SCREENRECORD`:

```c
enum QuickAction {
	QUICK_NONE = 0,
	QUICK_WIFI,
	QUICK_BLUETOOTH,
	QUICK_SLEEP,
	QUICK_REBOOT,
	QUICK_POWEROFF,
	QUICK_SETTINGS,
	QUICK_PAK_STORE,
	QUICK_SCREENRECORD,
	QUICK_SCREENSHOT,
};
```

**Step 2: Commit**

```bash
git add workspace/all/nextui/types.h
git commit -m "feat: add QUICK_SCREENSHOT to QuickAction enum"
```

---

### Task 4: Add Screenshot toggle to Quick Menu content

**Files:**
- Modify: `workspace/all/nextui/content.c:757-807` (function `getQuickToggles`)

**Step 1: Add the screenshot toggle entry**

Add after the screen recorder block (line ~794) and before the reboot block:

```c
{
	bool is_ss = access("/tmp/screenshot.pid", F_OK) == 0;
	Entry* ss = Entry_new(is_ss ? "Screenshot On" : "Screenshot", ENTRY_DIP);
	ss->quickId = QUICK_SCREENSHOT;
	Array_push(entries, ss);
}
```

This shows "Screenshot" when off and "Screenshot On" when the daemon is running, following the recorder's naming pattern.

**Step 2: Commit**

```bash
git add workspace/all/nextui/content.c
git commit -m "feat: add screenshot toggle to quick menu content"
```

---

### Task 5: Handle Screenshot toggle in Quick Menu input

**Files:**
- Modify: `workspace/all/nextui/quickmenu.c`

**Step 1: Add screenshot daemon helpers**

Add after the screen recording helpers section (after line ~157), before the `#define MENU_ITEM_SIZE` line:

```c
// ============================================
// Screenshot daemon helpers
// ============================================

#define SCREENSHOT_PID_FILE "/tmp/screenshot.pid"
#define SCREENSHOT_ELF_PATH SDCARD_PATH "/.system/bin/screenshot.elf"

static bool qm_is_screenshot_active(void) {
	FILE* f = fopen(SCREENSHOT_PID_FILE, "r");
	if (!f)
		return false;
	int pid = 0;
	fscanf(f, "%d", &pid);
	fclose(f);
	if (pid <= 0)
		return false;
	return kill(pid, 0) == 0;
}

static bool qm_start_screenshot(void) {
	pid_t pid = fork();
	if (pid < 0)
		return false;

	if (pid == 0) {
		setsid();
		freopen("/dev/null", "r", stdin);
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execl(SCREENSHOT_ELF_PATH, "screenshot", (char*)NULL);
		_exit(1);
	}

	// Brief wait to ensure daemon started
	usleep(200000);
	if (waitpid(pid, NULL, WNOHANG) != 0)
		return false;

	return true;
}

static void qm_stop_screenshot(void) {
	FILE* f = fopen(SCREENSHOT_PID_FILE, "r");
	if (!f)
		return;
	int pid = 0;
	fscanf(f, "%d", &pid);
	fclose(f);
	if (pid <= 0) {
		remove(SCREENSHOT_PID_FILE);
		return;
	}

	kill(pid, SIGTERM);
	for (int i = 0; i < 6; i++) {
		usleep(500000);
		int wr = waitpid(pid, NULL, WNOHANG);
		if (wr == pid)
			goto done;
		if (wr < 0 && kill(pid, 0) != 0)
			goto done;
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, WNOHANG);
done:
	remove(SCREENSHOT_PID_FILE);
}
```

**Step 2: Add screenshot toggle handler in `QuickMenu_handleInput`**

In the `BTN_A` handler section, add a new `else if` case after the `QUICK_SCREENRECORD` block (after line ~295) and before the generic `else` block:

```c
} else if (selected->type == ENTRY_DIP && selected->quickId == QUICK_SCREENSHOT) {
	if (qm_is_screenshot_active()) {
		qm_stop_screenshot();
	} else {
		qm_start_screenshot();
	}
	// Rebuild toggles to update label
	EntryArray_free(quickActions);
	quickActions = getQuickToggles(qm_simple_mode);
	qm_col = 0;
	result.dirty = true;
```

**Step 3: Add the icon case in `QuickMenu_render`**

In the toggle icon switch statement (around line ~575-601), add after the `QUICK_SCREENRECORD` case:

```c
case QUICK_SCREENSHOT:
	asset = ASSET_GAMEPAD; // reuse existing asset for now
	break;
```

**Step 4: Add screenshot toggle hint handling**

In the button hints section (around line ~444), update the `is_screenrec` check to also cover screenshot:

```c
bool is_screenrec = (qm_row == QM_ROW_TOGGLES &&
    (current->quickId == QUICK_SCREENRECORD || current->quickId == QUICK_SCREENSHOT));
```

**Step 5: Commit**

```bash
git add workspace/all/nextui/quickmenu.c
git commit -m "feat: add screenshot toggle handling in quick menu"
```

---

### Task 6: Add screenshot to the workspace build

**Files:**
- Modify: `workspace/makefile`

**Step 1: Add screenshot build step**

In the `else` (non-desktop) block, add after the `show2` line (line 57):

```makefile
	cd ./all/screenshot/ && make
```

Also add to the `clean` target:

```makefile
	cd ./all/screenshot/ && make clean
```

**Step 2: Commit**

```bash
git add workspace/makefile
git commit -m "feat: add screenshot daemon to workspace build"
```

---

### Task 7: Final review and integration commit

**Step 1: Review all changes**

Verify:
- `screenshot.c` compiles cleanly (review for syntax errors)
- `types.h` enum is correct
- `content.c` toggle entry follows existing pattern
- `quickmenu.c` start/stop/toggle logic is consistent with screen recorder
- `makefile` build order is correct
- Screenshot ELF path matches where the binary will be deployed (`.system/bin/screenshot.elf`)

**Step 2: Note for deployment**

The `screenshot.elf` binary needs to be placed at `SDCARD_PATH/.system/bin/screenshot.elf` on the device. Ensure the release/packaging step copies `build/$(PLATFORM)/screenshot.elf` to the correct location in the skeleton.

**Step 3: Final commit (if any fixups needed)**

```bash
git add -A && git commit -m "feat: system-wide screenshot capture via L2+R2+X"
```
