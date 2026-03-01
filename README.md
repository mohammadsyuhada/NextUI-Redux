# NextUI Redux

**My vision of how NextUI should be.**
Minimal on the surface. Structured underneath. Built to last.

[NextUI](https://github.com/LoveRetro/NextUI) is a custom firmware for retro handheld gaming devices. It replaces the stock operating system with a clean, minimal interface focused on playing retro games with no unnecessary bloat.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/Y8Y61SI04B)

![nextui-redux](https://raw.githubusercontent.com/mohammadsyuhada/NextUI-Redux/main/.github/resources/demo.gif)

## Supported Devices

- **Trimui Brick**
- **Trimui Smart Pro**
- **Trimui Smart Pro S**

## Why Fork?

NextUI is a great foundation — lightweight, focused, and true to its minimalist roots.

But over time I found myself wanting two things the upstream project couldn't give me:

1. **Creative freedom** — the ability to add features and UX improvements without waiting for upstream approval or aligning with someone else's roadmap.
2. **Structural clarity** — cleaner code organization, consistent formatting, and a codebase that's easier to maintain and extend.

NextUI Redux is where those two goals meet.

This is not a cosmetic fork.
It is a deliberate divergence — feature-driven and structure-focused.

## What's Different

Improvements:

- Refactored `nextui.c` from monolithic code to a smaller, focused components
- Various bug fixes and code optimizations across the refactored components
- Added clang-format tooling and code style enforcement, with VSCode support
- Introduced reusable UI components for consistent design across tools
- Fixed incorrect Wi-Fi/Bluetooth state icons in the quick menu
- Added a semi-transparent progress overlay for all blocking actions
- Added confirmation dialogs for actions that require them
- Rewrote the `Settings` app in C with a redesigned UI
- Updated the `Battery`, `Clock`, and `Input` app with the redesigned UI 
- Integrated the `Remove Loading` feature directly into the install script (no separate app required) 
- Moved the `LED Control` configurations and `Updater` into `Settings` app (no separate app required)
- Split release builds into per-platform zips (tg5040/tg5050) 

New Features:
- Redesigned UI with consistent styling across the system.
- Game art fallback for titles without save states in the game switcher
- Main menu shortcut for quick access to frequently used Tools and Games
- Option to disable the Emulator folders (ideal for users who prefer listing only selected games (via shortcut) on the main menu)
- Direct selection of Wi-Fi networks and Bluetooth devices from the quick menu
    - IP address is displayed in the bottom button hint bar when connected
- Added developer options in `Settings`
    - Toggle SSH service and autostart
    - Disable system sleep (useful for ADB)
    - Clean up macOS-specific dotfiles (if any were copied)
    - Quickly turn off Developer Mode from the quick menu
- Added smoother slide transition animations (can be disabled in Settings)
    - `Quick Menu` slides down on enter and slides up on exit
    - `Game Switcher` slides up on enter and slides down on exit
    - `Page Navigation` slides in from the right on enter and slides out to the left on exit
- Added `Simple Mode` option in `Settings`
- Added `Search` function in main menu (Press `Y` to activate)
- Added jostick and calibration feature in `Input` app
- Added `Device Sync` to sync game saves, states, and user settings across devices. 
- Built-in `Screenshot` and `Screen Recorder` option in quick menu
    - When Screenshot is enabled, press `L2` + `R2` + `X` to capture the screen
    - When Screen Recorder is enabled, recording runs in the background automatically
- Built-in [Music Player](https://github.com/mohammadsyuhada/nextui-music-player)
- Built-in [Media Player](https://github.com/mohammadsyuhada/nextui-video-player)
- Bundled `Drastic Nintendo DS` emulator
    - Optimized configurations for both platform.
    - Simplified in-game menu for TG5050 (Update to TG5040 coming soon)
- Bundled `Mupen64Plus Nintendo 64` emulator 
    - Support for high resolution textures (With limitation due to 1GB RAM)
    - Optimized configurations for both platform.
    - Custom in-game menu with consistent UI styling with system.
    - Save state with screenshot.
- Bundled `Portmaster` in the Tools.
    - Configured by default with Nintendo input layout
    - Optimized configurations for both platform.


Upcoming Features:
- Overlay for TG5050
- Integration with [Netplay](https://github.com/mohammadsyuhada/nextui-netplay)
- Netplay for `Mupen64Plus Nintendo 64` emulator
- Minarch refactor

Ongoing focus areas:

- Cleaner, more maintainable core code
- Improved file and module structure
- Refactoring for readability
- Selective feature improvements as needed

## Upstream

This project is a derivative of [LoveRetro/NextUI](https://github.com/LoveRetro/NextUI).

Upstream changes may be merged selectively.
Architectural decisions here prioritize clarity and maintainability over strict parity.

## Credits

- [ro8inmorgan](https://github.com/ro8inmorgan), [frysee](https://github.com/frysee) and all contributors for developing NextUI
- [KrutzOtrem](https://github.com/KrutzOtrem/Trimui-Brick-Overlays) for the overlays
- [timbueno](https://github.com/timbueno/ArtBookNextUI.theme) for the Artbook theme
- [anthonycaccese](https://github.com/anthonycaccese/art-book-next-es.git) for the Artbook artwork

## License

Licensed under **GNU GPL v3.0**, the same license as the original project.

All original copyrights are retained.
Modifications in this repository are also distributed under GPL-3.0.

See the [LICENSE](LICENSE) file for details.


> *NextUI Redux is an independent fork and is not affiliated with the original NextUI project.*
