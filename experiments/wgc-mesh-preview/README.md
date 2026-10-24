# Isolated live-window GPU mesh preview

This is a standalone feasibility test. It does not inject into DWM, change the
Windhawk mod, or hide the original window.

Build from this directory with `build.cmd`, then run `wgc-mesh-preview.exe`.
Within five seconds, activate a normal window. A separate preview will capture
that window through Windows.Graphics.Capture and draw its live frames on a
20×20 Direct3D 11 mesh. The preview shows captured and rendered frame counts.
Change the captured window's content to check whether it stays live. Closing
the preview stops the capture. The capture session excludes the mouse cursor.

`wgc-mesh-preview.exe --demo` renders an animated checkerboard without capture.
`wgc-mesh-preview.exe --self-test` checks D3D 11, shader compilation, and the
availability of Windows.Graphics.Capture. If the capture service is unavailable
in the test environment, the demo can still verify the GPU mesh renderer.

## Experimental Windhawk physics bridge

With Wobbly Windows 0.262, enable **External GPU mesh preview (experimental)**
in the mod settings. Leave **Renderer** on **Affine (stable)**. Then run
`wgc-mesh-preview.exe --physics`, activate the target window during the
five-second countdown, and drag it. The preview title should change to
`physics active` while the mod has a live 4x4 mesh for that window. It samples
those points into the 20x20 GPU mesh and keeps the window capture live. The
original window keeps its proven affine rendering. If the mod is disabled or
the source is idle, the preview remains neutral and shows `physics waiting`.

This is a diagnostic integration. It does not yet replace the original window
or suppress its unwarped pixels. The companion program and the mod communicate
through a small read-only-for-the-preview shared memory mapping; capture and
rendering stay outside dwm.exe.

`run-physics.cmd` starts this mode without needing to type a command. Keep its
console open: if opening the shared mapping fails, the Windows error appears
there. Close the preview window to stop the test.

## Five-second overlay alignment test

Keep the mod on **Affine (stable)** with **External GPU mesh preview
(experimental)** enabled. Run `run-overlay-test.cmd`, activate a normal window
within the five-second countdown, then drag it immediately. The separate GPU
program draws a transparent, click-through layer above that window only while
it receives active physics. The layer follows the visible window bounds and
disappears automatically after five seconds. It does not hide the real window,
so seeing the original behind the bent image is expected. Test with a simple
window first. The console prints the captured texture and overlay sizes, plus
the source window and overlay positions once per second.

If the image is offset, shows a duplicate while idle, blocks clicks, or remains
on screen after the timer, report that before proceeding. Do not use this mode
as the normal renderer yet; it only checks positioning and transparency.

The default sinusoidal demo is separate from the physics modes. Windows may
show its normal capture border on the source.
