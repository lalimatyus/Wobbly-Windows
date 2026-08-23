# Wobbly Windows

The classic Compiz/KDE Plasma style Wobbly Windows effect for Windows 11!

![Showcase](https://raw.githubusercontent.com/lalimatyus/Wobbly-Windows/refs/heads/main/showcase.gif)

## ⚠️ IMPORTANT ⚠️

Since this mod runs in `dwm.exe`, add `dwm.exe` to Windhawk's
**Settings > Advanced settings > More advanced settings > Process inclusion list**:

![Tutorial](https://raw.githubusercontent.com/lalimatyus/Wobbly-Windows/refs/heads/main/dwm.gif)

This mod is currently in **beta**, so things can occasionally break and it might not work on every Windows 11 build.
It was mostly tested and made on `Windows 11 23H2`, where it works great, and it was also tested on `25H2` and on `Insider Preview 26H2`.

## Features

* Change the wobbliness of the windows from 5 presets
* Enable advanced mode to change each parameter independently, instead of a preset
* Fluid wobble animations for dragging, snapping and even resizing windows
* Calculated as a 4x4 mesh for smooth movement

## Known Issues

* Some windows act weird/jumpy on 25H2/26H2. (This happens on other builds too but it's less common)
* Virtual desktop switching isn't supported yet. The wobble can become missing or visually
  incorrect after switching desktops; DWM itself should remain operational. Disable and re-enable
  the mod to reset the effect if this happens.
* Sometimes when dragging windows through multiple monitors, the window can have a weird offset
* ARM64 isn't supported yet; the mod safely refuses to initialize on ARM64 systems.

## Feedback

If you found a reproducible issue or need help, open an issue in the [GitHub repository](https://github.com/lalimatyus/Wobbly-Windows).

## Credits

The physics presets and edge-locking behavior are based on KDE Plasma/KWin's Wobbly Windows effect.