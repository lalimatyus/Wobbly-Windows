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
* Enable advanced mode to change each parameter independently, instead of a preset
* Fluid wobble animations for dragging, snapping and even resizing windows
* Uses a 4x4 spring simulation fitted to a smooth whole-window transform

## Known Issues

* ARM64 isn't supported yet; the mod safely refuses to initialize on ARM64 systems.
* If DWM stops servicing its scene thread while the mod is being disabled or updated,
  a transformed window can remain deformed until DWM recreates its visual.

## Feedback

If you found a reproducible issue or need help, open an issue in the [GitHub repository](https://github.com/lalimatyus/Wobbly-Windows).

## Credits

The physics presets and edge-locking behavior are based on KDE Plasma/KWin's Wobbly Windows effect.