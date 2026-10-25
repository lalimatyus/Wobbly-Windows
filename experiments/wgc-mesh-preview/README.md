# Standalone GPU mesh experiment

This directory contains the original Windows Graphics Capture and Direct3D 11
test program used to develop the 20×20 mesh renderer. It remains available as
an isolated test harness. Run `build.cmd` to rebuild it. `--demo` draws a test
pattern, and `--self-test` checks GPU and capture support.

Since Wobbly Windows 0.269, the experimental live mesh overlay is integrated
into the Windhawk mod. Select **GPU mesh overlay (experimental)** under
**Renderer**; the companion process starts automatically. The standalone
program and its `run-*.cmd` scripts are no longer required for normal use.

The overlay draws a live, curved copy of the window. The original affine
window remains visible behind it, and Windows may show a colored capture
border. Use **Affine (stable)** when these limitations are undesirable.
