#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "config.h"
#include "types.h"
#include "recents.h"
#include "content.h"
#include "launcher.h"

static CleanupPoolFunc _cleanupPool = NULL;

void Launcher_setCleanupFunc(CleanupPoolFunc func) {
	_cleanupPool = func;
}

///////////////////////////////////////

void queueNext(char* cmd) {
	LOG_info("cmd: %s\n", cmd);
	putFile("/tmp/next", cmd);
	quit = 1;
}

// based on https://stackoverflow.com/a/31775567/145965
int replaceString(char *line, const char *search, const char *replace) {
   char *sp; // start of pattern
   if ((sp = strstr(line, search)) == NULL) {
      return 0;
   }
   int count = 1;
   int sLen = strlen(search);
   int rLen = strlen(replace);
   if (sLen > rLen) {
      // move from right to left
      char *src = sp + sLen;
      char *dst = sp + rLen;
      while((*dst = *src) != '\0') { dst++; src++; }
   } else if (sLen < rLen) {
      // move from left to right
      int tLen = strlen(sp) - sLen;
      char *stop = sp + rLen;
      char *src = sp + sLen + tLen;
      char *dst = sp + rLen + tLen;
      while(dst >= stop) { *dst = *src; dst--; src--; }
   }
   memcpy(sp, replace, rLen);
   count += replaceString(sp + rLen, search, replace);
   return count;
}
char* escapeSingleQuotes(char* str) {
	// why not call replaceString directly?
	// call points require the modified string be returned
	// but replaceString is recursive and depends on its
	// own return value (but does it need to?)
	replaceString(str, "'", "'\\''");
	return str;
}

///////////////////////////////////////

void readyResumePath(char* rom_path, int type) {
	char* tmp;
	can_resume = 0;
	has_preview = 0;
	has_boxart = 0;
	char path[256];
	strcpy(path, rom_path);

	if (!prefixMatch(ROMS_PATH, path)) return;

	char auto_path[256];
	if (type==ENTRY_DIR) {
		if (!hasCue(path, auto_path)) { // no cue?
			tmp = strrchr(auto_path, '.') + 1; // extension
			strcpy(tmp, "m3u"); // replace with m3u
			if (!exists(auto_path)) return; // no m3u
		}
		strcpy(path, auto_path); // cue or m3u if one exists
	}

	if (!suffixMatch(".m3u", path)) {
		char m3u_path[256];
		if (hasM3u(path, m3u_path)) {
			// change path to m3u path
			strcpy(path, m3u_path);
		}
	}

	char emu_name[256];
	getEmuName(path, emu_name);

	char rom_file[256];
	tmp = strrchr(path, '/') + 1;
	strcpy(rom_file, tmp);

	sprintf(slot_path, "%s/.minui/%s/%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file); // /.userdata/.minui/<EMU>/<romname>.ext.txt
	can_resume = exists(slot_path);

	// slot_path contains a single integer representing the last used slot
	if (can_resume) {
		char slot[16];
		getFile(slot_path, slot, 16);
		int s = atoi(slot);
		sprintf(preview_path, "%s/.minui/%s/%s.%0d.bmp", SHARED_USERDATA_PATH, emu_name, rom_file, s); // /.userdata/.minui/<EMU>/<romname>.ext.<n>.bmp
		has_preview = exists(preview_path);
	}

	// Boxart fallback: if no savestate preview, check for boxart in .media folder
	if (!has_preview) {
		char rom_dir[256];
		char rom_name[256];
		strcpy(rom_dir, rom_path);
		char* last_slash = strrchr(rom_dir, '/');
		if (last_slash) {
			*last_slash = '\0';  // rom_dir now has directory
			strcpy(rom_name, last_slash + 1);  // rom_name has filename with ext
			char* dot = strrchr(rom_name, '.');
			if (dot) *dot = '\0';  // remove extension
			sprintf(boxart_path, "%s/.media/%s.png", rom_dir, rom_name);
			has_boxart = exists(boxart_path);

			// For multi-disk games in folders: if boxart not found, check parent folder
			// e.g., /Roms/PS1/GameFolder/game.m3u -> check /Roms/PS1/.media/GameFolder.png
			if (!has_boxart) {
				char parent_dir[256];
				char folder_name[256];
				strcpy(parent_dir, rom_dir);
				char* parent_slash = strrchr(parent_dir, '/');
				if (parent_slash) {
					*parent_slash = '\0';  // parent_dir now has grandparent directory
					strcpy(folder_name, parent_slash + 1);  // folder_name has the game folder name
					sprintf(boxart_path, "%s/.media/%s.png", parent_dir, folder_name);
					has_boxart = exists(boxart_path);
				}
			}
		}
	}
}
void readyResume(Entry* entry) {
	readyResumePath(entry->path, entry->type);
}

int autoResume(void) {
	// NOTE: bypasses recents

	if (!exists(AUTO_RESUME_PATH)) return 0;

	char path[256];
	getFile(AUTO_RESUME_PATH, path, 256);
	unlink(AUTO_RESUME_PATH);
	sync();

	// make sure rom still exists
	char sd_path[256];
	sprintf(sd_path, "%s%s", SDCARD_PATH, path);
	if (!exists(sd_path)) return 0;

	// make sure emu still exists
	char emu_name[256];
	getEmuName(sd_path, emu_name);

	char emu_path[256];
	getEmuPath(emu_name, emu_path);

	if (!exists(emu_path)) return 0;

	// putFile(LAST_PATH, FAUX_RECENT_PATH); // saveLast() will crash here because top is NULL

	char act[256];
	sprintf(act, "gametimectl.elf start '%s'", escapeSingleQuotes(sd_path));
	system(act);

	char cmd[256];
	// dont escape sd_path again because it was already escaped for gametimectl and function modifies input str aswell
	sprintf(cmd, "'%s' '%s'", escapeSingleQuotes(emu_path), sd_path);
	putInt(RESUME_SLOT_PATH, AUTO_RESUME_SLOT);
	queueNext(cmd);
	return 1;
}

void openPak(char* path) {
	// NOTE: escapeSingleQuotes() modifies the passed string
	// so we need to save the path before we call that
	// if (prefixMatch(ROMS_PATH, path)) {
	// 	addRecent(path);
	// }
	saveLast(path);

	char cmd[256];
	sprintf(cmd, "'%s/launch.sh'", escapeSingleQuotes(path));
	queueNext(cmd);
}
void openRom(char* path, char* last) {
	LOG_info("openRom(%s,%s)\n", path, last);

	char sd_path[256];
	strcpy(sd_path, path);

	char m3u_path[256];
	int has_m3u = hasM3u(sd_path, m3u_path);

	char recent_path[256];
	strcpy(recent_path, has_m3u ? m3u_path : sd_path);

	if (has_m3u && suffixMatch(".m3u", sd_path)) {
		getFirstDisc(m3u_path, sd_path);
	}

	char emu_name[256];
	getEmuName(sd_path, emu_name);

	if (should_resume) {
		char slot[16];
		getFile(slot_path, slot, 16);
		putFile(RESUME_SLOT_PATH, slot);
		should_resume = 0;

		if (has_m3u) {
			char rom_file[256];
			strcpy(rom_file, strrchr(m3u_path, '/') + 1);

			// get disc for state
			char disc_path_path[256];
			sprintf(disc_path_path, "%s/.minui/%s/%s.%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file, slot); // /.userdata/arm-480/.minui/<EMU>/<romname>.ext.0.txt

			if (exists(disc_path_path)) {
				// switch to disc path
				char disc_path[256];
				getFile(disc_path_path, disc_path, 256);
				if (disc_path[0]=='/') strcpy(sd_path, disc_path); // absolute
				else { // relative
					strcpy(sd_path, m3u_path);
					char* tmp = strrchr(sd_path, '/') + 1;
					strcpy(tmp, disc_path);
				}
			}
		}
	}
	else putInt(RESUME_SLOT_PATH,8); // resume hidden default state

	char emu_path[256];
	getEmuPath(emu_name, emu_path);

	// NOTE: escapeSingleQuotes() modifies the passed string
	// so we need to save the path before we call that
	Recents_add(recent_path, Recents_getAlias()); // yiiikes
	saveLast(last==NULL ? sd_path : last);
	char act[256];
	sprintf(act, "gametimectl.elf start '%s'", escapeSingleQuotes(sd_path));
	system(act);
	char cmd[256];
	// dont escape sd_path again because it was already escaped for gametimectl and function modifies input str aswell
	sprintf(cmd, "'%s' '%s'", escapeSingleQuotes(emu_path), sd_path);
	queueNext(cmd);
}

static bool isDirectSubdirectory(const Directory* parent, const char* child_path) {
    const char* parent_path = parent->path;

    size_t parent_len = strlen(parent_path);
    size_t child_len = strlen(child_path);

    // Child must be longer than parent to be a subdirectory
    if (child_len <= parent_len || strncmp(child_path, parent_path, parent_len) != 0) {
        return false;
    }

    // Next char after parent path must be '/'
    if (child_path[parent_len] != '/') return false;

    // Walk through the child path after parent, skipping PLATFORM segments
    const char* cursor = child_path + parent_len + 1; // skip the slash

    int levels = 0;
    while (*cursor) {
        const char* next = strchr(cursor, '/');
        size_t segment_len = next ? (size_t)(next - cursor) : strlen(cursor);

        if (segment_len == 0) break;

        // Copy segment into a buffer to compare
        char segment[PATH_MAX];
        if (segment_len >= PATH_MAX) return false;
        strncpy(segment, cursor, segment_len);
        segment[segment_len] = '\0';

        // Count level only if it's not PLATFORM
        if (strcmp(segment, PLATFORM) != 0 && strcmp(segment, "Roms") != 0) {
            levels++;
        }

        if (!next) break;
        cursor = next + 1;
    }

    return (levels == 1);  // exactly one meaningful level deeper
}

Array* pathToStack(const char* path) {
	Array* array = Array_new();

	if (!path || strlen(path) == 0) return array;

	if (!prefixMatch(SDCARD_PATH, path)) return array;

	// Always include root directory
	Directory* root_dir = Directory_new(SDCARD_PATH, 0);
	root_dir->start = 0;
	root_dir->end = (root_dir->entries->count < MAIN_ROW_COUNT) ? root_dir->entries->count : MAIN_ROW_COUNT;
	Array_push(array, root_dir);

	if (exactMatch(path, SDCARD_PATH)) return array;

	char temp_path[PATH_MAX];
	strcpy(temp_path, SDCARD_PATH);
	size_t current_len = strlen(SDCARD_PATH);

	const char* cursor = path + current_len;
	if (*cursor == '/') cursor++;

	while (*cursor) {
		const char* next = strchr(cursor, '/');
		size_t segment_len = next ? (size_t)(next - cursor) : strlen(cursor);
		if (segment_len == 0 || segment_len >= PATH_MAX) break;

		char segment[PATH_MAX];
		strncpy(segment, cursor, segment_len);
		segment[segment_len] = '\0';

		// Append '/' if needed
		if (temp_path[current_len - 1] != '/') {
			if (current_len + 1 >= PATH_MAX) break;
			temp_path[current_len++] = '/';
			temp_path[current_len] = '\0';
		}

		// Append segment
		if (current_len + segment_len >= PATH_MAX) break;
		strcat(temp_path, segment);
		current_len += segment_len;

		if (strcmp(segment, PLATFORM) == 0) {
			// Merge with previous directory
			if (array->count > 0) {
				// Remove the previous directory
				Directory* last = (Directory*)array->items[array->count - 1];
				Array_pop(array);
				Directory_free(last); // assuming you have a Directory_free

				// Replace with updated one using combined path
				Directory* merged = Directory_new(temp_path, 0);
				merged->start = 0;
				merged->end = (merged->entries->count < MAIN_ROW_COUNT) ? merged->entries->count : MAIN_ROW_COUNT;
				Array_push(array, merged);
			}
		} else {
			Directory* dir = Directory_new(temp_path, 0);
			dir->start = 0;
			dir->end = (dir->entries->count < MAIN_ROW_COUNT) ? dir->entries->count : MAIN_ROW_COUNT;
			Array_push(array, dir);
		}

		if (!next) break;
		cursor = next + 1;
	}

	return array;
}

void openDirectory(char* path, int auto_launch) {
	char auto_path[256];
	if (hasCue(path, auto_path) && auto_launch) {
		openRom(auto_path, path);
		return;
	}

	char m3u_path[256];
	strcpy(m3u_path, auto_path);
	char* tmp = strrchr(m3u_path, '.') + 1; // extension
	strcpy(tmp, "m3u"); // replace with m3u
	if (exists(m3u_path) && auto_launch) {
		auto_path[0] = '\0';
		if (getFirstDisc(m3u_path, auto_path)) {
			openRom(auto_path, path);
			return;
		}
		// TODO: doesn't handle empty m3u files
	}

	// If this is the exact same directory for some reason, just return.
	if(top && strcmp(top->path, path) == 0)
		return;

	// If this path is a direct subdirectory of top, push it on top of the stack
	// If it isnt, we need to recreate the stack to keep navigation consistent
	if(!top || isDirectSubdirectory(top, path)) {
		int selected = 0;
		int start = 0;
		int end = 0;
		if (top && top->entries->count>0) {
			if (restore_depth==stack->count && top->selected==restore_relative) {
				selected = restore_selected;
				start = restore_start;
				end = restore_end;
			}
		}

		top = Directory_new(path, selected);
		top->start = start;
		top->end = end ? end : ((top->entries->count<MAIN_ROW_COUNT) ? top->entries->count : MAIN_ROW_COUNT);

		Array_push(stack, top);
	}
	else {
		// keep a copy of path, which might be a reference into stack which is about to be freed
		char temp_path[256];
		strcpy(temp_path, path);

		// construct a fresh stack by walking upwards until SDCARD_ROOT
		DirectoryArray_free(stack);

		stack = pathToStack(temp_path);
		top = stack->items[stack->count - 1];
	}
}

void closeDirectory(void) {
	restore_selected = top->selected;
	restore_start = top->start;
	restore_end = top->end;
	DirectoryArray_pop(stack);
	restore_depth = stack->count;
	top = stack->items[stack->count-1];
	restore_relative = top->selected;
}

void toggleQuick(Entry* self)
{
	if(!self)
		return;

	if(!strcmp(self->name, "Wifi")) {
		WIFI_enable(!WIFI_enabled());
	}
	else if(!strcmp(self->name, "Bluetooth")) {
		BT_enable(!BT_enabled());
	}
	else if(!strcmp(self->name, "Sleep")) {
		PWR_sleep();
	}
	else if(!strcmp(self->name, "Reboot")) {
		if (_cleanupPool) _cleanupPool();
		PWR_powerOff(1);
	}
	else if(!strcmp(self->name, "Poweroff")) {
		if (_cleanupPool) _cleanupPool();
		PWR_powerOff(0);
	}
}

void Entry_open(Entry* self) {
	Recents_setAlias(self->name);  // yiiikes
	if (self->type==ENTRY_ROM) {
		startgame = 1;
		char *last = NULL;
		if (prefixMatch(COLLECTIONS_PATH, top->path)) {
			char* tmp;
			char filename[256];

			tmp = strrchr(self->path, '/');
			if (tmp) strcpy(filename, tmp+1);

			char last_path[256];
			sprintf(last_path, "%s/%s", top->path, filename);
			last = last_path;
		}
		openRom(self->path, last);
	}
	else if (self->type==ENTRY_PAK) {
		startgame = 1;
		openPak(self->path);
	}
	else if (self->type==ENTRY_DIR) {
		openDirectory(self->path, 1);
	}
	else if (self->type==ENTRY_DIP) {
		toggleQuick(self);
	}
}

///////////////////////////////////////

void saveLast(char* path) {
	// special case for recently played
	if (exactMatch(top->path, FAUX_RECENT_PATH)) {
		// NOTE: that we don't have to save the file because
		// your most recently played game will always be at
		// the top which is also the default selection
		path = FAUX_RECENT_PATH;
	}
	putFile(LAST_PATH, path);
}
void loadLast(void) { // call after loading root directory
	if (!exists(LAST_PATH)) return;

	char last_path[256];
	getFile(LAST_PATH, last_path, 256);

	char full_path[256];
	strcpy(full_path, last_path);

	char* tmp;
	char filename[256];
	tmp = strrchr(last_path, '/');
	if (tmp) strcpy(filename, tmp);

	Array* last = Array_new();
	while (!exactMatch(last_path, SDCARD_PATH)) {
		Array_push(last, strdup(last_path));

		char* slash = strrchr(last_path, '/');
		last_path[(slash-last_path)] = '\0';
	}

	while (last->count>0) {
		char* path = Array_pop(last);
		if (!exactMatch(path, ROMS_PATH)) { // romsDir is effectively root as far as restoring state after a game
			char collated_path[256];
			collated_path[0] = '\0';
			if (suffixMatch(")", path) && isConsoleDir(path)) {
				strcpy(collated_path, path);
				tmp = strrchr(collated_path, '(');
				if (tmp) tmp[1] = '\0'; // 1 because we want to keep the opening parenthesis to avoid collating "Game Boy Color" and "Game Boy Advance" into "Game Boy"
			}

			for (int i=0; i<top->entries->count; i++) {
				Entry* entry = top->entries->items[i];

				// NOTE: strlen() is required for collated_path, '\0' wasn't reading as NULL for some reason
				if (exactMatch(entry->path, path) || (strlen(collated_path) && prefixMatch(collated_path, entry->path)) || (prefixMatch(COLLECTIONS_PATH, full_path) && suffixMatch(filename, entry->path))) {
					top->selected = i;
					if (i>=top->end) {
						top->start = i;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>top->entries->count) {
							top->end = top->entries->count;
							top->start = top->end - MAIN_ROW_COUNT;
						}
					}
					if (last->count==0 && !exactMatch(entry->path, FAUX_RECENT_PATH) && !(!exactMatch(entry->path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, entry->path))) break; // don't show contents of auto-launch dirs

					if (entry->type==ENTRY_DIR) {
						openDirectory(entry->path, 0);
						break;
					}
				}
			}
		}
		free(path); // we took ownership when we popped it
	}

	StringArray_free(last);

	if (top->selected >= 0 && top->selected < top->entries->count) {
		Entry *selected_entry = top->entries->items[top->selected];
		readyResume(selected_entry);
	}
}
