#include <stdio.h>
#include "ui_utils.h"

// Format duration as HH:MM:SS or MM:SS
void format_time(char* buf, int seconds) {
	int hrs = seconds / 3600;
	int mins = (seconds % 3600) / 60;
	int secs = seconds % 60;
	if (hrs > 0) {
		sprintf(buf, "%d:%02d:%02d", hrs, mins, secs);
	} else {
		sprintf(buf, "%02d:%02d", mins, secs);
	}
}
