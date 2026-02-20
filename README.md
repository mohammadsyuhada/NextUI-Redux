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

Changes:

- Refactored nextui.c from monolithic code to a smaller, focused components
- Various bug fixes and code optimizations across the refactored components
- Added clang-format tooling and code style enforcement, also support for VSCode IDE
- Reusable UI component that can be used by other tools for a consistent design
- Fix wrong wifi/bluetooth state icon in quick menu
- Added semi transparent progress overlay for all blocking actions
- Redesign and convert the Settings app from C++ to C

New Features:
- Redesigned UI. It is now consistent throughout the system.
- Game art fallback for games without save state in the game switcher
- Shortcut in the main menu for quick access to the regularly used Tools/Games
- User can directly select a wifi network/bluetooth device to connect in quick menu

Upcoming Features:
- Integration with [Netplay](https://github.com/mohammadsyuhada/nextui-netplay)
- Built-in [Music Player](https://github.com/mohammadsyuhada/nextui-music-player)
- Built-in [Video Player](https://github.com/mohammadsyuhada/nextui-video-player)

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

- All contributors in [LoveRetro/NextUI](https://github.com/LoveRetro/NextUI)
- [KrutzOtrem](https://github.com/KrutzOtrem/Trimui-Brick-Overlays) for the overlays
- [timbueno](https://github.com/timbueno/ArtBookNextUI.theme) for the Artbook theme
- [anthonycaccese](https://github.com/anthonycaccese/art-book-next-es-de) for the Artbook artwork

## License

Licensed under **GNU GPL v3.0**, the same license as the original project.

All original copyrights are retained.
Modifications in this repository are also distributed under GPL-3.0.

See the [LICENSE](LICENSE) file for details.


> *NextUI Redux is an independent fork and is not affiliated with the original NextUI project.*
