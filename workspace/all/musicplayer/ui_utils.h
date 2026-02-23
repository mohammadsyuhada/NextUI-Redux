#ifndef __UI_UTILS_H__
#define __UI_UTILS_H__

#include <stdbool.h>
#include <stdint.h>
#include "player.h"

// Format duration as MM:SS
void format_time(char* buf, int ms);

// Get format name string
const char* get_format_name(AudioFormat format);

#endif
