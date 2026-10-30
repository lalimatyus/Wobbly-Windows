# Standalone GPU capture reference

This directory preserves the Windows Graphics Capture / Direct3D 11 test
program used during the earlier mesh experiments. Its source and executable
remain available for independent capture comparisons. `build.cmd` rebuilds
the program, `--demo` draws the test pattern and `--self-test` checks support.

Starting with Wobbly Windows 0.321, the Windhawk mod no longer embeds this
renderer, starts a companion process or publishes the v1 physics bridge.
The old `run-integrated.cmd` and bridge-following scripts are historical:
they need an older mod with that bridge enabled. They are preserved as test
references, not required for the current native Mesh Renderer.

Use the native mod's **Mesh Renderer** option for current experiments.
**Captured window source (diagnostic)** tests a separate Windows window
representation with a small live preview. Its material behavior still needs
validation. The standalone WGC program can provide an independent comparison
if that source cannot reproduce Mica or acrylic; public WGC may show a colored
capture border.
