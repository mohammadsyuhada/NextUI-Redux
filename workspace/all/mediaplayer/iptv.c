#include "iptv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vp_defines.h"
#include "parson/parson.h"

#define IPTV_CHANNELS_DIR APP_DATA_DIR "/tv"
#define IPTV_CHANNELS_FILE IPTV_CHANNELS_DIR "/channels.json"

static IPTVChannel user_channels[IPTV_MAX_USER_CHANNELS];
static int user_channel_count = 0;

static void ensure_data_dir(void) {
	mkdir(APP_DATA_DIR, 0755);
	mkdir(IPTV_CHANNELS_DIR, 0755);
}

void IPTV_loadUserChannels(void) {
	user_channel_count = 0;

	JSON_Value* root = json_parse_file(IPTV_CHANNELS_FILE);
	if (!root)
		return;

	JSON_Array* arr = json_value_get_array(root);
	if (!arr) {
		json_value_free(root);
		return;
	}

	int count = (int)json_array_get_count(arr);
	if (count > IPTV_MAX_USER_CHANNELS)
		count = IPTV_MAX_USER_CHANNELS;

	for (int i = 0; i < count; i++) {
		JSON_Object* obj = json_array_get_object(arr, i);
		if (!obj)
			continue;

		const char* name = json_object_get_string(obj, "name");
		const char* url = json_object_get_string(obj, "url");
		const char* group = json_object_get_string(obj, "group");

		if (name && url) {
			IPTVChannel* ch = &user_channels[user_channel_count];
			strncpy(ch->name, name, IPTV_MAX_NAME - 1);
			ch->name[IPTV_MAX_NAME - 1] = '\0';
			strncpy(ch->url, url, IPTV_MAX_URL - 1);
			ch->url[IPTV_MAX_URL - 1] = '\0';
			strncpy(ch->group, group && group[0] ? group : "General", IPTV_MAX_GROUP - 1);
			ch->group[IPTV_MAX_GROUP - 1] = '\0';
			const char* logo = json_object_get_string(obj, "logo");
			strncpy(ch->logo, logo && logo[0] ? logo : "", IPTV_MAX_LOGO - 1);
			ch->logo[IPTV_MAX_LOGO - 1] = '\0';
			const char* dkey = json_object_get_string(obj, "decryption_key");
			strncpy(ch->decryption_key, dkey && dkey[0] ? dkey : "", IPTV_MAX_KEY - 1);
			ch->decryption_key[IPTV_MAX_KEY - 1] = '\0';
			user_channel_count++;
		}
	}

	json_value_free(root);
}

void IPTV_saveUserChannels(void) {
	ensure_data_dir();

	JSON_Value* root = json_value_init_array();
	JSON_Array* arr = json_value_get_array(root);

	for (int i = 0; i < user_channel_count; i++) {
		JSON_Value* item = json_value_init_object();
		JSON_Object* obj = json_value_get_object(item);

		json_object_set_string(obj, "name", user_channels[i].name);
		json_object_set_string(obj, "url", user_channels[i].url);
		json_object_set_string(obj, "group", user_channels[i].group);
		json_object_set_string(obj, "logo", user_channels[i].logo);
		if (user_channels[i].decryption_key[0])
			json_object_set_string(obj, "decryption_key", user_channels[i].decryption_key);

		json_array_append_value(arr, item);
	}

	json_serialize_to_file_pretty(root, IPTV_CHANNELS_FILE);
	json_value_free(root);
}

void IPTV_init(void) {
	memset(user_channels, 0, sizeof(user_channels));
	user_channel_count = 0;
	IPTV_loadUserChannels();
}

const IPTVChannel* IPTV_getUserChannels(void) {
	return user_channels;
}

int IPTV_getUserChannelCount(void) {
	return user_channel_count;
}

int IPTV_addUserChannel(const char* name, const char* url, const char* group, const char* logo, const char* decryption_key) {
	if (!name || !url || user_channel_count >= IPTV_MAX_USER_CHANNELS)
		return -1;

	// Check for duplicate
	for (int i = 0; i < user_channel_count; i++) {
		if (strcmp(user_channels[i].url, url) == 0)
			return -1;
	}

	IPTVChannel* ch = &user_channels[user_channel_count];
	strncpy(ch->name, name, IPTV_MAX_NAME - 1);
	ch->name[IPTV_MAX_NAME - 1] = '\0';
	strncpy(ch->url, url, IPTV_MAX_URL - 1);
	ch->url[IPTV_MAX_URL - 1] = '\0';
	strncpy(ch->group, group && group[0] ? group : "General", IPTV_MAX_GROUP - 1);
	ch->group[IPTV_MAX_GROUP - 1] = '\0';
	strncpy(ch->logo, logo && logo[0] ? logo : "", IPTV_MAX_LOGO - 1);
	ch->logo[IPTV_MAX_LOGO - 1] = '\0';
	strncpy(ch->decryption_key, decryption_key && decryption_key[0] ? decryption_key : "", IPTV_MAX_KEY - 1);
	ch->decryption_key[IPTV_MAX_KEY - 1] = '\0';
	user_channel_count++;

	IPTV_saveUserChannels();
	return user_channel_count - 1;
}

void IPTV_removeUserChannel(int index) {
	if (index < 0 || index >= user_channel_count)
		return;

	for (int i = index; i < user_channel_count - 1; i++) {
		user_channels[i] = user_channels[i + 1];
	}
	user_channel_count--;
	IPTV_saveUserChannels();
}

bool IPTV_removeUserChannelByUrl(const char* url) {
	if (!url)
		return false;

	for (int i = 0; i < user_channel_count; i++) {
		if (strcmp(user_channels[i].url, url) == 0) {
			IPTV_removeUserChannel(i);
			return true;
		}
	}
	return false;
}

bool IPTV_userChannelExists(const char* url) {
	if (!url)
		return false;
	for (int i = 0; i < user_channel_count; i++) {
		if (strcmp(user_channels[i].url, url) == 0)
			return true;
	}
	return false;
}

void IPTV_cleanup(void) {
	// Nothing dynamic to free
}
