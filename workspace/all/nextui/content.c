#include <dirent.h>
#include <ctype.h>
#include "content.h"
#include "shortcuts.h"
#include "api.h"
#include "config.h"

static bool _simple_mode = false;

void Content_setSimpleMode(bool mode) {
	_simple_mode = mode;
}

///////////////////////////////////////
// Helpers

int getIndexChar(char* str) {
	char i = 0;
	char c = tolower(str[0]);
	if (c >= 'a' && c <= 'z')
		i = (c - 'a') + 1;
	return i;
}

void getUniqueName(Entry* entry, char* out_name) {
	char* slash = strrchr(entry->path, '/');
	if (!slash)
		return;
	char emu_tag[MAX_PATH];
	getEmuName(entry->path, emu_tag);
	snprintf(out_name, MAX_PATH, "%s (%s)", entry->name, emu_tag);
}

///////////////////////////////////////
// Directory indexing

void Directory_index(Directory* self) {
	int is_collection = prefixMatch(COLLECTIONS_PATH, self->path);
	int skip_index = exactMatch(FAUX_RECENT_PATH, self->path) || is_collection; // not alphabetized

	Hash* map = NULL;
	char map_path[MAX_PATH];
	snprintf(map_path, sizeof(map_path), "%s/map.txt", is_collection ? COLLECTIONS_PATH : self->path);

	if (exists(map_path)) {
		FILE* file = fopen(map_path, "r");
		if (file) {
			map = Hash_new();
			char line[MAX_PATH];
			while (fgets(line, sizeof(line), file) != NULL) {
				normalizeNewline(line);
				trimTrailingNewlines(line);
				if (strlen(line) == 0)
					continue; // skip empty lines

				char* tmp = strchr(line, '\t');
				if (tmp) {
					tmp[0] = '\0';
					char* key = line;
					char* value = tmp + 1;
					Hash_set(map, key, value);
				}
			}
			fclose(file);

			bool resort = false;
			bool filter = false;
			for (int i = 0; i < self->entries->count; i++) {
				Entry* entry = self->entries->items[i];
				char* slash = strrchr(entry->path, '/');
				if (!slash)
					continue;
				char* filename = slash + 1;
				char* alias = Hash_get(map, filename);
				if (alias) {
					free(entry->name);
					entry->name = strdup(alias);
					resort = true;
					if (!filter && hide(entry->name))
						filter = true;
				}
			}

			if (filter) {
				Array* entries = Array_new();
				for (int i = 0; i < self->entries->count; i++) {
					Entry* entry = self->entries->items[i];
					if (hide(entry->name)) {
						Entry_free(entry);
					} else {
						Array_push(entries, entry);
					}
				}
				Array_free(self->entries);
				self->entries = entries;
			}
			if (resort)
				EntryArray_sort(self->entries);
		}
	}

	Entry* prior = NULL;
	int alpha = -1;
	int index = 0;
	for (int i = 0; i < self->entries->count; i++) {
		Entry* entry = self->entries->items[i];

		if (prior != NULL && exactMatch(prior->name, entry->name)) {
			free(prior->unique);
			free(entry->unique);
			prior->unique = NULL;
			entry->unique = NULL;

			char* prior_slash = strrchr(prior->path, '/');
			char* entry_slash = strrchr(entry->path, '/');
			if (prior_slash && entry_slash) {
				char* prior_filename = prior_slash + 1;
				char* entry_filename = entry_slash + 1;
				if (exactMatch(prior_filename, entry_filename)) {
					char prior_unique[MAX_PATH] = {0};
					char entry_unique[MAX_PATH] = {0};
					getUniqueName(prior, prior_unique);
					getUniqueName(entry, entry_unique);

					prior->unique = strdup(prior_unique);
					entry->unique = strdup(entry_unique);
				} else {
					prior->unique = strdup(prior_filename);
					entry->unique = strdup(entry_filename);
				}
			}
		}

		if (!skip_index) {
			int a = getIndexChar(entry->name);
			if (a != alpha) {
				index = self->alphas.count;
				IntArray_push(&self->alphas, i);
				alpha = a;
			}
			entry->alpha = index;
		}

		prior = entry;
	}

	if (map)
		Hash_free(map);
}

///////////////////////////////////////
// Directory construction

Directory* Directory_new(char* path, int selected) {
	char display_name[MAX_PATH];
	getDisplayName(path, display_name);

	Directory* self = malloc(sizeof(Directory));
	self->path = strdup(path);
	self->name = strdup(display_name);
	if (exactMatch(path, SDCARD_PATH)) {
		self->entries = getRoot(_simple_mode);
	} else if (exactMatch(path, FAUX_RECENT_PATH)) {
		self->entries = Recents_getEntries();
	} else if (exactMatch(path, ROMS_PATH)) {
		self->entries = getRoms();
	} else if (!exactMatch(path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, path) && suffixMatch(".txt", path)) {
		self->entries = getCollection(path);
	} else if (suffixMatch(".m3u", path)) {
		self->entries = getDiscs(path);
	} else {
		self->entries = getEntries(path);
	}
	IntArray_init(&self->alphas);
	self->selected = selected;
	Directory_index(self);
	return self;
}

///////////////////////////////////////
// Content query helpers

Entry* entryFromPakName(char* pak_name) {
	char pak_path[MAX_PATH];
	// Check in Tools
	snprintf(pak_path, sizeof(pak_path), "%s/Tools/%s/%s.pak", SDCARD_PATH, PLATFORM, pak_name);
	if (exists(pak_path))
		return Entry_newNamed(pak_path, ENTRY_PAK, pak_name);

	// Check in Emus
	snprintf(pak_path, sizeof(pak_path), "%s/Emus/%s.pak", PAKS_PATH, pak_name);
	if (exists(pak_path))
		return Entry_newNamed(pak_path, ENTRY_PAK, pak_name);

	// Check in platform Emus
	snprintf(pak_path, sizeof(pak_path), "%s/Emus/%s/%s.pak", SDCARD_PATH, PLATFORM, pak_name);
	if (exists(pak_path))
		return Entry_newNamed(pak_path, ENTRY_PAK, pak_name);

	return NULL;
}

int hasEmu(char* emu_name) {
	char pak_path[MAX_PATH];
	snprintf(pak_path, sizeof(pak_path), "%s/Emus/%s.pak/launch.sh", PAKS_PATH, emu_name);
	if (exists(pak_path))
		return 1;

	snprintf(pak_path, sizeof(pak_path), "%s/Emus/%s/%s.pak/launch.sh", SDCARD_PATH, PLATFORM, emu_name);
	return exists(pak_path);
}

int hasCue(char* dir_path, char* cue_path) { // NOTE: dir_path not rom_path
	char* slash = strrchr(dir_path, '/');
	if (!slash)
		return 0;
	char* tmp = slash + 1;
	snprintf(cue_path, MAX_PATH, "%s/%s.cue", dir_path, tmp);
	return exists(cue_path);
}

int hasM3u(char* rom_path, char* m3u_path) { // NOTE: rom_path not dir_path
	char work[MAX_PATH];
	strncpy(work, rom_path, MAX_PATH - 1);
	work[MAX_PATH - 1] = '\0';

	// strip filename to get parent dir (e.g. /Roms/PSX/Game/disc1.bin → /Roms/PSX/Game)
	char* tmp = strrchr(work, '/');
	if (!tmp)
		return 0;
	tmp[0] = '\0';

	// get directory name (e.g. "Game" from /Roms/PSX/Game)
	char* dir_name = strrchr(work, '/');
	if (!dir_name)
		return 0;
	dir_name++; // skip the slash

	// build m3u path: parent_dir + "/" + dir_name + ".m3u"
	// e.g. /Roms/PSX/Game/Game.m3u
	snprintf(m3u_path, MAX_PATH, "%s/%s.m3u", work, dir_name);

	return exists(m3u_path);
}

int canPinEntry(Entry* entry) {
	// PAK and ROM can always be pinned
	if (entry->type == ENTRY_PAK || entry->type == ENTRY_ROM) {
		return 1;
	}
	// ENTRY_DIR can be pinned only if it has a .cue or .m3u file (multi-disc game)
	if (entry->type == ENTRY_DIR) {
		char cue_path[MAX_PATH];
		if (hasCue(entry->path, cue_path))
			return 1;
		// hasM3u expects a file path, so build the m3u path directly for directories:
		// e.g. /Roms/PSX/Game → /Roms/PSX/Game/Game.m3u
		char* dir_name = strrchr(entry->path, '/');
		if (dir_name) {
			char m3u_path[MAX_PATH];
			snprintf(m3u_path, sizeof(m3u_path), "%s%s.m3u", entry->path, dir_name);
			return exists(m3u_path);
		}
		return 0;
	}
	return 0;
}

int hasCollections(void) {
	int has = 0;
	if (!exists(COLLECTIONS_PATH))
		return has;

	DIR* dh = opendir(COLLECTIONS_PATH);
	if (!dh)
		return has;
	struct dirent* dp;
	while ((dp = readdir(dh)) != NULL) {
		if (hide(dp->d_name))
			continue;
		has = 1;
		break;
	}
	closedir(dh);
	return has;
}

int hasRoms(char* dir_name) {
	int has = 0;
	char emu_name[MAX_PATH];
	char rom_path[MAX_PATH];

	getEmuName(dir_name, emu_name);

	// check for emu pak
	if (!hasEmu(emu_name))
		return has;

	// check for at least one non-hidden file (we're going to assume it's a rom)
	snprintf(rom_path, sizeof(rom_path), "%s/%s/", ROMS_PATH, dir_name);
	DIR* dh = opendir(rom_path);
	if (dh != NULL) {
		struct dirent* dp;
		while ((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name))
				continue;
			has = 1;
			break;
		}
		closedir(dh);
	}
	return has;
}

int hasTools(void) {
	char tools_path[MAX_PATH];
	snprintf(tools_path, sizeof(tools_path), "%s/Tools/%s", SDCARD_PATH, PLATFORM);
	return exists(tools_path);
}

int isConsoleDir(char* path) {
	char parent_dir[MAX_PATH];
	strncpy(parent_dir, path, MAX_PATH - 1);
	parent_dir[MAX_PATH - 1] = '\0';
	char* tmp = strrchr(parent_dir, '/');
	if (!tmp)
		return 0;
	tmp[0] = '\0';

	return exactMatch(parent_dir, ROMS_PATH);
}

///////////////////////////////////////
// Content retrieval

Array* getRoms(void) {
	Array* entries = Array_new();
	DIR* dh = opendir(ROMS_PATH);
	if (dh) {
		struct dirent* dp;
		char full_path[MAX_PATH];
		snprintf(full_path, sizeof(full_path), "%s/", ROMS_PATH);
		char* tmp = full_path + strlen(full_path);

		Array* emus = Array_new();
		size_t remaining = sizeof(full_path) - (tmp - full_path);
		while ((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name))
				continue;
			if (hasRoms(dp->d_name)) {
				strncpy(tmp, dp->d_name, remaining - 1);
				full_path[MAX_PATH - 1] = '\0';
				Array_push(emus, Entry_new(full_path, ENTRY_DIR));
			}
		}
		closedir(dh);

		EntryArray_sort(emus);
		Entry* prev_entry = NULL;
		for (int i = 0; i < emus->count; i++) {
			Entry* entry = emus->items[i];
			if (prev_entry && exactMatch(prev_entry->name, entry->name)) {
				Entry_free(entry);
				continue;
			}
			Array_push(entries, entry);
			prev_entry = entry;
		}
		Array_free(emus);
	}

	// Handle mapping logic
	char map_path[MAX_PATH];
	snprintf(map_path, sizeof(map_path), "%s/map.txt", ROMS_PATH);
	if (entries->count > 0 && exists(map_path)) {
		FILE* file = fopen(map_path, "r");
		if (file) {
			Hash* map = Hash_new();
			char line[MAX_PATH];

			while (fgets(line, sizeof(line), file)) {
				normalizeNewline(line);
				trimTrailingNewlines(line);
				if (strlen(line) == 0)
					continue;

				char* tmp = strchr(line, '\t');
				if (tmp) {
					*tmp = '\0';
					char* key = line;
					char* value = tmp + 1;
					Hash_set(map, key, value);
				}
			}
			fclose(file);

			bool resort = false;
			for (int i = 0; i < entries->count; i++) {
				Entry* entry = entries->items[i];
				char* slash = strrchr(entry->path, '/');
				if (!slash)
					continue;
				char* filename = slash + 1;
				char* alias = Hash_get(map, filename);
				if (alias) {
					free(entry->name);
					entry->name = strdup(alias);
					resort = true;
				}
			}
			if (resort)
				EntryArray_sort(entries);
			Hash_free(map);
		}
	}

	return entries;
}

Array* getCollections(void) {
	Array* collections = Array_new();
	DIR* dh = opendir(COLLECTIONS_PATH);
	if (dh) {
		struct dirent* dp;
		char full_path[MAX_PATH];
		snprintf(full_path, sizeof(full_path), "%s/", COLLECTIONS_PATH);
		char* tmp = full_path + strlen(full_path);

		size_t remaining = sizeof(full_path) - (tmp - full_path);
		while ((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name))
				continue;
			strncpy(tmp, dp->d_name, remaining - 1);
			full_path[MAX_PATH - 1] = '\0';
			Array_push(collections, Entry_new(full_path, ENTRY_DIR));
		}
		closedir(dh);
		EntryArray_sort(collections);
	}
	return collections;
}

Array* getRoot(int simple_mode) {
	Array* root = Array_new();

	if (Recents_load() && CFG_getShowRecents())
		Array_push(root, Entry_new(FAUX_RECENT_PATH, ENTRY_DIR));

	Array* entries = getRoms();

	// Handle collections
	if (hasCollections() && CFG_getShowCollections()) {
		if (entries->count) {
			Array_push(root, Entry_new(COLLECTIONS_PATH, ENTRY_DIR));
		} else { // No visible systems, promote collections to root
			Array* collections = getCollections();
			Array_yoink(entries, collections);
		}
	}

	// Add shortcuts (after Recents and Collections, before user root folders)
	if (Shortcuts_getCount() > 0 && !simple_mode) {
		Shortcuts_validate();
		for (int i = 0; i < Shortcuts_getCount(); i++) {
			char* path = Shortcuts_getPath(i);
			char* name = Shortcuts_getName(i);
			char sd_path[MAX_PATH];
			snprintf(sd_path, sizeof(sd_path), "%s%s", SDCARD_PATH, path);

			// Determine entry type based on path
			int type;
			if (suffixMatch(".pak", sd_path)) {
				type = ENTRY_PAK;
			} else {
				DIR* dh = opendir(sd_path);
				if (dh) {
					closedir(dh);
					type = ENTRY_DIR;
				} else {
					type = ENTRY_ROM;
				}
			}

			Entry* entry = Entry_new(sd_path, type);
			if (name) {
				free(entry->name);
				entry->name = strdup(name);
			}
			Array_push(root, entry);
		}
	}

	// Move entries to root
	Array_yoink(root, entries);

	// Add tools if applicable
	if (hasTools() && CFG_getShowTools() && !simple_mode) {
		char tools_path[MAX_PATH];
		snprintf(tools_path, sizeof(tools_path), "%s/Tools/%s", SDCARD_PATH, PLATFORM);
		Array_push(root, Entry_new(tools_path, ENTRY_DIR));
	}

	return root;
}

Array* getCollection(char* path) {
	Array* entries = Array_new();
	FILE* file = fopen(path, "r");
	if (file) {
		char line[MAX_PATH];
		while (fgets(line, sizeof(line), file) != NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line) == 0)
				continue;

			char sd_path[MAX_PATH];
			snprintf(sd_path, sizeof(sd_path), "%s%s", SDCARD_PATH, line);
			if (exists(sd_path)) {
				int type = suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM;
				Array_push(entries, Entry_new(sd_path, type));
			}
		}
		fclose(file);
	}
	return entries;
}

Array* getDiscs(char* path) {
	Array* entries = Array_new();

	char base_path[MAX_PATH];
	strncpy(base_path, path, MAX_PATH - 1);
	base_path[MAX_PATH - 1] = '\0';
	char* slash = strrchr(base_path, '/');
	if (!slash)
		return entries;
	slash[1] = '\0';

	FILE* file = fopen(path, "r");
	if (file) {
		char line[MAX_PATH];
		int disc = 0;
		while (fgets(line, sizeof(line), file) != NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line) == 0)
				continue;

			char disc_path[MAX_PATH];
			snprintf(disc_path, sizeof(disc_path), "%s%s", base_path, line);

			if (exists(disc_path)) {
				disc += 1;
				Entry* entry = Entry_new(disc_path, ENTRY_ROM);
				free(entry->name);
				char name[16];
				sprintf(name, "Disc %i", disc);
				entry->name = strdup(name);
				Array_push(entries, entry);
			}
		}
		fclose(file);
	}
	return entries;
}

int getFirstDisc(char* m3u_path, char* disc_path) {
	int found = 0;

	char base_path[MAX_PATH];
	strncpy(base_path, m3u_path, MAX_PATH - 1);
	base_path[MAX_PATH - 1] = '\0';
	char* slash = strrchr(base_path, '/');
	if (!slash)
		return 0;
	slash[1] = '\0';

	FILE* file = fopen(m3u_path, "r");
	if (file) {
		char line[MAX_PATH];
		while (fgets(line, sizeof(line), file) != NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line) == 0)
				continue;

			snprintf(disc_path, MAX_PATH, "%s%s", base_path, line);

			if (exists(disc_path)) {
				found = 1;
				break;
			}
		}
		fclose(file);
	}
	return found;
}

void addEntries(Array* entries, char* path) {
	DIR* dh = opendir(path);
	if (dh != NULL) {
		struct dirent* dp;
		char* tmp;
		char full_path[MAX_PATH];
		snprintf(full_path, sizeof(full_path), "%s/", path);
		tmp = full_path + strlen(full_path);
		size_t remaining = sizeof(full_path) - (tmp - full_path);
		while ((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name))
				continue;
			strncpy(tmp, dp->d_name, remaining - 1);
			full_path[MAX_PATH - 1] = '\0';
			int is_dir = dp->d_type == DT_DIR;
			int type;
			if (is_dir) {
				if (suffixMatch(".pak", dp->d_name)) {
					type = ENTRY_PAK;
				} else {
					type = ENTRY_DIR;
				}
			} else {
				if (prefixMatch(COLLECTIONS_PATH, full_path)) {
					type = ENTRY_DIR;
				} else {
					type = ENTRY_ROM;
				}
			}
			Array_push(entries, Entry_new(full_path, type));
		}
		closedir(dh);
	}
}

Array* getEntries(char* path) {
	Array* entries = Array_new();

	if (isConsoleDir(path)) { // top-level console folder, might collate
		char collated_path[MAX_PATH];
		strncpy(collated_path, path, MAX_PATH - 1);
		collated_path[MAX_PATH - 1] = '\0';
		char* tmp = strrchr(collated_path, '(');
		if (tmp)
			tmp[1] = '\0';

		DIR* dh = opendir(ROMS_PATH);
		if (dh != NULL) {
			struct dirent* dp;
			char full_path[MAX_PATH];
			snprintf(full_path, sizeof(full_path), "%s/", ROMS_PATH);
			tmp = full_path + strlen(full_path);
			size_t remaining = sizeof(full_path) - (tmp - full_path);
			while ((dp = readdir(dh)) != NULL) {
				if (hide(dp->d_name))
					continue;
				if (dp->d_type != DT_DIR)
					continue;
				strncpy(tmp, dp->d_name, remaining - 1);
				full_path[MAX_PATH - 1] = '\0';

				if (!prefixMatch(collated_path, full_path))
					continue;
				addEntries(entries, full_path);
			}
			closedir(dh);
		}
	} else
		addEntries(entries, path);

	EntryArray_sort(entries);
	return entries;
}

///////////////////////////////////////
// Quick menu content

Array* getQuickEntries(int simple_mode) {
	Array* entries = Array_new();

	if (Recents_count())
		Array_push(entries, Entry_newNamed(FAUX_RECENT_PATH, ENTRY_DIR, "Recents"));

	if (hasCollections())
		Array_push(entries, Entry_new(COLLECTIONS_PATH, ENTRY_DIR));

	Array_push(entries, Entry_newNamed(ROMS_PATH, ENTRY_DIR, "Games"));

	if (hasTools() && !simple_mode) {
		char tools_path[MAX_PATH];
		snprintf(tools_path, sizeof(tools_path), "%s/Tools/%s", SDCARD_PATH, PLATFORM);
		Array_push(entries, Entry_new(tools_path, ENTRY_DIR));
	}

	return entries;
}

Array* getQuickToggles(int simple_mode) {
	Array* entries = Array_new();

	Entry* settings = entryFromPakName("Settings");
	if (settings) {
		settings->quickId = QUICK_SETTINGS;
		Array_push(entries, settings);
	}

	Entry* store = entryFromPakName("Pak Store");
	if (store) {
		store->quickId = QUICK_PAK_STORE;
		Array_push(entries, store);
	}

	if (WIFI_supported()) {
		Entry* wifi = Entry_new("Wifi", ENTRY_DIP);
		wifi->quickId = QUICK_WIFI;
		Array_push(entries, wifi);
	}
	if (BT_supported()) {
		Entry* bt = Entry_new("Bluetooth", ENTRY_DIP);
		bt->quickId = QUICK_BLUETOOTH;
		Array_push(entries, bt);
	}
	if (PLAT_supportsDeepSleep() && !simple_mode) {
		Entry* sleep = Entry_new("Sleep", ENTRY_DIP);
		sleep->quickId = QUICK_SLEEP;
		Array_push(entries, sleep);
	}
	{
		Entry* reboot = Entry_new("Reboot", ENTRY_DIP);
		reboot->quickId = QUICK_REBOOT;
		Array_push(entries, reboot);
	}
	{
		Entry* poweroff = Entry_new("Poweroff", ENTRY_DIP);
		poweroff->quickId = QUICK_POWEROFF;
		Array_push(entries, poweroff);
	}

	return entries;
}
