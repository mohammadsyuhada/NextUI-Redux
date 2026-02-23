#define _GNU_SOURCE
#include "iptv_curated.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "api.h"
#include "parson/parson.h"

#define MAX_CURATED_COUNTRIES 32
#define MAX_CURATED_CHANNELS 256

static CuratedTVCountry curated_countries[MAX_CURATED_COUNTRIES];
static int curated_country_count = 0;

static CuratedTVChannel curated_channels[MAX_CURATED_CHANNELS];
static int curated_channel_count = 0;

static char channels_path[512] = "";

static int load_country_channels(const char* filepath) {
	JSON_Value* root = json_parse_file(filepath);
	if (!root) {
		LOG_error("Failed to parse JSON: %s\n", filepath);
		return -1;
	}

	JSON_Object* obj = json_value_get_object(root);
	if (!obj) {
		json_value_free(root);
		return -1;
	}

	const char* country_name = json_object_get_string(obj, "country");
	const char* country_code = json_object_get_string(obj, "code");

	if (!country_name || !country_code) {
		json_value_free(root);
		return -1;
	}

	bool country_exists = false;
	for (int i = 0; i < curated_country_count; i++) {
		if (strcmp(curated_countries[i].code, country_code) == 0) {
			country_exists = true;
			break;
		}
	}

	if (!country_exists && curated_country_count < MAX_CURATED_COUNTRIES) {
		strncpy(curated_countries[curated_country_count].name, country_name, 63);
		curated_countries[curated_country_count].name[63] = '\0';
		strncpy(curated_countries[curated_country_count].code, country_code, 7);
		curated_countries[curated_country_count].code[7] = '\0';
		curated_country_count++;
	}

	JSON_Array* channels_arr = json_object_get_array(obj, "channels");
	if (channels_arr) {
		int count = json_array_get_count(channels_arr);
		for (int i = 0; i < count && curated_channel_count < MAX_CURATED_CHANNELS; i++) {
			JSON_Object* channel = json_array_get_object(channels_arr, i);
			if (!channel)
				continue;

			const char* name = json_object_get_string(channel, "name");
			const char* url = json_object_get_string(channel, "url");
			const char* category = json_object_get_string(channel, "category");
			const char* logo = json_object_get_string(channel, "logo");
			const char* dkey = json_object_get_string(channel, "decryption_key");

			if (name && url) {
				strncpy(curated_channels[curated_channel_count].name, name, IPTV_MAX_NAME - 1);
				curated_channels[curated_channel_count].name[IPTV_MAX_NAME - 1] = '\0';
				strncpy(curated_channels[curated_channel_count].url, url, IPTV_MAX_URL - 1);
				curated_channels[curated_channel_count].url[IPTV_MAX_URL - 1] = '\0';
				strncpy(curated_channels[curated_channel_count].category, category ? category : "", IPTV_MAX_GROUP - 1);
				curated_channels[curated_channel_count].category[IPTV_MAX_GROUP - 1] = '\0';
				strncpy(curated_channels[curated_channel_count].logo, logo ? logo : "", IPTV_MAX_LOGO - 1);
				curated_channels[curated_channel_count].logo[IPTV_MAX_LOGO - 1] = '\0';
				strncpy(curated_channels[curated_channel_count].decryption_key, dkey ? dkey : "", IPTV_MAX_KEY - 1);
				curated_channels[curated_channel_count].decryption_key[IPTV_MAX_KEY - 1] = '\0';
				strncpy(curated_channels[curated_channel_count].country_code, country_code, 7);
				curated_channels[curated_channel_count].country_code[7] = '\0';
				curated_channel_count++;
			}
		}
	}

	json_value_free(root);
	return 0;
}

static void load_curated_channels(void) {
	curated_country_count = 0;
	curated_channel_count = 0;

	strcpy(channels_path, "./channels");

	DIR* dir = opendir(channels_path);
	if (!dir)
		return;

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		const char* ext = strrchr(ent->d_name, '.');
		if (!ext || strcasecmp(ext, ".json") != 0)
			continue;

		char filepath[768];
		snprintf(filepath, sizeof(filepath), "%s/%s", channels_path, ent->d_name);
		load_country_channels(filepath);
	}

	closedir(dir);
}

void IPTV_curated_init(void) {
	load_curated_channels();
}

void IPTV_curated_cleanup(void) {
	curated_country_count = 0;
	curated_channel_count = 0;
	channels_path[0] = '\0';
}

int IPTV_curated_get_country_count(void) {
	return curated_country_count;
}

const CuratedTVCountry* IPTV_curated_get_countries(void) {
	return curated_countries;
}

int IPTV_curated_get_channel_count(const char* country_code) {
	int count = 0;
	for (int i = 0; i < curated_channel_count; i++) {
		if (strcmp(curated_channels[i].country_code, country_code) == 0) {
			count++;
		}
	}
	return count;
}

const CuratedTVChannel* IPTV_curated_get_channels(const char* country_code, int* count) {
	const CuratedTVChannel* first = NULL;
	*count = 0;

	for (int i = 0; i < curated_channel_count; i++) {
		if (strcmp(curated_channels[i].country_code, country_code) == 0) {
			if (!first) {
				first = &curated_channels[i];
			}
			(*count)++;
		}
	}

	return first;
}
