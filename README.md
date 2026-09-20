# Wobbly Windows

The classic Compiz/KDE Plasma style Wobbly Windows effect for Windows 11!

![Showcase](https://raw.githubusercontent.com/lalimatyus/Wobbly-Windows/refs/heads/main/showcase.gif)

## ⚠️ IMPORTANT ⚠️

Since this mod runs in `dwm.exe`, add `dwm.exe` to Windhawk's
**Settings > Advanced settings > More advanced settings > Process inclusion list**:

![Tutorial](https://raw.githubusercontent.com/lalimatyus/Wobbly-Windows/refs/heads/main/dwm.gif)

This mod runs inside `dwm.exe` and has been tested on Windows 11 `23H2`, `24H2`, `25H2`
and `Insider Preview 26H2`. If the required private
uDWM symbols or validated object layouts aren't available, the mod refuses to
initialize instead of using unverified addresses.

## Features

* Change the wobbliness of the windows from 5 presets
* Enable custom physics to change each parameter independently, instead of a preset
* Fluid wobble animations for dragging, snapping and even resizing windows
* Uses a 4x4 spring simulation fitted to a smooth whole-window transform

## Known Issues

* ARM64 isn't supported yet; the mod safely refuses to initialize on ARM64 systems.
* If DWM stops servicing its scene thread while the mod is being disabled or updated,
  a transformed window can remain deformed until DWM recreates its visual.
  Try minimizing and restoring or reopening the affected window to recover.
* Some windows may show blur, ghosting, or temporary artifacts along their thin borders during wobble animations.
These effects can be more noticeable on high-refresh-rate displays.
* Snap detection intentionally uses permissive geometry checks to support custom
  layouts such as PowerToys FancyZones, rather than only standard Windows Snap zones.
  An unsnapped window placed close to a work-area corner may also trigger a snap wobble.

## Feedback

If you found a reproducible issue or need help, open an issue in the [GitHub repository](https://github.com/lalimatyus/Wobbly-Windows).

## Credits

* The physics presets and edge-locking behavior are based on
  [KDE Plasma/KWin's Wobbly Windows effect](https://invent.kde.org/plasma/kwin/-/tree/master/src/plugins/wobblywindows).
* The Dwminit crash-loop guard and the `CWindowData::IsGhostWindow`-based HWND-offset
  discovery draw on [Custom Window Corner Radius](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/custom-corner-radius.wh.cpp)
  by m417z and contributors, published under GPLv3.

## License

This mod is distributed under the [GNU General Public License, version 3](https://www.gnu.org/licenses/gpl-3.0.html)
(`GPL-3.0-only`). GPLv3 is used to accommodate the GPLv3-derived code credited above;
the combined mod is not offered under GPLv2.