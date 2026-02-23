#include <stdio.h>
#include <string.h>
#include "ui_utils.h"

// Format duration as MM:SS
void format_time(char* buf, int ms) {
	int total_secs = ms / 1000;
	int mins = total_secs / 60;
	int secs = total_secs % 60;
	sprintf(buf, "%02d:%02d", mins, secs);
}

// Get format name string
const char* get_format_name(AudioFormat format) {
	switch (format) {
	case AUDIO_FORMAT_MP3:
		return "MP3";
	case AUDIO_FORMAT_FLAC:
		return "FLAC";
	case AUDIO_FORMAT_OGG:
		return "OGG";
	case AUDIO_FORMAT_WAV:
		return "WAV";
	case AUDIO_FORMAT_MOD:
		return "MOD";
	case AUDIO_FORMAT_M4A:
		return "M4A";
	case AUDIO_FORMAT_AAC:
		return "AAC";
	case AUDIO_FORMAT_OPUS:
		return "OPUS";
	default:
		return "---";
	}
}
