#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

void UIKeyboard_init(void);

// Returns malloc'd string with user input, or NULL if cancelled.
// Caller must free() the returned string.
// NOTE: The keyboard binary must be deployed to SHARED_BIN_PATH/keyboard
char* UIKeyboard_open(const char* prompt);

#endif // UI_KEYBOARD_H
