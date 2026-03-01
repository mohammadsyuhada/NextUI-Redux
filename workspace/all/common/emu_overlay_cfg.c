#include "emu_overlay_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "cjson/cJSON.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void safe_strcpy(char* dst, size_t dst_size, const char* src) {
	if (!dst || dst_size == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_size, "%s", src);
}

static const char* json_get_string(const cJSON* obj, const char* key) {
	const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsString(item) && item->valuestring)
		return item->valuestring;
	return NULL;
}

static int json_get_int(const cJSON* obj, const char* key, int fallback) {
	const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsNumber(item))
		return item->valueint;
	return fallback;
}

static bool json_get_bool(const cJSON* obj, const char* key, bool fallback) {
	const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsBool(item))
		return cJSON_IsTrue(item) ? true : false;
	return fallback;
}

static char* read_file_to_string(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	if (len < 0) {
		fclose(f);
		return NULL;
	}
	fseek(f, 0, SEEK_SET);

	char* buf = (char*)malloc((size_t)len + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}

	size_t read_len = fread(buf, 1, (size_t)len, f);
	buf[read_len] = '\0';
	fclose(f);
	return buf;
}

// Strip leading/trailing whitespace in-place, return pointer into same buffer.
static char* strip(char* s) {
	if (!s)
		return s;
	while (*s && isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return s;
	char* end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

// Case-insensitive string comparison for bool parsing.
static bool str_eq_nocase(const char* a, const char* b) {
	if (!a || !b)
		return false;
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
		a++;
		b++;
	}
	return *a == *b;
}

// ---------------------------------------------------------------------------
// JSON loading
// ---------------------------------------------------------------------------

static void parse_item(const cJSON* json_item, EmuOvlItem* item) {
	memset(item, 0, sizeof(*item));

	const char* s;

	s = json_get_string(json_item, "key");
	safe_strcpy(item->key, sizeof(item->key), s);

	s = json_get_string(json_item, "label");
	safe_strcpy(item->label, sizeof(item->label), s);

	s = json_get_string(json_item, "description");
	safe_strcpy(item->description, sizeof(item->description), s);

	// type
	const char* type_str = json_get_string(json_item, "type");
	if (type_str) {
		if (strcmp(type_str, "bool") == 0)
			item->type = EMU_OVL_TYPE_BOOL;
		else if (strcmp(type_str, "cycle") == 0)
			item->type = EMU_OVL_TYPE_CYCLE;
		else if (strcmp(type_str, "int") == 0)
			item->type = EMU_OVL_TYPE_INT;
		else
			item->type = EMU_OVL_TYPE_BOOL;
	}

	// values array (for cycle type)
	item->value_count = 0;
	const cJSON* values_arr = cJSON_GetObjectItemCaseSensitive(json_item, "values");
	if (cJSON_IsArray(values_arr)) {
		int count = cJSON_GetArraySize(values_arr);
		if (count > EMU_OVL_MAX_VALUES)
			count = EMU_OVL_MAX_VALUES;
		for (int i = 0; i < count; i++) {
			const cJSON* v = cJSON_GetArrayItem(values_arr, i);
			if (cJSON_IsNumber(v))
				item->values[i] = v->valueint;
		}
		item->value_count = count;
	}

	// labels array (for cycle type)
	const cJSON* labels_arr = cJSON_GetObjectItemCaseSensitive(json_item, "labels");
	if (cJSON_IsArray(labels_arr)) {
		int count = cJSON_GetArraySize(labels_arr);
		if (count > EMU_OVL_MAX_VALUES)
			count = EMU_OVL_MAX_VALUES;
		for (int i = 0; i < count; i++) {
			const cJSON* l = cJSON_GetArrayItem(labels_arr, i);
			if (cJSON_IsString(l) && l->valuestring)
				safe_strcpy(item->labels[i], sizeof(item->labels[i]), l->valuestring);
		}
	}

	// int range
	item->int_min = json_get_int(json_item, "min", 0);
	item->int_max = json_get_int(json_item, "max", 100);
	item->int_step = json_get_int(json_item, "step", 1);
	if (item->int_step < 1)
		item->int_step = 1;

	// float_scale: if >0, INI stores float value; multiply by scale for internal int
	item->float_scale = json_get_int(json_item, "float_scale", 0);

	// default — JSON bools need special handling
	if (item->type == EMU_OVL_TYPE_BOOL) {
		item->default_value = json_get_bool(json_item, "default", false) ? 1 : 0;
	} else {
		item->default_value = json_get_int(json_item, "default", 0);
	}
	item->current_value = item->default_value;
	item->staged_value = item->default_value;
	item->dirty = false;
}

static void parse_section(const cJSON* json_sec, EmuOvlSection* sec) {
	memset(sec, 0, sizeof(*sec));

	const char* name = json_get_string(json_sec, "name");
	safe_strcpy(sec->name, sizeof(sec->name), name);

	sec->item_count = 0;
	const cJSON* items_arr = cJSON_GetObjectItemCaseSensitive(json_sec, "items");
	if (!cJSON_IsArray(items_arr))
		return;

	int count = cJSON_GetArraySize(items_arr);
	if (count > EMU_OVL_MAX_ITEMS)
		count = EMU_OVL_MAX_ITEMS;

	for (int i = 0; i < count; i++) {
		const cJSON* json_item = cJSON_GetArrayItem(items_arr, i);
		if (!json_item)
			continue;
		parse_item(json_item, &sec->items[sec->item_count]);
		sec->item_count++;
	}
}

int emu_ovl_cfg_load(EmuOvlConfig* cfg, const char* json_path) {
	if (!cfg || !json_path)
		return -1;

	memset(cfg, 0, sizeof(*cfg));

	char* json_str = read_file_to_string(json_path);
	if (!json_str) {
		printf("[emu_ovl_cfg] failed to read %s\n", json_path);
		return -1;
	}

	cJSON* root = cJSON_Parse(json_str);
	free(json_str);
	if (!root) {
		printf("[emu_ovl_cfg] failed to parse JSON from %s\n", json_path);
		return -1;
	}

	const char* s;

	s = json_get_string(root, "emulator");
	safe_strcpy(cfg->emulator, sizeof(cfg->emulator), s);

	s = json_get_string(root, "config_file");
	safe_strcpy(cfg->config_file, sizeof(cfg->config_file), s);

	s = json_get_string(root, "config_section");
	safe_strcpy(cfg->config_section, sizeof(cfg->config_section), s);

	s = json_get_string(root, "options_hint");
	safe_strcpy(cfg->options_hint, sizeof(cfg->options_hint), s);

	cfg->save_state = json_get_bool(root, "save_state", false);
	cfg->load_state = json_get_bool(root, "load_state", false);

	cfg->section_count = 0;
	const cJSON* sections_arr = cJSON_GetObjectItemCaseSensitive(root, "sections");
	if (cJSON_IsArray(sections_arr)) {
		int count = cJSON_GetArraySize(sections_arr);
		if (count > EMU_OVL_MAX_SECTIONS)
			count = EMU_OVL_MAX_SECTIONS;

		for (int i = 0; i < count; i++) {
			const cJSON* json_sec = cJSON_GetArrayItem(sections_arr, i);
			if (!json_sec)
				continue;
			parse_section(json_sec, &cfg->sections[cfg->section_count]);
			cfg->section_count++;
		}
	}

	cJSON_Delete(root);
	return 0;
}

void emu_ovl_cfg_free(EmuOvlConfig* cfg) {
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
}

// ---------------------------------------------------------------------------
// INI reading — mupen64plus.cfg format: "key = value" inside [section]
// ---------------------------------------------------------------------------

// Parse a bool value from an INI string: "True"/"False" or "1"/"0".
static int parse_ini_bool(const char* val) {
	if (str_eq_nocase(val, "true") || str_eq_nocase(val, "1"))
		return 1;
	return 0;
}

// Parse an integer value from an INI string.  Falls back to 0 on failure.
static int parse_ini_int(const char* val) {
	if (!val || !*val)
		return 0;
	return atoi(val);
}

int emu_ovl_cfg_read_ini(EmuOvlConfig* cfg, const char* ini_path) {
	if (!cfg || !ini_path)
		return -1;

	FILE* f = fopen(ini_path, "r");
	if (!f) {
		printf("[emu_ovl_cfg] failed to open INI %s for reading\n", ini_path);
		return -1;
	}

	bool in_target_section = false;
	char line[1024];

	while (fgets(line, sizeof(line), f)) {
		// Strip trailing newline / whitespace
		char* trimmed = strip(line);

		// Section header?
		if (trimmed[0] == '[') {
			char* end = strchr(trimmed, ']');
			if (end) {
				*end = '\0';
				const char* section_name = trimmed + 1;
				in_target_section = (strcmp(section_name, cfg->config_section) == 0);
			} else {
				in_target_section = false;
			}
			continue;
		}

		if (!in_target_section)
			continue;

		// Skip comments and blank lines
		if (trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '\0')
			continue;

		// Parse "key = value"
		char* eq = strchr(trimmed, '=');
		if (!eq)
			continue;

		*eq = '\0';
		char* ini_key = strip(trimmed);
		char* ini_val = strip(eq + 1);

		// Match against all items in all sections
		for (int s = 0; s < cfg->section_count; s++) {
			EmuOvlSection* sec = &cfg->sections[s];
			for (int i = 0; i < sec->item_count; i++) {
				EmuOvlItem* item = &sec->items[i];
				if (strcmp(item->key, ini_key) != 0)
					continue;

				int val;
				switch (item->type) {
				case EMU_OVL_TYPE_BOOL:
					val = parse_ini_bool(ini_val);
					break;
				case EMU_OVL_TYPE_CYCLE:
				case EMU_OVL_TYPE_INT:
					if (item->float_scale > 0)
						val = (int)(atof(ini_val) * item->float_scale + 0.5);
					else
						val = parse_ini_int(ini_val);
					break;
				default:
					val = parse_ini_int(ini_val);
					break;
				}

				item->current_value = val;
				item->staged_value = val;
				item->dirty = false;
			}
		}
	}

	fclose(f);
	return 0;
}

// ---------------------------------------------------------------------------
// INI writing — preserve entire file, only replace matching keys in [section]
// ---------------------------------------------------------------------------

// Helper: write a single item's value to file
static void write_item_value(FILE* out, const EmuOvlItem* item) {
	switch (item->type) {
	case EMU_OVL_TYPE_BOOL:
		fprintf(out, "%s = %s\n", item->key,
				item->staged_value ? "True" : "False");
		break;
	case EMU_OVL_TYPE_CYCLE:
	case EMU_OVL_TYPE_INT:
		if (item->float_scale > 0)
			fprintf(out, "%s = %f\n", item->key,
					(double)item->staged_value / item->float_scale);
		else
			fprintf(out, "%s = %d\n", item->key, item->staged_value);
		break;
	}
}

int emu_ovl_cfg_write_ini(EmuOvlConfig* cfg, const char* ini_path) {
	if (!cfg || !ini_path)
		return -1;

	// Read the entire original file into memory
	char* original = read_file_to_string(ini_path);
	if (!original) {
		printf("[emu_ovl_cfg] failed to read INI %s for writing\n", ini_path);
		return -1;
	}

	// Build a flat list of dirty items for tracking
	EmuOvlItem* dirty_items[EMU_OVL_MAX_SECTIONS * EMU_OVL_MAX_ITEMS];
	bool dirty_written[EMU_OVL_MAX_SECTIONS * EMU_OVL_MAX_ITEMS];
	int dirty_count = 0;
	for (int s = 0; s < cfg->section_count; s++) {
		EmuOvlSection* sec = &cfg->sections[s];
		for (int i = 0; i < sec->item_count; i++) {
			if (sec->items[i].dirty) {
				dirty_items[dirty_count] = &sec->items[i];
				dirty_written[dirty_count] = false;
				dirty_count++;
			}
		}
	}

	if (dirty_count == 0) {
		free(original);
		return 0; // nothing to write
	}

	// Open output file
	FILE* out = fopen(ini_path, "w");
	if (!out) {
		printf("[emu_ovl_cfg] failed to open INI %s for writing\n", ini_path);
		free(original);
		return -1;
	}

	bool in_target_section = false;
	char* cursor = original;

	while (cursor && *cursor) {
		// Find end of current line
		char* eol = strchr(cursor, '\n');
		size_t line_len;
		if (eol) {
			line_len = (size_t)(eol - cursor + 1); // include the '\n'
		} else {
			line_len = strlen(cursor);
		}

		// Make a mutable copy of this line for inspection
		char line_buf[1024];
		size_t copy_len = line_len;
		if (copy_len >= sizeof(line_buf))
			copy_len = sizeof(line_buf) - 1;
		memcpy(line_buf, cursor, copy_len);
		line_buf[copy_len] = '\0';

		char* trimmed = strip(line_buf);

		// Check for section header
		if (trimmed[0] == '[') {
			// If leaving target section, append any unwritten dirty items
			if (in_target_section) {
				for (int d = 0; d < dirty_count; d++) {
					if (!dirty_written[d])
						write_item_value(out, dirty_items[d]);
				}
			}

			char* end = strchr(trimmed, ']');
			if (end) {
				*end = '\0';
				const char* section_name = trimmed + 1;
				in_target_section = (strcmp(section_name, cfg->config_section) == 0);
			} else {
				in_target_section = false;
			}
			// Write the original line unchanged
			fwrite(cursor, 1, line_len, out);
			cursor += line_len;
			continue;
		}

		// If not in the target section, write unchanged
		if (!in_target_section) {
			fwrite(cursor, 1, line_len, out);
			cursor += line_len;
			continue;
		}

		// In target section: check if this line matches a dirty key
		char parse_buf[1024];
		memcpy(parse_buf, cursor, copy_len);
		parse_buf[copy_len] = '\0';
		char* parse_trimmed = strip(parse_buf);

		// Skip comments and blank lines — write as-is
		if (parse_trimmed[0] == '#' || parse_trimmed[0] == ';' || parse_trimmed[0] == '\0') {
			fwrite(cursor, 1, line_len, out);
			cursor += line_len;
			continue;
		}

		char* eq = strchr(parse_trimmed, '=');
		if (!eq) {
			// Not a key=value line, write as-is
			fwrite(cursor, 1, line_len, out);
			cursor += line_len;
			continue;
		}

		*eq = '\0';
		char* ini_key = strip(parse_trimmed);

		// Search for a dirty item with this key
		int matched_idx = -1;
		for (int d = 0; d < dirty_count; d++) {
			if (!dirty_written[d] && strcmp(dirty_items[d]->key, ini_key) == 0) {
				matched_idx = d;
				break;
			}
		}

		if (matched_idx >= 0) {
			// Write replacement line and mark as written
			write_item_value(out, dirty_items[matched_idx]);
			dirty_written[matched_idx] = true;
		} else {
			// Not a dirty key, write unchanged
			fwrite(cursor, 1, line_len, out);
		}

		cursor += line_len;
	}

	// If we ended while still in target section, append unwritten items
	if (in_target_section) {
		for (int d = 0; d < dirty_count; d++) {
			if (!dirty_written[d])
				write_item_value(out, dirty_items[d]);
		}
	}

	fclose(out);
	free(original);
	return 0;
}

// ---------------------------------------------------------------------------
// Staged value helpers
// ---------------------------------------------------------------------------

void emu_ovl_cfg_reset_section_to_defaults(EmuOvlSection* sec) {
	if (!sec)
		return;
	for (int i = 0; i < sec->item_count; i++) {
		sec->items[i].staged_value = sec->items[i].default_value;
		sec->items[i].dirty = (sec->items[i].staged_value != sec->items[i].current_value);
	}
}

void emu_ovl_cfg_reset_staged(EmuOvlConfig* cfg) {
	if (!cfg)
		return;
	for (int s = 0; s < cfg->section_count; s++) {
		EmuOvlSection* sec = &cfg->sections[s];
		for (int i = 0; i < sec->item_count; i++) {
			sec->items[i].staged_value = sec->items[i].current_value;
			sec->items[i].dirty = false;
		}
	}
}

void emu_ovl_cfg_apply_staged(EmuOvlConfig* cfg) {
	if (!cfg)
		return;
	for (int s = 0; s < cfg->section_count; s++) {
		EmuOvlSection* sec = &cfg->sections[s];
		for (int i = 0; i < sec->item_count; i++) {
			if (sec->items[i].dirty) {
				sec->items[i].current_value = sec->items[i].staged_value;
				sec->items[i].dirty = false;
			}
		}
	}
}

bool emu_ovl_cfg_has_changes(EmuOvlConfig* cfg) {
	if (!cfg)
		return false;
	for (int s = 0; s < cfg->section_count; s++) {
		EmuOvlSection* sec = &cfg->sections[s];
		for (int i = 0; i < sec->item_count; i++) {
			if (sec->items[i].dirty)
				return true;
		}
	}
	return false;
}
