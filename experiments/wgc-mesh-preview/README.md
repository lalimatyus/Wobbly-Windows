# Isolated live-window GPU mesh preview

This is a standalone feasibility test. It does not inject into DWM, change the
Windhawk mod, hide the original window, or attach an overlay to it.

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

The sinusoidal bend deliberately makes the mesh shape obvious; it is not yet
connected to Wobbly Windows physics. The experiment tests whether public GPU
capture and mesh rendering are technically sound and fast enough before any
integration work. Windows may show its normal capture border on the source.
