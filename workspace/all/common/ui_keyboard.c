#include "ui_keyboard.h"
#include "defines.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static char keyboard_path[512] = "";
static int keyboard_initialized = 0;

void UIKeyboard_init(void) {
	if (keyboard_initialized)
		return;

	snprintf(keyboard_path, sizeof(keyboard_path), BIN_PATH "/keyboard");
	chmod(keyboard_path, 0755);

	keyboard_initialized = 1;
}

char* UIKeyboard_open(const char* prompt) {
	(void)prompt;

	if (!keyboard_initialized)
		UIKeyboard_init();

	if (access(keyboard_path, X_OK) != 0) {
		LOG_error("Keyboard binary not found: %s\n", keyboard_path);
		return NULL;
	}

	const char* font_path = RES_PATH "/font1.ttf";

	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "%s \"%s\" 2>/dev/null", keyboard_path, font_path);

	FILE* pipe = popen(cmd, "r");
	if (!pipe)
		return NULL;

	char* result = malloc(512);
	if (result) {
		result[0] = '\0';
		if (fgets(result, 512, pipe)) {
			char* nl = strchr(result, '\n');
			if (nl)
				*nl = '\0';
		}

		if (result[0] == '\0') {
			free(result);
			result = NULL;
		}
	}

	pclose(pipe);

	// Flush stale SDL events accumulated while keyboard binary was running
	SDL_Event e;
	while (SDL_PollEvent(&e))
		;
	PAD_reset();

	return result;
}
