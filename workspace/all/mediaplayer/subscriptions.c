#include "subscriptions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vp_defines.h"
#include "parson/parson.h"

static SubscriptionList subs;

static void ensure_data_dir(void) {
	mkdir(APP_DATA_DIR, 0755);
}

void Subscriptions_init(void) {
	memset(&subs, 0, sizeof(subs));

	FILE* fp = fopen(APP_SUBSCRIPTIONS_FILE, "r");
	if (!fp)
		return;

	// Read entire file
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (size <= 0 || size > 1024 * 1024) {
		fclose(fp);
		return;
	}

	char* buf = malloc(size + 1);
	if (!buf) {
		fclose(fp);
		return;
	}
	fread(buf, 1, size, fp);
	buf[size] = '\0';
	fclose(fp);

	// Parse JSON
	JSON_Value* root = json_parse_string(buf);
	free(buf);
	if (!root)
		return;

	JSON_Array* channels = json_value_get_array(root);
	if (!channels) {
		json_value_free(root);
		return;
	}

	int count = (int)json_array_get_count(channels);
	if (count > SUBS_MAX_CHANNELS)
		count = SUBS_MAX_CHANNELS;

	for (int i = 0; i < count; i++) {
		JSON_Object* ch = json_array_get_object(channels, i);
		if (!ch)
			continue;

		const char* id = json_object_get_string(ch, "id");
		const char* name = json_object_get_string(ch, "name");

		if (name && name[0]) {
			strncpy(subs.channels[subs.count].channel_name, name, SUBS_MAX_NAME - 1);
			if (id)
				strncpy(subs.channels[subs.count].channel_id, id, SUBS_MAX_ID - 1);
			const char* url = json_object_get_string(ch, "url");
			if (url)
				strncpy(subs.channels[subs.count].channel_url, url, SUBS_MAX_URL - 1);
			subs.channels[subs.count].video_count = (int)json_object_get_number(ch, "video_count");
			subs.channels[subs.count].seen_video_count = (int)json_object_get_number(ch, "seen_count");
			subs.channels[subs.count].last_updated = (time_t)json_object_get_number(ch, "last_updated");
			subs.count++;
		}
	}

	json_value_free(root);
}

const SubscriptionList* Subscriptions_getList(void) {
	return &subs;
}

bool Subscriptions_isSubscribed(const char* channel_name) {
	if (!channel_name)
		return false;
	for (int i = 0; i < subs.count; i++) {
		if (strcmp(subs.channels[i].channel_name, channel_name) == 0) {
			return true;
		}
	}
	return false;
}

bool Subscriptions_add(const char* channel_name) {
	if (!channel_name || channel_name[0] == '\0')
		return false;
	if (subs.count >= SUBS_MAX_CHANNELS)
		return false;
	if (Subscriptions_isSubscribed(channel_name))
		return false;

	SubscriptionChannel* ch = &subs.channels[subs.count];
	memset(ch, 0, sizeof(*ch));
	strncpy(ch->channel_name, channel_name, SUBS_MAX_NAME - 1);
	subs.count++;

	Subscriptions_save();
	return true;
}

void Subscriptions_removeAt(int index) {
	if (index < 0 || index >= subs.count)
		return;

	// Shift remaining entries
	for (int i = index; i < subs.count - 1; i++) {
		subs.channels[i] = subs.channels[i + 1];
	}
	subs.count--;

	Subscriptions_save();
}

bool Subscriptions_removeByName(const char* channel_name) {
	if (!channel_name)
		return false;
	for (int i = 0; i < subs.count; i++) {
		if (strcmp(subs.channels[i].channel_name, channel_name) == 0) {
			Subscriptions_removeAt(i);
			return true;
		}
	}
	return false;
}

void Subscriptions_save(void) {
	ensure_data_dir();

	JSON_Value* root = json_value_init_array();
	JSON_Array* arr = json_value_get_array(root);

	for (int i = 0; i < subs.count; i++) {
		JSON_Value* item = json_value_init_object();
		JSON_Object* obj = json_value_get_object(item);

		json_object_set_string(obj, "name", subs.channels[i].channel_name);
		if (subs.channels[i].channel_id[0]) {
			json_object_set_string(obj, "id", subs.channels[i].channel_id);
		}
		if (subs.channels[i].channel_url[0]) {
			json_object_set_string(obj, "url", subs.channels[i].channel_url);
		}
		json_object_set_number(obj, "video_count", subs.channels[i].video_count);
		json_object_set_number(obj, "seen_count", subs.channels[i].seen_video_count);
		json_object_set_number(obj, "last_updated", (double)subs.channels[i].last_updated);

		json_array_append_value(arr, item);
	}

	json_serialize_to_file_pretty(root, APP_SUBSCRIPTIONS_FILE);
	json_value_free(root);
}

bool Subscriptions_addFull(const char* channel_name, const char* channel_id, const char* channel_url) {
	if (!channel_name || channel_name[0] == '\0')
		return false;
	if (subs.count >= SUBS_MAX_CHANNELS)
		return false;
	if (Subscriptions_isSubscribed(channel_name))
		return false;

	SubscriptionChannel* ch = &subs.channels[subs.count];
	memset(ch, 0, sizeof(*ch));
	strncpy(ch->channel_name, channel_name, SUBS_MAX_NAME - 1);
	if (channel_id)
		strncpy(ch->channel_id, channel_id, SUBS_MAX_ID - 1);
	if (channel_url)
		strncpy(ch->channel_url, channel_url, SUBS_MAX_URL - 1);
	subs.count++;

	Subscriptions_save();
	return true;
}

void Subscriptions_updateMeta(int index, int video_count, time_t last_updated) {
	if (index < 0 || index >= subs.count)
		return;
	subs.channels[index].video_count = video_count;
	subs.channels[index].last_updated = last_updated;
	Subscriptions_save();
}

void Subscriptions_markSeen(int index, int count) {
	if (index < 0 || index >= subs.count)
		return;
	subs.channels[index].seen_video_count = count;
	Subscriptions_save();
}

bool Subscriptions_getChannelDir(const char* channel_id, char* path_out, int size) {
	if (!channel_id || !channel_id[0])
		return false;
	snprintf(path_out, size, APP_YOUTUBE_DIR "/%s", channel_id);
	return true;
}

bool Subscriptions_getVideosPath(const char* channel_id, char* path_out, int size) {
	if (!channel_id || !channel_id[0])
		return false;
	snprintf(path_out, size, APP_YOUTUBE_DIR "/%s/videos.json", channel_id);
	return true;
}

bool Subscriptions_getAvatarPath(const char* channel_id, char* path_out, int size) {
	if (!channel_id || !channel_id[0])
		return false;
	snprintf(path_out, size, APP_YOUTUBE_DIR "/%s/avatar.jpg", channel_id);
	return true;
}

bool Subscriptions_getThumbDir(const char* channel_id, char* path_out, int size) {
	if (!channel_id || !channel_id[0])
		return false;
	snprintf(path_out, size, APP_YOUTUBE_DIR "/%s/thumbnails", channel_id);
	return true;
}

void Subscriptions_deleteChannelData(const char* channel_id) {
	if (!channel_id || !channel_id[0])
		return;
	char dir[512];
	Subscriptions_getChannelDir(channel_id, dir, sizeof(dir));
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	system(cmd);
}

void Subscriptions_cleanup(void) {
	// Nothing dynamic to free
}
