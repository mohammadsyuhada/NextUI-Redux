/* Stub libtmenu.so — disables TrimUI's built-in tmenu overlay.
 * When loaded via LD_LIBRARY_PATH before /usr/trimui/lib/libtmenu.so,
 * ShowMenu becomes a no-op so the emulator's own overlay can handle
 * the menu button instead.
 */
void ShowMenu(void) {
	/* no-op: let the emulator handle menu */
}
