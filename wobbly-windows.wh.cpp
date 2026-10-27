// ==WindhawkMod==
// @id              wobbly-windows
// @name            Wobbly Windows
// @description     The classic Compiz/KDE Plasma style Wobbly Windows effect for Windows 11!
// @version         0.297
// @author          lalimatyus
// @github          https://github.com/lalimatyus
// @include         dwm.exe
// @include         windhawk.exe
// @architecture    amd64
// @architecture    x86
// @compilerOptions -lwevtapi -lole32 -lwindowscodecs -ladvapi32 -ld3d11 -ldxgi -ld3dcompiler -ldcomp -ldwmapi -lwindowsapp -luser32 -lshell32 -lgdi32
// @license         GPL-3.0-only
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
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

**Mesh Renderer** is an experimental in-process DWM path on the validated 23H2
build. Other builds use the Affine fallback. Its live image can render backdrop
materials such as Mica and acrylic differently from the original window, and
the original window remains beneath the curved layer. The separate **GPU mesh
overlay** uses a companion process and may show a Windows capture border.
Keep **Affine** selected for normal use.

## Known Issues

* ARM64 isn't supported yet.
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
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- WobblinessPreset: "2"
  $name: Wobbliness
  $description: Select one of the five original KDE Plasma Wobbly Windows presets.
  $options:
  - "0": "Less (Stiffness 15, Drag 80, Move Factor 10)"
  - "1": "Low (Stiffness 10, Drag 85, Move Factor 10)"
  - "2": "Medium (Stiffness 6, Drag 90, Move Factor 10)"
  - "3": "High (Stiffness 3, Drag 92, Move Factor 20)"
  - "4": "More (Stiffness 1, Drag 97, Move Factor 25)"

- EnableResizeWobble: true
  $name: Resize wobble
  $description: Animate windows while resizing them from an edge or corner.

- EnableWindowStateWobble: true
  $name: Snap and maximize wobble
  $description: Animate Snap, maximize and restore transitions.

- Renderer: "affine"
  $name: Renderer
  $description: Mesh Renderer bends a live DWM image on the validated 23H2 build. Windows with backdrop materials may look different; unsupported builds use Affine.
  $options:
  - "affine": "Affine (stable)"
  - "mesh": "Mesh Renderer (experimental)"
  - "external-gpu": "GPU mesh overlay (experimental)"

- AdvancedMode:
  - enable: false
    $name: Enable
    $description: Use the three physics values instead of the selected preset.

  - Stiffness: 6
    $name: Stiffness
    $description: Custom spring stiffness. 1-100

  - Drag: 90
    $name: Drag
    $description: Velocity retention. Higher values mean less damping. 1-100

  - MoveFactor: 10
    $name: Move Factor
    $description: Custom deformation amount. 1-25

  $name: Custom Physics

*/
// ==/WindhawkModSettings==


#include <windows.h>
#include <shellapi.h>
#include <winevt.h>
#include <wincodec.h>
#include <sddl.h>
#include <dxgiformat.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <windhawk_utils.h>

// The Windhawk SDK omits this Windows SDK interop header.
extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(
    IDXGIDevice* dxgiDevice, IInspectable** graphicsDevice);
struct IDirect3DDxgiInterfaceAccess : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** result) = 0;
};
__CRT_UUID_DECL(IDirect3DDxgiInterfaceAccess, 0xA9B3D012, 0x3DF2, 0x4EE3,
                0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{

// Invariants: private visual calls run only on the verified scene owner.
// Cached window pointers are invalidated by destructor hooks; page probes check
// accessibility, not lifetime. Slot pins protect proxies across unlocked calls,
// and generations reject stale results. Each slot owns one factory proxy ref;
// visuals bind its underlying composition resource, not the wrapper itself.

enum class RendererMode : unsigned char
{
    Affine,
    Mesh,
    ExternalGpu,
};

struct WobblySettings
{
    bool resizeWobbleEnabled;
    bool windowStateWobbleEnabled;
    double stiffness;
    double drag;
    double moveFactor;
    RendererMode rendererMode;
};

static constexpr WobblySettings PHYSICS_PRESETS[] = {
    {false, false, 15.0, 80.0, 10.0, RendererMode::Affine},
    {false, false, 10.0, 85.0, 10.0, RendererMode::Affine},
    {false, false, 6.0, 90.0, 10.0, RendererMode::Affine},
    {false, false, 3.0, 92.0, 20.0, RendererMode::Affine},
    {false, false, 1.0, 97.0, 25.0, RendererMode::Affine}};

WobblySettings g_settings = {};
SRWLOCK g_settingsLock = SRWLOCK_INIT;

struct Vec2
{
    double x;
    double y;
};

struct WobblePoint
{
    Vec2 basePosition;
    Vec2 position;
    Vec2 velocity;
    Vec2 force;
    bool fixed;
};

static constexpr int GRID_WIDTH = 4;
static constexpr int GRID_HEIGHT = 4;
static constexpr int GRID_POINT_COUNT = GRID_WIDTH * GRID_HEIGHT;
static constexpr double MAX_PHYSICS_STEP_MS = 10.0;
static constexpr double MAX_VELOCITY_RETENTION = 0.99;

struct WobbleMesh
{
    WobblePoint points[GRID_POINT_COUNT];
    double width;
    double height;
    bool active;
    bool dragging;
    bool resizing;
    bool canWobbleTop;
    bool canWobbleLeft;
    bool canWobbleRight;
    bool canWobbleBottom;
    int dragPointIndex;
    Vec2 dragOffset;
};

using IsGhostWindow_t = bool(__cdecl*)(void* pThis, HWND* ghostWindow);
IsGhostWindow_t g_isGhostWindowOriginal = nullptr;
size_t g_windowDataHwndOffset = SIZE_MAX;
size_t g_windowDataTopLevelWindowOffset = SIZE_MAX;
size_t g_windowDataTopLevelWindow3DOffset = SIZE_MAX;
size_t g_topLevelWindow3DWindowDataOffset = SIZE_MAX;
static bool IsReadableMemory(const void* address, size_t size);
static bool IsWritableMemory(const void* address, size_t size);

static HWND GetHwndFromWindowData(void* windowData)
{
    if (!windowData || g_windowDataHwndOffset == SIZE_MAX)
    {
        return nullptr;
    }
    BYTE* hwndField = static_cast<BYTE*>(windowData) + g_windowDataHwndOffset;
    if (!IsReadableMemory(hwndField, sizeof(HWND)))
    {
        return nullptr;
    }
    HWND hwnd = *reinterpret_cast<HWND*>(hwndField);
    if (!IsWindow(hwnd))
    {
        return nullptr;
    }
    return hwnd;
}

static HWND GetHwndFromTrustedWindowData(void* windowData)
{
    if (!windowData || g_windowDataHwndOffset == SIZE_MAX)
    {
        return nullptr;
    }
    // Hook arguments are live CWindowData objects for the duration of the call.
    return *reinterpret_cast<HWND*>(static_cast<BYTE*>(windowData) +
                                    g_windowDataHwndOffset);
}

enum WindowEventHookIndex
{
    MOVE_SIZE_HOOK,
    LOCATION_HOOK,
    FOREGROUND_HOOK,
    MINIMIZE_HOOK,
    DESTROY_HOOK,
    SYSTEM_DESKTOP_HOOK,
    CLOAK_HOOK,
    WINDOW_EVENT_HOOK_COUNT
};

HWINEVENTHOOK g_windowEventHooks[WINDOW_EVENT_HOOK_COUNT] = {};
static constexpr int MAX_OBSERVED_WINDOWS = 256;

struct ObservedWindowState
{
    HWND hwnd;
    bool zoomed;
    bool iconic;
    bool snapped;
    bool nativeTransitionPending;
    bool expectedZoomed;
    RECT rect;
    ULONGLONG lastSeen;
    ULONGLONG nativeTransitionDeadline;
    ULONGLONG nativeTransitionStartedAt;
    ULONGLONG suppressStateThrobUntil;
    ULONGLONG snapTransitionDeadline;
};

ObservedWindowState g_observedWindows[MAX_OBSERVED_WINDOWS] = {};
HANDLE g_eventThread = nullptr;
HANDLE g_eventThreadReady = nullptr;
HANDLE g_eventThreadStop = nullptr;

static void CloseKernelHandle(HANDLE& handle)
{
    if (handle)
    {
        CloseHandle(handle);
        handle = nullptr;
    }
}
std::atomic<DWORD> g_eventThreadMessageTarget = 0;
std::atomic<HWND> g_pendingMaximizedStateWindow = nullptr;
std::atomic_bool g_maximizedStateCheckQueued = false;
static constexpr UINT WM_WOBBLY_MAXIMIZED_CHANGE = WM_APP + 0x31A;
static constexpr UINT WM_WOBBLY_NATIVE_WINDOW_TRANSITION = WM_APP + 0x31B;
static constexpr LPARAM NATIVE_TRANSITION_TARGET_IS_WORK_AREA = 0x01;
static constexpr LPARAM NATIVE_TRANSITION_SOURCE_IS_WORK_AREA = 0x02;
static constexpr LPARAM NATIVE_TRANSITION_WINDOW_IS_ZOOMED = 0x04;
static constexpr LPARAM NATIVE_TRANSITION_TARGET_IS_SNAP_LAYOUT = 0x08;
static constexpr LPARAM NATIVE_TRANSITION_DIRECTION_LEFT = 0x10;
static constexpr LPARAM NATIVE_TRANSITION_DIRECTION_RIGHT = 0x20;
static constexpr LPARAM NATIVE_TRANSITION_DIRECTION_UP = 0x40;
static constexpr LPARAM NATIVE_TRANSITION_DIRECTION_DOWN = 0x80;
std::atomic<HWND> g_pendingInteractiveTransitionWindow = nullptr;
std::atomic<LPARAM> g_pendingInteractiveTransitionFlags = 0;
RECT g_realDraggedWindowRect = {};
RECT g_lastDraggedWindowRect = {};
bool g_lastDraggedWindowZoomed = false;
bool g_finalizingMoveSize = false;
std::atomic_bool g_realDragging = false;
std::atomic<HWND> g_realDraggedWindow = nullptr;
std::atomic<ULONGLONG> g_nextDragTraceAt = 0;
std::atomic<ULONGLONG> g_nextMatrixTraceAt = 0;

static bool ShouldEmitTimedTrace(std::atomic<ULONGLONG>& nextAt)
{
    ULONGLONG now = GetTickCount64();
    ULONGLONG expected = nextAt.load(std::memory_order_relaxed);
    return now >= expected && nextAt.compare_exchange_strong(
                                  expected, now + 1000, std::memory_order_relaxed);
}
using OnPositionChange_t = void(__cdecl*)(void* pThis, void* pWindowData, bool unknown);
OnPositionChange_t g_onPositionChangeOriginal = nullptr;
using FindWindowDataByHwnd_t = void*(__cdecl*)(void* pThis, HWND hwnd);
FindWindowDataByHwnd_t g_findWindowDataByHwnd = nullptr;
using GetSyncedWindowDataLong_t = long(__cdecl*)(void* pThis, void* dwmWindow, bool synchronize,
                                                 void** windowData);
using GetSyncedWindowDataVoid_t = void(__cdecl*)(void* pThis, void* dwmWindow, bool synchronize,
                                                 void** windowData);
GetSyncedWindowDataLong_t g_getSyncedWindowDataLong = nullptr;
GetSyncedWindowDataVoid_t g_getSyncedWindowDataVoid = nullptr;
using CheckForMaximizedChange_t = void(__cdecl*)(void* pThis, void* pWindowData);
CheckForMaximizedChange_t g_checkForMaximizedChangeOriginal = nullptr;
using StartAnimationForMaximizeSnapTransition_t = long(__cdecl*)(void* pThis, int animationType,
                                                                 const RECT& targetRect);
StartAnimationForMaximizeSnapTransition_t g_startAnimationForMaximizeSnapTransitionOriginal =
    nullptr;
using CTopLevelWindow3DStartAnimation_t = long(__cdecl*)(void* pThis, int animationType);
CTopLevelWindow3DStartAnimation_t g_topLevelWindow3DStartAnimationOriginal = nullptr;
using WindowTransitionChange_t = long(__cdecl*)(void* pThis, void* dwmWindow, int transitionTarget,
                                                const RECT& targetRect, const RECT& rect2,
                                                const RECT& rect3, const RECT& rect4,
                                                const RECT& rect5);
WindowTransitionChange_t g_windowTransitionChangeOriginal = nullptr;
POINT g_lastMousePosition = {};
bool g_hasLastMousePosition = false;
HMONITOR g_dragCursorMonitor = nullptr;
ULONGLONG g_monitorTransitionRebaseUntil = 0;
using CTopLevelWindowGetVisualProxy_t = void*(__cdecl*)(void* pThis);
CTopLevelWindowGetVisualProxy_t g_getCanvasRootVisualProxy = nullptr;
using CTopLevelWindowGetRootVisual_t = void*(__cdecl*)(void* pThis, int rootVisualType);
CTopLevelWindowGetRootVisual_t g_topLevelWindowGetRootVisual = nullptr;
void* g_transitionWrapperGetVisualWeakFunction = nullptr;
void* g_transitionWrapperGetVisualProxyWeakFunction = nullptr;
using CTopLevelWindowGetWindowData_t = void*(__cdecl*)(void* pThis);
CTopLevelWindowGetWindowData_t g_topLevelWindowGetWindowData = nullptr;
bool g_realResizing = false;
bool g_moveTypeKnown = false;
bool g_dragResizeWobbleEnabled = true;
bool g_dragStartedWindowZoomed = false;
bool g_waitingForInitialRestore = false;
enum class InteractiveStateThrobKind : unsigned char
{
    None,
    Restore,
    Maximize,
    Snap,
};
InteractiveStateThrobKind g_interactiveStateThrob = InteractiveStateThrobKind::None;
LPARAM g_interactiveStateThrobDirection = 0;
bool g_interactiveStateThrobFromPointerEdge = false;
double g_resizeCoordinateScale = 1.0;
using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR monitor, int dpiType, UINT* dpiX, UINT* dpiY);
HMODULE g_shcoreModule = nullptr;
GetDpiForMonitor_t g_getDpiForMonitor = nullptr;
using DwmGetWindowAttribute_t = HRESULT(WINAPI*)(HWND hwnd, DWORD attribute, void* value,
                                                 DWORD valueSize);
HMODULE g_dwmApiModule = nullptr;
DwmGetWindowAttribute_t g_dwmGetWindowAttribute = nullptr;
HWND g_lastForegroundWindow = nullptr;
UINT_PTR g_desktopVisibilityTimer = 0;
ULONGLONG g_desktopVisibilityDeadline = 0;
bool g_systemDesktopResetPending = false;
struct MilMatrix3x2D;
struct D2DMatrix3x2F;
using CMatrixTransformProxyUpdateDouble_t = long(__cdecl*)(void* pThis,
                                                           const MilMatrix3x2D& matrix);
using CMatrixTransformProxyUpdateFloat_t = long(__cdecl*)(void* pThis,
                                                          const D2DMatrix3x2F& matrix);
CMatrixTransformProxyUpdateDouble_t g_cMatrixTransformProxyUpdate = nullptr;
CMatrixTransformProxyUpdateFloat_t g_cMatrixTransformProxyUpdateFloat = nullptr;
using CVisualProxySetTransform_t = long(__cdecl*)(void* pThis, void* transform);
CVisualProxySetTransform_t g_cVisualProxySetTransform = nullptr;
using CCompositorCreateMatrixTransformProxy_t = long(__cdecl*)(void* pThis, void** transformProxy);
CCompositorCreateMatrixTransformProxy_t g_createMatrixTransformProxy = nullptr;

// Experimental one-shot source and geometry probes. No DWM visual is attached.
struct NativeProbeRectF
{
    float left, top, right, bottom;
};
struct NativeProbeSizeD
{
    double width, height;
};
struct NativeProbeSizeF
{
    float width, height;
};
struct NativeProbePoint3F
{
    float x, y, z;
};
struct NativeProbePoint2D
{
    double x, y;
};
using NativeProbeCreateImage_t = long(__cdecl*)(void*, void**);
using NativeProbeUpdateImageD_t = long(__cdecl*)(
    void*, const NativeProbeRectF&, const NativeProbeSizeD&, const void*,
    const void*, void*, int);
using NativeProbeUpdateImageF_t = long(__cdecl*)(
    void*, const NativeProbeRectF&, const NativeProbeSizeF&, const void*,
    const void*, void*, int);
using NativeProbeCreateBitmapInstruction_t = long(__cdecl*)(void*, void**);
using NativeMeshCreateVisual_t = long(__cdecl*)(void**);
using NativeMeshAddInstruction_t = long(__cdecl*)(void*, void*);
using NativeMeshUpdateRenderData_t = long(__cdecl*)(void*);
using NativeMeshInsertRelative_t = long(__cdecl*)(
    void*, void*, void*, bool, bool);
using NativeMeshRemoveFromParent_t = long(__cdecl*)(void*);
using NativeProbeCreateMesh_t = long(__cdecl*)(void*, void**);
using NativeProbeUpdateMesh_t = long(__cdecl*)(
    void*, int, const NativeProbePoint3F*, const NativeProbePoint2D*,
    unsigned int, const unsigned int*, unsigned int);
using NativeProbeUpdateGroup_t = long(__cdecl*)(void*, void*);
// Its declared second argument is CBitmapSourceProxy*. The exact 23H2
// implementation only retains the base object and serializes its resource ID;
// the receiving core path accepts CImageSource. Other builds stay disabled.
using NativeProbeCreateMeshInstruction_t = long(__cdecl*)(void*, void*, void**);
using NativeProbeGetVisual_t = void*(__cdecl*)(void*);
NativeProbeCreateImage_t g_nativeProbeCreateImage = nullptr;
NativeProbeUpdateImageD_t g_nativeProbeUpdateImageD = nullptr;
NativeProbeUpdateImageF_t g_nativeProbeUpdateImageF = nullptr;
NativeProbeCreateBitmapInstruction_t g_nativeProbeCreateBitmapInstruction = nullptr;
NativeMeshCreateVisual_t g_nativeMeshCreateVisual = nullptr;
NativeMeshAddInstruction_t g_nativeMeshAddInstruction = nullptr;
NativeMeshUpdateRenderData_t g_nativeMeshUpdateRenderData = nullptr;
NativeMeshInsertRelative_t g_nativeMeshInsertRelative = nullptr;
NativeMeshRemoveFromParent_t g_nativeMeshRemoveFromParent = nullptr;
void* g_nativeMeshCollectionRemove = nullptr;
void* g_nativeMeshCollectionVtable = nullptr;
std::atomic_bool g_nativeMeshActive = false;
std::atomic_bool g_nativeMeshStopRequested = false;
std::atomic<HWND> g_nativeMeshHwnd = nullptr;
struct NativeMeshState
{
    void* root = nullptr;
    void* image = nullptr;
    void* group = nullptr;
    void* mesh = nullptr;
    void* instruction = nullptr;
    void* visual = nullptr;
    void* transform = nullptr;
    void* sourceProxy = nullptr;
    HWND hwnd = nullptr;
    int width = 0;
    int height = 0;
    bool detachFailureLogged = false;
    ULONGLONG meshGeneration = 0;
    ULONGLONG submittedMeshRevision = 0;
    unsigned int meshUpdateCount = 0;
};
NativeMeshState g_nativeMeshState; // DWM scene thread only.
NativeProbeCreateMesh_t g_nativeProbeCreateMesh = nullptr;
NativeProbeUpdateMesh_t g_nativeProbeUpdateMesh = nullptr;
NativeProbeCreateMesh_t g_nativeProbeCreateGroup = nullptr;
NativeProbeUpdateGroup_t g_nativeProbeUpdateGroup = nullptr;
NativeProbeCreateMeshInstruction_t g_nativeProbeCreateMeshInstruction = nullptr;
NativeProbeGetVisual_t g_nativeProbeGetVisualParent = nullptr;
NativeProbeGetVisual_t g_nativeProbeGetVisualProxy = nullptr;
void* g_nativeProbeImageVtableSymbol = nullptr;
std::atomic<void*> g_nativeProbeImageVtable = nullptr;
HWND g_nativeMeshLastAttemptHwnd = nullptr; // DWM scene thread only.
ULONGLONG g_nativeMeshLastAttemptAt = 0;

using CBaseObjectRelease_t = unsigned long(__cdecl*)(void* pThis);
CBaseObjectRelease_t g_cBaseObjectRelease = nullptr;
using CTopLevelWindowConstructor_t = void*(__cdecl*)(void* pThis, void* windowData, bool unknown);
CTopLevelWindowConstructor_t g_topLevelWindowConstructorFunction = nullptr;
CTopLevelWindowConstructor_t g_topLevelWindowConstructorOriginal = nullptr;
using CTopLevelWindow3DSetWindowData_t = void(__cdecl*)(void* pThis, void* windowData);
CTopLevelWindow3DSetWindowData_t g_topLevelWindow3DSetWindowDataOriginal = nullptr;
using CWindowDataDestructor_t = void(__cdecl*)(void* pThis);
CWindowDataDestructor_t g_windowDataDestructorOriginal = nullptr;
using CWindowListForceUpdateScene_t = long(__cdecl*)(void* pThis);
CWindowListForceUpdateScene_t g_windowListForceUpdateSceneOriginal = nullptr;
using CWindowListUpdateScene_t = long(__cdecl*)(void* pThis);
CWindowListUpdateScene_t g_windowListUpdateSceneOriginal = nullptr;
using CDesktopManagerAdvanceTimelines_t = void(__cdecl*)(void* pThis, double currentTime);
CDesktopManagerAdvanceTimelines_t g_desktopManagerAdvanceTimelinesOriginal = nullptr;
using CDesktopManagerHandleThreadMessage_t =
    void(__cdecl*)(UINT message, UINT_PTR wParam, INT_PTR lParam);
CDesktopManagerHandleThreadMessage_t g_desktopManagerHandleThreadMessageOriginal = nullptr;
using CDesktopManagerPostStartAnimations_t = long(__cdecl*)(void* pThis);
CDesktopManagerPostStartAnimations_t g_desktopManagerPostStartAnimations = nullptr;
void* g_desktopManagerInstanceAddress = nullptr;
void* g_desktopManagerInitializeFunction = nullptr;
void* g_cCompositorCreateFunction = nullptr;
using CTopLevelWindow3DDestructor_t = void(__cdecl*)(void* pThis);
CTopLevelWindow3DDestructor_t g_topLevelWindow3DDestructorOriginal = nullptr;
using CTopLevelWindowDestructor_t = void(__cdecl*)(void* pThis);
CTopLevelWindowDestructor_t g_topLevelWindowDestructorOriginal = nullptr;
using EnsureTopLevelWindow_t = long(__cdecl*)(void* pThis, void* windowData);
EnsureTopLevelWindow_t g_ensureTopLevelWindowOriginal = nullptr;
std::atomic<void*> g_desktopManagerVtable = nullptr;
std::atomic<void*> g_windowListVtable = nullptr;
std::atomic<void*> g_compositorVtable = nullptr;
std::atomic<void*> g_topLevelWindowVtable = nullptr;
std::atomic<void*> g_topLevelWindow3DVtable = nullptr;
std::atomic<void*> g_visualProxyVtable = nullptr;
std::atomic<void*> g_redirectVisualProxyVtable = nullptr;
std::atomic<void*> g_containerVisualProxyVtable = nullptr;
std::atomic<void*> g_matrixTransformProxyVtable = nullptr;
std::atomic<void*> g_dwmCompositor = nullptr;
std::atomic<void*> g_desktopManager = nullptr;
void* g_desktopManagerVtableSymbol = nullptr;
void* g_windowListVtableSymbol = nullptr;
void* g_compositorVtableSymbol = nullptr;
void* g_topLevelWindowVtableSymbol = nullptr;
void* g_topLevelWindow3DVtableSymbol = nullptr;
void* g_visualProxyVtableSymbol = nullptr;
void* g_redirectVisualProxyVtableSymbol = nullptr;
void* g_containerVisualProxyVtableSymbol = nullptr;
void* g_matrixTransformProxyVtableSymbol = nullptr;
size_t g_desktopManagerCompositorOffset = SIZE_MAX;
size_t g_desktopManagerThreadIdOffset = SIZE_MAX;
size_t g_canvasVisualOwnerOffset = SIZE_MAX;
size_t g_visualProxyOffset = SIZE_MAX;
size_t g_transitionVisualProxyOffset = SIZE_MAX;
size_t g_topLevelWindowWindowDataOffset = SIZE_MAX;
static bool IsDwmObjectPointerValid(void* object, std::atomic<void*>& expectedVtable);
static bool IsVisualProxyPointerValid(void* proxy);
static bool HasExactDwmVtableTrusted(void* object,
                                    std::atomic<void*>& expectedVtable);
static void RegisterDwmWindowMapping(void* windowData, void* topLevelWindow,
                                     void* topLevelWindow3D);

struct DwmAddressRange
{
    const BYTE* begin;
    const BYTE* end;
};

struct DwmModuleLayout
{
    HMODULE module;
    const BYTE* imageBegin;
    const BYTE* imageEnd;
    DWORD timeDateStamp;
    DWORD sizeOfImage;
    DwmAddressRange executableRanges[16];
    unsigned int executableRangeCount;
};

DwmModuleLayout g_dwmModuleLayout = {};
static constexpr int MAX_DWM_WINDOW_MAPPINGS = 256;
static constexpr unsigned int MAX_EXISTING_WINDOW_BACKFILL = 256;

struct DwmWindowObjectMapping
{
    void* windowData;
    void* topLevelWindow;
    void* topLevelWindow3D;
    ULONGLONG lastSeen;
};

DwmWindowObjectMapping g_dwmWindowMappings[MAX_DWM_WINDOW_MAPPINGS] = {};
SRWLOCK g_dwmWindowMappingsLock = SRWLOCK_INIT;
HWND g_existingWindowBackfill[MAX_EXISTING_WINDOW_BACKFILL] = {};
SRWLOCK g_existingWindowBackfillLock = SRWLOCK_INIT;
std::atomic<unsigned int> g_existingWindowBackfillCount = 0;
std::atomic<unsigned int> g_existingWindowBackfillIndex = 0;
std::atomic<unsigned int> g_existingWindowBackfillMapped = 0;

static void ResetExistingWindowBackfill()
{
    g_existingWindowBackfillCount.store(0, std::memory_order_release);
    g_existingWindowBackfillIndex.store(0, std::memory_order_release);
    g_existingWindowBackfillMapped.store(0, std::memory_order_release);
}
std::atomic<DWORD> g_dwmSceneThreadId = 0;
UINT g_dwmSceneWakeMessage = 0;
static constexpr UINT_PTR DWM_SCENE_WAKE_WPARAM = 0x574F42424C59574BULL;
std::atomic<INT_PTR> g_dwmSceneWakeToken = 0;
std::atomic<unsigned int> g_sceneWakeOutstanding = 0;
std::atomic_bool g_proxyCreationDisabled = false;
std::atomic_bool g_dwmThreadMismatchLogged = false;
std::atomic<unsigned int> g_dwmObjectDiscoveryFailureCount = 0;

struct MilMatrix3x2D
{
    double m11;
    double m12;
    double m21;
    double m22;
    double dx;
    double dy;
};

struct D2DMatrix3x2F
{
    float m11;
    float m12;
    float m21;
    float m22;
    float dx;
    float dy;
};

static void* FindWindowDataByHwnd(void* windowList, HWND hwnd)
{
    return windowList && hwnd && g_findWindowDataByHwnd
               ? g_findWindowDataByHwnd(windowList, hwnd)
               : nullptr;
}

static bool HasSyncedWindowData()
{
    return g_getSyncedWindowDataLong || g_getSyncedWindowDataVoid;
}

static bool GetSyncedWindowDataCompat(void* windowList, void* dwmWindow, bool synchronize,
                                      void** windowData)
{
    *windowData = nullptr;
    if (g_getSyncedWindowDataVoid)
    {
        g_getSyncedWindowDataVoid(windowList, dwmWindow, synchronize, windowData);
        return *windowData != nullptr;
    }
    return g_getSyncedWindowDataLong &&
           g_getSyncedWindowDataLong(windowList, dwmWindow, synchronize, windowData) >= 0 &&
           *windowData;
}

static bool HasMatrixTransformUpdate()
{
    return g_cMatrixTransformProxyUpdate || g_cMatrixTransformProxyUpdateFloat;
}

static long UpdateMatrixTransformProxy(void* proxy, const MilMatrix3x2D& matrix)
{
    if (g_cMatrixTransformProxyUpdate)
    {
        return g_cMatrixTransformProxyUpdate(proxy, matrix);
    }
    if (g_cMatrixTransformProxyUpdateFloat)
    {
        D2DMatrix3x2F floatMatrix = {static_cast<float>(matrix.m11),
                                     static_cast<float>(matrix.m12),
                                     static_cast<float>(matrix.m21),
                                     static_cast<float>(matrix.m22),
                                     static_cast<float>(matrix.dx),
                                     static_cast<float>(matrix.dy)};
        return g_cMatrixTransformProxyUpdateFloat(proxy, floatMatrix);
    }
    return E_NOTIMPL;
}

static void* ReadPointerMember(void* object, size_t offset)
{
    if (!object || offset == SIZE_MAX)
    {
        return nullptr;
    }
    const BYTE* field = static_cast<const BYTE*>(object) + offset;
    return IsReadableMemory(field, sizeof(void*))
               ? *reinterpret_cast<void* const*>(field)
               : nullptr;
}

static void* GetTopLevelVisualProxy(void* topLevelWindow,
                                    const wchar_t** source = nullptr)
{
    if (source)
    {
        *source = L"None";
    }
    if (!IsDwmObjectPointerValid(topLevelWindow, g_topLevelWindowVtable))
    {
        return nullptr;
    }

    // Type 0 is the complete window, including the non-client frame. The proxy
    // member offset is decoded from the exact Canvas getter instead of hardcoded.
    if (g_topLevelWindowGetRootVisual && g_visualProxyOffset != SIZE_MAX)
    {
        constexpr int completeWindowRoot = 0;
        void* rootVisual =
            g_topLevelWindowGetRootVisual(topLevelWindow, completeWindowRoot);
        void* proxy = ReadPointerMember(rootVisual, g_visualProxyOffset);
        if (IsVisualProxyPointerValid(proxy))
        {
            if (source)
            {
                *source = L"CompleteWindowRoot";
            }
            return proxy;
        }
    }

    if (!g_getCanvasRootVisualProxy)
    {
        return nullptr;
    }

    // Accept only exact PDB-resolved CVisualProxy/base-compatible proxy types.
    void* proxy = g_getCanvasRootVisualProxy(topLevelWindow);
    if (!IsVisualProxyPointerValid(proxy))
    {
        return nullptr;
    }

    bool decodedPath = g_canvasVisualOwnerOffset != SIZE_MAX && g_visualProxyOffset != SIZE_MAX;
    if (decodedPath &&
        ReadPointerMember(ReadPointerMember(topLevelWindow, g_canvasVisualOwnerOffset),
                          g_visualProxyOffset) != proxy)
    {
        return nullptr;
    }
    if (source)
    {
        *source = decodedPath ? L"VerifiedCanvasPath" : L"TypedCanvasAccessor";
    }
    return proxy;
}

static void* GetTransitionVisualProxy(void* topLevelWindow3D)
{
    if (!HasExactDwmVtableTrusted(topLevelWindow3D, g_topLevelWindow3DVtable))
    {
        return nullptr;
    }
    void* proxy = ReadPointerMember(topLevelWindow3D, g_transitionVisualProxyOffset);
    return IsVisualProxyPointerValid(proxy) ? proxy : nullptr;
}

static constexpr int MAX_ANIMATION_SLOTS = 6;
static constexpr ULONGLONG DWM_SCENE_WAKE_FRESHNESS_MS = 250;
static constexpr ULONGLONG DWM_SCENE_OWNER_STALE_MS = 1000;
static constexpr ULONGLONG DWM_SCENE_STALL_TIMEOUT_MS = 3000;
static constexpr ULONGLONG DWM_UNLOAD_CLEANUP_TIMEOUT_MS = 3000;

struct WindowAnimationSlot
{
    bool active;
    bool retiring;
    bool dragging;
    bool freeStepPending;
    bool windowStateThrob;
    unsigned int hookUsers;
    ULONGLONG generation;
    ULONGLONG order;
    HWND hwnd;
    WobblySettings settings;
    WobbleMesh mesh;
    void* matrixTransformProxy;
    void* boundTopLevelVisualProxy;
    void* boundTransitionVisualProxy;
    bool transformAttached;
    bool transitionTransformAttached;
    ULONGLONG transformRebindRevision;
    ULONGLONG submittedTransformRebindRevision;
    ULONGLONG meshRevision;
    ULONGLONG submittedMeshRevision;
    bool meshIdentityPending;
    bool proxyCreationPending;
    ULONGLONG nextWindowValidation;
    ULONGLONG nextVisualValidation;
    bool identityApplied;
    ULONGLONG lastMatrixErrorLog;
    ULONGLONG lastBindFailureLog;
};

thread_local bool g_insideWobblyScenePass = false;
WindowAnimationSlot g_animationSlots[MAX_ANIMATION_SLOTS] = {};
SRWLOCK g_animationSlotsLock = SRWLOCK_INIT;
CONDITION_VARIABLE g_animationSlotsCondition = CONDITION_VARIABLE_INIT;

// DWM publishes 4x4 physics. The companion captures and renders windows.
static constexpr wchar_t MESH_BRIDGE_NAME[] =
    L"Local\\Windhawk.WobblyWindows.MeshBridge.v1";
static constexpr DWORD MESH_BRIDGE_MAGIC = 0x48535757; // "WWSH"
struct MeshBridgePoint
{
    float baseX, baseY, x, y;
};
struct MeshBridgePayload
{
    DWORD active, reserved;
    ULONGLONG hwnd, generation, timestamp;
    float width, height;
    MeshBridgePoint points[16];
};
struct MeshBridgeSlot
{
    volatile LONG sequence;
    LONG reserved;
    MeshBridgePayload payload;
};
struct MeshBridgeData
{
    DWORD magic, version, size, ownerPid;
    MeshBridgeSlot slots[MAX_ANIMATION_SLOTS];
};
static_assert(sizeof(MeshBridgePoint) == 16);
static_assert(sizeof(MeshBridgePayload) == 296);
static_assert(sizeof(MeshBridgeSlot) == 304);
static_assert(offsetof(MeshBridgeData, slots) == 16);

HANDLE g_meshBridgeHandle = nullptr;
std::atomic<MeshBridgeData*> g_meshBridge = nullptr;
std::atomic_bool g_meshBridgeEnabled = false;

static void InitializeMeshBridge()
{
    if (!g_meshBridgeEnabled.load(std::memory_order_acquire) ||
        g_meshBridge.load(std::memory_order_acquire))
        return;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        Wh_Log(L"External mesh bridge unavailable: OpenProcessToken error=%u",
               GetLastError());
        return;
    }
    DWORD userBytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &userBytes);
    std::vector<BYTE> userBuffer(userBytes);
    bool gotUser = userBytes >= sizeof(TOKEN_USER) &&
                   GetTokenInformation(token, TokenUser, userBuffer.data(),
                                       userBytes, &userBytes);
    CloseHandle(token);
    LPWSTR ownerSid = nullptr;
    if (!gotUser || !ConvertSidToStringSidW(
            reinterpret_cast<TOKEN_USER*>(userBuffer.data())->User.Sid,
            &ownerSid))
    {
        Wh_Log(L"External mesh bridge unavailable: token SID error=%u",
               GetLastError());
        return;
    }
    // The compositor's account can write. Other authenticated processes in
    // this session can read the physics snapshot but cannot modify it.
    std::wstring sddl = L"D:(A;;GR;;;AU)(A;;GA;;;";
    sddl += ownerSid;
    sddl += L")";
    LocalFree(ownerSid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    {
        Wh_Log(L"External mesh bridge unavailable: security descriptor error=%u",
               GetLastError());
        return;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security,
        PAGE_READWRITE, 0, sizeof(MeshBridgeData), MESH_BRIDGE_NAME);
    LocalFree(descriptor);
    if (!mapping)
    {
        Wh_Log(L"External mesh bridge unavailable: CreateFileMapping error=%u",
               GetLastError());
        return;
    }
    auto* bridge = static_cast<MeshBridgeData*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                      sizeof(MeshBridgeData)));
    if (!bridge)
    {
        Wh_Log(L"External mesh bridge unavailable: MapViewOfFile error=%u",
               GetLastError());
        CloseHandle(mapping);
        return;
    }
    bridge->magic = 0;
    MemoryBarrier();
    for (auto& slot : bridge->slots)
    {
        if (slot.sequence & 1) InterlockedIncrement(&slot.sequence);
        InterlockedIncrement(&slot.sequence);
        slot.payload = {};
        InterlockedIncrement(&slot.sequence);
    }
    bridge->version = 1;
    bridge->size = sizeof(MeshBridgeData);
    bridge->ownerPid = GetCurrentProcessId();
    MemoryBarrier();
    bridge->magic = MESH_BRIDGE_MAGIC;
    g_meshBridgeHandle = mapping;
    g_meshBridge.store(bridge, std::memory_order_release);
    Wh_Log(L"External mesh bridge ready: version=1 slots=%d name=%s",
           MAX_ANIMATION_SLOTS, MESH_BRIDGE_NAME);
}

static void CloseMeshBridge()
{
    auto* bridge = g_meshBridge.exchange(nullptr, std::memory_order_acq_rel);
    if (bridge)
    {
        bridge->magic = 0;
        UnmapViewOfFile(bridge);
    }
    if (g_meshBridgeHandle)
    {
        CloseHandle(g_meshBridgeHandle);
        g_meshBridgeHandle = nullptr;
    }
}

static void PublishMeshBridgeSnapshot()
{
    if (!g_meshBridgeEnabled.load(std::memory_order_acquire))
        return;
    auto* bridge = g_meshBridge.load(std::memory_order_acquire);
    if (!bridge) return;
    MeshBridgePayload snapshots[MAX_ANIMATION_SLOTS] = {};
    ULONGLONG timestamp = GetTickCount64();
    AcquireSRWLockShared(&g_animationSlotsLock);
    for (int i = 0; i < MAX_ANIMATION_SLOTS; ++i)
    {
        const auto& slot = g_animationSlots[i];
        auto& payload = snapshots[i];
        if (!slot.active || !slot.hwnd || !slot.mesh.active ||
            slot.mesh.resizing || slot.windowStateThrob ||
            !std::isfinite(slot.mesh.width) ||
            !std::isfinite(slot.mesh.height) ||
            slot.mesh.width < 1.0 || slot.mesh.height < 1.0)
            continue;
        payload.hwnd = reinterpret_cast<UINT_PTR>(slot.hwnd);
        payload.generation = slot.generation;
        payload.timestamp = timestamp;
        payload.width = static_cast<float>(slot.mesh.width);
        payload.height = static_cast<float>(slot.mesh.height);
        bool valid = std::isfinite(payload.width) &&
                     std::isfinite(payload.height);
        for (int p = 0; p < 16 && valid; ++p)
        {
            const auto& point = slot.mesh.points[p];
            payload.points[p] = {
                static_cast<float>(point.basePosition.x),
                static_cast<float>(point.basePosition.y),
                static_cast<float>(point.position.x),
                static_cast<float>(point.position.y)};
            const auto& out = payload.points[p];
            valid = std::isfinite(out.baseX) && std::isfinite(out.baseY) &&
                    std::isfinite(out.x) && std::isfinite(out.y);
        }
        payload.active = valid;
    }
    ReleaseSRWLockShared(&g_animationSlotsLock);
    for (int i = 0; i < MAX_ANIMATION_SLOTS; ++i)
    {
        auto& slot = bridge->slots[i];
        InterlockedIncrement(&slot.sequence);
        slot.payload = snapshots[i];
        MemoryBarrier();
        InterlockedIncrement(&slot.sequence);
    }
}

static void ReleaseAnimationSlotPinLocked(WindowAnimationSlot& slot)
{
    if (slot.hookUsers > 0 && --slot.hookUsers == 0)
    {
        WakeAllConditionVariable(&g_animationSlotsCondition);
    }
}

static void ResetAnimationSlotLocked(WindowAnimationSlot& slot, ULONGLONG generation)
{
    slot = {};
    slot.generation = generation;
}

int g_dragAnimationSlot = -1;
ULONGLONG g_animationOrderCounter = 0;
std::atomic_bool g_unloading = false;
std::atomic_bool g_sceneWakeScheduled = false;
std::atomic_bool g_sceneWakeAwaitingNativeTimeline = false;
std::atomic<ULONGLONG> g_sceneRequestedSerial = 0;
std::atomic<ULONGLONG> g_sceneSubmittedSerial = 0;
std::atomic<ULONGLONG> g_sceneWakePostTimestamp = 0;
std::atomic<ULONGLONG> g_lastNativeTimelineTimestamp = 0;
std::atomic<ULONGLONG> g_sceneWakeStallStartedAt = 0;
std::atomic_bool g_sceneWakeStalled = false;
std::atomic_bool g_sceneRecoveryCleanupPending = false;
std::atomic<ULONGLONG> g_scenePassCounter = 0;
std::atomic<unsigned int> g_abandonedProxyCount = 0;
std::atomic<void*> g_windowListForSceneWake = nullptr;
std::atomic_bool g_sceneOwnershipResetPending = false;
std::atomic<ULONGLONG> g_lastBindPrerequisiteLog = 0;
ULONGLONG g_lastObservedScenePassCounter = 0;
ULONGLONG g_lastSceneProgressTimestamp = 0;
HANDLE g_animationTimer = nullptr;
LARGE_INTEGER g_animationFrequency = {};
LARGE_INTEGER g_lastAnimationCounter = {};
LARGE_INTEGER g_nextAnimationCounter = {};
double g_animationTargetHz = 0.0;
bool g_animationClockArmed = false;

static bool ResolveDwmWindowObjects(void* windowData, void** topLevelWindow,
                                    void** topLevelWindow3D);
static void* FindWindowDataForTopLevelWindow3D(void* topLevelWindow3D);
static long __cdecl ForceUpdateSceneHook(void* pThis);
static long __cdecl UpdateSceneHook(void* pThis);
static void __cdecl AdvanceTimelinesHook(void* pThis, double currentTime);
static void __cdecl DesktopManagerHandleThreadMessageHook(UINT message,
                                                          UINT_PTR wParam,
                                                          INT_PTR lParam);
static long __cdecl EnsureTopLevelWindowHook(void* pThis, void* windowData);
static void __cdecl TopLevelWindowDestructorHook(void* pThis);
static void __cdecl TopLevelWindow3DDestructorHook(void* pThis);
static void ApplyAnimationSlotTransform(int slotIndex, bool identityOnly = false);
static void RestorePendingAnimationIdentities();
static void FinalizeRetiringSlots();
static void EnsurePendingMatrixTransformProxies();
static bool HasAnyAnimationSlots();
static int GetPointIndex(int x, int y);
static bool ResetMatrixTransformProxy(void* matrixTransformProxy);
static void RequestDwmScenePass();
static void MarkAnimationSlotForDwmObjectRefresh(void* windowData);
static void MarkObservedSnapTransition(HWND hwnd, bool transitionStarted);
static void HandleObservedWindowLocationChange(HWND hwnd, LONG idObject, LONG idChild);
static void ResetObservedSnapStateForInteractiveMove(HWND hwnd);
static bool PostPendingDwmSceneWake(bool forceRepost);

static void QueueMaximizedStateCheck(HWND hwnd)
{
    if (!hwnd || g_unloading.load(std::memory_order_acquire) ||
        g_realDragging.load(std::memory_order_relaxed))
    {
        return;
    }
    DWORD eventThreadId = g_eventThreadMessageTarget.load(std::memory_order_acquire);
    if (eventThreadId == 0)
    {
        return;
    }
    g_pendingMaximizedStateWindow.store(hwnd, std::memory_order_release);
    if (g_maximizedStateCheckQueued.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    if (!PostThreadMessageW(eventThreadId, WM_WOBBLY_MAXIMIZED_CHANGE, 0, 0))
    {
        g_maximizedStateCheckQueued.store(false, std::memory_order_release);
    }
}

static constexpr LONGLONG WINDOW_STATE_EDGE_TOLERANCE = 32;

static bool GetMonitorWorkArea(const RECT& rect, RECT& workArea)
{
    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return false;
    }
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
    {
        return false;
    }
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        return false;
    }
    workArea = monitorInfo.rcWork;
    return true;
}

static bool WindowStateEdgesClose(LONG first, LONG second)
{
    LONGLONG difference = static_cast<LONGLONG>(first) - second;
    return difference >= -WINDOW_STATE_EDGE_TOLERANCE &&
           difference <= WINDOW_STATE_EDGE_TOLERANCE;
}

static bool IsApproximatelyMonitorWorkArea(const RECT& rect)
{
    RECT workArea = {};
    return GetMonitorWorkArea(rect, workArea) &&
           WindowStateEdgesClose(rect.left, workArea.left) &&
           WindowStateEdgesClose(rect.top, workArea.top) &&
           WindowStateEdgesClose(rect.right, workArea.right) &&
           WindowStateEdgesClose(rect.bottom, workArea.bottom);
}

static bool IsApproximatelySnapLayoutTarget(const RECT& rect)
{
    RECT workArea = {};
    if (!GetMonitorWorkArea(rect, workArea))
    {
        return false;
    }
    // Snap zones align with at least two work-area edges.
    if (rect.left < workArea.left - WINDOW_STATE_EDGE_TOLERANCE ||
        rect.top < workArea.top - WINDOW_STATE_EDGE_TOLERANCE ||
        rect.right > workArea.right + WINDOW_STATE_EDGE_TOLERANCE ||
        rect.bottom > workArea.bottom + WINDOW_STATE_EDGE_TOLERANCE)
    {
        return false;
    }
    int alignedEdges = 0;
    alignedEdges += WindowStateEdgesClose(rect.left, workArea.left);
    alignedEdges += WindowStateEdgesClose(rect.top, workArea.top);
    alignedEdges += WindowStateEdgesClose(rect.right, workArea.right);
    alignedEdges += WindowStateEdgesClose(rect.bottom, workArea.bottom);
    // Four aligned edges are the maximized work area, not a Snap Layout zone.
    return alignedEdges >= 2 && alignedEdges < 4;
}

static bool IsEligibleWindowForStateThrob(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd)
    {
        return false;
    }
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR extendedStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // Exclude shell surfaces, notifications, menus and tool windows.
    if ((style & WS_CHILD) != 0 || (style & WS_THICKFRAME) == 0 ||
        (extendedStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0 ||
        GetWindow(hwnd, GW_OWNER) != nullptr)
    {
        return false;
    }
    return true;
}

static bool IsShellCloakedWindow(HWND hwnd);

static bool CanInitializeMissingWindowVisual(HWND hwnd)
{
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) ||
        !IsEligibleWindowForStateThrob(hwnd) || !g_dwmGetWindowAttribute)
    {
        return false;
    }
    constexpr DWORD dwmwaCloaked = 14;
    DWORD cloaked = 0;
    return SUCCEEDED(g_dwmGetWindowAttribute(hwnd, dwmwaCloaked, &cloaked, sizeof(cloaked))) &&
           cloaked == 0;
}

struct ExistingWindowCollection
{
    HWND windows[MAX_EXISTING_WINDOW_BACKFILL];
    unsigned int count;
};

static BOOL CALLBACK CollectExistingWindowForBackfill(HWND hwnd, LPARAM parameter)
{
    auto* collection = reinterpret_cast<ExistingWindowCollection*>(parameter);
    if (collection->count >= MAX_EXISTING_WINDOW_BACKFILL)
    {
        return FALSE;
    }
    if (CanInitializeMissingWindowVisual(hwnd))
    {
        collection->windows[collection->count++] = hwnd;
    }
    return TRUE;
}

static void QueueExistingWindowBackfill()
{
    ExistingWindowCollection collection = {};
    EnumWindows(CollectExistingWindowForBackfill,
                reinterpret_cast<LPARAM>(&collection));
    AcquireSRWLockExclusive(&g_existingWindowBackfillLock);
    ResetExistingWindowBackfill();
    std::copy_n(collection.windows, collection.count, g_existingWindowBackfill);
    g_existingWindowBackfillCount.store(collection.count, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_existingWindowBackfillLock);
    if (collection.count != 0)
    {
        Wh_Log(L"DWM existing-window backfill queued: %u windows", collection.count);
        RequestDwmScenePass();
    }
}

static LPARAM EncodeWindowTransitionDirection(const RECT& sourceRect, const RECT& targetRect)
{
    if (sourceRect.right <= sourceRect.left || sourceRect.bottom <= sourceRect.top ||
        targetRect.right <= targetRect.left || targetRect.bottom <= targetRect.top)
    {
        return 0;
    }
    double sourceCentreX =
        (static_cast<double>(sourceRect.left) + static_cast<double>(sourceRect.right)) * 0.5;
    double sourceCentreY =
        (static_cast<double>(sourceRect.top) + static_cast<double>(sourceRect.bottom)) * 0.5;
    double targetCentreX =
        (static_cast<double>(targetRect.left) + static_cast<double>(targetRect.right)) * 0.5;
    double targetCentreY =
        (static_cast<double>(targetRect.top) + static_cast<double>(targetRect.bottom)) * 0.5;
    double deltaX = targetCentreX - sourceCentreX;
    double deltaY = targetCentreY - sourceCentreY;
    // Ignore resize-border and mixed-DPI rounding noise.
    constexpr double directionThreshold = 8.0;
    LPARAM directionFlags = 0;
    if (deltaX < -directionThreshold)
    {
        directionFlags |= NATIVE_TRANSITION_DIRECTION_LEFT;
    }
    else if (deltaX > directionThreshold)
    {
        directionFlags |= NATIVE_TRANSITION_DIRECTION_RIGHT;
    }
    if (deltaY < -directionThreshold)
    {
        directionFlags |= NATIVE_TRANSITION_DIRECTION_UP;
    }
    else if (deltaY > directionThreshold)
    {
        directionFlags |= NATIVE_TRANSITION_DIRECTION_DOWN;
    }
    return directionFlags;
}

static LPARAM EncodeWindowTransitionDirection(const Vec2& direction)
{
    LPARAM flags = direction.x < 0.0   ? NATIVE_TRANSITION_DIRECTION_LEFT
                   : direction.x > 0.0 ? NATIVE_TRANSITION_DIRECTION_RIGHT
                                       : 0;
    flags |= direction.y < 0.0   ? NATIVE_TRANSITION_DIRECTION_UP
             : direction.y > 0.0 ? NATIVE_TRANSITION_DIRECTION_DOWN
                                 : 0;
    return flags;
}

static LPARAM BuildWindowTransitionFlags(HWND hwnd, const RECT& targetRect)
{
    if (!hwnd)
    {
        return 0;
    }
    RECT sourceRect = {};
    bool hasSourceRect = GetWindowRect(hwnd, &sourceRect) != FALSE;
    LPARAM flags = 0;
    if (IsApproximatelyMonitorWorkArea(targetRect))
    {
        flags |= NATIVE_TRANSITION_TARGET_IS_WORK_AREA;
    }
    else if (IsApproximatelySnapLayoutTarget(targetRect))
    {
        flags |= NATIVE_TRANSITION_TARGET_IS_SNAP_LAYOUT;
    }
    if (hasSourceRect)
    {
        flags |= EncodeWindowTransitionDirection(sourceRect, targetRect);
        if (IsApproximatelyMonitorWorkArea(sourceRect))
        {
            flags |= NATIVE_TRANSITION_SOURCE_IS_WORK_AREA;
        }
    }
    if (IsZoomed(hwnd))
    {
        flags |= NATIVE_TRANSITION_WINDOW_IS_ZOOMED;
    }
    return flags;
}

static Vec2 DecodeWindowTransitionDirection(LPARAM transitionFlags)
{
    Vec2 direction = {};
    if ((transitionFlags & NATIVE_TRANSITION_DIRECTION_LEFT) != 0)
    {
        direction.x -= 1.0;
    }
    if ((transitionFlags & NATIVE_TRANSITION_DIRECTION_RIGHT) != 0)
    {
        direction.x += 1.0;
    }
    if ((transitionFlags & NATIVE_TRANSITION_DIRECTION_UP) != 0)
    {
        direction.y -= 1.0;
    }
    if ((transitionFlags & NATIVE_TRANSITION_DIRECTION_DOWN) != 0)
    {
        direction.y += 1.0;
    }
    return direction;
}

static void QueueNativeWindowTransition(HWND hwnd, LPARAM transitionFlags)
{
    if (!hwnd || g_unloading.load(std::memory_order_acquire))
    {
        return;
    }
    DWORD eventThreadId = g_eventThreadMessageTarget.load(std::memory_order_acquire);
    if (g_realDragging.load(std::memory_order_relaxed))
    {
        if (g_realDraggedWindow.load(std::memory_order_relaxed) != hwnd)
        {
            return;
        }
        constexpr LPARAM interactiveTargetFlags =
            NATIVE_TRANSITION_TARGET_IS_WORK_AREA |
            NATIVE_TRANSITION_TARGET_IS_SNAP_LAYOUT;
        if ((transitionFlags & interactiveTargetFlags) != 0)
        {
            // Publish direction before the release-store of its HWND.
            g_pendingInteractiveTransitionFlags.store(transitionFlags,
                                                       std::memory_order_relaxed);
            g_pendingInteractiveTransitionWindow.store(hwnd, std::memory_order_release);
        }
        else
        {
            HWND expectedWindow = hwnd;
            g_pendingInteractiveTransitionWindow.compare_exchange_strong(
                expectedWindow, nullptr, std::memory_order_acq_rel,
                std::memory_order_acquire);
        }
        if (eventThreadId != 0)
        {
            PostThreadMessageW(eventThreadId, WM_WOBBLY_NATIVE_WINDOW_TRANSITION,
                               reinterpret_cast<WPARAM>(hwnd), transitionFlags);
        }
        return;
    }
    if (transitionFlags != 0 && eventThreadId != 0)
    {
        PostThreadMessageW(eventThreadId, WM_WOBBLY_NATIVE_WINDOW_TRANSITION,
                           reinterpret_cast<WPARAM>(hwnd), transitionFlags);
    }
}

static long __cdecl WindowTransitionChangeHook(void* pThis, void* dwmWindow, int transitionTarget,
                                               const RECT& targetRect, const RECT& rect2,
                                               const RECT& rect3, const RECT& rect4,
                                               const RECT& rect5)
{
    if (HasExactDwmVtableTrusted(pThis, g_windowListVtable))
    {
        g_windowListForSceneWake.store(pThis, std::memory_order_release);
    }
    HWND hwnd = nullptr;
    if (pThis && dwmWindow && HasSyncedWindowData())
    {
        void* windowData = nullptr;
        if (GetSyncedWindowDataCompat(pThis, dwmWindow, true, &windowData) &&
            g_windowDataHwndOffset != SIZE_MAX)
        {
            hwnd = GetHwndFromWindowData(windowData);
        }
    }
    // Queue before the native transition timeline starts.
    QueueNativeWindowTransition(hwnd, BuildWindowTransitionFlags(hwnd, targetRect));
    return g_windowTransitionChangeOriginal(pThis, dwmWindow, transitionTarget, targetRect, rect2,
                                            rect3, rect4, rect5);
}

static void* RegisterAnimationTopLevelWindow3D(void* topLevelWindow3D, HWND* hwnd)
{
    *hwnd = nullptr;
    if (!HasExactDwmVtableTrusted(topLevelWindow3D, g_topLevelWindow3DVtable))
    {
        return nullptr;
    }
    void* windowData = FindWindowDataForTopLevelWindow3D(topLevelWindow3D);
    if (!windowData && g_topLevelWindow3DWindowDataOffset != SIZE_MAX)
    {
        BYTE* field = static_cast<BYTE*>(topLevelWindow3D) +
                      g_topLevelWindow3DWindowDataOffset;
        if (IsReadableMemory(field, sizeof(void*)))
        {
            windowData = *reinterpret_cast<void**>(field);
        }
    }
    if (windowData)
    {
        *hwnd = GetHwndFromWindowData(windowData);
        if (*hwnd)
        {
            RegisterDwmWindowMapping(windowData, nullptr, topLevelWindow3D);
        }
    }
    return windowData;
}

static long __cdecl StartAnimationForMaximizeSnapTransitionHook(void* pThis, int animationType,
                                                                 const RECT& targetRect)
{
    HWND hwnd = nullptr;
    RegisterAnimationTopLevelWindow3D(pThis, &hwnd);
    // Seed the wobble before uDWM starts its own maximize/restore timeline.
    QueueNativeWindowTransition(hwnd, BuildWindowTransitionFlags(hwnd, targetRect));
    return g_startAnimationForMaximizeSnapTransitionOriginal(pThis, animationType, targetRect);
}

static long __cdecl TopLevelWindow3DStartAnimationHook(void* pThis, int animationType)
{
    HWND hwnd = nullptr;
    void* windowData = RegisterAnimationTopLevelWindow3D(pThis, &hwnd);
    long result = g_topLevelWindow3DStartAnimationOriginal(pThis, animationType);
    if (!g_startAnimationForMaximizeSnapTransitionOriginal && result >= 0 && hwnd)
    {
        // 25H2 replaced the specific entry point with this generic one. Don't
        // infer animation types here; only expose the newly created transition
        // visual to an already active wobble.
        MarkAnimationSlotForDwmObjectRefresh(windowData);
        RequestDwmScenePass();
    }
    return result;
}

static void __cdecl OnPositionChangeHook(void* pThis, void* pWindowData, bool unknown)
{
    if (HasExactDwmVtableTrusted(pThis, g_windowListVtable))
    {
        g_windowListForSceneWake.store(pThis, std::memory_order_release);
    }
    g_onPositionChangeOriginal(pThis, pWindowData, unknown);
    if (!pWindowData || g_unloading.load(std::memory_order_acquire))
    {
        return;
    }
    HWND hwnd = GetHwndFromTrustedWindowData(pWindowData);
    if (!hwnd)
    {
        return;
    }
    QueueMaximizedStateCheck(hwnd);
}

static void __cdecl CheckForMaximizedChangeHook(void* pThis, void* pWindowData)
{
    if (HasExactDwmVtableTrusted(pThis, g_windowListVtable))
    {
        g_windowListForSceneWake.store(pThis, std::memory_order_release);
    }
    g_checkForMaximizedChangeOriginal(pThis, pWindowData);
    if (!pWindowData)
    {
        return;
    }
    HWND hwnd = GetHwndFromTrustedWindowData(pWindowData);
    QueueMaximizedStateCheck(hwnd);
}

static bool IsAddressRangeWithin(const void* address, size_t size, const BYTE* begin,
                                 const BYTE* end)
{
    if (!address || size == 0 || !begin || !end || begin >= end)
    {
        return false;
    }
    uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (start > UINTPTR_MAX - size)
    {
        return false;
    }
    uintptr_t finish = start + size;
    return start >= reinterpret_cast<uintptr_t>(begin) &&
           finish <= reinterpret_cast<uintptr_t>(end);
}

static bool IsReadableMemory(const void* address, size_t size)
{
    // Page accessibility only, not object lifetime; destructor hooks invalidate caches.
    if (!address || size == 0)
    {
        return false;
    }
    uintptr_t current = reinterpret_cast<uintptr_t>(address);
    if (current > UINTPTR_MAX - size)
    {
        return false;
    }
    uintptr_t finish = current + size;
    while (current < finish)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }
        DWORD readableProtection = mbi.Protect & 0xFF;
        if (readableProtection != PAGE_READONLY && readableProtection != PAGE_READWRITE &&
            readableProtection != PAGE_WRITECOPY && readableProtection != PAGE_EXECUTE_READ &&
            readableProtection != PAGE_EXECUTE_READWRITE &&
            readableProtection != PAGE_EXECUTE_WRITECOPY)
        {
            return false;
        }
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current)
        {
            return false;
        }
        current = std::min(regionEnd, finish);
    }
    return true;
}

static bool IsWritableMemory(const void* address, size_t size)
{
    if (!address || size == 0)
    {
        return false;
    }
    uintptr_t current = reinterpret_cast<uintptr_t>(address);
    if (current > UINTPTR_MAX - size)
    {
        return false;
    }
    uintptr_t finish = current + size;
    while (current < finish)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(current), &mbi,
                          sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }
        DWORD protection = mbi.Protect & 0xFF;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY)
        {
            return false;
        }
        uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current)
        {
            return false;
        }
        current = std::min(regionEnd, finish);
    }
    return true;
}


static bool InitializeDwmModuleLayout(HMODULE module)
{
    g_dwmModuleLayout = {};
    if (!module || !IsReadableMemory(module, sizeof(IMAGE_DOS_HEADER)))
    {
        return false;
    }
    const BYTE* imageBase = reinterpret_cast<const BYTE*>(module);
    const IMAGE_DOS_HEADER* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0 ||
        static_cast<size_t>(dosHeader->e_lfanew) > 0x100000)
    {
        return false;
    }
    const IMAGE_NT_HEADERS64* ntHeaders =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dosHeader->e_lfanew);
    if (!IsReadableMemory(ntHeaders, sizeof(*ntHeaders)) ||
        ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        ntHeaders->OptionalHeader.SizeOfImage < 0x10000)
    {
        return false;
    }
    const BYTE* imageEnd = imageBase + ntHeaders->OptionalHeader.SizeOfImage;
    g_dwmModuleLayout.module = module;
    g_dwmModuleLayout.imageBegin = imageBase;
    g_dwmModuleLayout.imageEnd = imageEnd;
    g_dwmModuleLayout.timeDateStamp = ntHeaders->FileHeader.TimeDateStamp;
    g_dwmModuleLayout.sizeOfImage = ntHeaders->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sections =
        IMAGE_FIRST_SECTION(const_cast<IMAGE_NT_HEADERS64*>(ntHeaders));
    if (!IsReadableMemory(sections, static_cast<size_t>(ntHeaders->FileHeader.NumberOfSections) *
                                        sizeof(*sections)))
    {
        g_dwmModuleLayout = {};
        return false;
    }
    for (unsigned int i = 0;
         i < ntHeaders->FileHeader.NumberOfSections &&
         g_dwmModuleLayout.executableRangeCount < ARRAYSIZE(g_dwmModuleLayout.executableRanges);
         i++)
    {
        if ((sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
        {
            continue;
        }
        size_t sectionSize =
            std::max<size_t>(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
        if (sectionSize == 0 || sections[i].VirtualAddress > g_dwmModuleLayout.sizeOfImage ||
            sectionSize > g_dwmModuleLayout.sizeOfImage - sections[i].VirtualAddress)
        {
            g_dwmModuleLayout = {};
            return false;
        }
        DwmAddressRange& range =
            g_dwmModuleLayout.executableRanges[g_dwmModuleLayout.executableRangeCount++];
        range.begin = imageBase + sections[i].VirtualAddress;
        range.end = range.begin + sectionSize;
    }
    return g_dwmModuleLayout.executableRangeCount != 0;
}

static bool IsDwmImageAddress(const void* address, size_t size)
{
    return IsAddressRangeWithin(address, size, g_dwmModuleLayout.imageBegin,
                                g_dwmModuleLayout.imageEnd);
}

static bool IsDwmExecutableAddress(const void* address)
{
    for (unsigned int i = 0; i < g_dwmModuleLayout.executableRangeCount; i++)
    {
        if (IsAddressRangeWithin(address, 1, g_dwmModuleLayout.executableRanges[i].begin,
                                 g_dwmModuleLayout.executableRanges[i].end))
        {
            return true;
        }
    }
    return false;
}

static bool IsDwmFunctionPointerValid(const void* function)
{
    return function && IsDwmExecutableAddress(function);
}

static bool HasExactDwmVtableTrusted(void* object, std::atomic<void*>& expectedVtable)
{
    void* expected = expectedVtable.load(std::memory_order_acquire);
    return object && expected && *reinterpret_cast<void**>(object) == expected;
}

static bool IsDwmObjectPointerValid(void* object, std::atomic<void*>& expectedVtable)
{
    void* knownVtable = expectedVtable.load(std::memory_order_acquire);
    if (!knownVtable || !object || !IsReadableMemory(object, sizeof(void*)))
    {
        return false;
    }
    void* actualVtable = *reinterpret_cast<void**>(object);
    return actualVtable == knownVtable;
}

static bool IsVisualProxyPointerValid(void* proxy)
{
    if (!proxy || !IsReadableMemory(proxy, sizeof(void*)))
    {
        return false;
    }
    void* vtable = *reinterpret_cast<void**>(proxy);
    return vtable && (vtable == g_visualProxyVtable.load(std::memory_order_acquire) ||
                      vtable == g_redirectVisualProxyVtable.load(std::memory_order_acquire) ||
                      vtable == g_containerVisualProxyVtable.load(std::memory_order_acquire));
}

static size_t FindOffsetFromFunction(void* function, size_t defaultValue)
{
    if (!IsDwmFunctionPointerValid(function))
    {
        return defaultValue;
    }
    BYTE* instruction = static_cast<BYTE*>(function);
    const std::regex pattern(
        R"(mov \w+, (?:qword ptr )?\[rcx\s*\+\s*0x([0-9a-f]{1,8})\])",
        std::regex_constants::icase);
    size_t bytesRead = 0;
    for (int i = 0; i < 32 && bytesRead < 256; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) || !Wh_Disasm(instruction, &result) ||
            result.length == 0)
        {
            break;
        }
        std::string_view text = result.text;
        std::match_results<std::string_view::const_iterator> match;
        if (std::regex_match(text.begin(), text.end(), match, pattern))
        {
            size_t offset = std::stoull(match[1].str(), nullptr, 16);
            if (offset <= 0x1000)
            {
                return offset;
            }
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }
    return defaultValue;
}

static size_t FindDesktopManagerThreadIdOffset(void* function)
{
    if (!IsDwmFunctionPointerValid(function))
    {
        return SIZE_MAX;
    }
    std::string thisAliases[8] = {"rcx"};
    unsigned int aliasCount = 1;
    auto isThisAlias = [&](const std::string& name)
    {
        for (unsigned int i = 0; i < aliasCount; i++)
        {
            if (thisAliases[i] == name)
            {
                return true;
            }
        }
        return false;
    };
    auto addThisAlias = [&](const std::string& name)
    {
        if (!isThisAlias(name) && aliasCount < ARRAYSIZE(thisAliases))
        {
            thisAliases[aliasCount++] = name;
        }
    };
    const std::regex movePattern(R"(^mov (r[a-z0-9]+), (r[a-z0-9]+)$)",
                                        std::regex_constants::icase);
    const std::regex loadPattern(
        R"(^mov (?:e[a-z0-9]+|r[0-9]+d), (?:dword ptr )?\[(r[a-z0-9]+)\s*\+\s*0x([0-9a-f]{1,8})\]$)",
        std::regex_constants::icase);
    const std::regex wakeMessagePattern(R"(^mov edx, 0x0*400$)",
                                               std::regex_constants::icase);
    BYTE* instruction = static_cast<BYTE*>(function);
    size_t candidate = SIZE_MAX;
    size_t bytesRead = 0;
    for (int i = 0; i < 64 && bytesRead < 512; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) || !Wh_Disasm(instruction, &result) ||
            result.length == 0)
        {
            break;
        }
        std::string text = result.text;
        std::smatch match;
        if (std::regex_match(text, match, movePattern) && isThisAlias(match[2].str()))
        {
            addThisAlias(match[1].str());
        }
        else if (std::regex_match(text, match, loadPattern) &&
                 isThisAlias(match[1].str()))
        {
            size_t offset = std::stoull(match[2].str(), nullptr, 16);
            if (offset >= sizeof(void*) && offset <= 0x1000 && offset % sizeof(DWORD) == 0)
            {
                candidate = offset;
            }
        }
        else if (std::regex_match(text, wakeMessagePattern))
        {
            return candidate;
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }
    return SIZE_MAX;
}

static size_t FindStoredWindowDataOffset(void* function)
{
    if (!IsDwmFunctionPointerValid(function))
    {
        return SIZE_MAX;
    }
    std::string thisAliases[8] = {"rcx"};
    std::string dataAliases[8] = {"rdx"};
    unsigned int thisAliasCount = 1;
    unsigned int dataAliasCount = 1;
    auto contains = [](const std::string* aliases, unsigned int count,
                       const std::string& name)
    {
        for (unsigned int i = 0; i < count; i++)
        {
            if (aliases[i] == name)
            {
                return true;
            }
        }
        return false;
    };
    auto addAlias = [&](std::string* aliases, unsigned int& count,
                        const std::string& name)
    {
        if (!contains(aliases, count, name) && count < 8)
        {
            aliases[count++] = name;
        }
    };
    const std::regex movePattern(R"(^mov (r[a-z0-9]+), (r[a-z0-9]+)$)",
                                        std::regex_constants::icase);
    const std::regex storePattern(
        R"(^mov (?:qword ptr )?\[(r[a-z0-9]+)\s*\+\s*0x([0-9a-f]{1,8})\], (r[a-z0-9]+)$)",
        std::regex_constants::icase);
    BYTE* instruction = static_cast<BYTE*>(function);
    size_t bytesRead = 0;
    for (int i = 0; i < 64 && bytesRead < 512; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) || !Wh_Disasm(instruction, &result) ||
            result.length == 0)
        {
            break;
        }
        std::string text = result.text;
        std::smatch match;
        if (std::regex_match(text, match, movePattern))
        {
            const std::string destination = match[1].str();
            const std::string source = match[2].str();
            if (contains(thisAliases, thisAliasCount, source))
            {
                addAlias(thisAliases, thisAliasCount, destination);
            }
            if (contains(dataAliases, dataAliasCount, source))
            {
                addAlias(dataAliases, dataAliasCount, destination);
            }
        }
        else if (std::regex_match(text, match, storePattern) &&
                 contains(thisAliases, thisAliasCount, match[1].str()) &&
                 contains(dataAliases, dataAliasCount, match[3].str()))
        {
            size_t offset = std::stoull(match[2].str(), nullptr, 16);
            if (offset >= sizeof(void*) && offset <= 0x1000 && offset % sizeof(void*) == 0)
            {
                return offset;
            }
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }
    return SIZE_MAX;
}

static bool FindVisualProxyAccessPath(void* function, size_t* ownerOffset,
                                      size_t* proxyOffset)
{
    *ownerOffset = SIZE_MAX;
    *proxyOffset = SIZE_MAX;
    if (!IsDwmFunctionPointerValid(function))
    {
        return false;
    }
    const std::regex loadPattern(
        R"(^mov (r[a-z0-9]+), (?:qword ptr )?\[(r[a-z0-9]+)\s*\+\s*0x([0-9a-f]{1,8})\]$)",
        std::regex_constants::icase);
    BYTE* instruction = static_cast<BYTE*>(function);
    std::string visualRegister;
    size_t visualOffset = SIZE_MAX;
    size_t bytesRead = 0;
    for (int i = 0; i < 32 && bytesRead < 256; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) || !Wh_Disasm(instruction, &result) ||
            result.length == 0)
        {
            break;
        }
        std::string text = result.text;
        std::smatch match;
        if (std::regex_match(text, match, loadPattern))
        {
            const std::string destination = match[1].str();
            const std::string base = match[2].str();
            size_t offset = std::stoull(match[3].str(), nullptr, 16);
            if (base == "rcx" && offset >= 0x80 && offset <= 0x400 &&
                offset % sizeof(void*) == 0)
            {
                visualRegister = destination;
                visualOffset = offset;
            }
            else if (!visualRegister.empty() && destination == "rax" &&
                     base == visualRegister && offset <= 0x80 &&
                     offset % sizeof(void*) == 0)
            {
                if (*ownerOffset != SIZE_MAX &&
                    (*ownerOffset != visualOffset || *proxyOffset != offset))
                {
                    return false;
                }
                *ownerOffset = visualOffset;
                *proxyOffset = offset;
            }
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }
    return *ownerOffset != SIZE_MAX && *proxyOffset != SIZE_MAX;
}

// Follow only the exact native function's reachable local branches. Accept the
// sprite member only when every reachable AttachBrush call has proven arguments.
static bool DecodePointerGetter(void* function, size_t* objectOffset,
                                size_t* memberOffset = nullptr)
{
    *objectOffset = SIZE_MAX;
    if (memberOffset) *memberOffset = SIZE_MAX;
    const std::regex loadPattern(
        R"(^mov rax, (?:qword ptr )?\[(rcx|rax)\s*\+\s*0x([0-9a-f]{1,8})\]$)");
    BYTE* instruction = static_cast<BYTE*>(function);
    unsigned int loads = memberOffset ? 2U : 1U;
    for (unsigned int i = 0; i < loads; i++)
    {
        WH_DISASM_RESULT result = {};
        std::cmatch match;
        if (!IsDwmExecutableAddress(instruction) || !Wh_Disasm(instruction, &result) ||
            result.length == 0 || result.length > 15 ||
            !IsDwmExecutableAddress(instruction + result.length - 1) ||
            !std::regex_match(result.text, match, loadPattern) ||
            match[1] != (i == 0 ? "rcx" : "rax"))
        {
            return false;
        }
        size_t offset = std::stoull(match[2].str(), nullptr, 16);
        if (offset > 0x400 || offset % sizeof(void*) != 0) return false;
        if (i == 0) *objectOffset = offset;
        else *memberOffset = offset;
        instruction += result.length;
    }
    WH_DISASM_RESULT result = {};
    return IsDwmExecutableAddress(instruction) && Wh_Disasm(instruction, &result) &&
           result.length == 1 && std::string_view(result.text) == "ret";
}

static size_t FindTransitionVisualProxyOffset(void* visualGetter, void* proxyGetter)
{
    // Both exact wrapper getters must use the same CTopLevelWindow3D* member.
    size_t visualOffset, proxyVisualOffset, proxyOffset;
    return DecodePointerGetter(visualGetter, &visualOffset) &&
                   DecodePointerGetter(proxyGetter, &proxyVisualOffset, &proxyOffset) &&
                   visualOffset == proxyVisualOffset
               ? proxyOffset
               : SIZE_MAX;
}

static size_t FindDesktopManagerCompositorOffset(void* initializeFunction,
                                                 void* compositorCreateFunction)
{
    if (!IsDwmFunctionPointerValid(initializeFunction) ||
        !IsDwmFunctionPointerValid(compositorCreateFunction))
    {
        return SIZE_MAX;
    }
    enum class RegisterKind
    {
        Unknown,
        This,
        MemberAddress
    };
    struct RegisterState
    {
        std::string name;
        RegisterKind kind;
        size_t offset;
    };
    RegisterState registers[16] = {{"rcx", RegisterKind::This, 0}};
    unsigned int registerCount = 1;
    auto findRegister = [&](const std::string& name) -> RegisterState*
    {
        for (unsigned int i = 0; i < registerCount; i++)
        {
            if (registers[i].name == name)
            {
                return &registers[i];
            }
        }
        if (registerCount >= ARRAYSIZE(registers))
        {
            return nullptr;
        }
        registers[registerCount] = {name, RegisterKind::Unknown, 0};
        return &registers[registerCount++];
    };
    auto clearVolatileRegisters = [&]
    {
        for (const char* name : {"rcx", "rdx", "r8", "r9", "r10", "r11"})
        {
            if (RegisterState* state = findRegister(name))
            {
                state->kind = RegisterKind::Unknown;
                state->offset = 0;
            }
        }
    };
    const std::regex movePattern(R"(^mov (r[a-z0-9]+), (r[a-z0-9]+)$)",
                                        std::regex_constants::icase);
    const std::regex leaPattern(
        R"(^lea (r[a-z0-9]+), \[(r[a-z0-9]+)\+0x([0-9a-f]{1,8})\]$)",
        std::regex_constants::icase);
    const std::regex callPattern(R"(^call 0x([0-9a-f]{1,16})$)",
                                        std::regex_constants::icase);
    BYTE* instruction = static_cast<BYTE*>(initializeFunction);
    size_t bytesRead = 0;
    for (int i = 0; i < 1024 && bytesRead < 4096; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) || !Wh_Disasm(instruction, &result) ||
            result.length == 0)
        {
            break;
        }
        std::string text = result.text;
        std::smatch match;
        if (std::regex_match(text, match, movePattern))
        {
            RegisterState* destination = findRegister(match[1].str());
            RegisterState* source = findRegister(match[2].str());
            if (destination && source)
            {
                destination->kind = source->kind;
                destination->offset = source->offset;
            }
        }
        else if (std::regex_match(text, match, leaPattern))
        {
            RegisterState* destination = findRegister(match[1].str());
            RegisterState* base = findRegister(match[2].str());
            size_t offset = std::stoull(match[3].str(), nullptr, 16);
            if (destination)
            {
                destination->kind = base && base->kind == RegisterKind::This
                                        ? RegisterKind::MemberAddress
                                        : RegisterKind::Unknown;
                destination->offset = offset;
            }
        }
        else if (std::regex_match(text, match, callPattern))
        {
            uintptr_t target = std::stoull(match[1].str(), nullptr, 16);
            if (target == reinterpret_cast<uintptr_t>(compositorCreateFunction))
            {
                RegisterState* argument = findRegister("rcx");
                if (argument && argument->kind == RegisterKind::MemberAddress &&
                    argument->offset >= sizeof(void*) && argument->offset <= 0x400 &&
                    argument->offset % sizeof(void*) == 0)
                {
                    return argument->offset;
                }
                return SIZE_MAX;
            }
            clearVolatileRegisters();
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }
    return SIZE_MAX;
}

static bool FindConstructorWindowDataOffsets(void* function, size_t* windowDataTopLevelOffset,
                                             size_t* topLevelWindowDataOffset)
{
    *windowDataTopLevelOffset = SIZE_MAX;
    *topLevelWindowDataOffset = SIZE_MAX;
    if (!IsDwmFunctionPointerValid(function))
    {
        return false;
    }
    std::string thisAliases[8] = {"rcx"};
    std::string windowDataAliases[8] = {"rdx"};
    unsigned int thisAliasCount = 1;
    unsigned int windowDataAliasCount = 1;
    auto contains = [](const std::string aliases[], unsigned int count,
                       const std::string& value)
    {
        for (unsigned int i = 0; i < count; i++)
        {
            if (aliases[i] == value)
            {
                return true;
            }
        }
        return false;
    };
    auto remove = [](std::string aliases[], unsigned int& count, const std::string& value)
    {
        for (unsigned int i = 0; i < count; i++)
        {
            if (aliases[i] == value)
            {
                aliases[i] = aliases[--count];
                return;
            }
        }
    };
    auto add = [&](std::string aliases[], unsigned int& count, const std::string& value)
    {
        if (!contains(aliases, count, value) && count < 8)
        {
            aliases[count++] = value;
        }
    };
    auto clearVolatileAliases = [&](std::string aliases[], unsigned int& count)
    {
        static const char* volatileRegisters[] = {"rcx", "rdx", "r8", "r9", "r10", "r11"};
        for (const char* reg : volatileRegisters)
        {
            remove(aliases, count, reg);
        }
    };
    const std::regex movePattern(R"(^mov (r[a-z0-9]+), (r[a-z0-9]+)$)",
                                        std::regex_constants::icase);
    const std::regex storePattern(
        R"(^mov (?:qword ptr )?\[(r[a-z0-9]+)\+0x([0-9a-f]{1,8})\], (r[a-z0-9]+)$)",
        std::regex_constants::icase);
    size_t forwardCandidate = SIZE_MAX;
    size_t reverseCandidate = SIZE_MAX;
    bool ambiguous = false;
    BYTE* instruction = static_cast<BYTE*>(function);
    size_t bytesRead = 0;
    for (int i = 0; i < 256 && bytesRead < 1024; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) || !Wh_Disasm(instruction, &result) ||
            result.length == 0)
        {
            break;
        }
        std::string text = result.text;
        std::smatch match;
        if (std::regex_match(text, match, movePattern))
        {
            std::string destination = match[1].str();
            std::string source = match[2].str();
            bool sourceIsThis = contains(thisAliases, thisAliasCount, source);
            bool sourceIsWindowData =
                contains(windowDataAliases, windowDataAliasCount, source);
            remove(thisAliases, thisAliasCount, destination);
            remove(windowDataAliases, windowDataAliasCount, destination);
            if (sourceIsThis)
            {
                add(thisAliases, thisAliasCount, destination);
            }
            else if (sourceIsWindowData)
            {
                add(windowDataAliases, windowDataAliasCount, destination);
            }
        }
        else if (std::regex_match(text, match, storePattern))
        {
            std::string base = match[1].str();
            std::string source = match[3].str();
            size_t offset = std::stoull(match[2].str(), nullptr, 16);
            if (offset >= 0x80 && offset <= 0x800 && offset % sizeof(void*) == 0)
            {
                size_t* candidate = nullptr;
                if (contains(windowDataAliases, windowDataAliasCount, base) &&
                    contains(thisAliases, thisAliasCount, source))
                {
                    candidate = &forwardCandidate;
                }
                else if (contains(thisAliases, thisAliasCount, base) &&
                         contains(windowDataAliases, windowDataAliasCount, source))
                {
                    candidate = &reverseCandidate;
                }
                if (candidate)
                {
                    if (*candidate != SIZE_MAX && *candidate != offset)
                    {
                        ambiguous = true;
                    }
                    *candidate = offset;
                }
            }
        }
        if (text.rfind("call ", 0) == 0)
        {
            clearVolatileAliases(thisAliases, thisAliasCount);
            clearVolatileAliases(windowDataAliases, windowDataAliasCount);
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }
    if (ambiguous || forwardCandidate == SIZE_MAX || reverseCandidate == SIZE_MAX)
    {
        Wh_Log(L"DWM constructor offset scan failed: forward=0x%zx reverse=0x%zx ambiguous=%d",
               forwardCandidate, reverseCandidate, ambiguous);
        return false;
    }
    *windowDataTopLevelOffset = forwardCandidate;
    *topLevelWindowDataOffset = reverseCandidate;
    return true;
}

static bool FindWindowDataTopLevelOffsets(void* function, size_t* topLevelWindowOffset,
                                          size_t* topLevelWindow3DOffset)
{
    *topLevelWindowOffset = SIZE_MAX;
    *topLevelWindow3DOffset = SIZE_MAX;
    if (!IsDwmFunctionPointerValid(function))
    {
        return false;
    }

    struct OffsetCandidate
    {
        size_t offset;
        unsigned int references;
        unsigned int zeroTests;
        unsigned int writes;
    };

    OffsetCandidate candidates[64] = {};
    unsigned int candidateCount = 0;
    std::string aliases[8] = {"rdx"};
    unsigned int aliasCount = 1;
    const std::regex movePattern(R"(mov (r[a-z0-9]+), (r[a-z0-9]+))",
                                        std::regex_constants::icase);
    // Accept common compiler encodings when deriving these fields.
    const std::regex memoryPattern(
        R"(\[(r[a-z0-9]+)\+0x([0-9a-f]{1,8})\])", std::regex_constants::icase);
    const std::regex zeroTestPattern(
        R"(^cmp (?:qword ptr )?\[[^\]]+\], (?:0x)?0$)", std::regex_constants::icase);
    const std::regex writePattern(
        R"(^mov (?:qword ptr )?\[[^\]]+\], )", std::regex_constants::icase);
    BYTE* instruction = static_cast<BYTE*>(function);
    size_t bytesRead = 0;
    for (int i = 0; i < 192 && bytesRead < 768; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) || !Wh_Disasm(instruction, &result) ||
            result.length == 0)
        {
            return false;
        }
        std::string text = result.text;
        std::smatch match;
        if (std::regex_match(text, match, movePattern))
        {
            bool sourceIsAlias = false;
            for (unsigned int aliasIndex = 0; aliasIndex < aliasCount; aliasIndex++)
            {
                if (match[2].str() == aliases[aliasIndex])
                {
                    sourceIsAlias = true;
                    break;
                }
            }
            if (sourceIsAlias && aliasCount < ARRAYSIZE(aliases))
            {
                std::string destination = match[1].str();
                bool alreadyKnown = false;
                for (unsigned int aliasIndex = 0; aliasIndex < aliasCount; aliasIndex++)
                {
                    alreadyKnown = alreadyKnown || aliases[aliasIndex] == destination;
                }
                if (!alreadyKnown)
                {
                    aliases[aliasCount++] = destination;
                }
            }
        }
        if (std::regex_search(text, match, memoryPattern))
        {
            bool baseIsAlias = false;
            for (unsigned int aliasIndex = 0; aliasIndex < aliasCount; aliasIndex++)
            {
                if (match[1].str() == aliases[aliasIndex])
                {
                    baseIsAlias = true;
                    break;
                }
            }
            if (baseIsAlias)
            {
                size_t offset = std::stoull(match[2].str(), nullptr, 16);
                if (offset >= 0x80 && offset <= 0x400 && offset % sizeof(void*) == 0)
                {
                    unsigned int candidateIndex = 0;
                    for (; candidateIndex < candidateCount; candidateIndex++)
                    {
                        if (candidates[candidateIndex].offset == offset)
                        {
                            candidates[candidateIndex].references++;
                            break;
                        }
                    }
                    if (candidateIndex == candidateCount && candidateCount < ARRAYSIZE(candidates))
                    {
                        candidates[candidateCount++] = {offset, 1, 0, 0};
                    }
                    OffsetCandidate& candidate = candidates[candidateIndex];
                    if (std::regex_match(text, zeroTestPattern))
                    {
                        candidate.zeroTests++;
                    }
                    if (std::regex_search(text, writePattern))
                    {
                        candidate.writes++;
                    }
                }
            }
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }
    unsigned int bestScore = 0;
    unsigned int bestPairCount = 0;
    size_t bestLowerOffset = SIZE_MAX;
    size_t bestUpperOffset = SIZE_MAX;
    for (unsigned int lowerIndex = 0; lowerIndex < candidateCount; lowerIndex++)
    {
        for (unsigned int upperIndex = 0; upperIndex < candidateCount; upperIndex++)
        {
            if (candidates[upperIndex].offset == candidates[lowerIndex].offset + sizeof(void*))
            {
                unsigned int score =
                    candidates[lowerIndex].references + candidates[upperIndex].references +
                    6 * (candidates[lowerIndex].zeroTests + candidates[upperIndex].zeroTests) +
                    4 * (candidates[lowerIndex].writes + candidates[upperIndex].writes);
                if (score > bestScore)
                {
                    bestScore = score;
                    bestPairCount = 1;
                    bestLowerOffset = candidates[lowerIndex].offset;
                    bestUpperOffset = candidates[upperIndex].offset;
                }
                else if (score == bestScore)
                {
                    bestPairCount++;
                }
            }
        }
    }
    if (bestPairCount != 1 || bestScore < 2)
    {
        Wh_Log(L"DWM offset scan: candidates=%u bestScore=%u ties=%u", candidateCount, bestScore,
               bestPairCount);
        return false;
    }
    *topLevelWindowOffset = bestLowerOffset;
    *topLevelWindow3DOffset = bestUpperOffset;
    return true;
}

static void ClearDwmWindowMappingByWindowData(void* windowData)
{
    if (!windowData)
    {
        return;
    }
    AcquireSRWLockExclusive(&g_dwmWindowMappingsLock);
    for (int i = 0; i < MAX_DWM_WINDOW_MAPPINGS; i++)
    {
        if (g_dwmWindowMappings[i].windowData == windowData)
        {
            g_dwmWindowMappings[i] = {};
        }
    }
    ReleaseSRWLockExclusive(&g_dwmWindowMappingsLock);
}

static void ClearAllDwmWindowMappings()
{
    AcquireSRWLockExclusive(&g_dwmWindowMappingsLock);
    for (DwmWindowObjectMapping& mapping : g_dwmWindowMappings)
    {
        mapping = {};
    }
    ReleaseSRWLockExclusive(&g_dwmWindowMappingsLock);
}

static void* ClearDwmWindowMappingByTopLevelWindow(void* topLevelWindow)
{
    if (!topLevelWindow)
    {
        return nullptr;
    }
    void* windowData = nullptr;
    AcquireSRWLockExclusive(&g_dwmWindowMappingsLock);
    for (DwmWindowObjectMapping& mapping : g_dwmWindowMappings)
    {
        if (mapping.topLevelWindow == topLevelWindow)
        {
            windowData = mapping.windowData;
            mapping.topLevelWindow = nullptr;
            mapping.lastSeen = GetTickCount64();
        }
    }
    ReleaseSRWLockExclusive(&g_dwmWindowMappingsLock);
    return windowData;
}

static void ClearDwmWindowMappingByTopLevelWindow3D(void* topLevelWindow3D)
{
    if (!topLevelWindow3D)
    {
        return;
    }
    AcquireSRWLockExclusive(&g_dwmWindowMappingsLock);
    for (int i = 0; i < MAX_DWM_WINDOW_MAPPINGS; i++)
    {
        DwmWindowObjectMapping& mapping = g_dwmWindowMappings[i];
        if (mapping.topLevelWindow3D == topLevelWindow3D)
        {
            mapping.topLevelWindow3D = nullptr;
            if (!mapping.topLevelWindow)
            {
                mapping = {};
            }
        }
    }
    ReleaseSRWLockExclusive(&g_dwmWindowMappingsLock);
}

static void RegisterDwmWindowMapping(void* windowData, void* topLevelWindow, void* topLevelWindow3D)
{
    if (!windowData || (!topLevelWindow && !topLevelWindow3D))
    {
        return;
    }
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_dwmWindowMappingsLock);
    int targetIndex = -1;
    int oldestIndex = 0;
    ULONGLONG oldestSeen = ULLONG_MAX;
    for (int i = 0; i < MAX_DWM_WINDOW_MAPPINGS; i++)
    {
        DwmWindowObjectMapping& mapping = g_dwmWindowMappings[i];
        if ((topLevelWindow && mapping.windowData != windowData &&
             mapping.topLevelWindow == topLevelWindow) ||
            (topLevelWindow3D && mapping.windowData != windowData &&
             mapping.topLevelWindow3D == topLevelWindow3D))
        {
            mapping = {};
        }
        if (mapping.windowData == windowData)
        {
            targetIndex = i;
        }
        if (!mapping.windowData && targetIndex < 0)
        {
            targetIndex = i;
        }
        if (mapping.lastSeen < oldestSeen)
        {
            oldestSeen = mapping.lastSeen;
            oldestIndex = i;
        }
    }
    if (targetIndex < 0)
    {
        targetIndex = oldestIndex;
        g_dwmWindowMappings[targetIndex] = {};
    }
    for (int i = 0; i < MAX_DWM_WINDOW_MAPPINGS; i++)
    {
        if (i != targetIndex && g_dwmWindowMappings[i].windowData == windowData)
        {
            g_dwmWindowMappings[i] = {};
        }
    }
    DwmWindowObjectMapping& mapping = g_dwmWindowMappings[targetIndex];
    mapping.windowData = windowData;
    if (topLevelWindow)
    {
        mapping.topLevelWindow = topLevelWindow;
    }
    if (topLevelWindow3D)
    {
        mapping.topLevelWindow3D = topLevelWindow3D;
    }
    mapping.lastSeen = now;
    ReleaseSRWLockExclusive(&g_dwmWindowMappingsLock);
}

static void MarkAnimationSlotForDwmObjectRefresh(void* windowData)
{
    HWND hwnd = GetHwndFromWindowData(windowData);
    if (!hwnd)
    {
        return;
    }
    bool refreshNeeded = false;
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        WindowAnimationSlot& slot = g_animationSlots[i];
        if (slot.active && slot.hwnd == hwnd)
        {
            slot.transformRebindRevision++;
            refreshNeeded = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (refreshNeeded)
    {
        RequestDwmScenePass();
    }
}

static bool IsTopLevelWindowForWindowData(void* candidate, void* windowData)
{
    if (!candidate || !windowData)
    {
        return false;
    }
    if (g_topLevelWindowWindowDataOffset != SIZE_MAX)
    {
        BYTE* backPointer =
            static_cast<BYTE*>(candidate) + g_topLevelWindowWindowDataOffset;
        if (!IsReadableMemory(backPointer, sizeof(void*)) ||
            *reinterpret_cast<void**>(backPointer) != windowData)
        {
            return false;
        }
    }
    return IsDwmObjectPointerValid(candidate, g_topLevelWindowVtable);
}

static bool ResolveDwmWindowObjects(void* windowData, void** topLevelWindow,
                                    void** topLevelWindow3D)
{
    *topLevelWindow = nullptr;
    *topLevelWindow3D = nullptr;
    if (!windowData)
    {
        return false;
    }
    // Constructor/SetWindowData hooks identify the visual that DWM published.
    // Offset reads are only a fallback while that mapping is being rebuilt.
    AcquireSRWLockShared(&g_dwmWindowMappingsLock);
    for (int i = 0; i < MAX_DWM_WINDOW_MAPPINGS; i++)
    {
        const DwmWindowObjectMapping& mapping = g_dwmWindowMappings[i];
        if (mapping.windowData == windowData)
        {
            *topLevelWindow = mapping.topLevelWindow;
            *topLevelWindow3D = mapping.topLevelWindow3D;
            break;
        }
    }
    ReleaseSRWLockShared(&g_dwmWindowMappingsLock);
    if (!IsTopLevelWindowForWindowData(*topLevelWindow, windowData))
    {
        *topLevelWindow = nullptr;
    }
    if (!IsDwmObjectPointerValid(*topLevelWindow3D, g_topLevelWindow3DVtable))
    {
        *topLevelWindow3D = nullptr;
    }
    if (!*topLevelWindow && g_windowDataTopLevelWindowOffset != SIZE_MAX)
    {
        BYTE* fieldAddress = static_cast<BYTE*>(windowData) + g_windowDataTopLevelWindowOffset;
        if (IsReadableMemory(fieldAddress, sizeof(void*)))
        {
            void* candidate = *reinterpret_cast<void**>(fieldAddress);
            if (IsTopLevelWindowForWindowData(candidate, windowData))
            {
                *topLevelWindow = candidate;
            }
        }
    }
    if (!*topLevelWindow3D &&
        g_topLevelWindow3DVtable.load(std::memory_order_acquire) &&
        g_windowDataTopLevelWindow3DOffset != SIZE_MAX)
    {
        BYTE* fieldAddress = static_cast<BYTE*>(windowData) + g_windowDataTopLevelWindow3DOffset;
        if (IsReadableMemory(fieldAddress, sizeof(void*)))
        {
            void* candidate = *reinterpret_cast<void**>(fieldAddress);
            if (IsDwmObjectPointerValid(candidate, g_topLevelWindow3DVtable))
            {
                *topLevelWindow3D = candidate;
            }
        }
    }
    if (*topLevelWindow || *topLevelWindow3D)
    {
        RegisterDwmWindowMapping(windowData, *topLevelWindow, *topLevelWindow3D);
    }
    return *topLevelWindow != nullptr;
}

static void* FindWindowDataForTopLevelWindow3D(void* topLevelWindow3D)
{
    if (!IsDwmObjectPointerValid(topLevelWindow3D, g_topLevelWindow3DVtable))
    {
        return nullptr;
    }
    void* windowData = nullptr;
    AcquireSRWLockShared(&g_dwmWindowMappingsLock);
    for (int i = 0; i < MAX_DWM_WINDOW_MAPPINGS; i++)
    {
        if (g_dwmWindowMappings[i].topLevelWindow3D == topLevelWindow3D)
        {
            windowData = g_dwmWindowMappings[i].windowData;
            break;
        }
    }
    ReleaseSRWLockShared(&g_dwmWindowMappingsLock);
    return windowData;
}

static void* __cdecl TopLevelWindowConstructorHook(void* pThis, void* windowData, bool unknown)
{
    void* result = g_topLevelWindowConstructorOriginal(pThis, windowData, unknown);
    void* constructedObject = result ? result : pThis;
    if (windowData && IsDwmObjectPointerValid(constructedObject, g_topLevelWindowVtable))
    {
        RegisterDwmWindowMapping(windowData, constructedObject, nullptr);
        MarkAnimationSlotForDwmObjectRefresh(windowData);
    }
    return result;
}

static void __cdecl TopLevelWindow3DSetWindowDataHook(void* pThis, void* windowData)
{
    g_topLevelWindow3DSetWindowDataOriginal(pThis, windowData);
    ClearDwmWindowMappingByTopLevelWindow3D(pThis);
    if (windowData && HasExactDwmVtableTrusted(pThis, g_topLevelWindow3DVtable))
    {
        RegisterDwmWindowMapping(windowData, nullptr, pThis);
        MarkAnimationSlotForDwmObjectRefresh(windowData);
    }
}

static void __cdecl WindowDataDestructorHook(void* pThis)
{
    ClearDwmWindowMappingByWindowData(pThis);
    g_windowDataDestructorOriginal(pThis);
}

static long __cdecl EnsureTopLevelWindowHook(void* pThis, void* windowData)
{
    long result = g_ensureTopLevelWindowOriginal(pThis, windowData);
    if (result >= 0 && windowData &&
        HasExactDwmVtableTrusted(pThis, g_windowListVtable))
    {
        g_windowListForSceneWake.store(pThis, std::memory_order_release);
        void* topLevelWindow = nullptr;
        void* topLevelWindow3D = nullptr;
        if (ResolveDwmWindowObjects(windowData, &topLevelWindow, &topLevelWindow3D))
        {
            MarkAnimationSlotForDwmObjectRefresh(windowData);
        }
    }
    return result;
}

static void __cdecl TopLevelWindowDestructorHook(void* pThis)
{
    void* windowData = ClearDwmWindowMappingByTopLevelWindow(pThis);
    if (windowData && !g_unloading.load(std::memory_order_acquire))
    {
        MarkAnimationSlotForDwmObjectRefresh(windowData);
    }
    g_topLevelWindowDestructorOriginal(pThis);
}

static void __cdecl TopLevelWindow3DDestructorHook(void* pThis)
{
    ClearDwmWindowMappingByTopLevelWindow3D(pThis);
    g_topLevelWindow3DDestructorOriginal(pThis);
}

static void ResetAnimationSlotsForSceneOwnerChange()
{
    // Scene-only proxy pins must never cross an owner-thread change.
    unsigned int droppedSlots = 0;
    unsigned int retainedProxies = 0;
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    for (WindowAnimationSlot& slot : g_animationSlots)
    {
        if (!slot.active && !slot.retiring)
        {
            continue;
        }
        retainedProxies += slot.matrixTransformProxy != nullptr;
        ResetAnimationSlotLocked(slot, slot.generation + 1);
        droppedSlots++;
    }
    WakeAllConditionVariable(&g_animationSlotsCondition);
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (droppedSlots)
    {
        g_abandonedProxyCount.fetch_add(retainedProxies, std::memory_order_acq_rel);
        g_sceneRecoveryCleanupPending.store(false, std::memory_order_release);
        g_sceneOwnershipResetPending.store(true, std::memory_order_release);
        Wh_Log(L"DWM scene owner changed: droppedSlots=%u retainedOldProxies=%u",
               droppedSlots, retainedProxies);
    }
}

enum class SceneThreadRegistration
{
    ExistingOwner,
    WakeBootstrap,
    AuthoritativeTimeline,
};

static bool RegisterDwmSceneThread(SceneThreadRegistration registration)
{
    DWORD currentThreadId = GetCurrentThreadId();
    DWORD ownerThreadId = g_dwmSceneThreadId.load(std::memory_order_acquire);
    if (ownerThreadId == 0 && registration == SceneThreadRegistration::ExistingOwner)
    {
        // Only the initial targeted wake or AdvanceTimelines can claim ownership.
        return false;
    }
    if (ownerThreadId == 0 &&
        g_dwmSceneThreadId.compare_exchange_strong(ownerThreadId, currentThreadId,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
    {
        return true;
    }
    if (ownerThreadId == currentThreadId)
    {
        return true;
    }
    if (registration == SceneThreadRegistration::AuthoritativeTimeline)
    {
        // DWM can replace its scene owner after compositor/desktop teardown.
        // AdvanceTimelines is authoritative. A live but stale thread ID can be
        // reused by another DWM thread after display/compositor teardown.
        ULONGLONG now = GetTickCount64();
        ULONGLONG lastTimeline =
            g_lastNativeTimelineTimestamp.load(std::memory_order_acquire);
        bool previousOwnerUnconfirmed = lastTimeline == 0;
        bool previousOwnerStale =
            lastTimeline && now - lastTimeline >= DWM_SCENE_OWNER_STALE_MS;
        bool previousOwnerStopped = false;
        HANDLE previousThread = OpenThread(SYNCHRONIZE, FALSE, ownerThreadId);
        if (previousThread)
        {
            previousOwnerStopped = WaitForSingleObject(previousThread, 0) == WAIT_OBJECT_0;
            CloseHandle(previousThread);
        }
        else
        {
            previousOwnerStopped = GetLastError() == ERROR_INVALID_PARAMETER;
        }
        // A bootstrap wake can run on a different DWM message thread. The first
        // native timeline callback is authoritative even while that thread lives.
        if ((previousOwnerUnconfirmed || previousOwnerStopped || previousOwnerStale) &&
            g_dwmSceneThreadId.compare_exchange_strong(ownerThreadId, currentThreadId,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
        {
            ResetAnimationSlotsForSceneOwnerChange();
            ClearAllDwmWindowMappings();
            g_dwmThreadMismatchLogged.store(false, std::memory_order_release);
            g_desktopManager.store(nullptr, std::memory_order_release);
            g_dwmCompositor.store(nullptr, std::memory_order_release);
            g_windowListForSceneWake.store(nullptr, std::memory_order_release);
            g_dwmObjectDiscoveryFailureCount.store(0, std::memory_order_release);
            g_sceneWakeOutstanding.store(0, std::memory_order_release);
            g_sceneWakeScheduled.store(false, std::memory_order_release);
            g_sceneWakeAwaitingNativeTimeline.store(true, std::memory_order_release);
            g_sceneWakePostTimestamp.store(0, std::memory_order_release);
            g_sceneSubmittedSerial.store(
                g_sceneRequestedSerial.load(std::memory_order_acquire),
                std::memory_order_release);
            Wh_Log(L"DWM scene thread changed; ownership re-armed");
            return true;
        }
    }
    if (!g_dwmThreadMismatchLogged.exchange(true, std::memory_order_acq_rel))
    {
        Wh_Log(L"DWM safety gate: ignoring a scene callback from a "
               L"non-owner thread");
    }
    return false;
}

static bool IsOnDwmSceneThread()
{
    DWORD sceneThreadId = g_dwmSceneThreadId.load(std::memory_order_acquire);
    return sceneThreadId != 0 && sceneThreadId == GetCurrentThreadId();
}

static void RearmAnimationSlotsForCompositorChange()
{
    unsigned int abandoned = 0;
    bool hasActiveSlots = false;
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    for (WindowAnimationSlot& slot : g_animationSlots)
    {
        if (!slot.active && !slot.retiring)
        {
            continue;
        }
        if (slot.matrixTransformProxy)
        {
            slot.matrixTransformProxy = nullptr;
            abandoned++;
        }
        slot.boundTopLevelVisualProxy = nullptr;
        slot.boundTransitionVisualProxy = nullptr;
        slot.transformAttached = false;
        slot.transitionTransformAttached = false;
        slot.identityApplied = false;
        slot.transformRebindRevision++;
        slot.proxyCreationPending = slot.active;
        hasActiveSlots = hasActiveSlots || slot.active;
    }
    WakeAllConditionVariable(&g_animationSlotsCondition);
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (abandoned)
    {
        g_abandonedProxyCount.fetch_add(abandoned, std::memory_order_acq_rel);
    }
    if (hasActiveSlots)
    {
        g_sceneRequestedSerial.fetch_add(1, std::memory_order_acq_rel);
    }
}

static bool CacheDwmObjectsFromDesktopManager(void* manager)
{
    void* expectedManagerVtable = g_desktopManagerVtable.load(std::memory_order_acquire);
    if (g_desktopManagerCompositorOffset == SIZE_MAX || !manager ||
        !expectedManagerVtable || *reinterpret_cast<void**>(manager) != expectedManagerVtable)
    {
        return false;
    }
    BYTE* compositorField =
        static_cast<BYTE*>(manager) + g_desktopManagerCompositorOffset;
    void* compositor = *reinterpret_cast<void**>(compositorField);
    void* cachedManager = g_desktopManager.load(std::memory_order_acquire);
    void* cachedCompositor = g_dwmCompositor.load(std::memory_order_acquire);
    if (cachedManager == manager && cachedCompositor == compositor &&
        HasExactDwmVtableTrusted(compositor, g_compositorVtable))
    {
        return true;
    }
    if (!IsDwmObjectPointerValid(compositor, g_compositorVtable))
    {
        if (g_dwmCompositor.exchange(nullptr, std::memory_order_acq_rel))
        {
            RearmAnimationSlotsForCompositorChange();
        }
        g_desktopManager.store(manager, std::memory_order_release);
        return false;
    }
    if (cachedCompositor && cachedCompositor != compositor)
    {
        RearmAnimationSlotsForCompositorChange();
    }
    g_desktopManager.store(manager, std::memory_order_release);
    g_dwmCompositor.store(compositor, std::memory_order_release);
    Wh_Log(L"DWM objects verified: CDesktopManager=%p CCompositor=%p offset=0x%zx",
           manager, compositor, g_desktopManagerCompositorOffset);
    return true;
}

static void* FindDwmCompositor()
{
    g_desktopManager.store(nullptr, std::memory_order_release);
    if (!IsDwmImageAddress(g_desktopManagerInstanceAddress, sizeof(void*)) ||
        !IsReadableMemory(g_desktopManagerInstanceAddress, sizeof(void*)))
    {
        Wh_Log(L"DWM startup discovery: desktop manager singleton unavailable");
        return nullptr;
    }
    void* desktopManager = *static_cast<void**>(g_desktopManagerInstanceAddress);
    if (!IsReadableMemory(desktopManager, sizeof(void*)))
    {
        Wh_Log(L"DWM startup discovery: desktop manager object unavailable");
        return nullptr;
    }
    if (!CacheDwmObjectsFromDesktopManager(desktopManager))
    {
        Wh_Log(L"DWM startup discovery: singleton object validation failed");
        return nullptr;
    }
    return g_dwmCompositor.load(std::memory_order_acquire);
}

static bool InitializeDwmHooks()
{
    HMODULE udwm = GetModuleHandleW(L"udwm.dll");
    if (!udwm)
    {
        Wh_Log(L"DWM hooks: udwm.dll not loaded");
        return false;
    }
    if (!InitializeDwmModuleLayout(udwm))
    {
        Wh_Log(L"DWM safety gate: invalid udwm.dll PE layout");
        return false;
    }
    // Exact PDB candidates; the selected core is validated below.
    WindhawkUtils::SYMBOL_HOOK udwmDllHooks[] = {
        {{L"public: bool __cdecl CWindowData::IsGhostWindow(struct HWND__ * *)const "},
         &g_isGhostWindowOriginal,
         nullptr,
         true},
        {{L"public: void __cdecl CWindowList::OnPositionChange(class CWindowData *,bool)"},
         &g_onPositionChangeOriginal,
         OnPositionChangeHook,
         true},
        {{L"public: class CWindowData * __cdecl "
           L"CWindowList::FindWindowDataByHwnd(struct HWND__ *)"},
         &g_findWindowDataByHwnd,
         nullptr,
         true},
        {{L"public: long __cdecl CWindowList::GetSyncedWindowData("
           L"struct IDwmWindow *,bool,class CWindowData * *)"},
         &g_getSyncedWindowDataLong,
         nullptr,
         true},
        {{L"public: void __cdecl CWindowList::GetSyncedWindowData("
           L"struct IDwmWindow *,bool,class CWindowData * *)"},
         &g_getSyncedWindowDataVoid,
         nullptr,
         true},
        {{L"public: virtual long __cdecl CWindowList::WindowTransitionChange("
           L"struct IDwmWindow *,enum DWMTRANSITION_TARGET,struct tagRECT const &,"
           L"struct tagRECT const &,struct tagRECT const &,struct tagRECT const &,"
           L"struct tagRECT const &)"},
         &g_windowTransitionChangeOriginal,
         WindowTransitionChangeHook,
         true},
        {{L"private: void __cdecl CWindowList::CheckForMaximizedChange(class CWindowData *)"},
         &g_checkForMaximizedChangeOriginal,
         CheckForMaximizedChangeHook,
         true},
        {{L"public: long __cdecl CTopLevelWindow3D::"
           L"StartAnimationForMaximizeSnapTransition("
           L"enum CTopLevelWindow3D::WindowAnimationType,struct tagRECT const &)"},
         &g_startAnimationForMaximizeSnapTransitionOriginal,
         StartAnimationForMaximizeSnapTransitionHook,
         true},
        {{L"public: long __cdecl CTopLevelWindow3D::StartAnimation("
           L"enum CTopLevelWindow3D::WindowAnimationType)"},
         &g_topLevelWindow3DStartAnimationOriginal,
         TopLevelWindow3DStartAnimationHook,
         true},
        {{L"public: class CVisualProxy * __cdecl "
           L"CTopLevelWindow::GetCanvasRootVisualProxy(void)"},
         &g_getCanvasRootVisualProxy,
         nullptr,
         true},
        {{L"public: class CVisual * __cdecl "
           L"CTopLevelWindow::GetRootVisualNoAddRef(enum TLWRootVisualType)"},
         &g_topLevelWindowGetRootVisual,
         nullptr,
         true},
        {{L"public: virtual class CTopLevelWindow3D * __cdecl "
            L"winrt::Udwm::Transitions::implementation::TopLevelWindow3DWrapper::GetVisualWeak(void)"},
         &g_transitionWrapperGetVisualWeakFunction, nullptr, true},
        {{L"public: virtual class CVisualProxy * __cdecl "
            L"winrt::Udwm::Transitions::implementation::TopLevelWindow3DWrapper::GetVisualProxyWeak(void)"},
         &g_transitionWrapperGetVisualProxyWeakFunction, nullptr, true},
        {{L"public: class CWindowData * __cdecl "
           L"CTopLevelWindow::GetWindowData(void)const",
          L"public: class CWindowData * __cdecl "
           L"CTopLevelWindow::GetWindowData(void)const "},
         &g_topLevelWindowGetWindowData,
         nullptr,
         true},
        {{L"public: long __cdecl CMatrixTransformProxy::Update("
           L"struct _MilMatrix3x2D const &)"},
         &g_cMatrixTransformProxyUpdate,
         nullptr,
         true},
        {{L"public: long __cdecl CMatrixTransformProxy::Update("
           L"struct D2D_MATRIX_3X2_F const &)"},
         &g_cMatrixTransformProxyUpdateFloat,
         nullptr,
         true},
        {{L"public: long __cdecl CVisualProxy::SetTransform("
           L"class CBaseTransformProxy *)"},
         &g_cVisualProxySetTransform,
         nullptr,
         true},
        {{L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CMatrixTransformProxy>(class CMatrixTransformProxy * *)"},
         &g_createMatrixTransformProxy,
         nullptr,
         true},
        {{L"public: unsigned long __cdecl CBaseObject::Release(void)"},
         &g_cBaseObjectRelease,
         nullptr,
         true},
        {{L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CCachedVisualImageProxy>("
           L"class CCachedVisualImageProxy * *)"},
         &g_nativeProbeCreateImage, nullptr, true},
        {{L"public: long __cdecl CCachedVisualImageProxy::Update("
           L"struct MilRectF const &,struct MilSizeD const &,"
           L"class CRectResourceProxy const *,"
           L"class CSizeResourceProxy const *,class CVisualProxy *,"
           L"enum MilBrushMappingMode::Enum)"},
         &g_nativeProbeUpdateImageD, nullptr, true},
        {{L"public: long __cdecl CCachedVisualImageProxy::Update("
           L"struct D2D_RECT_F const &,struct D2D_SIZE_F const &,"
           L"class CRectResourceProxy const *,"
           L"class CSizeResourceProxy const *,class CVisualProxy *,"
           L"enum MilBrushMappingMode::Enum)"},
         &g_nativeProbeUpdateImageF, nullptr, true},
        {{L"public: static long __cdecl CDrawBitmapInstruction::Create("
           L"class CBaseImageProxy *,class CDrawBitmapInstruction * *)"},
         &g_nativeProbeCreateBitmapInstruction, nullptr, true},
        {{L"public: static long __cdecl CRenderDataVisual::Create("
           L"class CRenderDataVisual * *)"},
         &g_nativeMeshCreateVisual, nullptr, true},
        {{L"public: long __cdecl CRenderDataVisual::AddInstruction("
           L"class CRenderDataInstruction *)"},
         &g_nativeMeshAddInstruction, nullptr, true},
        {{L"public: virtual long __cdecl CRenderDataVisual::UpdateRenderData(void)"},
         &g_nativeMeshUpdateRenderData, nullptr, true},
        {{L"public: long __cdecl VisualCollection::InsertRelative("
           L"class CVisual *,class CVisual *,bool,bool)"},
         &g_nativeMeshInsertRelative, nullptr, true},
        {{L"public: long __cdecl VisualCollection::Remove(class CVisual *)"},
         &g_nativeMeshCollectionRemove, nullptr, true},
        {{L"public: long __cdecl CVisual::RemoveSelfFromParent(void)"},
         &g_nativeMeshRemoveFromParent, nullptr, true},
        {{L"const VisualCollection::`vftable'"},
         &g_nativeMeshCollectionVtable, nullptr, true},
        {{L"public: long __cdecl CCompositor::CreateMeshGeometry2dProxy("
           L"class CMeshGeometry2dProxy * *)",
          L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CMeshGeometry2dProxy>(class CMeshGeometry2dProxy * *)"},
         &g_nativeProbeCreateMesh, nullptr, true},
        {{L"public: long __cdecl CMeshGeometry2dProxy::Update("
           L"int,struct MilPoint3F const *,struct MilPoint2D const *,"
           L"unsigned int,unsigned int const *,unsigned int)"},
         &g_nativeProbeUpdateMesh, nullptr, true},
        {{L"public: long __cdecl CCompositor::CreateGeometry2dGroupProxy("
           L"class CGeometry2dGroupProxy * *)",
          L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CGeometry2dGroupProxy>(class CGeometry2dGroupProxy * *)"},
         &g_nativeProbeCreateGroup, nullptr, true},
        {{L"public: long __cdecl CGeometry2dGroupProxy::Update("
           L"class CMeshGeometry2dProxy const *)"},
         &g_nativeProbeUpdateGroup, nullptr, true},
        {{L"public: static long __cdecl CDrawMesh2DInstruction::Create("
           L"class CGeometry2dGroupProxy *,class CBitmapSourceProxy *,"
           L"class CDrawMesh2DInstruction * *)"},
         &g_nativeProbeCreateMeshInstruction, nullptr, true},
        {{L"public: virtual class CVisual * __cdecl "
           L"CVisual::GetTransformParent(void)const",
          L"public: virtual class CVisual * __cdecl "
           L"CVisual::GetTransformParent(void)const "},
         &g_nativeProbeGetVisualParent, nullptr, true},
        {{L"public: virtual class CVisualProxy * __cdecl "
           L"CVisual::GetVisualProxyForStructure(void)"},
         &g_nativeProbeGetVisualProxy, nullptr, true},
        {{L"const CCachedVisualImageProxy::`vftable'"},
         &g_nativeProbeImageVtableSymbol, nullptr, true},

        {{L"private: static class CDesktopManager * "
           L"CDesktopManager::s_pDesktopManagerInstance"},
         &g_desktopManagerInstanceAddress,
         nullptr,
         true},
        {{L"private: long __cdecl CDesktopManager::Initialize(struct IUnknown *)"},
         &g_desktopManagerInitializeFunction,
         nullptr,
         true},
        {{L"public: static long __cdecl CCompositor::Create(class CCompositor * *)"},
         &g_cCompositorCreateFunction,
         nullptr,
         true},
        {{L"private: static void __cdecl "
           L"CDesktopManager::HandleThreadMessage(unsigned int,unsigned __int64,__int64)"},
         &g_desktopManagerHandleThreadMessageOriginal,
         DesktopManagerHandleThreadMessageHook,
         true},
        {{L"public: long __cdecl CDesktopManager::PostStartAnimations(void)"},
         &g_desktopManagerPostStartAnimations,
         nullptr,
         true},
        {{L"private: __cdecl CTopLevelWindow::CTopLevelWindow(class CWindowData *,bool)"},
         &g_topLevelWindowConstructorFunction,
         nullptr,
         true},
        {{L"protected: virtual __cdecl CTopLevelWindow::~CTopLevelWindow(void)",
          L"public: virtual __cdecl CTopLevelWindow::~CTopLevelWindow(void)",
          L"private: virtual __cdecl CTopLevelWindow::~CTopLevelWindow(void)"},
         &g_topLevelWindowDestructorOriginal,
         TopLevelWindowDestructorHook,
         true},
        {{L"public: void __cdecl CTopLevelWindow3D::SetWindowData(class CWindowData *)"},
         &g_topLevelWindow3DSetWindowDataOriginal,
         TopLevelWindow3DSetWindowDataHook,
         true},
        {{L"public: __cdecl CWindowData::~CWindowData(void)"},
         &g_windowDataDestructorOriginal,
         WindowDataDestructorHook,
         true},
        {{L"private: long __cdecl CWindowList::EnsureTopLevelWindow(class CWindowData *)"},
         &g_ensureTopLevelWindowOriginal,
         EnsureTopLevelWindowHook,
         true},
        {{L"protected: virtual __cdecl CTopLevelWindow3D::~CTopLevelWindow3D(void)",
          L"public: virtual __cdecl CTopLevelWindow3D::~CTopLevelWindow3D(void)"},
         &g_topLevelWindow3DDestructorOriginal,
         TopLevelWindow3DDestructorHook,
         true},
        {{L"public: long __cdecl CWindowList::ForceUpdateScene(void)"},
         &g_windowListForceUpdateSceneOriginal,
         ForceUpdateSceneHook,
         true},
        {{L"public: virtual long __cdecl CWindowList::UpdateScene(void)"},
         &g_windowListUpdateSceneOriginal,
         UpdateSceneHook,
         true},
        {{L"private: void __cdecl CDesktopManager::AdvanceTimelines(double)"},
         &g_desktopManagerAdvanceTimelinesOriginal,
         AdvanceTimelinesHook,
         true},
        {{L"const CTopLevelWindow::`vftable'"},
         &g_topLevelWindowVtableSymbol,
         nullptr,
         true},
        {{L"const CTopLevelWindow3D::`vftable'{for `CRenderDataVisual'}"},
         &g_topLevelWindow3DVtableSymbol,
         nullptr,
         true},
        {{L"const CDesktopManager::`vftable'"},
         &g_desktopManagerVtableSymbol,
         nullptr,
         true},
        {{L"const CWindowList::`vftable'"},
         &g_windowListVtableSymbol,
         nullptr,
         true},
        // Offset-0 base on both compositor layouts; CBaseObject is at +8.
        {{L"const CCompositor::`vftable'{for `Windows::UI::Composition::IInteropCompositorPartnerCallback'}"},
         &g_compositorVtableSymbol,
         nullptr,
         true},
        {{L"const CVisualProxy::`vftable'"},
         &g_visualProxyVtableSymbol,
         nullptr,
         true},
        {{L"const CRedirectVisualProxy::`vftable'"},
         &g_redirectVisualProxyVtableSymbol, nullptr, true},
        {{L"const CContainerVisualProxy::`vftable'"},
         &g_containerVisualProxyVtableSymbol, nullptr, true},
        {{L"const CMatrixTransformProxy::`vftable'"},
         &g_matrixTransformProxyVtableSymbol,
         nullptr,
         true},
    };
    if (!WindhawkUtils::HookSymbols(udwm, udwmDllHooks, ARRAYSIZE(udwmDllHooks)))
    {
        Wh_Log(L"DWM hooks: symbol resolver failed");
        return false;
    }
    auto keepValid = [](auto& function)
    {
        if (function && !IsDwmFunctionPointerValid(reinterpret_cast<void*>(function)))
        {
            function = nullptr;
        }
    };
    keepValid(g_findWindowDataByHwnd);
    keepValid(g_getSyncedWindowDataLong);
    keepValid(g_getSyncedWindowDataVoid);
    keepValid(g_cMatrixTransformProxyUpdate);
    keepValid(g_cMatrixTransformProxyUpdateFloat);
    keepValid(g_topLevelWindow3DStartAnimationOriginal);
    keepValid(g_getCanvasRootVisualProxy);
    keepValid(g_topLevelWindowGetRootVisual);
    keepValid(g_topLevelWindowGetWindowData);
    keepValid(g_desktopManagerPostStartAnimations);
    keepValid(g_cBaseObjectRelease);
    keepValid(g_nativeProbeCreateImage);
    keepValid(g_nativeProbeUpdateImageD);
    keepValid(g_nativeProbeUpdateImageF);
    keepValid(g_nativeProbeCreateBitmapInstruction);
    keepValid(g_nativeMeshCreateVisual);
    keepValid(g_nativeMeshAddInstruction);
    keepValid(g_nativeMeshUpdateRenderData);
    keepValid(g_nativeMeshInsertRelative);
    keepValid(g_nativeMeshRemoveFromParent);
    keepValid(g_nativeMeshCollectionRemove);
    if (!IsDwmImageAddress(g_nativeMeshCollectionVtable, sizeof(void*)))
    {
        g_nativeMeshCollectionVtable = nullptr;
    }
    keepValid(g_nativeProbeCreateMesh);
    keepValid(g_nativeProbeUpdateMesh);
    keepValid(g_nativeProbeCreateGroup);
    keepValid(g_nativeProbeUpdateGroup);
    keepValid(g_nativeProbeCreateMeshInstruction);
    keepValid(g_nativeProbeGetVisualParent);
    keepValid(g_nativeProbeGetVisualProxy);
    auto cacheVtableSymbol = [](void* symbol, std::atomic<void*>& target)
    {
        if (!IsDwmImageAddress(symbol, sizeof(void*) * 3))
        {
            return false;
        }
        void** functions = static_cast<void**>(symbol);
        for (int i = 0; i < 3; i++)
        {
            if (!IsDwmFunctionPointerValid(functions[i]))
            {
                return false;
            }
        }
        target.store(symbol, std::memory_order_release);
        return true;
    };
    bool hasExactDesktopManagerVtable =
        cacheVtableSymbol(g_desktopManagerVtableSymbol, g_desktopManagerVtable);
    bool hasExactWindowListVtable =
        cacheVtableSymbol(g_windowListVtableSymbol, g_windowListVtable);
    bool hasExactCompositorVtable =
        cacheVtableSymbol(g_compositorVtableSymbol, g_compositorVtable);
    bool hasExactTopLevelWindowVtable =
        cacheVtableSymbol(g_topLevelWindowVtableSymbol, g_topLevelWindowVtable);
    bool hasExactTopLevelWindow3DVtable =
        cacheVtableSymbol(g_topLevelWindow3DVtableSymbol, g_topLevelWindow3DVtable);
    bool hasExactVisualProxyVtable =
        cacheVtableSymbol(g_visualProxyVtableSymbol, g_visualProxyVtable);
    bool hasExactRedirectProxyVtable =
        cacheVtableSymbol(g_redirectVisualProxyVtableSymbol, g_redirectVisualProxyVtable);
    bool hasExactContainerProxyVtable =
        cacheVtableSymbol(g_containerVisualProxyVtableSymbol, g_containerVisualProxyVtable);
    bool hasAnyExactVisualProxyVtable =
        hasExactVisualProxyVtable || hasExactRedirectProxyVtable || hasExactContainerProxyVtable;
    bool hasExactMatrixProxyVtable =
        cacheVtableSymbol(g_matrixTransformProxyVtableSymbol, g_matrixTransformProxyVtable);
    cacheVtableSymbol(g_nativeProbeImageVtableSymbol, g_nativeProbeImageVtable);
    Wh_Log(L"NATIVE MESH SOURCE: create=%d update=%s imageVtable=%p",
           g_nativeProbeCreateImage != nullptr,
           g_nativeProbeUpdateImageD ? L"double" :
               (g_nativeProbeUpdateImageF ? L"float" : L"unavailable"),
           g_nativeProbeImageVtable.load(std::memory_order_acquire));
    Wh_Log(L"NATIVE MESH BRIDGE: mesh=%d update=%d group=%d "
           L"groupUpdate=%d meshFactory=%d cachedImageMesh=experimental "
           L"getParent=%d getProxy=%d",
           g_nativeProbeCreateMesh != nullptr,
           g_nativeProbeUpdateMesh != nullptr,
           g_nativeProbeCreateGroup != nullptr,
           g_nativeProbeUpdateGroup != nullptr,
           g_nativeProbeCreateMeshInstruction != nullptr,
           g_nativeProbeGetVisualParent != nullptr,
           g_nativeProbeGetVisualProxy != nullptr);
    if (g_cMatrixTransformProxyUpdate && g_cMatrixTransformProxyUpdateFloat)
    {
        Wh_Log(L"DWM compatibility: ambiguous ABI variants");
        return false;
    }
    if (g_getSyncedWindowDataLong && g_getSyncedWindowDataVoid)
    {
        Wh_Log(L"DWM compatibility: disabling ambiguous transition lookup ABI");
        g_getSyncedWindowDataLong = nullptr;
        g_getSyncedWindowDataVoid = nullptr;
    }
    const wchar_t* animationHook =
        g_startAnimationForMaximizeSnapTransitionOriginal
            ? L"specific"
            : (g_topLevelWindow3DStartAnimationOriginal
                   ? L"generic-visual-sync"
                   : L"unavailable; using finalized-state fallback");
    Wh_Log(L"Native maximize/restore hooks: request=%s animation=%s",
           g_windowTransitionChangeOriginal && HasSyncedWindowData() ? L"available"
                                                                     : L"unavailable",
           animationHook);
    struct RequiredDwmFunction
    {
        const wchar_t* name;
        void* address;
    };
    RequiredDwmFunction requiredFunctions[] = {
        {L"CVisualProxy::SetTransform", reinterpret_cast<void*>(g_cVisualProxySetTransform)},
        {L"CCompositor::CreateProxy<CMatrixTransformProxy>",
         reinterpret_cast<void*>(g_createMatrixTransformProxy)},
        {L"CBaseObject::Release", reinterpret_cast<void*>(g_cBaseObjectRelease)},
        {L"CWindowList::FindWindowDataByHwnd",
         reinterpret_cast<void*>(g_findWindowDataByHwnd)},
        {L"CDesktopManager::Initialize", g_desktopManagerInitializeFunction},
        {L"CCompositor::Create", g_cCompositorCreateFunction},
        {L"CDesktopManager::HandleThreadMessage",
         reinterpret_cast<void*>(g_desktopManagerHandleThreadMessageOriginal)},
        {L"CDesktopManager::PostStartAnimations",
         reinterpret_cast<void*>(g_desktopManagerPostStartAnimations)},
        {L"CDesktopManager::AdvanceTimelines",
         reinterpret_cast<void*>(g_desktopManagerAdvanceTimelinesOriginal)},
        {L"CTopLevelWindow::GetCanvasRootVisualProxy",
         reinterpret_cast<void*>(g_getCanvasRootVisualProxy)},
        {L"CTopLevelWindow::GetRootVisualNoAddRef",
         reinterpret_cast<void*>(g_topLevelWindowGetRootVisual)},
        {L"CTopLevelWindow::CTopLevelWindow",
         reinterpret_cast<void*>(g_topLevelWindowConstructorFunction)},
        {L"CTopLevelWindow::~CTopLevelWindow",
         reinterpret_cast<void*>(g_topLevelWindowDestructorOriginal)},
        {L"CWindowList::EnsureTopLevelWindow",
         reinterpret_cast<void*>(g_ensureTopLevelWindowOriginal)}};
    bool missingCoreFunction = false;
    for (const RequiredDwmFunction& function : requiredFunctions)
    {
        if (!IsDwmFunctionPointerValid(function.address))
        {
            Wh_Log(L"DWM compatibility: missing core symbol: %s", function.name);
            missingCoreFunction = true;
        }
    }
    struct RequiredDwmVtable
    {
        const wchar_t* name;
        bool available;
    };
    RequiredDwmVtable requiredVtables[] = {
        {L"CDesktopManager", hasExactDesktopManagerVtable},
        {L"CWindowList", hasExactWindowListVtable},
        {L"CCompositor", hasExactCompositorVtable},
        {L"CTopLevelWindow", hasExactTopLevelWindowVtable},
        {L"CVisualProxy/base-compatible proxy", hasAnyExactVisualProxyVtable},
        {L"CMatrixTransformProxy", hasExactMatrixProxyVtable}};
    for (const RequiredDwmVtable& vtable : requiredVtables)
    {
        if (!vtable.available)
        {
            Wh_Log(L"DWM compatibility: missing core vftable: %s", vtable.name);
            missingCoreFunction = true;
        }
    }
    if (missingCoreFunction)
    {
        return false;
    }
    Wh_Log(L"DWM compatibility: compositor primary vftable verified by PDB");
    if (!FindVisualProxyAccessPath(reinterpret_cast<void*>(g_getCanvasRootVisualProxy),
                                   &g_canvasVisualOwnerOffset,
                                   &g_visualProxyOffset))
    {
        Wh_Log(L"DWM compatibility: visual accessor path unavailable; "
               L"using typed accessor validation");
    }
    g_transitionVisualProxyOffset = FindTransitionVisualProxyOffset(
        g_transitionWrapperGetVisualWeakFunction, g_transitionWrapperGetVisualProxyWeakFunction);
    Wh_Log(L"Native transition visual: %s proxyOffset=0x%zx",
           hasExactTopLevelWindow3DVtable && g_transitionVisualProxyOffset != SIZE_MAX
               ? L"verified"
               : L"unavailable; main window animation remains enabled",
           g_transitionVisualProxyOffset);
    g_desktopManagerCompositorOffset = FindDesktopManagerCompositorOffset(
        g_desktopManagerInitializeFunction, g_cCompositorCreateFunction);
    if (g_desktopManagerCompositorOffset == SIZE_MAX)
    {
        Wh_Log(L"DWM compatibility: compositor member could not be proven from PDB functions");
        return false;
    }
    g_desktopManagerThreadIdOffset = FindDesktopManagerThreadIdOffset(
        reinterpret_cast<void*>(g_desktopManagerPostStartAnimations));
    if (g_desktopManagerThreadIdOffset == SIZE_MAX)
    {
        Wh_Log(L"DWM compatibility: message-thread member could not be derived");
        return false;
    }
    bool hasMatrixUpdate =
        IsDwmFunctionPointerValid(reinterpret_cast<void*>(g_cMatrixTransformProxyUpdate)) ||
        IsDwmFunctionPointerValid(
            reinterpret_cast<void*>(g_cMatrixTransformProxyUpdateFloat));
    if (!hasMatrixUpdate)
    {
        Wh_Log(L"DWM compatibility: no compatible CMatrixTransformProxy::Update is available");
        return false;
    }
    bool hasForceUpdateScene = IsDwmFunctionPointerValid(
        reinterpret_cast<void*>(g_windowListForceUpdateSceneOriginal));
    bool hasUpdateScene =
        IsDwmFunctionPointerValid(reinterpret_cast<void*>(g_windowListUpdateSceneOriginal));
    if (!hasForceUpdateScene && !hasUpdateScene)
    {
        Wh_Log(L"DWM compatibility: neither CWindowList scene hook is available");
        return false;
    }
    g_windowDataHwndOffset = FindOffsetFromFunction(
        reinterpret_cast<void*>(g_isGhostWindowOriginal), SIZE_MAX);
    size_t accessorWindowDataOffset = FindOffsetFromFunction(
        reinterpret_cast<void*>(g_topLevelWindowGetWindowData), SIZE_MAX);
    if (g_windowDataHwndOffset == SIZE_MAX)
    {
        Wh_Log(L"DWM compatibility: CWindowData HWND offset could not be derived");
        return false;
    }
    if (!FindConstructorWindowDataOffsets(
            reinterpret_cast<void*>(g_topLevelWindowConstructorFunction),
            &g_windowDataTopLevelWindowOffset, &g_topLevelWindowWindowDataOffset))
    {
        Wh_Log(L"DWM compatibility: bidirectional top-level mapping could not be derived");
        return false;
    }
    if (accessorWindowDataOffset != SIZE_MAX &&
        accessorWindowDataOffset != g_topLevelWindowWindowDataOffset)
    {
        Wh_Log(L"DWM compatibility: conflicting CTopLevelWindow mapping offsets");
        return false;
    }
    g_topLevelWindow3DWindowDataOffset = FindStoredWindowDataOffset(
        reinterpret_cast<void*>(g_topLevelWindow3DSetWindowDataOriginal));
    if (g_topLevelWindow3DWindowDataOffset == SIZE_MAX)
    {
        Wh_Log(L"DWM compatibility: early transition reverse mapping unavailable");
    }
    size_t pairedTopLevelWindowOffset = SIZE_MAX;
    size_t pairedTopLevelWindow3DOffset = SIZE_MAX;
    if (FindWindowDataTopLevelOffsets(reinterpret_cast<void*>(g_ensureTopLevelWindowOriginal),
                                      &pairedTopLevelWindowOffset,
                                      &pairedTopLevelWindow3DOffset) &&
        pairedTopLevelWindowOffset == g_windowDataTopLevelWindowOffset)
    {
        g_windowDataTopLevelWindow3DOffset = pairedTopLevelWindow3DOffset;
    }
    else
    {
        g_windowDataTopLevelWindow3DOffset = SIZE_MAX;
        Wh_Log(L"DWM compatibility: existing-window transition mapping unavailable");
    }
    Wh_Log(L"DWM compatibility ABI: HWND lookup=pointer transition lookup=%s "
           L"matrix=%s visual=complete-root ownerOffset=0x%zx proxyOffset=0x%zx "
           L"compositorOffset=0x%zx "
           L"threadIdOffset=0x%zx TLW3DDataOffset=0x%zx",
           g_getSyncedWindowDataVoid
               ? L"void"
               : (g_getSyncedWindowDataLong ? L"HRESULT" : L"unavailable"),
           g_cMatrixTransformProxyUpdate ? L"double" : L"float",
           g_canvasVisualOwnerOffset, g_visualProxyOffset,
           g_desktopManagerCompositorOffset, g_desktopManagerThreadIdOffset,
           g_topLevelWindow3DWindowDataOffset);
    void* compositor = FindDwmCompositor();
    g_dwmCompositor.store(compositor, std::memory_order_release);
    if (!compositor)
    {
        Wh_Log(L"DWM compatibility: compositor discovery deferred to the first scene timeline");
    }
    Wh_Log(L"DWM startup cache ready: timestamp=0x%08X imageSize=0x%X "
           L"HWND=0x%zx TLW=0x%zx TLW3D=0x%zx TLWData=0x%zx "
           L"TLWVtable=%p sceneHooks=%s%s",
           g_dwmModuleLayout.timeDateStamp, g_dwmModuleLayout.sizeOfImage, g_windowDataHwndOffset,
           g_windowDataTopLevelWindowOffset, g_windowDataTopLevelWindow3DOffset,
           g_topLevelWindowWindowDataOffset, g_topLevelWindowVtableSymbol,
           hasForceUpdateScene ? L"ForceUpdateScene " : L"",
           hasUpdateScene ? L"UpdateScene" : L"");
    if (!WindhawkUtils::SetFunctionHook(g_topLevelWindowConstructorFunction,
                                        TopLevelWindowConstructorHook,
                                        &g_topLevelWindowConstructorOriginal))
    {
        Wh_Log(L"DWM hooks: failed to register CTopLevelWindow constructor hook");
        return false;
    }
    return true;
}

static bool NativeMeshAbiVerified()
{
    if (g_dwmModuleLayout.timeDateStamp != 0xE19C3234 ||
        g_dwmModuleLayout.sizeOfImage != 0x16A000 ||
        !g_nativeMeshCreateVisual ||
        !g_nativeMeshAddInstruction || !g_nativeMeshUpdateRenderData ||
        !g_nativeMeshInsertRelative || !g_nativeMeshRemoveFromParent ||
        !g_nativeMeshCollectionRemove || !g_nativeMeshCollectionVtable ||
        !g_nativeProbeGetVisualParent || !g_nativeProbeGetVisualProxy ||
        !g_createMatrixTransformProxy || !g_cVisualProxySetTransform ||
        !HasMatrixTransformUpdate() || !g_cBaseObjectRelease)
    {
        return false;
    }
    const auto* remove = reinterpret_cast<const BYTE*>(g_nativeMeshRemoveFromParent);
    const auto* insert = reinterpret_cast<const BYTE*>(g_nativeMeshInsertRelative);
    const auto* release = reinterpret_cast<const BYTE*>(g_cBaseObjectRelease);
    constexpr BYTE removePrefix[] = {0x48,0x8B,0xD1,0x48,0x8B,0x49,0x18,
                                      0x48,0x85,0xC9,0x74,0x09,0x48,0x83,0xC1,0x20};
    constexpr BYTE siblingCompare[] = {0x49,0x3B,0xC8,0x0F,0x84,0xB6,0x01,0x00,0x00};
    constexpr BYTE siblingIndex[] = {0x44,0x8B,0xF8};
    constexpr BYTE afterReference[] = {0x43,0x8D,0x1C,0x27};
    constexpr BYTE ownerLoad[] = {0x48,0x8B,0x59,0x08};
    constexpr BYTE releasePrefix[] = {0x40,0x53,0x48,0x83,0xEC,0x20,
                                      0x83,0xCB,0xFF,0xF0,0x0F,0xC1,0x59,0x08};
    constexpr BYTE releaseSuffix[] = {0x83,0xEB,0x01};
    if (!IsReadableMemory(remove, 24) || !IsReadableMemory(insert, 0x247) ||
        !IsReadableMemory(release, 17) ||
        std::memcmp(remove, removePrefix, sizeof(removePrefix)) ||
        remove[16] != 0xE9 || remove[21] != 0x33 ||
        remove[22] != 0xC0 || remove[23] != 0xC3 ||
        std::memcmp(insert + 0x85, siblingCompare, sizeof(siblingCompare)) ||
        std::memcmp(insert + 0x244, siblingIndex, sizeof(siblingIndex)) ||
        std::memcmp(insert + 0xFC, afterReference, sizeof(afterReference)) ||
        std::memcmp(insert + 37, ownerLoad, sizeof(ownerLoad)) ||
        std::memcmp(release, releasePrefix, sizeof(releasePrefix)) ||
        std::memcmp(release + 14, releaseSuffix, sizeof(releaseSuffix)))
    {
        return false;
    }
    int32_t jump = 0;
    std::memcpy(&jump, remove + 17, sizeof(jump));
    return reinterpret_cast<uintptr_t>(remove) + 21 + jump ==
           reinterpret_cast<uintptr_t>(g_nativeMeshCollectionRemove);
}

static void* FindNativeMeshSiblingCollection(void* root)
{
    if (!root || !IsReadableMemory(root, sizeof(void*)) ||
        !IsDwmImageAddress(*reinterpret_cast<void**>(root), sizeof(void*)))
    {
        return nullptr;
    }
    void* parent = ReadPointerMember(root, 0x18);
    if (!parent || parent != g_nativeProbeGetVisualParent(root) ||
        !IsReadableMemory(parent, sizeof(void*)) ||
        !IsDwmImageAddress(*reinterpret_cast<void**>(parent), sizeof(void*)))
    {
        return nullptr;
    }
    auto* collection = static_cast<BYTE*>(parent) + 0x20;
    if (!IsReadableMemory(collection, 0x2C) ||
        *reinterpret_cast<void**>(collection) != g_nativeMeshCollectionVtable ||
        ReadPointerMember(collection, 8) != parent)
    {
        return nullptr;
    }
    unsigned int count = 0;
    std::memcpy(&count, collection + 0x28, sizeof(count));
    if (!count || count > 256)
        return nullptr;
    void* entries = ReadPointerMember(collection, 0x10);
    if (!IsReadableMemory(entries, count * sizeof(void*)))
        return nullptr;
    unsigned int matches = 0;
    for (unsigned int i = 0; i < count; ++i)
        matches += static_cast<void**>(entries)[i] == root;
    return matches == 1 ? collection : nullptr;
}

static bool RetainNativeMeshRoot(void* root)
{
    auto* count = reinterpret_cast<volatile LONG*>(static_cast<BYTE*>(root) + 8);
    if (!IsWritableMemory(const_cast<LONG*>(count), sizeof(LONG)))
        return false;
    LONG current = InterlockedCompareExchange(count, 0, 0);
    for (unsigned int i = 0; i < 8 && current > 0 && current < 0x100000; ++i)
    {
        LONG previous = InterlockedCompareExchange(count, current + 1, current);
        if (previous == current) return true;
        current = previous;
    }
    return false;
}

static void ReleaseNativeMeshState(NativeMeshState& state)
{
    if (state.visual) g_cBaseObjectRelease(state.visual);
    if (state.transform) g_cBaseObjectRelease(state.transform);
    if (state.instruction) g_cBaseObjectRelease(state.instruction);
    if (state.group) g_cBaseObjectRelease(state.group);
    if (state.mesh) g_cBaseObjectRelease(state.mesh);
    if (state.image) g_cBaseObjectRelease(state.image);
    if (state.root) g_cBaseObjectRelease(state.root);
    state = {};
}

static void InvalidateAnimationTransformForNativeMesh(HWND hwnd)
{
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    for (WindowAnimationSlot& slot : g_animationSlots)
    {
        if (slot.active && slot.hwnd == hwnd)
        {
            slot.submittedMeshRevision = 0;
            slot.identityApplied = false;
        }
    }
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    RequestDwmScenePass();
}

static bool DetachNativeMesh(const wchar_t* reason)
{
    auto& state = g_nativeMeshState;
    if (!state.visual)
    {
        g_nativeMeshHwnd.store(nullptr, std::memory_order_release);
        g_nativeMeshActive.store(false, std::memory_order_release);
        return true;
    }
    void* parent = ReadPointerMember(state.visual, 0x18);
    long result = parent ? g_nativeMeshRemoveFromParent(state.visual) : S_OK;
    parent = ReadPointerMember(state.visual, 0x18);
    if (result < 0 || parent)
    {
        if (!state.detachFailureLogged)
        {
            Wh_Log(L"NATIVE RECTANGLE DETACH DEFERRED: HWND=%p reason=%s "
                   L"result=0x%08X parent=%p", state.hwnd, reason,
                   static_cast<unsigned>(result), parent);
            state.detachFailureLogged = true;
        }
        RequestDwmScenePass();
        return false;
    }
    HWND hwnd = state.hwnd;
    Wh_Log(L"NATIVE MESH DETACHED: HWND=%p reason=%s", hwnd, reason);
    ReleaseNativeMeshState(state);
    g_nativeMeshHwnd.store(nullptr, std::memory_order_release);
    g_nativeMeshActive.store(false, std::memory_order_release);
    g_nativeMeshStopRequested.store(false, std::memory_order_release);
    if (!g_unloading.load(std::memory_order_acquire))
        InvalidateAnimationTransformForNativeMesh(hwnd);
    return true;
}

static constexpr unsigned int NATIVE_MESH_CELLS = 20;
static constexpr unsigned int NATIVE_MESH_SIDE = NATIVE_MESH_CELLS + 1;
static constexpr unsigned int NATIVE_MESH_VERTEX_COUNT =
    NATIVE_MESH_SIDE * NATIVE_MESH_SIDE;
static constexpr unsigned int NATIVE_MESH_INDEX_COUNT =
    NATIVE_MESH_CELLS * NATIVE_MESH_CELLS * 6;

static void CalculateBernsteinBasis(double value, double basis[4]);

static void BuildNativeMeshIndices(unsigned int (&indices)[NATIVE_MESH_INDEX_COUNT])
{
    unsigned int count = 0;
    for (unsigned int y = 0; y < NATIVE_MESH_CELLS; ++y)
    {
        for (unsigned int x = 0; x < NATIVE_MESH_CELLS; ++x)
        {
            const unsigned int a = y * NATIVE_MESH_SIDE + x;
            const unsigned int b = a + 1;
            const unsigned int c = a + NATIVE_MESH_SIDE;
            const unsigned int d = c + 1;
            indices[count++] = a; indices[count++] = c; indices[count++] = b;
            indices[count++] = b; indices[count++] = c; indices[count++] = d;
        }
    }
}

static void UpdateNativeVertexMesh(NativeMeshState& state)
{
    if (!state.mesh || !g_nativeProbeUpdateMesh)
        return;

    WobbleMesh physics = {};
    ULONGLONG generation = 0;
    ULONGLONG revision = 0;
    bool found = false;
    AcquireSRWLockShared(&g_animationSlotsLock);
    for (const WindowAnimationSlot& slot : g_animationSlots)
    {
        if (slot.active && slot.hwnd == state.hwnd)
        {
            physics = slot.mesh;
            generation = slot.generation;
            revision = slot.meshRevision;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_animationSlotsLock);
    if (!found)
    {
        DetachNativeMesh(L"slot-inactive");
        return;
    }
    if (!std::isfinite(physics.width) ||
        !std::isfinite(physics.height) ||
        physics.width < 1.0 || physics.height < 1.0)
        return;
    if (state.meshGeneration && state.meshGeneration != generation)
    {
        DetachNativeMesh(L"mesh-generation-changed");
        return;
    }
    if (state.meshGeneration == generation &&
        state.submittedMeshRevision == revision)
        return;

    NativeProbePoint3F positions[NATIVE_MESH_VERTEX_COUNT] = {};
    NativeProbePoint2D coordinates[NATIVE_MESH_VERTEX_COUNT] = {};
    unsigned int indices[NATIVE_MESH_INDEX_COUNT] = {};
    Vec2 control[GRID_POINT_COUNT] = {};
    const double scaleX = static_cast<double>(state.width) / physics.width;
    const double scaleY = static_cast<double>(state.height) / physics.height;
    double maximumDisplacement = 0.0;
    for (unsigned int i = 0; i < GRID_POINT_COUNT; ++i)
    {
        const WobblePoint& point = physics.points[i];
        if (!std::isfinite(point.basePosition.x) ||
            !std::isfinite(point.basePosition.y) ||
            !std::isfinite(point.position.x) ||
            !std::isfinite(point.position.y))
        {
            DetachNativeMesh(L"mesh-position-invalid");
            return;
        }
        double limitX = std::min(400.0, std::max(100.0, physics.width * 0.35));
        double limitY = std::min(400.0, std::max(100.0, physics.height * 0.35));
        double dx = std::clamp(point.position.x - point.basePosition.x,
                               -limitX, limitX);
        double dy = std::clamp(point.position.y - point.basePosition.y,
                               -limitY, limitY);
        maximumDisplacement = std::max(maximumDisplacement,
                                        std::hypot(dx, dy));
        control[i] = {(point.basePosition.x + dx) * scaleX,
                      (point.basePosition.y + dy) * scaleY};
    }
    for (unsigned int y = 0; y < NATIVE_MESH_SIDE; ++y)
    {
        const double v = static_cast<double>(y) / NATIVE_MESH_CELLS;
        double basisY[4];
        CalculateBernsteinBasis(v, basisY);
        for (unsigned int x = 0; x < NATIVE_MESH_SIDE; ++x)
        {
            const double u = static_cast<double>(x) / NATIVE_MESH_CELLS;
            double basisX[4];
            CalculateBernsteinBasis(u, basisX);
            Vec2 position = {};
            for (unsigned int cy = 0; cy < GRID_HEIGHT; ++cy)
            {
                for (unsigned int cx = 0; cx < GRID_WIDTH; ++cx)
                {
                    const double weight = basisY[cy] * basisX[cx];
                    const Vec2& point = control[cy * GRID_WIDTH + cx];
                    position.x += weight * point.x;
                    position.y += weight * point.y;
                }
            }
            const unsigned int index = y * NATIVE_MESH_SIDE + x;
            positions[index] = {static_cast<float>(position.x),
                                static_cast<float>(position.y), 0.0f};
            coordinates[index] = {u * state.width, v * state.height};
        }
    }
    BuildNativeMeshIndices(indices);
    long result = g_nativeProbeUpdateMesh(state.mesh, 0, positions,
                                           coordinates, NATIVE_MESH_VERTEX_COUNT,
                                           indices, NATIVE_MESH_INDEX_COUNT);
    if (result < 0)
    {
        Wh_Log(L"NATIVE VERTEX MESH UPDATE FAILED: HWND=%p result=0x%08X",
               state.hwnd, static_cast<unsigned>(result));
        DetachNativeMesh(L"mesh-update-failed");
        return;
    }
    state.meshGeneration = generation;
    state.submittedMeshRevision = revision;
    if (++state.meshUpdateCount == 1 || state.meshUpdateCount % 120 == 0)
    {
        Wh_Log(L"NATIVE VERTEX MESH UPDATE: HWND=%p count=%u revision=%llu "
               L"maxDisplacement=%.1f", state.hwnd, state.meshUpdateCount,
               revision, maximumDisplacement);
    }
}

static bool GetNativeMeshWindowSize(HWND hwnd, int& width, int& height,
                                    bool logDimensions = false)
{
    RECT windowBounds = {};
    if (!GetWindowRect(hwnd, &windowBounds))
        return false;
    const int windowWidth = windowBounds.right - windowBounds.left;
    const int windowHeight = windowBounds.bottom - windowBounds.top;
    RECT frameBounds = {};
    bool hasFrameBounds = g_dwmGetWindowAttribute &&
        SUCCEEDED(g_dwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                          &frameBounds, sizeof(frameBounds)));
    const int frameWidth = hasFrameBounds ?
        frameBounds.right - frameBounds.left : 0;
    const int frameHeight = hasFrameBounds ?
        frameBounds.bottom - frameBounds.top : 0;
    // Extended frame bounds are physical but omit invisible resize borders.
    // If GetWindowRect is virtualized, restore its full physical outer size.
    UINT windowDpi = GetDpiForWindow(hwnd);
    UINT monitorDpiX = 0;
    UINT monitorDpiY = 0;
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor || !g_getDpiForMonitor ||
        FAILED(g_getDpiForMonitor(monitor, 0, &monitorDpiX, &monitorDpiY)))
        monitorDpiX = monitorDpiY = 0;
    width = std::max(windowWidth, frameWidth);
    height = std::max(windowHeight, frameHeight);
    if (hasFrameBounds && frameWidth > windowWidth && windowDpi &&
        monitorDpiX > windowDpi)
    {
        width = std::max(width, static_cast<int>(std::lround(
            static_cast<double>(windowWidth) * monitorDpiX / windowDpi)));
    }
    if (hasFrameBounds && frameHeight > windowHeight && windowDpi &&
        monitorDpiY > windowDpi)
    {
        height = std::max(height, static_cast<int>(std::lround(
            static_cast<double>(windowHeight) * monitorDpiY / windowDpi)));
    }
    if (logDimensions)
    {
        Wh_Log(L"NATIVE MESH BOUNDS: HWND=%p window=%dx%d frame=%dx%d "
               L"selected=%dx%d windowDpi=%u monitorDpi=%ux%u",
               hwnd, windowWidth, windowHeight, frameWidth, frameHeight,
               width, height, windowDpi, monitorDpiX, monitorDpiY);
    }
    return width >= 1 && height >= 1 && width <= 8192 && height <= 8192;
}

static long ResizeNativeMeshSourceImage(NativeMeshState& state,
                                        int width, int height)
{
    if (!state.image || !IsVisualProxyPointerValid(state.sourceProxy))
        return E_NOINTERFACE;
    NativeProbeRectF rect = {0, 0, static_cast<float>(width),
                             static_cast<float>(height)};
    constexpr int absoluteMappingMode = 0;
    if (g_nativeProbeUpdateImageD)
    {
        NativeProbeSizeD size = {static_cast<double>(width),
                                 static_cast<double>(height)};
        return g_nativeProbeUpdateImageD(state.image, rect, size, nullptr,
                                         nullptr, state.sourceProxy,
                                         absoluteMappingMode);
    }
    if (g_nativeProbeUpdateImageF)
    {
        NativeProbeSizeF size = {static_cast<float>(width),
                                 static_cast<float>(height)};
        return g_nativeProbeUpdateImageF(state.image, rect, size, nullptr,
                                         nullptr, state.sourceProxy,
                                         absoluteMappingMode);
    }
    return E_NOTIMPL;
}

static void MaintainNativeMesh()
{
    if (!IsOnDwmSceneThread() ||
        !g_nativeMeshActive.load(std::memory_order_acquire))
        return;
    auto& state = g_nativeMeshState;
    if (g_unloading.load(std::memory_order_acquire) ||
        g_nativeMeshStopRequested.load(std::memory_order_acquire))
    {
        DetachNativeMesh(g_unloading.load(std::memory_order_acquire)
                                  ? L"unload"
                                  : L"stop-requested");
        return;
    }
    int width = 0;
    int height = 0;
    bool sourceValid = state.root &&
        g_nativeProbeGetVisualProxy(state.root) == state.sourceProxy &&
        GetNativeMeshWindowSize(state.hwnd, width, height);
    if (!sourceValid)
        DetachNativeMesh(L"source-changed");
    else
    {
        if (width != state.width || height != state.height)
        {
            long result = ResizeNativeMeshSourceImage(state, width, height);
            if (result < 0)
            {
                Wh_Log(L"NATIVE MESH SOURCE RESIZE FAILED: HWND=%p "
                       L"size=%dx%d result=0x%08X", state.hwnd,
                       width, height, static_cast<unsigned>(result));
                DetachNativeMesh(L"source-resize-failed");
                return;
            }
            state.width = width;
            state.height = height;
            state.submittedMeshRevision = 0;
            Wh_Log(L"NATIVE MESH SOURCE RESIZED: HWND=%p size=%dx%d",
                   state.hwnd, width, height);
        }
        UpdateNativeVertexMesh(state);
    }
}

static bool TryAttachNativeMesh(HWND hwnd, void* topLevelWindow,
                                     void* sourceProxy, void* compositor,
                                     void*& image, void*& group,
                                     void*& instruction, void*& mesh,
                                     int width, int height)
{
    AcquireSRWLockShared(&g_settingsLock);
    bool meshSelected = g_settings.rendererMode == RendererMode::Mesh;
    ReleaseSRWLockShared(&g_settingsLock);
    if (!meshSelected || !NativeMeshAbiVerified() ||
        !g_topLevelWindowGetRootVisual ||
        g_nativeMeshActive.load(std::memory_order_acquire) ||
        g_unloading.load(std::memory_order_acquire))
    {
        Wh_Log(L"NATIVE MESH SKIPPED: HWND=%p mesh=%d abi=%d",
               hwnd, meshSelected, NativeMeshAbiVerified());
        return false;
    }
    g_nativeMeshStopRequested.store(false, std::memory_order_release);
    void* root = g_topLevelWindowGetRootVisual(topLevelWindow, 0);
    if (!root || !IsReadableMemory(root, sizeof(void*)) ||
        !IsDwmImageAddress(*reinterpret_cast<void**>(root), sizeof(void*)) ||
        g_nativeProbeGetVisualProxy(root) != sourceProxy)
        return false;
    void* collection = FindNativeMeshSiblingCollection(root);
    if (!collection || !RetainNativeMeshRoot(root))
    {
        Wh_Log(L"NATIVE MESH SKIPPED: HWND=%p sibling=%p",
               hwnd, collection);
        return false;
    }

    NativeMeshState pending = {};
    pending.root = root;
    pending.hwnd = hwnd;
    pending.sourceProxy = sourceProxy;
    pending.width = width;
    pending.height = height;
    const wchar_t* stage = L"CreateVisual";
    Wh_Log(L"NATIVE MESH ATTACH STAGE: %s HWND=%p", stage, hwnd);
    long result = g_nativeMeshCreateVisual(&pending.visual);
    if (result >= 0 && !pending.visual) result = E_UNEXPECTED;
    if (result >= 0)
    {
        stage = L"AddInstruction";
        Wh_Log(L"NATIVE MESH ATTACH STAGE: %s HWND=%p", stage, hwnd);
        result = g_nativeMeshAddInstruction(pending.visual, instruction);
    }
    if (result >= 0)
    {
        stage = L"PublishRenderData";
        Wh_Log(L"NATIVE MESH ATTACH STAGE: %s HWND=%p", stage, hwnd);
        result = g_nativeMeshUpdateRenderData(pending.visual);
    }
    if (result >= 0)
    {
        stage = L"CreateTransform";
        Wh_Log(L"NATIVE MESH ATTACH STAGE: %s HWND=%p", stage, hwnd);
        result = g_createMatrixTransformProxy(compositor, &pending.transform);
        if (result >= 0 && !pending.transform) result = E_UNEXPECTED;
    }
    if (result >= 0)
    {
        stage = L"PositionShape";
        const MilMatrix3x2D matrix = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
        Wh_Log(L"NATIVE MESH ATTACH STAGE: %s HWND=%p", stage, hwnd);
        result = UpdateMatrixTransformProxy(pending.transform, matrix);
    }
    if (result >= 0)
    {
        stage = L"InsertSibling";
        Wh_Log(L"NATIVE MESH ATTACH STAGE: %s HWND=%p", stage, hwnd);
        result = g_nativeMeshInsertRelative(collection, pending.visual,
                                            root, true, false);
    }
    bool inserted = pending.visual && ReadPointerMember(pending.visual, 0x18);
    if (result >= 0 && inserted)
    {
        stage = L"VerifySibling";
        if (ReadPointerMember(pending.visual, 0x18) !=
                ReadPointerMember(root, 0x18) ||
            FindNativeMeshSiblingCollection(root) != collection)
            result = E_UNEXPECTED;
    }
    if (result >= 0 && inserted)
    {
        stage = L"BindTransform";
        Wh_Log(L"NATIVE MESH ATTACH STAGE: %s HWND=%p", stage, hwnd);
        void* visualProxy = g_nativeProbeGetVisualProxy(pending.visual);
        result = IsVisualProxyPointerValid(visualProxy)
                     ? g_cVisualProxySetTransform(visualProxy, pending.transform)
                     : E_NOINTERFACE;
    }
    if (result >= 0 && inserted)
    {
        pending.image = image;
        pending.group = group;
        pending.mesh = mesh;
        pending.instruction = instruction;
        image = group = mesh = instruction = nullptr;
        g_nativeMeshState = pending;
        g_nativeMeshHwnd.store(hwnd, std::memory_order_release);
        g_nativeMeshActive.store(true, std::memory_order_release);
        InvalidateAnimationTransformForNativeMesh(hwnd);
        Wh_Log(L"NATIVE MESH ATTACHED: HWND=%p root=%p "
               L"visual=%p duration=until-slot-retirement snapshot=none",
               hwnd, root, pending.visual);
        RequestDwmScenePass();
        return true;
    }
    Wh_Log(L"NATIVE MESH ATTACH FAILED: HWND=%p stage=%s "
           L"result=0x%08X inserted=%d", hwnd, stage,
           static_cast<unsigned>(result), inserted);
    if (inserted)
    {
        pending.image = image;
        pending.group = group;
        pending.mesh = mesh;
        pending.instruction = instruction;
        image = group = mesh = instruction = nullptr;
        g_nativeMeshState = pending;
        g_nativeMeshHwnd.store(hwnd, std::memory_order_release);
        g_nativeMeshActive.store(true, std::memory_order_release);
        DetachNativeMesh(L"failed-attach");
    }
    else
    {
        ReleaseNativeMeshState(pending);
    }
    return false;
}

static void CreateVisibleNativeMesh(HWND hwnd, void* topLevelWindow,
                                      void* sourceProxy, void* compositor,
                                      void*& image, int width, int height)
{
    // Exact 23H2 ABI only. The instruction stores base-object references and
    // serializes resource IDs; the core DrawMesh2D path consumes CImageSource.
    if (!IsOnDwmSceneThread() ||
        g_dwmModuleLayout.timeDateStamp != 0xE19C3234 ||
        g_dwmModuleLayout.sizeOfImage != 0x16A000 ||
        !g_nativeProbeCreateMesh || !g_nativeProbeUpdateMesh ||
        !g_nativeProbeCreateGroup || !g_nativeProbeUpdateGroup ||
        !g_nativeProbeCreateMeshInstruction || !g_cBaseObjectRelease ||
        !image || width < 160 || height < 100)
    {
        Wh_Log(L"NATIVE VERTEX MESH: skipped HWND=%p", hwnd);
        return;
    }

    NativeProbePoint3F positions[NATIVE_MESH_VERTEX_COUNT] = {};
    NativeProbePoint2D coordinates[NATIVE_MESH_VERTEX_COUNT] = {};
    unsigned int indices[NATIVE_MESH_INDEX_COUNT] = {};
    for (unsigned int y = 0; y < NATIVE_MESH_SIDE; ++y)
    {
        for (unsigned int x = 0; x < NATIVE_MESH_SIDE; ++x)
        {
            const unsigned int index = y * NATIVE_MESH_SIDE + x;
            const double px = static_cast<double>(width) * x / NATIVE_MESH_CELLS;
            const double py = static_cast<double>(height) * y / NATIVE_MESH_CELLS;
            positions[index] = {static_cast<float>(px),
                                static_cast<float>(py), 0.0f};
            coordinates[index] = {px, py};
        }
    }
    BuildNativeMeshIndices(indices);

    void* mesh = nullptr;
    void* group = nullptr;
    void* instruction = nullptr;
    const wchar_t* stage = L"CreateMesh";
    Wh_Log(L"NATIVE VERTEX MESH STAGE: %s HWND=%p", stage, hwnd);
    long result = g_nativeProbeCreateMesh(compositor, &mesh);
    if (result >= 0 && !mesh) result = E_UNEXPECTED;
    if (result >= 0)
    {
        stage = L"UpdateMesh";
        Wh_Log(L"NATIVE VERTEX MESH STAGE: %s HWND=%p", stage, hwnd);
        result = g_nativeProbeUpdateMesh(mesh, 0, positions, coordinates,
                                         NATIVE_MESH_VERTEX_COUNT, indices,
                                         NATIVE_MESH_INDEX_COUNT);
    }
    if (result >= 0)
    {
        stage = L"CreateGroup";
        Wh_Log(L"NATIVE VERTEX MESH STAGE: %s HWND=%p", stage, hwnd);
        result = g_nativeProbeCreateGroup(compositor, &group);
        if (result >= 0 && !group) result = E_UNEXPECTED;
    }
    if (result >= 0)
    {
        stage = L"UpdateGroup";
        Wh_Log(L"NATIVE VERTEX MESH STAGE: %s HWND=%p", stage, hwnd);
        result = g_nativeProbeUpdateGroup(group, mesh);
    }
    if (result >= 0)
    {
        stage = L"CreateInstruction";
        Wh_Log(L"NATIVE VERTEX MESH STAGE: %s HWND=%p", stage, hwnd);
        result = g_nativeProbeCreateMeshInstruction(group, image, &instruction);
        if (result >= 0 &&
            (!instruction || ReadPointerMember(instruction, 0x10) != group ||
             ReadPointerMember(instruction, 0x18) != image))
            result = E_UNEXPECTED;
    }
    Wh_Log(L"NATIVE VERTEX MESH: HWND=%p stage=%s result=0x%08X "
           L"vertices=%u indices=%u instruction=%p attached=0",
           hwnd, stage, static_cast<unsigned>(result),
           NATIVE_MESH_VERTEX_COUNT, NATIVE_MESH_INDEX_COUNT, instruction);
    if (result >= 0)
    {
        if (TryAttachNativeMesh(hwnd, topLevelWindow, sourceProxy,
                                     compositor, image, group, instruction,
                                     mesh, width, height))
        {
            Wh_Log(L"NATIVE VERTEX MESH: attached HWND=%p "
                   L"physicsLinked=1 liveSource=1 original=translation", hwnd);
        }
    }
    if (instruction) g_cBaseObjectRelease(instruction);
    if (group) g_cBaseObjectRelease(group);
    if (mesh) g_cBaseObjectRelease(mesh);
}

static void CreateNativeMeshSource(HWND hwnd, void* topLevelWindow,
                                 void* sourceVisualProxy)
{
    if (!IsOnDwmSceneThread() || !IsVisualProxyPointerValid(sourceVisualProxy) ||
        g_nativeMeshActive.load(std::memory_order_acquire))
    {
        return;
    }
    AcquireSRWLockShared(&g_settingsLock);
    bool meshSelected = g_settings.rendererMode == RendererMode::Mesh;
    ReleaseSRWLockShared(&g_settingsLock);
    if (!meshSelected || !NativeMeshAbiVerified() ||
        g_dwmModuleLayout.timeDateStamp != 0xE19C3234 ||
        g_dwmModuleLayout.sizeOfImage != 0x16A000)
        return;
    bool meshSlotActive = false;
    AcquireSRWLockShared(&g_animationSlotsLock);
    for (const WindowAnimationSlot& slot : g_animationSlots)
    {
        if (slot.active && slot.hwnd == hwnd &&
            slot.settings.rendererMode == RendererMode::Mesh)
        {
            meshSlotActive = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_animationSlotsLock);
    if (!meshSlotActive)
        return;
    ULONGLONG now = GetTickCount64();
    if (g_nativeMeshLastAttemptHwnd == hwnd &&
        now - g_nativeMeshLastAttemptAt < 250)
        return;
    g_nativeMeshLastAttemptHwnd = hwnd;
    g_nativeMeshLastAttemptAt = now;
    void* compositor = g_dwmCompositor.load(std::memory_order_acquire);
    void* expectedVtable =
        g_nativeProbeImageVtable.load(std::memory_order_acquire);
    if (!IsDwmObjectPointerValid(compositor, g_compositorVtable) ||
        !expectedVtable || !g_cBaseObjectRelease || !g_nativeProbeCreateImage ||
        !g_nativeProbeCreateBitmapInstruction ||
        (g_nativeProbeUpdateImageD != nullptr) ==
            (g_nativeProbeUpdateImageF != nullptr))
    {
        return;
    }

    int width = 0;
    int height = 0;
    if (!GetNativeMeshWindowSize(hwnd, width, height, true))
    {
        return;
    }
    void* image = nullptr;
    void* validationInstruction = nullptr;
    long createResult = g_nativeProbeCreateImage(compositor, &image);
    long updateResult = E_NOTIMPL;
    long validationResult = E_NOTIMPL;
    bool exactImageType = image && IsReadableMemory(image, sizeof(void*)) &&
                          *reinterpret_cast<void**>(image) == expectedVtable;
    if (createResult >= 0 && exactImageType)
    {
        NativeProbeRectF rect = {0, 0, static_cast<float>(width),
                                 static_cast<float>(height)};
        constexpr int absoluteMappingMode = 0;
        if (g_nativeProbeUpdateImageD)
        {
            NativeProbeSizeD size = {static_cast<double>(width),
                                     static_cast<double>(height)};
            updateResult = g_nativeProbeUpdateImageD(
                image, rect, size, nullptr, nullptr, sourceVisualProxy,
                absoluteMappingMode);
        }
        else
        {
            NativeProbeSizeF size = {static_cast<float>(width),
                                     static_cast<float>(height)};
            updateResult = g_nativeProbeUpdateImageF(
                image, rect, size, nullptr, nullptr, sourceVisualProxy,
                absoluteMappingMode);
        }
        if (updateResult >= 0)
            validationResult = g_nativeProbeCreateBitmapInstruction(
                image, &validationInstruction);
    }

    // The exact uDWM build was checked before entering this path.
    // Its image resource is at +0x10 and the resource ID at +0x18.
    unsigned int imageResourceId = 0;
    bool resourceReadable = false;
    if (updateResult >= 0 && exactImageType)
    {
        void* resource = ReadPointerMember(image, 0x10);
        const BYTE* id = resource ? static_cast<const BYTE*>(resource) + 0x18
                                  : nullptr;
        if (id && IsReadableMemory(id, sizeof(imageResourceId)))
        {
            memcpy(&imageResourceId, id, sizeof(imageResourceId));
            resourceReadable = true;
        }
    }
    bool instructionKeepsImage = validationInstruction &&
        ReadPointerMember(validationInstruction, 0x10) == image;
    Wh_Log(L"NATIVE MESH SOURCE: HWND=%p create=0x%08X update=0x%08X "
           L"validation=0x%08X exactType=%d resourceReadable=%d "
           L"resourceId=%u retain=%d",
           hwnd, static_cast<unsigned>(createResult),
           static_cast<unsigned>(updateResult),
           static_cast<unsigned>(validationResult), exactImageType,
           resourceReadable, imageResourceId, instructionKeepsImage);
    if (resourceReadable && imageResourceId && validationResult >= 0 &&
        instructionKeepsImage)
    {
        CreateVisibleNativeMesh(hwnd, topLevelWindow, sourceVisualProxy,
                                  compositor, image, width, height);
    }
    if (validationInstruction && validationResult >= 0 &&
        IsReadableMemory(validationInstruction, sizeof(void*)) &&
        IsDwmImageAddress(*reinterpret_cast<void**>(validationInstruction),
                          sizeof(void*)))
    {
        g_cBaseObjectRelease(validationInstruction);
    }
    if (image && createResult >= 0 && exactImageType)
    {
        g_cBaseObjectRelease(image);
    }
}

static void BindPendingAnimationSlotTransforms(bool validateCurrentVisuals)
{
    if (!IsOnDwmSceneThread() || g_unloading.load(std::memory_order_acquire) ||
        !HasAnyAnimationSlots())
    {
        return;
    }
    void* windowList = g_windowListForSceneWake.load(std::memory_order_acquire);
    ULONGLONG now = GetTickCount64();
    bool validWindowList = IsDwmObjectPointerValid(windowList, g_windowListVtable);
    if (!validWindowList || !g_findWindowDataByHwnd || !g_cVisualProxySetTransform)
    {
        ULONGLONG previous = g_lastBindPrerequisiteLog.load(std::memory_order_acquire);
        if (HasAnyAnimationSlots() && (previous == 0 || now - previous >= 2000) &&
            g_lastBindPrerequisiteLog.compare_exchange_strong(
                previous, now, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            Wh_Log(L"BIND PIPELINE UNAVAILABLE: WindowList=%p Valid=%d "
                   L"FindWindowData=%p SetTransform=%p",
                   windowList, validWindowList, g_findWindowDataByHwnd,
                   g_cVisualProxySetTransform);
        }
        return;
    }
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        HWND hwnd = nullptr;
        ULONGLONG generation = 0;
        ULONGLONG rebindRevision = 0;
        void* matrixTransformProxy = nullptr;
        void* previouslyBoundTopLevelVisualProxy = nullptr;
        void* previouslyBoundTransitionVisualProxy = nullptr;
        bool previouslyTopLevelAttached = false;
        bool previouslyTransitionAttached = false;
        bool windowStateThrob = false;
        bool bindingPending = false;
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        WindowAnimationSlot& slot = g_animationSlots[i];
        bindingPending = !slot.transformAttached ||
                         (slot.windowStateThrob && !slot.transitionTransformAttached) ||
                         slot.transformRebindRevision != slot.submittedTransformRebindRevision;
        bool periodicValidation = validateCurrentVisuals && now >= slot.nextVisualValidation;
        bool forceNativeTransitionRebind = validateCurrentVisuals && slot.windowStateThrob;
        if (slot.active && slot.hwnd && slot.matrixTransformProxy &&
            (bindingPending || periodicValidation || forceNativeTransitionRebind))
        {
            slot.hookUsers++;
            slot.nextVisualValidation = now + 250;
            hwnd = slot.hwnd;
            generation = slot.generation;
            rebindRevision = slot.transformRebindRevision;
            matrixTransformProxy = slot.matrixTransformProxy;
            previouslyBoundTopLevelVisualProxy = slot.boundTopLevelVisualProxy;
            previouslyBoundTransitionVisualProxy = slot.boundTransitionVisualProxy;
            previouslyTopLevelAttached = slot.transformAttached;
            previouslyTransitionAttached = slot.transitionTransformAttached;
            windowStateThrob = slot.windowStateThrob;
        }
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (!matrixTransformProxy)
        {
            continue;
        }
        void* windowData = FindWindowDataByHwnd(windowList, hwnd);
        void* topLevelWindow = nullptr;
        void* topLevelWindow3D = nullptr;
        void* topLevelVisualProxy = nullptr;
        void* transitionVisualProxy = nullptr;
        const wchar_t* visualSource = L"None";
        const wchar_t* bindFailureStage = nullptr;
        HWND mappedHwnd = windowData ? GetHwndFromWindowData(windowData) : nullptr;
        if (!windowData)
        {
            bindFailureStage = L"WindowData";
        }
        else if (mappedHwnd != hwnd)
        {
            bindFailureStage = L"HwndMismatch";
        }
        else
        {
            // Lazy initialization is needed for already-open/desktop-restored
            // windows, but never force a hidden, minimized or cloaked visual.
            if (!ResolveDwmWindowObjects(windowData, &topLevelWindow, &topLevelWindow3D) &&
                CanInitializeMissingWindowVisual(hwnd) &&
                g_ensureTopLevelWindowOriginal(windowList, windowData) >= 0)
            {
                ResolveDwmWindowObjects(windowData, &topLevelWindow, &topLevelWindow3D);
            }
            if (topLevelWindow)
            {
                topLevelVisualProxy = GetTopLevelVisualProxy(topLevelWindow,
                                                             &visualSource);
                if (!topLevelVisualProxy)
                {
                    bindFailureStage = L"VisualProxy";
                }
            }
            else
            {
                bindFailureStage = L"TopLevelWindow";
            }
            if (windowStateThrob && topLevelWindow3D)
            {
                transitionVisualProxy = GetTransitionVisualProxy(topLevelWindow3D);
            }
        }
        if (topLevelVisualProxy &&
            !g_nativeMeshActive.load(std::memory_order_acquire))
        {
            CreateNativeMeshSource(hwnd, topLevelWindow, topLevelVisualProxy);
        }
        bool topLevelBindingAttempted =
            topLevelVisualProxy &&
            (bindingPending || topLevelVisualProxy != previouslyBoundTopLevelVisualProxy ||
             forceNativeTransitionRebind);
        bool transitionBindingAttempted =
            transitionVisualProxy && transitionVisualProxy != topLevelVisualProxy &&
            (bindingPending ||
             transitionVisualProxy != previouslyBoundTransitionVisualProxy ||
             forceNativeTransitionRebind);
        long topLevelBindResult = E_FAIL;
        long transitionBindResult = E_FAIL;
        if (topLevelBindingAttempted)
        {
            topLevelBindResult =
                g_cVisualProxySetTransform(topLevelVisualProxy, matrixTransformProxy);
            if (topLevelBindResult < 0)
            {
                bindFailureStage = L"SetTransform";
            }
        }
        if (transitionBindingAttempted)
        {
            transitionBindResult =
                g_cVisualProxySetTransform(transitionVisualProxy, matrixTransformProxy);
        }
        bool logBinding = false;
        bool logBindingFailure = false;
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        WindowAnimationSlot& currentSlot = g_animationSlots[i];
        if (currentSlot.active && currentSlot.generation == generation &&
            currentSlot.matrixTransformProxy == matrixTransformProxy)
        {
            if (!topLevelVisualProxy)
            {
                currentSlot.transformAttached = false;
                currentSlot.boundTopLevelVisualProxy = nullptr;
            }
            else if (topLevelBindingAttempted && topLevelBindResult >= 0)
            {
                currentSlot.transformAttached = true;
                currentSlot.boundTopLevelVisualProxy = topLevelVisualProxy;
                currentSlot.submittedTransformRebindRevision = rebindRevision;
                logBinding = !previouslyTopLevelAttached ||
                             previouslyBoundTopLevelVisualProxy != topLevelVisualProxy;
            }
            else if (topLevelBindingAttempted)
            {
                currentSlot.transformAttached = false;
                currentSlot.boundTopLevelVisualProxy = nullptr;
            }
            if (bindingPending && !currentSlot.transformAttached && bindFailureStage &&
                (currentSlot.lastBindFailureLog == 0 ||
                 now - currentSlot.lastBindFailureLog >= 1000))
            {
                currentSlot.lastBindFailureLog = now;
                logBindingFailure = true;
            }
            if (!windowStateThrob || !transitionVisualProxy ||
                transitionVisualProxy == topLevelVisualProxy)
            {
                currentSlot.transitionTransformAttached =
                    transitionVisualProxy && transitionVisualProxy == topLevelVisualProxy &&
                    currentSlot.transformAttached;
                currentSlot.boundTransitionVisualProxy =
                    currentSlot.transitionTransformAttached ? transitionVisualProxy : nullptr;
            }
            else if (transitionBindingAttempted && transitionBindResult >= 0)
            {
                currentSlot.transitionTransformAttached = true;
                currentSlot.boundTransitionVisualProxy = transitionVisualProxy;
                logBinding = logBinding || !previouslyTransitionAttached ||
                             previouslyBoundTransitionVisualProxy != transitionVisualProxy;
            }
            else if (transitionBindingAttempted)
            {
                currentSlot.transitionTransformAttached = false;
                currentSlot.boundTransitionVisualProxy = nullptr;
            }
        }
        ReleaseAnimationSlotPinLocked(currentSlot);
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (logBinding)
        {
            Wh_Log(L"TRANSFORM BOUND FROM SCENE: Slot=%d HWND=%p "
                   L"CWindowData=%p CTopLevelWindow=%p "
                   L"VisualProxy=%p VisualSource=%s TransitionProxy=%p MatrixProxy=%p",
                   i, hwnd, windowData, topLevelWindow, topLevelVisualProxy,
                   visualSource, transitionVisualProxy, matrixTransformProxy);
        }
        if (logBindingFailure)
        {
            Wh_Log(L"BIND FAILED: Stage=%s Slot=%d HWND=%p WindowList=%p "
                   L"WindowData=%p MappedHWND=%p CTopLevelWindow=%p "
                   L"VisualProxy=%p MatrixProxy=%p SetTransformAttempted=%d "
                   L"SetTransform=0x%08X",
                   bindFailureStage, i, hwnd, windowList, windowData, mappedHwnd,
                   topLevelWindow, topLevelVisualProxy, matrixTransformProxy,
                   topLevelBindingAttempted,
                   static_cast<unsigned int>(topLevelBindResult));
        }
    }
}

static void BackfillExistingDwmWindowMappings()
{
    if (!IsOnDwmSceneThread())
    {
        return;
    }
    AcquireSRWLockShared(&g_existingWindowBackfillLock);
    void* windowList = g_windowListForSceneWake.load(std::memory_order_acquire);
    unsigned int count = g_existingWindowBackfillCount.load(std::memory_order_acquire);
    unsigned int index = g_existingWindowBackfillIndex.load(std::memory_order_acquire);
    if (!IsDwmObjectPointerValid(windowList, g_windowListVtable) ||
        !g_findWindowDataByHwnd || index >= count)
    {
        ReleaseSRWLockShared(&g_existingWindowBackfillLock);
        return;
    }
    constexpr unsigned int windowsPerPass = 16;
    unsigned int processed = 0;
    while (index < count && processed++ < windowsPerPass)
    {
        HWND hwnd = g_existingWindowBackfill[index++];
        // Recheck: desktop/visibility state can change after enumeration.
        if (!CanInitializeMissingWindowVisual(hwnd))
        {
            continue;
        }
        void* windowData = FindWindowDataByHwnd(windowList, hwnd);
        if (!windowData || GetHwndFromWindowData(windowData) != hwnd)
        {
            continue;
        }
        void* topLevelWindow = nullptr;
        void* topLevelWindow3D = nullptr;
        bool mapped = ResolveDwmWindowObjects(windowData, &topLevelWindow, &topLevelWindow3D);
        // As in binding, initialize only verified window data on its scene owner.
        if (!mapped && g_ensureTopLevelWindowOriginal(windowList, windowData) >= 0)
        {
            mapped = ResolveDwmWindowObjects(windowData, &topLevelWindow, &topLevelWindow3D);
        }
        if (mapped)
        {
            g_existingWindowBackfillMapped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_existingWindowBackfillIndex.store(index, std::memory_order_release);
    unsigned int mapped = g_existingWindowBackfillMapped.load(std::memory_order_relaxed);
    ReleaseSRWLockShared(&g_existingWindowBackfillLock);
    if (index < count)
    {
        RequestDwmScenePass();
    }
    else
    {
        Wh_Log(L"DWM existing-window backfill completed: %u/%u windows mapped", mapped, count);
    }
}

static void SubmitPendingWobblySceneWork()
{
    if (!IsOnDwmSceneThread())
    {
        return;
    }
    // A newer serial published during this pass receives another wake below.
    ULONGLONG submittedThrough = g_sceneRequestedSerial.load(std::memory_order_acquire);
    g_sceneWakeScheduled.store(false, std::memory_order_release);
    g_sceneWakePostTimestamp.store(0, std::memory_order_release);
    g_scenePassCounter.fetch_add(1, std::memory_order_release);
    MaintainNativeMesh();
    // Restore retiring/quiet windows before discovery, creation or normal rendering.
    RestorePendingAnimationIdentities();
    if (!g_unloading.load(std::memory_order_acquire))
    {
        BackfillExistingDwmWindowMappings();
        EnsurePendingMatrixTransformProxies();
        for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
        {
            ApplyAnimationSlotTransform(i);
        }
    }
    FinalizeRetiringSlots();
    g_sceneSubmittedSerial.store(submittedThrough, std::memory_order_release);
    ULONGLONG stallStartedAt = g_sceneWakeStallStartedAt.exchange(0, std::memory_order_acq_rel);
    if (g_sceneWakeStalled.exchange(false, std::memory_order_acq_rel))
    {
        Wh_Log(L"DWM scene wake recovered after %llu ms",
               stallStartedAt ? GetTickCount64() - stallStartedAt : 0);
    }
    // Wake again if the worker published a newer matrix.
    if (g_sceneRequestedSerial.load(std::memory_order_acquire) > submittedThrough)
    {
        PostPendingDwmSceneWake(g_unloading.load(std::memory_order_acquire));
    }
}

static bool HasPendingWobblySceneWork()
{
    return g_nativeMeshActive.load(std::memory_order_acquire) ||
           g_sceneRequestedSerial.load(std::memory_order_acquire) >
               g_sceneSubmittedSerial.load(std::memory_order_acquire) ||
           g_existingWindowBackfillIndex.load(std::memory_order_acquire) <
               g_existingWindowBackfillCount.load(std::memory_order_acquire) ||
           HasAnyAnimationSlots();
}

static void BindAnimationTransformsAfterNativeScene()
{
    // Check every active slot, but write only when the proxy changed or the
    // binding is pending. Native state transitions are the only forced case.
    BindPendingAnimationSlotTransforms(true);
}

static long __cdecl ForceUpdateSceneHook(void* pThis)
{
    if (HasExactDwmVtableTrusted(pThis, g_windowListVtable))
    {
        g_windowListForSceneWake.store(pThis, std::memory_order_release);
    }
    if (!HasPendingWobblySceneWork())
    {
        return g_windowListForceUpdateSceneOriginal(pThis);
    }
    bool canSubmit = RegisterDwmSceneThread(SceneThreadRegistration::ExistingOwner);
    bool outermostPass = canSubmit && !g_insideWobblyScenePass;
    if (outermostPass)
    {
        g_sceneWakeAwaitingNativeTimeline.store(false, std::memory_order_release);
        g_insideWobblyScenePass = true;
        SubmitPendingWobblySceneWork();
    }
    long result = g_windowListForceUpdateSceneOriginal(pThis);
    if (outermostPass)
    {
        BindAnimationTransformsAfterNativeScene();
        g_insideWobblyScenePass = false;
    }
    return result;
}

static long __cdecl UpdateSceneHook(void* pThis)
{
    if (HasExactDwmVtableTrusted(pThis, g_windowListVtable))
    {
        g_windowListForSceneWake.store(pThis, std::memory_order_release);
    }
    if (!HasPendingWobblySceneWork())
    {
        return g_windowListUpdateSceneOriginal(pThis);
    }
    bool canSubmit = RegisterDwmSceneThread(SceneThreadRegistration::ExistingOwner);
    bool outermostPass = canSubmit && !g_insideWobblyScenePass;
    if (outermostPass)
    {
        g_sceneWakeAwaitingNativeTimeline.store(false, std::memory_order_release);
        g_insideWobblyScenePass = true;
        SubmitPendingWobblySceneWork();
    }
    long result = g_windowListUpdateSceneOriginal(pThis);
    if (outermostPass)
    {
        BindAnimationTransformsAfterNativeScene();
        g_insideWobblyScenePass = false;
    }
    return result;
}

static void __cdecl AdvanceTimelinesHook(void* pThis, double currentTime)
{
    bool canSubmit =
        RegisterDwmSceneThread(SceneThreadRegistration::AuthoritativeTimeline);
    if (canSubmit)
    {
        g_lastNativeTimelineTimestamp.store(GetTickCount64(), std::memory_order_release);
        if (g_sceneWakeAwaitingNativeTimeline.exchange(false,
                                                        std::memory_order_acq_rel))
        {
            g_sceneWakeScheduled.store(false, std::memory_order_release);
            g_sceneWakePostTimestamp.store(0, std::memory_order_release);
        }
        if (CacheDwmObjectsFromDesktopManager(pThis))
        {
            g_dwmObjectDiscoveryFailureCount.store(0, std::memory_order_release);
        }
        else
        {
            unsigned int failures =
                g_dwmObjectDiscoveryFailureCount.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (failures == 1 || failures % 600 == 0)
            {
                Wh_Log(L"DWM compositor temporarily unavailable; retrying (%u)", failures);
            }
            canSubmit = false;
        }
    }
    // Serial comparison prevents coalesced scene passes from stranding work.
    if (g_sceneRequestedSerial.load(std::memory_order_acquire) >
            g_sceneSubmittedSerial.load(std::memory_order_acquire) &&
        canSubmit && !g_insideWobblyScenePass)
    {
        g_insideWobblyScenePass = true;
        SubmitPendingWobblySceneWork();
        g_insideWobblyScenePass = false;
    }
    g_desktopManagerAdvanceTimelinesOriginal(pThis, currentTime);
    if (canSubmit && !g_insideWobblyScenePass)
    {
        g_insideWobblyScenePass = true;
        BindAnimationTransformsAfterNativeScene();
        g_insideWobblyScenePass = false;
    }
}

static void __cdecl DesktopManagerHandleThreadMessageHook(UINT message,
                                                          UINT_PTR wParam,
                                                          INT_PTR lParam)
{
    if (message != g_dwmSceneWakeMessage || wParam != DWM_SCENE_WAKE_WPARAM)
    {
        g_desktopManagerHandleThreadMessageOriginal(message, wParam, lParam);
        return;
    }
    if (lParam != g_dwmSceneWakeToken.load(std::memory_order_acquire))
    {
        // Swallow a wake left by an older instance of the mod.
        return;
    }
    auto acknowledgeWake = []
    {
        unsigned int outstanding = g_sceneWakeOutstanding.load(std::memory_order_acquire);
        while (outstanding &&
               !g_sceneWakeOutstanding.compare_exchange_weak(
                   outstanding, outstanding - 1, std::memory_order_acq_rel,
                   std::memory_order_acquire))
        {
        }
    };
    if (g_unloading.load(std::memory_order_acquire) &&
        !HasPendingWobblySceneWork())
    {
        // Cleanup can finish through a native scene pass before this wake arrives.
        acknowledgeWake();
        return;
    }
    bool bootstrapWake =
        g_dwmSceneThreadId.load(std::memory_order_acquire) == 0;
    if (!RegisterDwmSceneThread(SceneThreadRegistration::WakeBootstrap) ||
        g_insideWobblyScenePass)
    {
        g_sceneWakeScheduled.store(false, std::memory_order_release);
        g_sceneWakePostTimestamp.store(0, std::memory_order_release);
        acknowledgeWake();
        return;
    }
    ULONGLONG now = GetTickCount64();
    ULONGLONG postedAt =
        g_sceneWakePostTimestamp.load(std::memory_order_acquire);
    if (!bootstrapWake && postedAt &&
        now - postedAt > DWM_SCENE_WAKE_FRESHNESS_MS)
    {
        // Never drive private scene methods from a wake after display sleep or
        // compositor teardown. The next native scene callback safely consumes
        // the still-pending serial and revalidates cached DWM objects.
        if (!g_sceneWakeAwaitingNativeTimeline.exchange(true,
                                                         std::memory_order_acq_rel))
        {
            Wh_Log(L"DWM scene wake deferred until native scene activity resumes");
        }
        acknowledgeWake();
        return;
    }
    g_insideWobblyScenePass = true;
    SubmitPendingWobblySceneWork();
    BindPendingAnimationSlotTransforms(false);

    void* windowList = g_windowListForSceneWake.load(std::memory_order_acquire);
    bool nativeSceneUpdated = false;
    if (IsDwmObjectPointerValid(windowList, g_windowListVtable))
    {
        if (g_windowListForceUpdateSceneOriginal)
        {
            g_windowListForceUpdateSceneOriginal(windowList);
            nativeSceneUpdated = true;
        }
        else if (g_windowListUpdateSceneOriginal)
        {
            g_windowListUpdateSceneOriginal(windowList);
            nativeSceneUpdated = true;
        }
    }
    else
    {
        // Startup may not have a WindowList yet; wake native animation discovery.
        void* manager = g_desktopManager.load(std::memory_order_acquire);
        if (HasExactDwmVtableTrusted(manager, g_desktopManagerVtable) &&
            g_desktopManagerPostStartAnimations)
        {
            g_desktopManagerPostStartAnimations(manager);
        }
    }
    if (nativeSceneUpdated)
    {
        // uDWM can replace the root transform while maximizing or restoring.
        BindPendingAnimationSlotTransforms(true);
    }
    g_insideWobblyScenePass = false;
    acknowledgeWake();
}

static double SmoothSymmetricCompression(double value, double referenceRange)
{
    if (!std::isfinite(value) || referenceRange <= 0.0)
    {
        return 0.0;
    }
    double magnitude = std::abs(value);
    double knee = referenceRange * 0.75;
    if (magnitude <= knee)
    {
        return value;
    }
    double transitionRange = referenceRange - knee;
    // Monotonic compression avoids sticking at extreme deformation.
    double softenedMagnitude =
        knee + transitionRange * std::asinh((magnitude - knee) / transitionRange);
    return std::copysign(softenedMagnitude, value);
}

static double SmoothCenteredCompression(double value, double center, double radius)
{
    return center + SmoothSymmetricCompression(value - center, radius);
}

static void CalculateBernsteinBasis(double value, double basis[4])
{
    double inverse = 1.0 - value;
    basis[0] = inverse * inverse * inverse;
    basis[1] = 3.0 * inverse * inverse * value;
    basis[2] = 3.0 * inverse * value * value;
    basis[3] = value * value * value;
}

static int GetPointIndex(int x, int y)
{
    return y * GRID_WIDTH + x;
}

static void InitializeMesh(WobbleMesh& mesh, double width, double height)
{
    mesh.width = width;
    mesh.height = height;
    mesh.active = false;
    mesh.dragging = false;
    mesh.resizing = false;
    mesh.canWobbleTop = true;
    mesh.canWobbleLeft = true;
    mesh.canWobbleRight = true;
    mesh.canWobbleBottom = true;
    mesh.dragPointIndex = -1;
    mesh.dragOffset = {0.0, 0.0};
    for (int y = 0; y < GRID_HEIGHT; y++)
    {
        for (int x = 0; x < GRID_WIDTH; x++)
        {
            double normalizedX = static_cast<double>(x) / static_cast<double>(GRID_WIDTH - 1);
            double normalizedY = static_cast<double>(y) / static_cast<double>(GRID_HEIGHT - 1);
            WobblePoint& point = mesh.points[GetPointIndex(x, y)];
            point.basePosition = {normalizedX * width, normalizedY * height};
            point.position = point.basePosition;
            point.velocity = {0.0, 0.0};
            point.force = {0.0, 0.0};
            point.fixed = false;
        }
    }
}

static void UpdateMeshBaseGrid(WobbleMesh& mesh, double width, double height)
{
    mesh.width = width;
    mesh.height = height;
    for (int y = 0; y < GRID_HEIGHT; y++)
    {
        for (int x = 0; x < GRID_WIDTH; x++)
        {
            double normalizedX = static_cast<double>(x) / static_cast<double>(GRID_WIDTH - 1);
            double normalizedY = static_cast<double>(y) / static_cast<double>(GRID_HEIGHT - 1);
            mesh.points[GetPointIndex(x, y)].basePosition = {normalizedX * width,
                                                             normalizedY * height};
        }
    }
}

static void ResizeMeshPreservingDeformation(WobbleMesh& mesh, double width, double height)
{
    Vec2 displacement[GRID_POINT_COUNT] = {};
    for (int i = 0; i < GRID_POINT_COUNT; i++)
    {
        displacement[i] = {mesh.points[i].position.x - mesh.points[i].basePosition.x,
                           mesh.points[i].position.y - mesh.points[i].basePosition.y};
    }
    UpdateMeshBaseGrid(mesh, width, height);
    for (int i = 0; i < GRID_POINT_COUNT; i++)
    {
        mesh.points[i].position.x += displacement[i].x;
        mesh.points[i].position.y += displacement[i].y;
    }
}

static void UpdateMeshDragOffset(WobbleMesh& mesh, const Vec2& mousePosition)
{
    if (mesh.dragPointIndex >= 0 && mesh.dragPointIndex < GRID_POINT_COUNT)
    {
        const Vec2& basePosition = mesh.points[mesh.dragPointIndex].basePosition;
        mesh.dragOffset = {mousePosition.x - basePosition.x,
                           mousePosition.y - basePosition.y};
    }
}

static void OffsetMeshPositions(WobbleMesh& mesh, double x, double y)
{
    for (WobblePoint& point : mesh.points)
    {
        point.position.x += x;
        point.position.y += y;
    }
}

static void SetMeshResizeMode(WobbleMesh& mesh, bool resizing)
{
    mesh.resizing = resizing;
    // Lock all resize edges; UpdateResizeEdges unlocks the moving ones.
    mesh.canWobbleTop = !resizing;
    mesh.canWobbleLeft = !resizing;
    mesh.canWobbleRight = !resizing;
    mesh.canWobbleBottom = !resizing;
}

static void UpdateResizeEdges(WobbleMesh& mesh, const RECT& originalRect, const RECT& currentRect)
{
    if (!mesh.resizing)
    {
        return;
    }
    if (currentRect.top != originalRect.top)
    {
        mesh.canWobbleTop = true;
    }
    if (currentRect.left != originalRect.left)
    {
        mesh.canWobbleLeft = true;
    }
    if (currentRect.right != originalRect.right)
    {
        mesh.canWobbleRight = true;
    }
    if (currentRect.bottom != originalRect.bottom)
    {
        mesh.canWobbleBottom = true;
    }
}

static void ApplyResizeConstraints(WobbleMesh& mesh)
{
    if (!mesh.resizing)
    {
        return;
    }
    for (int y = 0; y < GRID_HEIGHT; y++)
    {
        for (int x = 0; x < GRID_WIDTH; x++)
        {
            WobblePoint& point = mesh.points[GetPointIndex(x, y)];
            // Match KWin's edge-locking rules.
            if ((!mesh.canWobbleLeft && x < GRID_WIDTH - 1) || (!mesh.canWobbleRight && x > 0))
            {
                point.position.x = point.basePosition.x;
            }
            if ((!mesh.canWobbleTop && y < GRID_HEIGHT - 1) || (!mesh.canWobbleBottom && y > 0))
            {
                point.position.y = point.basePosition.y;
            }
        }
    }
}

static void ApplyWindowStateThrob(WobbleMesh& mesh, bool maximizing, bool seedInitialDisplacement,
                                  const WobblySettings& settings, Vec2 direction)
{
    double directionLength = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (directionLength < 0.001)
    {
        // Maximize rises towards the monitor edge; restore falls away from it.
        direction = maximizing ? Vec2{0.0, -1.0} : Vec2{0.0, 1.0};
        directionLength = 1.0;
    }
    direction.x /= directionLength;
    direction.y /= directionLength;
    double dragRatio = std::clamp(settings.drag / 100.0, 0.01, MAX_VELOCITY_RETENTION);
    double moveRatio = std::clamp(settings.moveFactor / 100.0, 0.01, 0.25);
    double stiffnessProgress = std::clamp((15.0 - settings.stiffness) / 14.0, 0.0, 1.0);
    double dragProgress = std::clamp((settings.drag - 80.0) / 17.0, 0.0, 1.0);
    double moveProgress = std::clamp((settings.moveFactor - 10.0) / 15.0, 0.0, 1.0);
    double wobbleProgress = 0.30 * stiffnessProgress + 0.45 * dragProgress + 0.25 * moveProgress;
    // Balance impulses across presets without flattening high wobbliness.
    double responseGain = moveRatio / std::max(1.0 - dragRatio, 0.01);
    double compensatedGain = std::clamp(responseGain, 0.65, 8.50);
    double impulseMagnitude = 20.0 / std::sqrt(compensatedGain);
    impulseMagnitude = std::clamp(impulseMagnitude, 6.0, 25.0);
    if (!maximizing)
    {
        impulseMagnitude *= 1.20;
    }
    double initialOffset = seedInitialDisplacement ? 2.5 + 1.5 * wobbleProgress : 0.0;
    if (!maximizing)
    {
        initialOffset *= 1.20;
    }
    for (int y = 0; y < GRID_HEIGHT; y++)
    {
        for (int x = 0; x < GRID_WIDTH; x++)
        {
            WobblePoint& point = mesh.points[GetPointIndex(x, y)];
            point.velocity = {direction.x * impulseMagnitude, direction.y * impulseMagnitude};
            point.position.x += direction.x * initialOffset;
            point.position.y += direction.y * initialOffset;
            if (x > 0 && x < GRID_WIDTH - 1 && y > 0 && y < GRID_HEIGHT - 1)
            {
                point.fixed = true;
            }
        }
    }
    mesh.active = true;
}

static void ClearWindowStateThrobConstraints(WobbleMesh& mesh)
{
    // Inner anchors are used only for passive state transitions.
    for (int i = 0; i < GRID_POINT_COUNT; i++)
    {
        mesh.points[i].fixed = false;
    }
    mesh.dragPointIndex = -1;
    mesh.dragging = false;
}

static int FindNearestPoint(const WobbleMesh& mesh, const Vec2& position)
{
    int nearestIndex = 0;
    double nearestDistanceSquared = DBL_MAX;
    for (int i = 0; i < GRID_POINT_COUNT; i++)
    {
        const WobblePoint& point = mesh.points[i];
        double dx = point.basePosition.x - position.x;
        double dy = point.basePosition.y - position.y;
        double distanceSquared = dx * dx + dy * dy;
        if (distanceSquared < nearestDistanceSquared)
        {
            nearestDistanceSquared = distanceSquared;
            nearestIndex = i;
        }
    }
    return nearestIndex;
}

static void SmoothMeshField(WobbleMesh& mesh, bool velocityField)
{
    Vec2 smoothed[GRID_POINT_COUNT] = {};
    for (int y = 0; y < GRID_HEIGHT; y++)
    {
        for (int x = 0; x < GRID_WIDTH; x++)
        {
            int index = GetPointIndex(x, y);
            const Vec2& value =
                velocityField ? mesh.points[index].velocity : mesh.points[index].force;
            Vec2 neighborSum = {};
            int neighborCount = 0;
            for (int offsetY = -1; offsetY <= 1; offsetY++)
            {
                for (int offsetX = -1; offsetX <= 1; offsetX++)
                {
                    if (offsetX == 0 && offsetY == 0)
                    {
                        continue;
                    }
                    int neighborX = x + offsetX;
                    int neighborY = y + offsetY;
                    if (neighborX < 0 || neighborX >= GRID_WIDTH || neighborY < 0 ||
                        neighborY >= GRID_HEIGHT)
                    {
                        continue;
                    }
                    const WobblePoint& neighbor = mesh.points[GetPointIndex(neighborX, neighborY)];
                    const Vec2& neighborValue = velocityField ? neighbor.velocity : neighbor.force;
                    neighborSum.x += neighborValue.x;
                    neighborSum.y += neighborValue.y;
                    neighborCount++;
                }
            }
            if (neighborCount > 0)
            {
                double inverseNeighborCount = 1.0 / static_cast<double>(neighborCount);
                smoothed[index] = {value.x * 0.5 + neighborSum.x * inverseNeighborCount * 0.5,
                                   value.y * 0.5 + neighborSum.y * inverseNeighborCount * 0.5};
            }
            else
            {
                smoothed[index] = value;
            }
        }
    }
    for (int i = 0; i < GRID_POINT_COUNT; i++)
    {
        if (velocityField)
        {
            mesh.points[i].velocity = smoothed[i];
        }
        else
        {
            mesh.points[i].force = smoothed[i];
        }
    }
}

static void CalculateMeshForces(WobbleMesh& mesh, const WobblySettings& settings)
{
    double stiffness = std::clamp(settings.stiffness / 100.0, 0.01, 1.0);
    static constexpr int neighborOffsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int y = 0; y < GRID_HEIGHT; y++)
    {
        for (int x = 0; x < GRID_WIDTH; x++)
        {
            int index = GetPointIndex(x, y);
            WobblePoint& point = mesh.points[index];
            if (point.fixed)
            {
                point.force = {(point.basePosition.x - point.position.x) * stiffness,
                               (point.basePosition.y - point.position.y) * stiffness};
                continue;
            }
            Vec2 springSum = {};
            int neighborCount = 0;
            for (const auto& offset : neighborOffsets)
            {
                int neighborX = x + offset[0];
                int neighborY = y + offset[1];
                if (neighborX < 0 || neighborX >= GRID_WIDTH || neighborY < 0 ||
                    neighborY >= GRID_HEIGHT)
                {
                    continue;
                }
                const WobblePoint& neighbor = mesh.points[GetPointIndex(neighborX, neighborY)];
                springSum.x += (neighbor.position.x - point.position.x) -
                               (neighbor.basePosition.x - point.basePosition.x);
                springSum.y += (neighbor.position.y - point.position.y) -
                               (neighbor.basePosition.y - point.basePosition.y);
                neighborCount++;
            }
            double forceScale =
                neighborCount > 0 ? stiffness / static_cast<double>(neighborCount) : 0.0;
            point.force = {springSum.x * forceScale, springSum.y * forceScale};
        }
    }
    SmoothMeshField(mesh, false);
}

static void SimulateMeshSubstep(WobbleMesh& mesh, const WobblySettings& settings,
                               double deltaTimeMilliseconds, double drag)
{
    CalculateMeshForces(mesh, settings);
    double accelerationSum = 0.0;
    for (int i = 0; i < GRID_POINT_COUNT; i++)
    {
        WobblePoint& point = mesh.points[i];
        Vec2 acceleration = {std::clamp(point.force.x, -1000.0, 1000.0),
                             std::clamp(point.force.y, -1000.0, 1000.0)};
        accelerationSum += std::abs(acceleration.x) + std::abs(acceleration.y);
        point.velocity.x = acceleration.x * deltaTimeMilliseconds + point.velocity.x * drag;
        point.velocity.y = acceleration.y * deltaTimeMilliseconds + point.velocity.y * drag;
    }
    SmoothMeshField(mesh, true);
    double moveFactor = std::clamp(settings.moveFactor / 100.0, 0.01, 0.25);
    double velocitySum = 0.0;
    for (int i = 0; i < GRID_POINT_COUNT; i++)
    {
        WobblePoint& point = mesh.points[i];
        point.velocity.x = std::clamp(point.velocity.x, -1000.0, 1000.0);
        point.velocity.y = std::clamp(point.velocity.y, -1000.0, 1000.0);
        velocitySum += std::abs(point.velocity.x) + std::abs(point.velocity.y);
        point.position.x += point.velocity.x * deltaTimeMilliseconds * moveFactor;
        point.position.y += point.velocity.y * deltaTimeMilliseconds * moveFactor;
        if (!std::isfinite(point.position.x) || !std::isfinite(point.position.y) ||
            !std::isfinite(point.velocity.x) || !std::isfinite(point.velocity.y))
        {
            point.position = point.basePosition;
            point.velocity = {};
            point.force = {};
        }
    }
    ApplyResizeConstraints(mesh);
    mesh.active = !(accelerationSum < 0.5 && velocitySum < 0.5);
}

static double GetMaximumPhysicsStep(const WobblySettings& settings)
{
    double stiffness = std::clamp(settings.stiffness / 100.0, 0.01, 1.0);
    double moveFactor = std::clamp(settings.moveFactor / 100.0, 0.01, 0.25);
    double drag = std::clamp(settings.drag / 100.0, 0.01, MAX_VELOCITY_RETENTION);
    // Conservative spring-step bound; all five presets retain the legacy step.
    return std::min(MAX_PHYSICS_STEP_MS, std::sqrt(0.9 * (1.0 + drag) / (stiffness * moveFactor)));
}

static bool SimulateMeshStep(WobbleMesh& mesh, const WobblySettings& settings, double deltaTime)
{
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0)
    {
        return mesh.active;
    }
    double milliseconds = std::min(deltaTime * 1000.0, MAX_PHYSICS_STEP_MS);
    int substeps = static_cast<int>(std::ceil(milliseconds / GetMaximumPhysicsStep(settings)));
    double drag = std::clamp(settings.drag / 100.0, 0.01, MAX_VELOCITY_RETENTION);
    if (substeps > 1)
    {
        // Subdivision must not multiply the damping selected by the user.
        drag = std::pow(drag, 1.0 / static_cast<double>(substeps));
    }
    for (int i = 0; i < substeps; i++)
    {
        SimulateMeshSubstep(mesh, settings, milliseconds / static_cast<double>(substeps), drag);
    }
    return mesh.active;
}

static void BeginDrag(WobbleMesh& mesh, const Vec2& mousePosition)
{
    // Replace the release anchor without discarding the remaining motion.
    for (WobblePoint& point : mesh.points)
    {
        point.fixed = false;
    }
    mesh.dragPointIndex = FindNearestPoint(mesh, mousePosition);
    WobblePoint& dragPoint = mesh.points[mesh.dragPointIndex];
    // Preserve the exact grab offset for a stable affine pivot.
    mesh.dragOffset = {mousePosition.x - dragPoint.basePosition.x,
                       mousePosition.y - dragPoint.basePosition.y};
    // KDE's grabbed point follows a soft spring constraint.
    dragPoint.fixed = true;
    mesh.dragging = true;
    mesh.active = true;
}

static void EndDrag(WobbleMesh& mesh)
{
    if (!mesh.dragging || mesh.dragPointIndex < 0)
    {
        return;
    }
    // Keep the release point anchored to the real window position while the
    // remaining mesh decays. Unpinning it makes the visual coast away from the
    // HWND and snap back when the identity transform is restored.
    mesh.dragging = false;
}

static bool ReplaceAnimationWithStateThrob(WindowAnimationSlot& slot, int width, int height,
                                           bool maximizing, Vec2 direction)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }
    // Invalidate a scene snapshot of the preceding drag before replacing it.
    slot.generation++;
    InitializeMesh(slot.mesh, static_cast<double>(width), static_cast<double>(height));
    ApplyWindowStateThrob(slot.mesh, maximizing, true, slot.settings, direction);
    slot.dragging = false;
    slot.freeStepPending = true;
    slot.windowStateThrob = true;
    slot.transformRebindRevision++;
    slot.meshRevision++;
    slot.identityApplied = false;
    slot.meshIdentityPending = false;
    slot.order = ++g_animationOrderCounter;
    return true;
}

static WobblySettings GetSettingsSnapshot();
static void UpdateAnimationFrame();
static void RetireAnimationSlot(int slotIndex, HWND expectedWindow = nullptr);
static void StopAllAnimations();
static void MarkObservedWindowTransitionPending(HWND hwnd, bool expectedZoomed);
static void HandleLocationChange(HWND hwnd, LONG idObject, LONG idChild);

template<typename Predicate>
static int FindAnimationSlotMatching(const Predicate& matches)
{
    int slotIndex = -1;
    AcquireSRWLockShared(&g_animationSlotsLock);
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        if (matches(g_animationSlots[i], i))
        {
            slotIndex = i;
            break;
        }
    }
    ReleaseSRWLockShared(&g_animationSlotsLock);
    return slotIndex;
}

static bool HasAnyAnimationSlots()
{
    return FindAnimationSlotMatching([](const WindowAnimationSlot& slot, int)
                                     { return slot.active || slot.retiring; }) >= 0;
}

static double GetExactMonitorRefreshRate(const MONITORINFOEXW& monitorInfo)
{
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) !=
            ERROR_SUCCESS)
        {
            break;
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        LONG result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                         &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
        {
            continue;
        }
        if (result != ERROR_SUCCESS)
        {
            break;
        }
        for (UINT32 i = 0; i < pathCount; i++)
        {
            const DISPLAYCONFIG_PATH_INFO& path = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
                _wcsicmp(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0)
            {
                continue;
            }
            const DISPLAYCONFIG_RATIONAL& refreshRate = path.targetInfo.refreshRate;
            if (refreshRate.Numerator && refreshRate.Denominator)
            {
                double rate = static_cast<double>(refreshRate.Numerator) /
                              static_cast<double>(refreshRate.Denominator);
                if (std::isfinite(rate) && rate >= 30.0 && rate <= 1000.0)
                {
                    return rate;
                }
            }
        }
        break;
    }
    return 0.0;
}

static double GetPreferredAnimationRate(HMONITOR monitor)
{
    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    DEVMODEW displayMode = {};
    displayMode.dmSize = sizeof(displayMode);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
    {
        double displayRate = GetExactMonitorRefreshRate(monitorInfo);
        if (displayRate == 0.0 &&
            EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode) &&
            displayMode.dmDisplayFrequency >= 30 && displayMode.dmDisplayFrequency <= 1000)
        {
            displayRate = static_cast<double>(displayMode.dmDisplayFrequency);
        }
        if (displayRate == 0.0)
        {
            return 60.0;
        }
        displayRate = std::clamp(displayRate, 30.0, 500.0);
        if ((monitorInfo.dwFlags & MONITORINFOF_PRIMARY) == 0)
        {
            // Oversample secondary outputs because DWM exposes no per-output phase.
            return std::min(500.0, displayRate * 2.0);
        }
        return displayRate;
    }
    return 60.0;
}

static double GetPreferredAnimationRate(HWND hwnd)
{
    return GetPreferredAnimationRate(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
}

static void RetargetAnimationClock(HMONITOR monitor)
{
    if (!g_animationClockArmed || !monitor)
    {
        return;
    }
    double preferredRate = GetPreferredAnimationRate(monitor);
    if (std::fabs(preferredRate - g_animationTargetHz) > 0.01)
    {
        // The currently armed tick can finish; the next one starts a new exact-rate epoch.
        g_animationTargetHz = preferredRate;
        g_nextAnimationCounter = {};
    }
}

static bool ArmNextAnimationTick()
{
    if (!g_animationTimer || !g_animationClockArmed || g_animationFrequency.QuadPart <= 0 ||
        g_animationTargetHz <= 0.0)
    {
        return false;
    }
    LARGE_INTEGER now = {};
    if (!QueryPerformanceCounter(&now))
    {
        return false;
    }
    LONGLONG interval = std::max<LONGLONG>(
        1, static_cast<LONGLONG>(std::llround(static_cast<double>(g_animationFrequency.QuadPart) /
                                              g_animationTargetHz)));
    if (g_nextAnimationCounter.QuadPart <= 0)
    {
        g_nextAnimationCounter.QuadPart = now.QuadPart + interval;
    }
    else
    {
        do
        {
            g_nextAnimationCounter.QuadPart += interval;
        } while (g_nextAnimationCounter.QuadPart <= now.QuadPart);
    }
    LONGLONG remainingCounter = g_nextAnimationCounter.QuadPart - now.QuadPart;
    LONGLONG due100Nanoseconds = std::max<LONGLONG>(
        1, static_cast<LONGLONG>(std::ceil(static_cast<double>(remainingCounter) * 10000000.0 /
                                           static_cast<double>(g_animationFrequency.QuadPart))));
    LARGE_INTEGER dueTime = {};
    dueTime.QuadPart = -due100Nanoseconds;
    return SetWaitableTimer(g_animationTimer, &dueTime, 0, nullptr, nullptr, FALSE) != FALSE;
}

static void DisableAnimationThreadPowerThrottling()
{
    // Keep free decay out of EcoQoS throttling.
    THREAD_POWER_THROTTLING_STATE powerThrottling = {};
    powerThrottling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    powerThrottling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    powerThrottling.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &powerThrottling,
                         sizeof(powerThrottling));
}

static bool EnsureAnimationClockRunning(HWND hwnd)
{
    if (!g_animationTimer)
    {
        return false;
    }
    double preferredRate = GetPreferredAnimationRate(hwnd);
    if (g_animationClockArmed)
    {
        // Follow the newest interaction's monitor rate in both directions.
        if (std::fabs(preferredRate - g_animationTargetHz) > 0.5)
        {
            double previousRate = g_animationTargetHz;
            LARGE_INTEGER previousNextCounter = g_nextAnimationCounter;
            g_animationTargetHz = preferredRate;
            g_nextAnimationCounter = {};
            if (!ArmNextAnimationTick())
            {
                g_animationTargetHz = previousRate;
                g_nextAnimationCounter = previousNextCounter;
            }
        }
        return true;
    }
    g_animationTargetHz = preferredRate;
    g_nextAnimationCounter = {};
    if (!QueryPerformanceCounter(&g_lastAnimationCounter))
    {
        g_lastAnimationCounter = {};
    }
    g_animationClockArmed = true;
    if (!ArmNextAnimationTick())
    {
        DWORD timerError = GetLastError();
        g_animationClockArmed = false;
        g_animationTargetHz = 0.0;
        g_lastAnimationCounter = {};
        g_nextAnimationCounter = {};
        Wh_Log(L"Failed to arm animation timer: %u", timerError);
        return false;
    }
    return true;
}

static void ResetDragInputState()
{
    g_realDragging = false;
    g_realDraggedWindow = nullptr;
    g_pendingInteractiveTransitionWindow.store(nullptr, std::memory_order_release);
    g_pendingInteractiveTransitionFlags.store(0, std::memory_order_relaxed);
    g_realResizing = false;
    g_moveTypeKnown = false;
    g_dragResizeWobbleEnabled = true;
    g_dragStartedWindowZoomed = false;
    g_waitingForInitialRestore = false;
    g_interactiveStateThrob = InteractiveStateThrobKind::None;
    g_interactiveStateThrobDirection = 0;
    g_interactiveStateThrobFromPointerEdge = false;
    g_resizeCoordinateScale = 1.0;
    g_dragAnimationSlot = -1;
    g_hasLastMousePosition = false;
    g_lastMousePosition = {};
    g_dragCursorMonitor = nullptr;
    g_monitorTransitionRebaseUntil = 0;
    g_realDraggedWindowRect = {};
    g_lastDraggedWindowRect = {};
    g_lastDraggedWindowZoomed = false;
    g_finalizingMoveSize = false;
}

static void DisarmAnimationClock()
{
    if (g_animationTimer)
    {
        CancelWaitableTimer(g_animationTimer);
    }
    g_animationClockArmed = false;
    g_animationTargetHz = 0.0;
    g_lastAnimationCounter = {};
    g_nextAnimationCounter = {};
}

static bool IsInteractiveMoveSizeLoop(HWND hwnd)
{
    DWORD windowThreadId = GetWindowThreadProcessId(hwnd, nullptr);
    if (windowThreadId != 0)
    {
        GUITHREADINFO guiThreadInfo = {};
        guiThreadInfo.cbSize = sizeof(guiThreadInfo);
        if (GetGUIThreadInfo(windowThreadId, &guiThreadInfo))
        {
            return (guiThreadInfo.flags & GUI_INMOVESIZE) != 0;
        }
    }
    // Don't mistake programmatic state changes for interactive moves.
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}

struct MonitorEdgeState
{
    bool top;
    bool side;
    Vec2 direction;
};

static MonitorEdgeState GetPointMonitorEdgeState(const POINT& point,
                                                  LONG edgeTolerance = 3)
{
    MonitorEdgeState result = {};
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
    {
        return result;
    }
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        return result;
    }
    // Aero Snap uses physical monitor edges. Normal tracking keeps this narrow;
    // MOVESIZEEND can request a wider intent band for coalesced fast input.
    result.top = point.x >= monitorInfo.rcMonitor.left && point.x < monitorInfo.rcMonitor.right &&
                 point.y <= monitorInfo.rcMonitor.top + edgeTolerance;
    if (result.top)
    {
        result.direction.y = -1.0;
    }
    bool atLeftEdge = point.y >= monitorInfo.rcMonitor.top &&
                      point.y < monitorInfo.rcMonitor.bottom &&
                      point.x <= monitorInfo.rcMonitor.left + edgeTolerance;
    bool atRightEdge = point.y >= monitorInfo.rcMonitor.top &&
                       point.y < monitorInfo.rcMonitor.bottom &&
                       point.x >= monitorInfo.rcMonitor.right - edgeTolerance - 1;
    result.side = atLeftEdge || atRightEdge;
    if (atLeftEdge)
    {
        result.direction.x = -1.0;
    }
    else if (atRightEdge)
    {
        result.direction.x = 1.0;
    }
    return result;
}

static bool ResetMatrixTransformProxy(void* matrixTransformProxy)
{
    if (!IsOnDwmSceneThread() ||
        !IsDwmObjectPointerValid(matrixTransformProxy, g_matrixTransformProxyVtable) ||
        !HasMatrixTransformUpdate())
    {
        return false;
    }
    MilMatrix3x2D identityMatrix = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    return UpdateMatrixTransformProxy(matrixTransformProxy, identityMatrix) >= 0;
}

static bool PostPendingDwmSceneWake(bool forceRepost)
{
    if (g_unloading.load(std::memory_order_acquire) && !forceRepost)
    {
        return false;
    }
    if (g_sceneWakeAwaitingNativeTimeline.load(std::memory_order_acquire))
    {
        return true;
    }
    if (!forceRepost)
    {
        bool expected = false;
        if (!g_sceneWakeScheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
        {
            return true;
        }
    }
    else
    {
        // Serial numbers make watchdog reposts idempotent.
        g_sceneWakeScheduled.store(true, std::memory_order_release);
    }
    if (g_sceneWakeOutstanding.load(std::memory_order_acquire) != 0)
    {
        return true;
    }
    void* manager = g_desktopManager.load(std::memory_order_acquire);
    DWORD sceneThreadId = g_dwmSceneThreadId.load(std::memory_order_acquire);
    if (!sceneThreadId && HasExactDwmVtableTrusted(manager, g_desktopManagerVtable) &&
        g_desktopManagerThreadIdOffset != SIZE_MAX)
    {
        BYTE* threadIdField = static_cast<BYTE*>(manager) + g_desktopManagerThreadIdOffset;
        if (IsReadableMemory(threadIdField, sizeof(DWORD)))
        {
            sceneThreadId = *reinterpret_cast<DWORD*>(threadIdField);
        }
    }
    if (sceneThreadId)
    {
        // A decoded or recycled ID must never target another process.
        HANDLE sceneThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, sceneThreadId);
        bool ownThread = sceneThread && GetProcessIdOfThread(sceneThread) == GetCurrentProcessId();
        if (sceneThread)
        {
            CloseHandle(sceneThread);
        }
        if (!ownThread)
        {
            sceneThreadId = 0;
        }
    }
    if (sceneThreadId)
    {
        // PostThreadMessage queues are unbounded for our purposes. Keep one
        // wake in flight so a powered-off display can't accumulate thousands
        // of synchronous ForceUpdateScene calls for resume.
        unsigned int expectedOutstanding = 0;
        if (!g_sceneWakeOutstanding.compare_exchange_strong(
                expectedOutstanding, 1, std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return true;
        }
        if (PostThreadMessageW(sceneThreadId, g_dwmSceneWakeMessage,
                               DWM_SCENE_WAKE_WPARAM,
                               g_dwmSceneWakeToken.load(std::memory_order_acquire)))
        {
            g_sceneWakePostTimestamp.store(GetTickCount64(), std::memory_order_release);
            return true;
        }
        g_sceneWakeOutstanding.store(0, std::memory_order_release);
    }
    // AdvanceTimelines consumes the pending serial when the scene thread is
    // first discovered or a thread-message wake can't be posted.
    g_sceneWakeScheduled.store(false, std::memory_order_release);
    g_sceneWakeAwaitingNativeTimeline.store(false, std::memory_order_release);
    g_sceneWakePostTimestamp.store(0, std::memory_order_release);
    return false;
}

static void RequestDwmScenePass()
{
    if (g_unloading.load(std::memory_order_acquire))
    {
        return;
    }
    g_sceneRequestedSerial.fetch_add(1, std::memory_order_acq_rel);
    PostPendingDwmSceneWake(false);
}

static void FinalizeRetiringSlots()
{
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        void* matrixTransformProxy = nullptr;
        bool finalizeResources = false;
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        WindowAnimationSlot& slot = g_animationSlots[i];
        if (slot.retiring && slot.hookUsers == 0 && slot.identityApplied)
        {
            if (!slot.matrixTransformProxy)
            {
                ResetAnimationSlotLocked(slot, slot.generation);
            }
            else if (IsOnDwmSceneThread() && g_cBaseObjectRelease)
            {
                matrixTransformProxy = slot.matrixTransformProxy;
                slot.matrixTransformProxy = nullptr;
                slot.hookUsers++;
                finalizeResources = true;
            }
        }
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (finalizeResources)
        {
            // The visual retains the resource, not this wrapper. Keep identity:
            // the private setter cannot unbind with nullptr.
            g_cBaseObjectRelease(matrixTransformProxy);
            AcquireSRWLockExclusive(&g_animationSlotsLock);
            ReleaseAnimationSlotPinLocked(slot);
            ResetAnimationSlotLocked(slot, slot.generation);
            ReleaseSRWLockExclusive(&g_animationSlotsLock);
        }
    }
}

static int CollectActiveAnimationSlots(int* indices, bool* hasProxy = nullptr)
{
    int count = 0;
    bool proxyFound = false;
    AcquireSRWLockShared(&g_animationSlotsLock);
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        const WindowAnimationSlot& slot = g_animationSlots[i];
        if (slot.active)
        {
            indices[count++] = i;
        }
        proxyFound |= slot.matrixTransformProxy != nullptr;
    }
    ReleaseSRWLockShared(&g_animationSlotsLock);
    if (hasProxy)
    {
        *hasProxy = proxyFound;
    }
    return count;
}

static void AbandonAnimationSlotsAfterSceneStall(const wchar_t* reason)
{
    unsigned int abandonedSlots = 0;
    unsigned int retainedProxies = 0;
    unsigned int busySlots = 0;
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    for (WindowAnimationSlot& slot : g_animationSlots)
    {
        if (!slot.active && !slot.retiring)
        {
            continue;
        }
        if (slot.hookUsers != 0)
        {
            busySlots++;
            continue;
        }
        retainedProxies += slot.matrixTransformProxy != nullptr;
        ResetAnimationSlotLocked(slot, slot.generation + 1);
        abandonedSlots++;
    }
    WakeAllConditionVariable(&g_animationSlotsCondition);
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    DisarmAnimationClock();
    g_sceneWakeScheduled.store(false, std::memory_order_release);
    g_sceneWakePostTimestamp.store(0, std::memory_order_release);
    g_sceneWakeStallStartedAt.store(0, std::memory_order_release);
    g_sceneWakeStalled.store(false, std::memory_order_release);
    g_sceneRecoveryCleanupPending.store(false, std::memory_order_release);
    g_sceneSubmittedSerial.store(g_sceneRequestedSerial.load(std::memory_order_acquire),
                                 std::memory_order_release);
    g_lastObservedScenePassCounter = g_scenePassCounter.load(std::memory_order_acquire);
    g_lastSceneProgressTimestamp = 0;
    ResetDragInputState();
    unsigned int totalRetainedProxies =
        g_abandonedProxyCount.fetch_add(retainedProxies, std::memory_order_acq_rel) +
        retainedProxies;
    Wh_Log(L"DWM scene work abandoned (%s): slots=%u retainedProxies=%u "
           L"totalRetainedProxies=%u busySlots=%u",
           reason, abandonedSlots, retainedProxies, totalRetainedProxies,
           busySlots);
}

static void BeginSceneStallCleanup(const wchar_t* reason)
{
    if (g_sceneRecoveryCleanupPending.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    int slotsToRetire[MAX_ANIMATION_SLOTS] = {};
    int retireCount = CollectActiveAnimationSlots(slotsToRetire);
    for (int i = 0; i < retireCount; i++)
    {
        RetireAnimationSlot(slotsToRetire[i]);
    }
    // Keep only a low-rate identity retry alive until a scene pass recovers.
    g_animationTargetHz = 10.0;
    g_nextAnimationCounter = {};
    Wh_Log(L"DWM scene stalled (%s): retiring %d slots for identity cleanup",
           reason, retireCount);
}

static void StopAnimationClockIfIdle()
{
    FinalizeRetiringSlots();
    if (HasAnyAnimationSlots())
    {
        return;
    }
    DisarmAnimationClock();
    g_lastObservedScenePassCounter = g_scenePassCounter.load(std::memory_order_acquire);
    g_lastSceneProgressTimestamp = 0;
    g_sceneRecoveryCleanupPending.store(false, std::memory_order_release);
}

static void RetireAnimationSlot(int slotIndex, HWND expectedWindow)
{
    if (slotIndex < 0 || slotIndex >= MAX_ANIMATION_SLOTS)
    {
        return;
    }
    bool wasDragSlot = g_dragAnimationSlot == slotIndex;
    HWND sceneWakeWindow = nullptr;
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    WindowAnimationSlot& slot = g_animationSlots[slotIndex];
    if (expectedWindow && (!slot.active || slot.hwnd != expectedWindow))
    {
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        return;
    }
    if (slot.active)
    {
        slot.active = false;
        slot.retiring = true;
        slot.dragging = false;
        slot.freeStepPending = false;
        slot.transformAttached = false;
        slot.transitionTransformAttached = false;
        slot.boundTopLevelVisualProxy = nullptr;
        slot.boundTransitionVisualProxy = nullptr;
        slot.proxyCreationPending = false;
        if (slot.matrixTransformProxy)
        {
            slot.meshIdentityPending = true;
            slot.identityApplied = false;
            slot.meshRevision++;
            sceneWakeWindow = slot.hwnd;
        }
        else
        {
            slot.meshIdentityPending = false;
            slot.identityApplied = true;
        }
        slot.generation++;
    }
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (sceneWakeWindow)
    {
        RequestDwmScenePass();
    }
    if (wasDragSlot)
    {
        ResetDragInputState();
    }
    StopAnimationClockIfIdle();
}

static int FindAnimationSlotForWindow(HWND hwnd)
{
    return FindAnimationSlotMatching([hwnd](const WindowAnimationSlot& slot, int)
                                     { return slot.active && slot.hwnd == hwnd; });
}

static bool WaitForRetiringSlotForWindow(HWND hwnd)
{
    ULONGLONG deadline = GetTickCount64() + 50;
    for (;;)
    {
        FinalizeRetiringSlots();
        bool retiringWindowFound = false;
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
        {
            if (g_animationSlots[i].retiring && g_animationSlots[i].hwnd == hwnd)
            {
                retiringWindowFound = true;
                break;
            }
        }
        if (!retiringWindowFound)
        {
            ReleaseSRWLockExclusive(&g_animationSlotsLock);
            return true;
        }
        ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            ReleaseSRWLockExclusive(&g_animationSlotsLock);
            return false;
        }
        DWORD remaining = static_cast<DWORD>(deadline - now);
        BOOL awakened = SleepConditionVariableSRW(&g_animationSlotsCondition, &g_animationSlotsLock,
                                                  remaining, 0);
        DWORD waitError = awakened ? ERROR_SUCCESS : GetLastError();
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (!awakened && waitError == ERROR_TIMEOUT)
        {
            return false;
        }
    }
}

static int FindFreeAnimationSlot()
{
    FinalizeRetiringSlots();
    return FindAnimationSlotMatching([](const WindowAnimationSlot& slot, int)
                                     { return !slot.active && !slot.retiring; });
}

static int EvictOldestSettlingSlot()
{
    int candidate = -1;
    ULONGLONG oldestOrder = ULLONG_MAX;
    AcquireSRWLockShared(&g_animationSlotsLock);
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        const WindowAnimationSlot& slot = g_animationSlots[i];
        if (slot.active && !slot.dragging && slot.hookUsers == 0 && i != g_dragAnimationSlot &&
            slot.order < oldestOrder)
        {
            candidate = i;
            oldestOrder = slot.order;
        }
    }
    ReleaseSRWLockShared(&g_animationSlotsLock);
    if (candidate >= 0)
    {
        RetireAnimationSlot(candidate);
    }
    return FindFreeAnimationSlot();
}

static bool CreateMatrixTransformProxy(void** matrixTransformProxy)
{
    *matrixTransformProxy = nullptr;
    void* compositor = g_dwmCompositor.load(std::memory_order_acquire);
    if (g_proxyCreationDisabled.load(std::memory_order_acquire) || !IsOnDwmSceneThread() ||
        !HasExactDwmVtableTrusted(compositor, g_compositorVtable) ||
        !g_createMatrixTransformProxy)
    {
        return false;
    }
    long result = g_createMatrixTransformProxy(compositor, matrixTransformProxy);
    bool validProxy = IsDwmObjectPointerValid(*matrixTransformProxy, g_matrixTransformProxyVtable);
    if (result >= 0 && validProxy)
    {
        return true;
    }
    if (*matrixTransformProxy)
    {
        if (validProxy)
        {
            // Failed factory result: this verified proxy was never bound.
            g_cBaseObjectRelease(*matrixTransformProxy);
        }
        else if (result >= 0)
        {
            // A wrong factory type means the private ABI changed; stop using it.
            g_proxyCreationDisabled.store(true, std::memory_order_release);
        }
        *matrixTransformProxy = nullptr;
    }
    Wh_Log(L"Matrix transform creation failed: 0x%08X", static_cast<unsigned int>(result));
    return false;
}

static void EnsurePendingMatrixTransformProxies()
{
    if (!IsOnDwmSceneThread() || g_unloading.load(std::memory_order_acquire))
    {
        return;
    }
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        ULONGLONG generation = 0;
        HWND hwnd = nullptr;
        bool createProxy = false;
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        WindowAnimationSlot& slot = g_animationSlots[i];
        if (slot.active && slot.proxyCreationPending && !slot.matrixTransformProxy)
        {
            slot.hookUsers++;
            generation = slot.generation;
            hwnd = slot.hwnd;
            createProxy = true;
        }
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (!createProxy)
        {
            continue;
        }
        void* matrixTransformProxy = nullptr;
        bool created = CreateMatrixTransformProxy(&matrixTransformProxy);
        if (created)
        {
            // Test new proxies with identity before attaching them.
            created = ResetMatrixTransformProxy(matrixTransformProxy);
        }
        bool stored = false;
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        WindowAnimationSlot& currentSlot = g_animationSlots[i];
        if (currentSlot.active && currentSlot.generation == generation &&
            currentSlot.proxyCreationPending && !currentSlot.matrixTransformProxy)
        {
            currentSlot.proxyCreationPending = false;
            if (created)
            {
                currentSlot.matrixTransformProxy = matrixTransformProxy;
                currentSlot.transformAttached = false;
                currentSlot.transitionTransformAttached = false;
                currentSlot.transformRebindRevision++;
                currentSlot.identityApplied = true;
                stored = true;
            }
            else
            {
                currentSlot.active = false;
                currentSlot.retiring = true;
                currentSlot.dragging = false;
                currentSlot.freeStepPending = false;
                currentSlot.meshIdentityPending = false;
                currentSlot.identityApplied = true;
                currentSlot.generation++;
            }
        }
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (matrixTransformProxy && !stored)
        {
            // No slot adopted this factory ref and no visual was bound.
            g_cBaseObjectRelease(matrixTransformProxy);
        }
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        ReleaseAnimationSlotPinLocked(currentSlot);
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (!created)
        {
            Wh_Log(L"DWM safety gate: matrix proxy canary failed for HWND=%p", hwnd);
        }
    }
}

static bool IsResizeHitTestResult(LRESULT hitTestResult)
{
    switch (hitTestResult)
    {
    case HTLEFT:
    case HTRIGHT:
    case HTTOP:
    case HTTOPLEFT:
    case HTTOPRIGHT:
    case HTBOTTOM:
    case HTBOTTOMLEFT:
    case HTBOTTOMRIGHT:
        return true;
    default:
        return false;
    }
}

static bool IsCursorOnResizableFrame(HWND hwnd, const POINT& mousePosition,
                                     const RECT& rect, bool includeTopEdge)
{
    if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) == 0 || IsZoomed(hwnd))
    {
        return false;
    }
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0)
    {
        dpi = USER_DEFAULT_SCREEN_DPI;
    }
    int frameX = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                 GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    int frameY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                 GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    frameX = std::clamp(frameX, 4, 32);
    frameY = std::clamp(frameY, 4, 32);
    LONG localX = mousePosition.x - rect.left;
    LONG localY = mousePosition.y - rect.top;
    LONG width = rect.right - rect.left;
    LONG height = rect.bottom - rect.top;
    if (localX < -1 || localY < -1 || localX > width + 1 || localY > height + 1)
    {
        return false;
    }
    bool horizontalEdge = localX <= frameX || localX >= width - frameX;
    bool verticalEdge = localY >= height - frameY || (includeTopEdge && localY <= frameY);
    return horizontalEdge || verticalEdge;
}

static bool ClassifyMoveSizeOperation(HWND hwnd, const POINT& mousePosition, bool& resizing)
{
    // Restoring a maximized window under the cursor is still a move.
    if (IsZoomed(hwnd))
    {
        resizing = false;
        return true;
    }
    RECT rect = {};
    bool hasRect = GetWindowRect(hwnd, &rect) != FALSE;
    DWORD_PTR hitTestResult = HTNOWHERE;
    bool hasHitTest =
        SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0,
                            MAKELPARAM(static_cast<short>(mousePosition.x),
                                       static_cast<short>(mousePosition.y)),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &hitTestResult) != FALSE;
    if (hasHitTest && IsResizeHitTestResult(static_cast<LRESULT>(hitTestResult)))
    {
        resizing = true;
        return true;
    }
    // Some custom frames report HTCAPTION for the invisible side/bottom border.
    // Top-edge caption grabs remain moves unless WM_NCHITTEST identified resize.
    if (hasRect && IsCursorOnResizableFrame(hwnd, mousePosition, rect, false))
    {
        resizing = true;
        return true;
    }
    if (hasHitTest && static_cast<LRESULT>(hitTestResult) == HTCAPTION)
    {
        resizing = false;
        return true;
    }
    // Custom title bars may need geometry-delta classification.
    return false;
}

static void InitializeDpiSupport()
{
    g_shcoreModule = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_shcoreModule)
    {
        g_getDpiForMonitor = reinterpret_cast<GetDpiForMonitor_t>(
            GetProcAddress(g_shcoreModule, "GetDpiForMonitor"));
    }
    g_dwmApiModule = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_dwmApiModule)
    {
        g_dwmGetWindowAttribute = reinterpret_cast<DwmGetWindowAttribute_t>(
            GetProcAddress(g_dwmApiModule, "DwmGetWindowAttribute"));
    }
}

static void UninitializeDpiSupport()
{
    g_getDpiForMonitor = nullptr;
    if (g_shcoreModule)
    {
        FreeLibrary(g_shcoreModule);
        g_shcoreModule = nullptr;
    }
    g_dwmGetWindowAttribute = nullptr;
    if (g_dwmApiModule)
    {
        FreeLibrary(g_dwmApiModule);
        g_dwmApiModule = nullptr;
    }
}

static double GetResizeVisualCoordinateScale(HWND hwnd, const RECT& windowRect)
{
    if (!hwnd || !g_getDpiForMonitor)
    {
        return 1.0;
    }
    HMONITOR monitor = MonitorFromRect(&windowRect, MONITOR_DEFAULTTONEAREST);
    UINT monitorDpiX = 0;
    UINT monitorDpiY = 0;
    if (!monitor ||
        FAILED(g_getDpiForMonitor(monitor,
                                  0, // MDT_EFFECTIVE_DPI
                                  &monitorDpiX, &monitorDpiY)) ||
        monitorDpiX == 0)
    {
        return 1.0;
    }
    UINT windowDpi = GetDpiForWindow(hwnd);
    if (windowDpi == 0)
    {
        return 1.0;
    }
    // Correct system-DPI surfaces transformed by DWM on another monitor.
    return std::clamp(static_cast<double>(windowDpi) / static_cast<double>(monitorDpiX), 0.50,
                      2.00);
}

static int AcquireAnimationSlot(HWND hwnd, const WobblySettings& settings, double width,
                                double height, const Vec2& mousePosition)
{
    if (g_dragAnimationSlot >= 0)
    {
        RetireAnimationSlot(g_dragAnimationSlot);
    }
    int slotIndex = FindAnimationSlotForWindow(hwnd);
    if (slotIndex >= 0)
    {
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        WindowAnimationSlot& slot = g_animationSlots[slotIndex];
        bool reused = slot.active && slot.hwnd == hwnd;
        if (reused)
        {
            slot.settings = settings;
            bool resetStateThrob = slot.windowStateThrob;
            bool resetResizeDecay = slot.mesh.resizing && !slot.dragging;
            if (resetStateThrob || resetResizeDecay)
            {
                InitializeMesh(slot.mesh, width, height);
                slot.windowStateThrob = false;
                if (resetStateThrob)
                {
                    slot.boundTransitionVisualProxy = nullptr;
                    slot.transitionTransformAttached = false;
                }
            }
            BeginDrag(slot.mesh, mousePosition);
            slot.dragging = true;
            slot.freeStepPending = false;
            slot.nextWindowValidation = GetTickCount64() + 250;
            slot.nextVisualValidation = 0;
            slot.lastBindFailureLog = 0;
            slot.order = ++g_animationOrderCounter;
            slot.identityApplied = false;
            slot.meshIdentityPending = false;
            slot.meshRevision++;
        }
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (reused)
        {
            return slotIndex;
        }
    }
    // Revive an unpinned retiring slot when the same window is re-grabbed.
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        WindowAnimationSlot& slot = g_animationSlots[i];
        if (!slot.retiring || slot.hwnd != hwnd || slot.hookUsers != 0)
        {
            continue;
        }
        slot.active = true;
        slot.retiring = false;
        slot.dragging = true;
        slot.freeStepPending = false;
        slot.generation++;
        slot.settings = settings;
        bool resetStateThrob = slot.windowStateThrob;
        if (resetStateThrob || slot.mesh.resizing)
        {
            InitializeMesh(slot.mesh, width, height);
            slot.windowStateThrob = false;
        }
        BeginDrag(slot.mesh, mousePosition);
        slot.transformAttached = false;
        slot.transitionTransformAttached = false;
        slot.boundTopLevelVisualProxy = nullptr;
        slot.boundTransitionVisualProxy = nullptr;
        slot.transformRebindRevision++;
        slot.submittedTransformRebindRevision = 0;
        slot.meshIdentityPending = false;
        slot.proxyCreationPending = slot.matrixTransformProxy == nullptr;
        slot.identityApplied = false;
        slot.nextWindowValidation = GetTickCount64() + 250;
        slot.nextVisualValidation = 0;
        slot.lastBindFailureLog = 0;
        slot.order = ++g_animationOrderCounter;
        slot.meshRevision++;
        slotIndex = i;
        break;
    }
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (slotIndex >= 0)
    {
        return slotIndex;
    }
    WobbleMesh mesh = {};
    InitializeMesh(mesh, width, height);
    BeginDrag(mesh, mousePosition);
    if (!WaitForRetiringSlotForWindow(hwnd))
    {
        Wh_Log(L"Previous animation hook is still active for HWND=%p", hwnd);
        return -1;
    }
    slotIndex = FindFreeAnimationSlot();
    if (slotIndex < 0)
    {
        slotIndex = EvictOldestSettlingSlot();
    }
    if (slotIndex < 0)
    {
        Wh_Log(L"Animation buffer is full");
        return -1;
    }
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    WindowAnimationSlot& slot = g_animationSlots[slotIndex];
    slot.active = true;
    slot.retiring = false;
    slot.dragging = true;
    slot.freeStepPending = false;
    slot.windowStateThrob = false;
    slot.hookUsers = 0;
    slot.generation++;
    slot.order = ++g_animationOrderCounter;
    slot.hwnd = hwnd;
    slot.settings = settings;
    slot.mesh = mesh;
    slot.matrixTransformProxy = nullptr;
    slot.boundTopLevelVisualProxy = nullptr;
    slot.boundTransitionVisualProxy = nullptr;
    slot.transformAttached = false;
    slot.transitionTransformAttached = false;
    slot.transformRebindRevision = 1;
    slot.submittedTransformRebindRevision = 0;
    slot.meshRevision = 1;
    slot.submittedMeshRevision = 0;
    slot.meshIdentityPending = false;
    slot.proxyCreationPending = true;
    slot.nextWindowValidation = GetTickCount64() + 250;
    slot.nextVisualValidation = 0;
    slot.identityApplied = false;
    slot.lastMatrixErrorLog = 0;
    slot.lastBindFailureLog = 0;
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    return slotIndex;
}

static void HandleMoveSizeStart(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) ||
        !IsInteractiveMoveSizeLoop(hwnd))
    {
        return;
    }
    // Reset Snap state so the same side can trigger again.
    ResetObservedSnapStateForInteractiveMove(hwnd);
    g_pendingInteractiveTransitionWindow.store(nullptr, std::memory_order_release);
    g_pendingInteractiveTransitionFlags.store(0, std::memory_order_relaxed);
    RECT rect = {};
    if (!GetWindowRect(hwnd, &rect))
    {
        return;
    }
    POINT mousePosition = {};
    if (!GetCursorPos(&mousePosition))
    {
        return;
    }
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }
    WobblySettings activeSettings = GetSettingsSnapshot();
    Vec2 localMousePosition = {static_cast<double>(mousePosition.x - rect.left),
                               static_cast<double>(mousePosition.y - rect.top)};
    bool startedWindowZoomed = IsZoomed(hwnd) != FALSE || IsApproximatelyMonitorWorkArea(rect);
    bool operationResizing = false;
    bool operationTypeKnown = startedWindowZoomed ||
                              ClassifyMoveSizeOperation(hwnd, mousePosition, operationResizing);
    if (operationTypeKnown && operationResizing && !activeSettings.resizeWobbleEnabled)
    {
        return;
    }
    g_resizeCoordinateScale =
        operationTypeKnown && operationResizing ? GetResizeVisualCoordinateScale(hwnd, rect) : 1.0;
    if (operationTypeKnown && operationResizing)
    {
        localMousePosition.x *= g_resizeCoordinateScale;
        localMousePosition.y *= g_resizeCoordinateScale;
    }
    double meshWidth = static_cast<double>(width) * g_resizeCoordinateScale;
    double meshHeight = static_cast<double>(height) * g_resizeCoordinateScale;
    int slotIndex =
        AcquireAnimationSlot(hwnd, activeSettings, meshWidth, meshHeight, localMousePosition);
    if (slotIndex < 0)
    {
        return;
    }
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    WindowAnimationSlot& dragSlot = g_animationSlots[slotIndex];
    if (dragSlot.active && dragSlot.hwnd == hwnd)
    {
        SetMeshResizeMode(dragSlot.mesh, operationTypeKnown && operationResizing);
        // A new move loop may replace the visual transform.
        dragSlot.transformRebindRevision++;
        dragSlot.meshRevision++;
        dragSlot.identityApplied = false;
        dragSlot.meshIdentityPending = false;
    }
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    g_dragAnimationSlot = slotIndex;
    g_realDraggedWindow = hwnd;
    g_realDraggedWindowRect = rect;
    g_lastDraggedWindowRect = rect;
    g_lastDraggedWindowZoomed = startedWindowZoomed;
    g_dragStartedWindowZoomed = startedWindowZoomed;
    g_waitingForInitialRestore = startedWindowZoomed;
    g_interactiveStateThrob = InteractiveStateThrobKind::None;
    g_interactiveStateThrobDirection = 0;
    g_interactiveStateThrobFromPointerEdge = false;
    g_lastMousePosition = mousePosition;
    g_dragCursorMonitor = MonitorFromPoint(mousePosition, MONITOR_DEFAULTTONEAREST);
    g_monitorTransitionRebaseUntil = 0;
    g_hasLastMousePosition = true;
    g_realDragging = true;
    g_nextDragTraceAt.store(0, std::memory_order_relaxed);
    g_nextMatrixTraceAt.store(0, std::memory_order_relaxed);
    g_realResizing = operationTypeKnown && operationResizing;
    g_moveTypeKnown = operationTypeKnown;
    g_dragResizeWobbleEnabled = activeSettings.resizeWobbleEnabled;
    if (!EnsureAnimationClockRunning(hwnd))
    {
        Wh_Log(L"Failed to start drag animation timer");
        RetireAnimationSlot(slotIndex);
        return;
    }
    Wh_Log(L"DRAG TRACE START: HWND=%p Slot=%d Renderer=%s size=%dx%d "
           L"resizing=%d typeKnown=%d",
           hwnd, slotIndex,
           activeSettings.rendererMode == RendererMode::ExternalGpu ? L"external-gpu" :
               activeSettings.rendererMode == RendererMode::Mesh ? L"mesh" : L"affine",
           width, height, g_realResizing, g_moveTypeKnown);
    RequestDwmScenePass();
}

static void ApplyAnimationSlotTransform(int slotIndex, bool identityOnly)
{
    if (!IsOnDwmSceneThread() || slotIndex < 0 || slotIndex >= MAX_ANIMATION_SLOTS)
    {
        return;
    }
    struct RenderSnapshot
    {
        ULONGLONG generation;
        ULONGLONG meshRevision;
        HWND hwnd;
        void* matrixTransformProxy;
        WobbleMesh mesh;
    } snapshot = {};
    bool identityUpdate = false;
    bool nativeMeshActive = false;
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    WindowAnimationSlot& slot = g_animationSlots[slotIndex];
    // The slot owns its verified proxy until scene-thread retirement.
    if ((slot.active || slot.retiring) && slot.matrixTransformProxy &&
        HasMatrixTransformUpdate() &&
        (slot.retiring || slot.meshIdentityPending ||
         slot.meshRevision != slot.submittedMeshRevision ||
         g_unloading.load(std::memory_order_acquire)) &&
        (!identityOnly ||
         ((!slot.identityApplied || slot.meshIdentityPending) &&
          (slot.retiring || slot.meshIdentityPending || !slot.mesh.active ||
           g_unloading.load(std::memory_order_acquire)))))
    {
        snapshot.generation = slot.generation;
        snapshot.meshRevision = slot.meshRevision;
        snapshot.hwnd = slot.hwnd;
        snapshot.matrixTransformProxy = slot.matrixTransformProxy;
        nativeMeshActive = slot.settings.rendererMode == RendererMode::Mesh &&
            g_nativeMeshActive.load(std::memory_order_acquire) &&
            g_nativeMeshHwnd.load(std::memory_order_acquire) == slot.hwnd;
        identityUpdate = slot.retiring || slot.meshIdentityPending ||
                         !slot.mesh.active ||
                         g_unloading.load(std::memory_order_acquire);
        if (!identityUpdate)
        {
            snapshot.mesh = slot.mesh;
        }
        // Pin the slot while calling its proxy outside the lock.
        slot.hookUsers++;
    }
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (!snapshot.matrixTransformProxy)
    {
        return;
    }
    auto finishUpdate = [&](long updateResult, bool appliedIdentity)
    {
        ULONGLONG now = GetTickCount64();
        bool logFailure = false;
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        WindowAnimationSlot& currentSlot = g_animationSlots[slotIndex];
        if (currentSlot.generation == snapshot.generation &&
            currentSlot.matrixTransformProxy == snapshot.matrixTransformProxy)
        {
            if (updateResult >= 0)
            {
                currentSlot.submittedMeshRevision = snapshot.meshRevision;
                currentSlot.identityApplied = appliedIdentity;
                if (appliedIdentity && currentSlot.meshRevision == snapshot.meshRevision)
                {
                    currentSlot.meshIdentityPending = false;
                }
            }
            else
            {
                if (now - currentSlot.lastMatrixErrorLog >= 1000)
                {
                    currentSlot.lastMatrixErrorLog = now;
                    logFailure = true;
                }
            }
        }
        ReleaseAnimationSlotPinLocked(currentSlot);
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (logFailure)
        {
            Wh_Log(L"Matrix update failed for HWND=%p: 0x%08X", snapshot.hwnd,
                   static_cast<unsigned int>(updateResult));
        }
    };
    if (identityUpdate)
    {
        MilMatrix3x2D identityMatrix = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
        long updateResult =
            UpdateMatrixTransformProxy(snapshot.matrixTransformProxy, identityMatrix);
        finishUpdate(updateResult, true);
        return;
    }
    if (snapshot.mesh.width <= 0.0 || snapshot.mesh.height <= 0.0)
    {
        finishUpdate(E_INVALIDARG, false);
        return;
    }
    Vec2 renderedCenter = {};
    for (int i = 0; i < GRID_POINT_COUNT; i++)
    {
        renderedCenter.x += snapshot.mesh.points[i].position.x;
        renderedCenter.y += snapshot.mesh.points[i].position.y;
    }
    renderedCenter.x /= static_cast<double>(GRID_POINT_COUNT);
    renderedCenter.y /= static_cast<double>(GRID_POINT_COUNT);
    Vec2 baseCenter = {snapshot.mesh.width * 0.5, snapshot.mesh.height * 0.5};
    double denominatorX = 0.0;
    double denominatorY = 0.0;
    double numeratorXX = 0.0;
    double numeratorXY = 0.0;
    double numeratorYX = 0.0;
    double numeratorYY = 0.0;
    for (int i = 0; i < GRID_POINT_COUNT; i++)
    {
        const Vec2& basePosition = snapshot.mesh.points[i].basePosition;
        double baseX = basePosition.x - baseCenter.x;
        double baseY = basePosition.y - baseCenter.y;
        double renderedX = snapshot.mesh.points[i].position.x - renderedCenter.x;
        double renderedY = snapshot.mesh.points[i].position.y - renderedCenter.y;
        denominatorX += baseX * baseX;
        denominatorY += baseY * baseY;
        numeratorXX += baseX * renderedX;
        numeratorXY += baseX * renderedY;
        numeratorYX += baseY * renderedX;
        numeratorYY += baseY * renderedY;
    }
    double m11 = denominatorX > 0.0001 ? numeratorXX / denominatorX : 1.0;
    double m12 = denominatorX > 0.0001 ? numeratorXY / denominatorX : 0.0;
    double m21 = denominatorY > 0.0001 ? numeratorYX / denominatorY : 0.0;
    double m22 = denominatorY > 0.0001 ? numeratorYY / denominatorY : 1.0;
    // Compress extreme affine values without flattening their response.
    m11 = SmoothCenteredCompression(m11, 1.0, 0.30);
    m12 = SmoothSymmetricCompression(m12, 0.35);
    m21 = SmoothSymmetricCompression(m21, 0.35);
    m22 = SmoothCenteredCompression(m22, 1.0, 0.30);
    // Remote corruption guards; normal motion stays well inside them.
    m11 = std::clamp(m11, 0.10, 2.00);
    m12 = std::clamp(m12, -1.25, 1.25);
    m21 = std::clamp(m21, -1.25, 1.25);
    m22 = std::clamp(m22, 0.10, 2.00);
    if (nativeMeshActive)
    {
        // The curved layer owns deformation. The original visual follows only
        // the physical grab/resize pivot so it cannot jump at direction changes.
        m11 = m22 = 1.0;
        m12 = m21 = 0.0;
    }
    Vec2 translationBase = baseCenter;
    Vec2 translationRendered = renderedCenter;
    bool useSurfacePivot = false;
    if (snapshot.mesh.resizing)
    {
        // Anchor affine resize to the stationary opposite edge.
        if (snapshot.mesh.canWobbleLeft && !snapshot.mesh.canWobbleRight)
        {
            translationBase.x = snapshot.mesh.width;
        }
        else if (snapshot.mesh.canWobbleRight && !snapshot.mesh.canWobbleLeft)
        {
            translationBase.x = 0.0;
        }
        if (snapshot.mesh.canWobbleTop && !snapshot.mesh.canWobbleBottom)
        {
            translationBase.y = snapshot.mesh.height;
        }
        else if (snapshot.mesh.canWobbleBottom && !snapshot.mesh.canWobbleTop)
        {
            translationBase.y = 0.0;
        }
        useSurfacePivot = true;
    }
    else if (snapshot.mesh.dragPointIndex >= 0 && snapshot.mesh.dragPointIndex < GRID_POINT_COUNT)
    {
        const WobblePoint& dragPoint = snapshot.mesh.points[snapshot.mesh.dragPointIndex];
        translationBase = {dragPoint.basePosition.x + snapshot.mesh.dragOffset.x,
                           dragPoint.basePosition.y + snapshot.mesh.dragOffset.y};
        useSurfacePivot = true;
    }
    if (useSurfacePivot)
    {
        double basisX[4] = {};
        double basisY[4] = {};
        CalculateBernsteinBasis(std::clamp(translationBase.x / snapshot.mesh.width, 0.0, 1.0),
                                basisX);
        CalculateBernsteinBasis(std::clamp(translationBase.y / snapshot.mesh.height, 0.0, 1.0),
                                basisY);
        translationRendered = {};
        for (int y = 0; y < GRID_HEIGHT; y++)
        {
            for (int x = 0; x < GRID_WIDTH; x++)
            {
                double weight = basisX[x] * basisY[y];
                const Vec2& position = snapshot.mesh.points[GetPointIndex(x, y)].position;
                translationRendered.x += position.x * weight;
                translationRendered.y += position.y * weight;
            }
        }
    }
    double translationX = translationRendered.x - m11 * translationBase.x - m21 * translationBase.y;
    double translationY = translationRendered.y - m12 * translationBase.x - m22 * translationBase.y;
    // Keep the grab/resize pivot attached, with only a corruption guard.
    double emergencyTranslationLimit =
        std::max(4096.0, std::max(snapshot.mesh.width, snapshot.mesh.height) * 4.0);
    if (!std::isfinite(translationX))
    {
        translationX = 0.0;
    }
    if (!std::isfinite(translationY))
    {
        translationY = 0.0;
    }
    translationX = std::clamp(translationX, -emergencyTranslationLimit, emergencyTranslationLimit);
    translationY = std::clamp(translationY, -emergencyTranslationLimit, emergencyTranslationLimit);
    MilMatrix3x2D matrix = {m11, m12, m21, m22, translationX, translationY};
    long updateResult = UpdateMatrixTransformProxy(snapshot.matrixTransformProxy, matrix);
    finishUpdate(updateResult, false);
    if (g_realDragging.load(std::memory_order_relaxed) &&
        snapshot.hwnd == g_realDraggedWindow.load(std::memory_order_relaxed) &&
        ShouldEmitTimedTrace(g_nextMatrixTraceAt))
    {
        Wh_Log(L"DRAG TRACE MATRIX: HWND=%p revision=%llu result=0x%08X "
               L"m11=%.4f m12=%.4f m21=%.4f m22=%.4f dx=%.2f dy=%.2f",
               snapshot.hwnd, snapshot.meshRevision,
               static_cast<unsigned int>(updateResult), matrix.m11, matrix.m12,
               matrix.m21, matrix.m22, matrix.dx, matrix.dy);
    }
}

static void RestorePendingAnimationIdentities()
{
    if (!IsOnDwmSceneThread())
    {
        return;
    }
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        ApplyAnimationSlotTransform(i, true);
    }
    FinalizeRetiringSlots();
}

static void StopAllAnimations()
{
    int slotsToRetire[MAX_ANIMATION_SLOTS] = {};
    bool sceneCleanupNeeded = false;
    int retireCount = CollectActiveAnimationSlots(slotsToRetire, &sceneCleanupNeeded);
    sceneCleanupNeeded |= g_nativeMeshActive.load(std::memory_order_acquire);
    g_nativeMeshStopRequested.store(true, std::memory_order_release);
    for (int i = 0; i < retireCount; i++)
    {
        RetireAnimationSlot(slotsToRetire[i]);
    }
    if (sceneCleanupNeeded)
    {
        // Publish once even during unload, so native scene ticks can also clean up.
        g_sceneRequestedSerial.fetch_add(1, std::memory_order_acq_rel);
        PostPendingDwmSceneWake(true);
    }
    ULONGLONG hookWaitDeadline = GetTickCount64() + DWM_UNLOAD_CLEANUP_TIMEOUT_MS;
    bool cleanupTimedOut = false;
    for (;;)
    {
        unsigned int pendingIdentities = 0;
        unsigned int retainedProxies = 0;
        unsigned int hookUsers = 0;
        AcquireSRWLockShared(&g_animationSlotsLock);
        for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
        {
            const WindowAnimationSlot& slot = g_animationSlots[i];
            hookUsers += slot.hookUsers;
            if (slot.retiring && slot.matrixTransformProxy)
            {
                retainedProxies++;
                pendingIdentities += !slot.identityApplied;
            }
        }
        ReleaseSRWLockShared(&g_animationSlotsLock);
        // Natural scene passes can finish cleanup before queued wakes are read.
        // Wakes carry only an instance token, not pointers; keep their counter
        // intact for late acknowledgments, but don't mistake them for resources.
        bool wakePending = g_sceneWakeOutstanding.load(std::memory_order_acquire) != 0;
        bool rectActive = g_nativeMeshActive.load(std::memory_order_acquire);
        if (!hookUsers && !retainedProxies && !rectActive)
        {
            break;
        }
        ULONGLONG now = GetTickCount64();
        if (now >= hookWaitDeadline)
        {
            Wh_Log(L"DWM cleanup timeout: pendingIdentities=%u retainedProxies=%u "
                   L"hookUsers=%u wakePending=%d",
                   pendingIdentities, retainedProxies, hookUsers,
                   wakePending);
            cleanupTimedOut = true;
            break;
        }
        ULONGLONG lastWakePost = g_sceneWakePostTimestamp.load(std::memory_order_acquire);
        if ((retainedProxies || rectActive) &&
            (!wakePending || !lastWakePost || now - lastWakePost >= 50))
        {
            PostPendingDwmSceneWake(true);
        }
        Sleep(16);
    }
    FinalizeRetiringSlots();
    if (cleanupTimedOut)
    {
        // Off-scene private calls are unsafe. Drop bookkeeping and intentionally
        // retain any unresolved DWM proxies instead of blocking unload forever.
        AbandonAnimationSlotsAfterSceneStall(L"cleanup timeout");
        if (g_nativeMeshActive.load(std::memory_order_acquire))
            Wh_Log(L"NATIVE RECTANGLE cleanup timed out; retaining DWM resources");
    }
    DisarmAnimationClock();
    ResetDragInputState();
}

static void UpdateAnimationFrame()
{
    if (g_sceneOwnershipResetPending.exchange(false, std::memory_order_acq_rel))
    {
        ResetDragInputState();
    }
    FinalizeRetiringSlots();
    if (!HasAnyAnimationSlots())
    {
        PublishMeshBridgeSnapshot();
        StopAnimationClockIfIdle();
        return;
    }
    ULONGLONG now = GetTickCount64();
    LARGE_INTEGER currentCounter = {};
    if (g_animationFrequency.QuadPart <= 0 || !QueryPerformanceCounter(&currentCounter))
    {
        if (!ArmNextAnimationTick())
        {
            StopAllAnimations();
        }
        return;
    }
    double elapsed = 0.0;
    if (g_lastAnimationCounter.QuadPart == 0)
    {
        g_lastAnimationCounter = currentCounter;
    }
    else
    {
        elapsed = static_cast<double>(currentCounter.QuadPart - g_lastAnimationCounter.QuadPart) /
                  static_cast<double>(g_animationFrequency.QuadPart);
        g_lastAnimationCounter = currentCounter;
        if (!std::isfinite(elapsed) || elapsed < 0.0)
        {
            elapsed = 0.0;
        }
        else if (elapsed > 1.0)
        {
            // Start a new clock epoch after suspend/resume.
            elapsed = 0.0;
        }
    }
    double frameMilliseconds = elapsed * 1000.0;
    // Sample dragged geometry at animation cadence, not WinEvent cadence.
    if (g_realDragging.load(std::memory_order_relaxed))
    {
        HWND draggedWindow = g_realDraggedWindow.load(std::memory_order_relaxed);
        if (draggedWindow)
        {
            HandleLocationChange(draggedWindow, OBJID_WINDOW, CHILDID_SELF);
        }
    }
    HWND sceneWakeWindow = nullptr;
    // Publish only fully integrated local mesh snapshots.
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        struct PhysicsSnapshot
        {
            bool active;
            bool dragging;
            bool freeStepPending;
            bool identityApplied;
            ULONGLONG generation;
            HWND hwnd;
            WobblySettings settings;
            WobbleMesh mesh;
        } snapshot = {};
        AcquireSRWLockShared(&g_animationSlotsLock);
        if (g_animationSlots[i].active)
        {
            const WindowAnimationSlot& slot = g_animationSlots[i];
            snapshot.active = true;
            snapshot.dragging = slot.dragging;
            snapshot.freeStepPending = slot.freeStepPending;
            snapshot.identityApplied = slot.identityApplied;
            snapshot.generation = slot.generation;
            snapshot.hwnd = slot.hwnd;
            snapshot.settings = slot.settings;
            snapshot.mesh = slot.mesh;
        }
        ReleaseSRWLockShared(&g_animationSlotsLock);
        if (!snapshot.active ||
            (!snapshot.dragging && !snapshot.mesh.active && !snapshot.freeStepPending) ||
            frameMilliseconds <= 0.0)
        {
            continue;
        }
        bool wasMeshActive = snapshot.mesh.active;
        WobbleMesh& simulatedMesh = snapshot.mesh;
        double remainingMilliseconds = frameMilliseconds;
        bool simulated = false;
        bool freeStepPending = snapshot.freeStepPending;
        while (remainingMilliseconds > 0.0 &&
               (snapshot.dragging || simulatedMesh.active || freeStepPending))
        {
            double stepMilliseconds = std::min(remainingMilliseconds, MAX_PHYSICS_STEP_MS);
            bool wobbling =
                SimulateMeshStep(simulatedMesh, snapshot.settings, stepMilliseconds / 1000.0);
            simulated = true;
            freeStepPending = false;
            remainingMilliseconds -= stepMilliseconds;
            // Match KWin's free-effect stop threshold.
            if (!snapshot.dragging && !wobbling)
            {
                break;
            }
        }
        if (!simulated)
        {
            continue;
        }
        AcquireSRWLockExclusive(&g_animationSlotsLock);
        WindowAnimationSlot& currentSlot = g_animationSlots[i];
        if (currentSlot.active && currentSlot.generation == snapshot.generation)
        {
            bool renderRevisionNeeded =
                simulatedMesh.active || wasMeshActive != simulatedMesh.active;
            currentSlot.mesh = simulatedMesh;
            currentSlot.freeStepPending = false;
            if (renderRevisionNeeded)
            {
                currentSlot.meshRevision++;
                if ((currentSlot.matrixTransformProxy || currentSlot.proxyCreationPending) &&
                    !sceneWakeWindow)
                {
                    sceneWakeWindow = currentSlot.hwnd;
                }
            }
            if (simulatedMesh.active)
            {
                currentSlot.identityApplied = false;
                currentSlot.meshIdentityPending = false;
            }
            else if (currentSlot.dragging && (wasMeshActive || !snapshot.identityApplied))
            {
                // Keep quiet grabbed state but render identity.
                currentSlot.meshIdentityPending = true;
                currentSlot.identityApplied = false;
            }
        }
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
    }
    PublishMeshBridgeSnapshot();
    int slotsToRetire[MAX_ANIMATION_SLOTS] = {};
    int retireCount = 0;
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        struct RetirementSnapshot
        {
            bool active;
            bool dragging;
            bool meshActive;
            bool freeStepPending;
            bool identityApplied;
            bool proxyCreationPending;
            ULONGLONG generation;
            ULONGLONG nextWindowValidation;
            HWND hwnd;
            void* matrixTransformProxy;
        } snapshot = {};
        AcquireSRWLockShared(&g_animationSlotsLock);
        if (g_animationSlots[i].active)
        {
            const WindowAnimationSlot& slot = g_animationSlots[i];
            snapshot.active = true;
            snapshot.dragging = slot.dragging;
            snapshot.meshActive = slot.mesh.active;
            snapshot.freeStepPending = slot.freeStepPending;
            snapshot.identityApplied = slot.identityApplied;
            snapshot.proxyCreationPending = slot.proxyCreationPending;
            snapshot.generation = slot.generation;
            snapshot.nextWindowValidation = slot.nextWindowValidation;
            snapshot.hwnd = slot.hwnd;
            snapshot.matrixTransformProxy = slot.matrixTransformProxy;
        }
        ReleaseSRWLockShared(&g_animationSlotsLock);
        if (!snapshot.active)
        {
            continue;
        }
        bool invalidWindow = !snapshot.hwnd;
        if (!invalidWindow && now >= snapshot.nextWindowValidation)
        {
            invalidWindow = !IsWindow(snapshot.hwnd);
            AcquireSRWLockExclusive(&g_animationSlotsLock);
            WindowAnimationSlot& currentSlot = g_animationSlots[i];
            if (currentSlot.active && currentSlot.generation == snapshot.generation)
            {
                currentSlot.nextWindowValidation = now + 250;
            }
            ReleaseSRWLockExclusive(&g_animationSlotsLock);
        }
        if (invalidWindow ||
            (!snapshot.dragging && !snapshot.meshActive && !snapshot.freeStepPending))
        {
            slotsToRetire[retireCount++] = i;
            continue;
        }
        if (!snapshot.meshActive)
        {
            if (!snapshot.identityApplied)
            {
                AcquireSRWLockExclusive(&g_animationSlotsLock);
                WindowAnimationSlot& currentSlot = g_animationSlots[i];
                if (currentSlot.active && currentSlot.generation == snapshot.generation &&
                    !currentSlot.meshIdentityPending)
                {
                    currentSlot.meshIdentityPending = true;
                    currentSlot.identityApplied = false;
                    currentSlot.meshRevision++;
                    if ((snapshot.matrixTransformProxy || snapshot.proxyCreationPending) &&
                        !sceneWakeWindow)
                    {
                        sceneWakeWindow = currentSlot.hwnd;
                    }
                }
                ReleaseSRWLockExclusive(&g_animationSlotsLock);
            }
            continue;
        }
    }
    if (!sceneWakeWindow)
    {
        AcquireSRWLockShared(&g_animationSlotsLock);
        for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
        {
            const WindowAnimationSlot& slot = g_animationSlots[i];
            if (slot.retiring && slot.matrixTransformProxy && !slot.identityApplied)
            {
                sceneWakeWindow = slot.hwnd;
                break;
            }
        }
        ReleaseSRWLockShared(&g_animationSlotsLock);
    }
    if (sceneWakeWindow)
    {
        ULONGLONG scenePassCounter = g_scenePassCounter.load(std::memory_order_acquire);
        if (g_lastSceneProgressTimestamp == 0 || scenePassCounter != g_lastObservedScenePassCounter)
        {
            g_lastObservedScenePassCounter = scenePassCounter;
            g_lastSceneProgressTimestamp = now;
        }
        if (now - g_lastSceneProgressTimestamp >= DWM_SCENE_STALL_TIMEOUT_MS)
        {
            // Keep disable/update bounded if DWM stops servicing scene work
            // while several transformed windows are settling.
            BeginSceneStallCleanup(L"no scene progress");
        }
        RequestDwmScenePass();
        ULONGLONG lastWakePost = g_sceneWakePostTimestamp.load(std::memory_order_acquire);
        ULONGLONG lastSceneActivity = std::max(g_lastSceneProgressTimestamp, lastWakePost);
        if (sceneWakeWindow && now - lastSceneActivity >= 50)
        {
            if (!g_sceneWakeStalled.exchange(true, std::memory_order_acq_rel))
            {
                g_sceneWakeStallStartedAt.store(now, std::memory_order_release);
                Wh_Log(L"DWM scene wake stalled: requested=%llu "
                       L"submitted=%llu; reposting",
                       g_sceneRequestedSerial.load(std::memory_order_acquire),
                       g_sceneSubmittedSerial.load(std::memory_order_acquire));
            }
            PostPendingDwmSceneWake(true);
        }
    }
    else if (!g_sceneRecoveryCleanupPending.load(std::memory_order_acquire))
    {
        // A quiet grabbed window has no new render work. Start any later
        // stall measurement when work resumes, not at the last idle frame.
        g_lastSceneProgressTimestamp = 0;
    }
    for (int i = 0; i < retireCount; i++)
    {
        RetireAnimationSlot(slotsToRetire[i]);
    }
    FinalizeRetiringSlots();
    StopAnimationClockIfIdle();
    if (g_animationClockArmed && !ArmNextAnimationTick())
    {
        Wh_Log(L"Failed to schedule animation frame: %u", GetLastError());
        StopAllAnimations();
    }
}

static void HandleLocationChange(HWND hwnd, LONG idObject, LONG idChild)
{
    HWND draggedWindow = g_realDraggedWindow.load(std::memory_order_relaxed);
    if (!g_realDragging.load(std::memory_order_relaxed) || !draggedWindow ||
        hwnd != draggedWindow || g_dragAnimationSlot < 0 ||
        g_dragAnimationSlot >= MAX_ANIMATION_SLOTS || idObject != OBJID_WINDOW ||
        idChild != CHILDID_SELF)
    {
        return;
    }
    int slotIndex = g_dragAnimationSlot;
    RECT rect = {};
    if (!GetWindowRect(hwnd, &rect))
    {
        return;
    }
    int originalWidth = g_realDraggedWindowRect.right - g_realDraggedWindowRect.left;
    int originalHeight = g_realDraggedWindowRect.bottom - g_realDraggedWindowRect.top;
    int currentWidth = rect.right - rect.left;
    int currentHeight = rect.bottom - rect.top;
    if (currentWidth <= 0 || currentHeight <= 0)
    {
        return;
    }
    bool rectChanged = !EqualRect(&rect, &g_lastDraggedWindowRect);
    bool resizeDetectedThisEvent = false;
    if (!g_moveTypeKnown && rectChanged)
    {
        g_realResizing = !g_dragStartedWindowZoomed &&
                         (currentWidth != originalWidth || currentHeight != originalHeight);
        if (g_realResizing)
        {
            g_resizeCoordinateScale = GetResizeVisualCoordinateScale(hwnd, rect);
            resizeDetectedThisEvent = true;
        }
        g_moveTypeKnown = true;
    }
    POINT mousePosition = {};
    if (!GetCursorPos(&mousePosition))
    {
        return;
    }
    bool mouseMoved =
        mousePosition.x != g_lastMousePosition.x || mousePosition.y != g_lastMousePosition.y;
    LPARAM nativeTargetFlags =
        g_pendingInteractiveTransitionWindow.load(std::memory_order_acquire) == hwnd
            ? g_pendingInteractiveTransitionFlags.load(std::memory_order_relaxed)
            : 0;
    constexpr LPARAM nativeTargetMask = NATIVE_TRANSITION_TARGET_IS_WORK_AREA |
                                        NATIVE_TRANSITION_TARGET_IS_SNAP_LAYOUT;
    bool nativeTargetPending = (nativeTargetFlags & nativeTargetMask) != 0;
    MonitorEdgeState mouseEdgeState = GetPointMonitorEdgeState(mousePosition);
    bool windowZoomed = IsZoomed(hwnd) != FALSE;
    bool pointerSnapPreviewAbandoned =
        g_interactiveStateThrobFromPointerEdge &&
        (g_interactiveStateThrob == InteractiveStateThrobKind::Maximize ||
         g_interactiveStateThrob == InteractiveStateThrobKind::Snap) &&
        !mouseEdgeState.top && !mouseEdgeState.side && !windowZoomed &&
        !IsApproximatelyMonitorWorkArea(rect) &&
        !IsApproximatelySnapLayoutTarget(rect);
    if (pointerSnapPreviewAbandoned)
    {
        HWND expectedWindow = hwnd;
        g_pendingInteractiveTransitionWindow.compare_exchange_strong(
            expectedWindow, nullptr, std::memory_order_acq_rel,
            std::memory_order_acquire);
        nativeTargetFlags = 0;
        nativeTargetPending = false;
    }
    bool sizeChangedFromStart = currentWidth != originalWidth || currentHeight != originalHeight;
    if (!g_dragStartedWindowZoomed && g_moveTypeKnown && !g_realResizing &&
        sizeChangedFromStart &&
        IsCursorOnResizableFrame(hwnd, mousePosition, rect, true) &&
        MonitorFromPoint(mousePosition, MONITOR_DEFAULTTONEAREST) == g_dragCursorMonitor)
    {
        g_realResizing = true;
        g_resizeCoordinateScale = GetResizeVisualCoordinateScale(hwnd, rect);
        resizeDetectedThisEvent = true;
    }
    if (g_realResizing && !g_dragResizeWobbleEnabled)
    {
        RetireAnimationSlot(slotIndex);
        return;
    }
    if (!rectChanged && !mouseMoved && !nativeTargetPending &&
        !pointerSnapPreviewAbandoned)
    {
        return;
    }
    bool nativeMaximizeIntent =
        (nativeTargetFlags & NATIVE_TRANSITION_TARGET_IS_WORK_AREA) != 0;
    bool nativeSnapIntent =
        (nativeTargetFlags & NATIVE_TRANSITION_TARGET_IS_SNAP_LAYOUT) != 0;
    bool maximizeIntent = nativeMaximizeIntent ||
                          (mouseEdgeState.top && !mouseEdgeState.side && !nativeSnapIntent);
    bool snapIntent = nativeSnapIntent || mouseEdgeState.side;
    Vec2 snapIntentDirection = nativeSnapIntent
                                   ? DecodeWindowTransitionDirection(nativeTargetFlags)
                                   : mouseEdgeState.direction;
    if (snapIntentDirection.x == 0.0 && snapIntentDirection.y == 0.0)
    {
        snapIntentDirection = mouseEdgeState.direction;
    }
    double coordinateScale = g_realResizing ? g_resizeCoordinateScale : 1.0;
    double windowDeltaX =
        static_cast<double>(rect.left - g_lastDraggedWindowRect.left) * coordinateScale;
    double windowDeltaY =
        static_cast<double>(rect.top - g_lastDraggedWindowRect.top) * coordinateScale;
    int previousWidth = g_lastDraggedWindowRect.right - g_lastDraggedWindowRect.left;
    int previousHeight = g_lastDraggedWindowRect.bottom - g_lastDraggedWindowRect.top;
    bool sizeChanged = currentWidth != previousWidth || currentHeight != previousHeight;
    bool zoomStateChanged = windowZoomed != g_lastDraggedWindowZoomed;
    if (g_waitingForInitialRestore && !windowZoomed &&
        !IsApproximatelyMonitorWorkArea(rect))
    {
        g_waitingForInitialRestore = false;
    }
    ULONGLONG now = GetTickCount64();
    HMONITOR cursorMonitor = MonitorFromPoint(mousePosition, MONITOR_DEFAULTTONEAREST);
    bool monitorChanged = cursorMonitor && cursorMonitor != g_dragCursorMonitor;
    if (monitorChanged)
    {
        g_dragCursorMonitor = cursorMonitor;
        g_monitorTransitionRebaseUntil = now + 750;
        RetargetAnimationClock(cursorMonitor);
    }
    bool dpiReflow = !g_realResizing && sizeChanged && !zoomStateChanged &&
                     g_interactiveStateThrob == InteractiveStateThrobKind::None;
    if (dpiReflow)
    {
        g_monitorTransitionRebaseUntil = now + 750;
    }
    bool largeNativeCatchUp = std::abs(windowDeltaX) > 64.0 || std::abs(windowDeltaY) > 64.0;
    bool rebaseMonitorTransition =
        !g_realResizing && now <= g_monitorTransitionRebaseUntil &&
        (monitorChanged || dpiReflow || largeNativeCatchUp);
    Vec2 localMousePosition = {static_cast<double>(mousePosition.x - rect.left) * coordinateScale,
                               static_cast<double>(mousePosition.y - rect.top) * coordinateScale};
    double currentMeshWidth = static_cast<double>(currentWidth) * coordinateScale;
    double currentMeshHeight = static_cast<double>(currentHeight) * coordinateScale;
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    WindowAnimationSlot& slot = g_animationSlots[slotIndex];
    if (!slot.active || slot.hwnd != hwnd)
    {
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        return;
    }
    if (!slot.transformAttached)
    {
        // Don't accumulate invisible deformation while an existing window's
        // DWM visual is being discovered and bound for the first time.
        InitializeMesh(slot.mesh, currentMeshWidth, currentMeshHeight);
        BeginDrag(slot.mesh, localMousePosition);
        SetMeshResizeMode(slot.mesh, g_realResizing);
        if (g_realResizing)
        {
            UpdateResizeEdges(slot.mesh, g_realDraggedWindowRect, rect);
            ApplyResizeConstraints(slot.mesh);
        }
        slot.identityApplied = false;
        slot.meshIdentityPending = false;
        slot.meshRevision++;
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        if (ShouldEmitTimedTrace(g_nextDragTraceAt))
            Wh_Log(L"DRAG TRACE LOCATION: HWND=%p Slot=%d transformAttached=0 "
                   L"delta=(%.1f,%.1f) resizing=%d",
                   hwnd, slotIndex, windowDeltaX, windowDeltaY, g_realResizing);
        g_lastDraggedWindowRect = rect;
        g_lastDraggedWindowZoomed = IsZoomed(hwnd) != FALSE;
        g_lastMousePosition = mousePosition;
        RequestDwmScenePass();
        return;
    }
    auto startInteractiveStateThrob = [&](InteractiveStateThrobKind kind, bool maximizing,
                                          Vec2 direction)
    {
        InitializeMesh(slot.mesh, static_cast<double>(currentWidth),
                       static_cast<double>(currentHeight));
        ApplyWindowStateThrob(slot.mesh, maximizing, true, slot.settings, direction);
        ClearWindowStateThrobConstraints(slot.mesh);
        BeginDrag(slot.mesh, localMousePosition);
        slot.windowStateThrob = true;
        g_interactiveStateThrob = kind;
        g_interactiveStateThrobDirection = EncodeWindowTransitionDirection(direction);
        g_interactiveStateThrobFromPointerEdge =
            mouseEdgeState.top || mouseEdgeState.side;
    };
    bool resumeNormalDrag =
        (g_interactiveStateThrob == InteractiveStateThrobKind::Restore &&
         slot.windowStateThrob && !sizeChanged) ||
        pointerSnapPreviewAbandoned;
    if (resumeNormalDrag)
    {
        slot.windowStateThrob = false;
        g_interactiveStateThrob = InteractiveStateThrobKind::None;
        g_interactiveStateThrobDirection = 0;
        g_interactiveStateThrobFromPointerEdge = false;
    }
    bool canStartInteractiveStateThrob = slot.settings.windowStateWobbleEnabled &&
                                         g_moveTypeKnown && !g_realResizing &&
                                         (!g_waitingForInitialRestore || nativeMaximizeIntent ||
                                          nativeSnapIntent);
    if (canStartInteractiveStateThrob && maximizeIntent &&
        g_interactiveStateThrob != InteractiveStateThrobKind::Maximize)
    {
        // Start maximize wobble as soon as Aero Snap activates.
        g_waitingForInitialRestore = false;
        startInteractiveStateThrob(InteractiveStateThrobKind::Maximize, true, {0.0, -1.0});
    }
    else if (canStartInteractiveStateThrob && snapIntent &&
             (g_interactiveStateThrob != InteractiveStateThrobKind::Snap ||
              g_interactiveStateThrobDirection !=
                  EncodeWindowTransitionDirection(snapIntentDirection)))
    {
        g_waitingForInitialRestore = false;
        startInteractiveStateThrob(InteractiveStateThrobKind::Snap, true,
                                   snapIntentDirection);
    }
    else if (g_realResizing)
    {
        if (resizeDetectedThisEvent)
        {
            // Rebase custom-title-bar resize state into local DPI coordinates.
            InitializeMesh(slot.mesh, currentMeshWidth, currentMeshHeight);
            BeginDrag(slot.mesh, localMousePosition);
            windowDeltaX = 0.0;
            windowDeltaY = 0.0;
        }
        if (!slot.mesh.resizing)
        {
            SetMeshResizeMode(slot.mesh, true);
        }
        // Preserve screen-space points during top/left resize.
        OffsetMeshPositions(slot.mesh, -windowDeltaX, -windowDeltaY);
        UpdateMeshBaseGrid(slot.mesh, currentMeshWidth, currentMeshHeight);
        UpdateResizeEdges(slot.mesh, g_realDraggedWindowRect, rect);
        ApplyResizeConstraints(slot.mesh);
    }
    else if (g_interactiveStateThrob != InteractiveStateThrobKind::None &&
             slot.windowStateThrob)
    {
        // Native Snap can resize the HWND before IsZoomed changes. Keep the
        // already-seeded state pulse instead of replacing it with drag wobble.
        if (sizeChanged)
        {
            ResizeMeshPreservingDeformation(slot.mesh, currentMeshWidth, currentMeshHeight);
        }
        UpdateMeshDragOffset(slot.mesh, localMousePosition);
    }
    else if (rebaseMonitorTransition)
    {
        // Per-monitor DPI changes can make the native window catch up in one
        // large step. Treat that step as new geometry, not as wobble velocity.
        InitializeMesh(slot.mesh, static_cast<double>(currentWidth),
                       static_cast<double>(currentHeight));
        BeginDrag(slot.mesh, localMousePosition);
        slot.windowStateThrob = false;
    }
    else if (sizeChanged)
    {
        // Only IsZoomed transitions trigger state wobble during a move.
        InitializeMesh(slot.mesh, static_cast<double>(currentWidth),
                       static_cast<double>(currentHeight));
        if (zoomStateChanged && slot.settings.windowStateWobbleEnabled)
        {
            ApplyWindowStateThrob(slot.mesh, windowZoomed, false, slot.settings,
                                  windowZoomed ? Vec2{0.0, -1.0} : Vec2{0.0, 1.0});
            ClearWindowStateThrobConstraints(slot.mesh);
            slot.windowStateThrob = true;
            g_interactiveStateThrob = windowZoomed ? InteractiveStateThrobKind::Maximize
                                                    : InteractiveStateThrobKind::Restore;
            g_interactiveStateThrobDirection = EncodeWindowTransitionDirection(
                windowZoomed ? Vec2{0.0, -1.0} : Vec2{0.0, 1.0});
            g_interactiveStateThrobFromPointerEdge = false;
        }
        BeginDrag(slot.mesh, localMousePosition);
    }
    else
    {
        OffsetMeshPositions(slot.mesh, -windowDeltaX, -windowDeltaY);
    }
    slot.mesh.active = true;
    slot.identityApplied = false;
    slot.meshIdentityPending = false;
    slot.meshRevision++;
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (ShouldEmitTimedTrace(g_nextDragTraceAt))
        Wh_Log(L"DRAG TRACE LOCATION: HWND=%p Slot=%d transformAttached=1 "
               L"delta=(%.1f,%.1f) resizing=%d rebase=%d",
               hwnd, slotIndex, windowDeltaX, windowDeltaY,
               g_realResizing, rebaseMonitorTransition);
    g_lastDraggedWindowRect = rect;
    g_lastDraggedWindowZoomed = windowZoomed;
    g_lastMousePosition = mousePosition;
}

static void HandleMoveSizeEnd(HWND hwnd)
{
    HWND draggedWindow = g_realDraggedWindow.load(std::memory_order_relaxed);
    if (!g_realDragging.load(std::memory_order_relaxed) || hwnd != draggedWindow ||
        g_dragAnimationSlot < 0 || g_dragAnimationSlot >= MAX_ANIMATION_SLOTS)
    {
        return;
    }
    int slotIndex = g_dragAnimationSlot;
    POINT releaseMousePosition = {};
    bool hasReleaseMousePosition = !g_realResizing && GetCursorPos(&releaseMousePosition);
    MonitorEdgeState releaseEdgeState = hasReleaseMousePosition
                                            ? GetPointMonitorEdgeState(releaseMousePosition)
                                            : MonitorEdgeState{};
    if (g_hasLastMousePosition && (!releaseEdgeState.top || !releaseEdgeState.side))
    {
        // MOVESIZEEND may arrive after the cursor leaves the edge.
        MonitorEdgeState lastDragEdgeState = GetPointMonitorEdgeState(g_lastMousePosition);
        releaseEdgeState.top = releaseEdgeState.top || lastDragEdgeState.top;
        releaseEdgeState.side = releaseEdgeState.side || lastDragEdgeState.side;
        if (releaseEdgeState.direction.x == 0.0)
        {
            releaseEdgeState.direction.x = lastDragEdgeState.direction.x;
        }
        if (releaseEdgeState.direction.y == 0.0)
        {
            releaseEdgeState.direction.y = lastDragEdgeState.direction.y;
        }
    }
    bool releasedAtMonitorTopEdge =
        !g_realResizing && releaseEdgeState.top && !releaseEdgeState.side;
    bool releasedAtMonitorSideEdge = !g_realResizing && releaseEdgeState.side;
    constexpr LONG fastDragSnapBand = 24;
    MonitorEdgeState releaseSnapBand =
        hasReleaseMousePosition
            ? GetPointMonitorEdgeState(releaseMousePosition, fastDragSnapBand)
            : MonitorEdgeState{};
    MonitorEdgeState lastDragSnapBand =
        g_hasLastMousePosition
            ? GetPointMonitorEdgeState(g_lastMousePosition, fastDragSnapBand)
            : MonitorEdgeState{};
    bool fastTopSnapIntent =
        !g_realResizing &&
        ((releaseSnapBand.top && !releaseSnapBand.side) ||
         (lastDragSnapBand.top && !lastDragSnapBand.side));
    // Publish final geometry before releasing the grabbed point.
    g_finalizingMoveSize = true;
    HandleObservedWindowLocationChange(hwnd, OBJID_WINDOW, CHILDID_SELF);
    g_finalizingMoveSize = false;
    HandleLocationChange(hwnd, OBJID_WINDOW, CHILDID_SELF);
    // Late Snap notifications can now use the normal queue.
    g_realDragging.store(false, std::memory_order_release);
    HWND pendingInteractiveTransitionWindow =
        g_pendingInteractiveTransitionWindow.exchange(nullptr, std::memory_order_acq_rel);
    LPARAM pendingInteractiveTransitionFlags =
        g_pendingInteractiveTransitionFlags.exchange(0, std::memory_order_relaxed);
    bool hasPendingInteractiveTarget = pendingInteractiveTransitionWindow == hwnd;
    bool pendingInteractiveMaximize =
        hasPendingInteractiveTarget &&
        (pendingInteractiveTransitionFlags & NATIVE_TRANSITION_TARGET_IS_WORK_AREA) != 0;
    bool pendingInteractiveSnap =
        hasPendingInteractiveTarget &&
        (pendingInteractiveTransitionFlags & NATIVE_TRANSITION_TARGET_IS_SNAP_LAYOUT) != 0;
    bool animate = false;
    bool stateTransitionWobble = false;
    bool snapTransitionWobble = false;
    bool transitionExpectedZoomed = false;
    LPARAM transitionReplayFlags = 0;
    RECT releaseRect = {};
    bool hasReleaseRect = GetWindowRect(hwnd, &releaseRect) != FALSE;
    // Final geometry detects Snap even when edge notifications coalesce.
    bool releasedAtSnapGeometry = !g_realResizing && hasReleaseRect && !IsZoomed(hwnd) &&
                                  IsEligibleWindowForStateThrob(hwnd) &&
                                  IsApproximatelySnapLayoutTarget(releaseRect);
    bool releasedIntoSnapTarget =
        !g_realResizing && (releasedAtMonitorSideEdge || pendingInteractiveSnap ||
                            releasedAtSnapGeometry);
    bool releasedIntoMaximizeTarget =
        !g_realResizing && (releasedAtMonitorTopEdge || fastTopSnapIntent ||
                            pendingInteractiveMaximize);
    Vec2 snapDirection = pendingInteractiveSnap
                             ? DecodeWindowTransitionDirection(
                                   pendingInteractiveTransitionFlags)
                             : Vec2{};
    if (snapDirection.x == 0.0 && snapDirection.y == 0.0)
    {
        snapDirection = releaseEdgeState.direction;
    }
    if (snapDirection.x == 0.0 && snapDirection.y == 0.0)
    {
        snapDirection = DecodeWindowTransitionDirection(
            EncodeWindowTransitionDirection(g_realDraggedWindowRect, releaseRect));
    }
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    WindowAnimationSlot& slot = g_animationSlots[slotIndex];
    if (!slot.active || slot.hwnd != hwnd)
    {
        ReleaseSRWLockExclusive(&g_animationSlotsLock);
        ResetDragInputState();
        return;
    }
    bool wasDragging = slot.mesh.dragging;
    if (slot.settings.windowStateWobbleEnabled && releasedIntoMaximizeTarget)
    {
        int releaseWidth = releaseRect.right - releaseRect.left;
        int releaseHeight = releaseRect.bottom - releaseRect.top;
        if (ReplaceAnimationWithStateThrob(slot, releaseWidth, releaseHeight, true,
                                           {0.0, -1.0}))
        {
            animate = slot.matrixTransformProxy != nullptr;
            stateTransitionWobble = true;
            transitionExpectedZoomed = true;
            transitionReplayFlags = NATIVE_TRANSITION_TARGET_IS_WORK_AREA |
                                    NATIVE_TRANSITION_DIRECTION_UP;
        }
        else if (wasDragging)
        {
            EndDrag(slot.mesh);
            animate = slot.matrixTransformProxy != nullptr;
        }
    }
    else if (slot.settings.windowStateWobbleEnabled && releasedIntoSnapTarget)
    {
        int releaseWidth = releaseRect.right - releaseRect.left;
        int releaseHeight = releaseRect.bottom - releaseRect.top;
        if (ReplaceAnimationWithStateThrob(slot, releaseWidth, releaseHeight, true,
                                           snapDirection))
        {
            animate = slot.matrixTransformProxy != nullptr;
            snapTransitionWobble = true;
            transitionReplayFlags = NATIVE_TRANSITION_TARGET_IS_SNAP_LAYOUT |
                                    EncodeWindowTransitionDirection(snapDirection);
        }
    }
    else
    {
        // Preserve restore pulses after release.
        stateTransitionWobble = slot.windowStateThrob;
        transitionExpectedZoomed = false;
        if (wasDragging)
        {
            EndDrag(slot.mesh);
        }
        animate = wasDragging && slot.matrixTransformProxy;
    }
    if (animate)
    {
        slot.dragging = false;
        slot.freeStepPending = true;
        slot.order = ++g_animationOrderCounter;
    }
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (stateTransitionWobble && animate)
    {
        MarkObservedWindowTransitionPending(hwnd, transitionExpectedZoomed);
        RequestDwmScenePass();
    }
    if (snapTransitionWobble && animate)
    {
        MarkObservedSnapTransition(hwnd, false);
        RequestDwmScenePass();
    }
    if (stateTransitionWobble && transitionExpectedZoomed)
    {
        const wchar_t* source = pendingInteractiveMaximize
                                    ? L"NATIVE_TARGET"
                                : releasedAtMonitorTopEdge
                                    ? L"CURSOR_EDGE"
                                    : L"FAST_DRAG_BAND";
        Wh_Log(L"WINDOW STATE THROB HWND=%p State=MAXIMIZED Source=%s", hwnd,
               source);
    }
    else if (snapTransitionWobble)
    {
        Wh_Log(L"WINDOW STATE THROB HWND=%p State=SNAPPED "
               L"Source=INTERACTIVE_EDGE_OR_FINAL_GEOMETRY",
               hwnd);
    }
    if (!animate)
    {
        ResetDragInputState();
        RetireAnimationSlot(slotIndex);
        return;
    }
    ResetDragInputState();
    // Re-sample the destination monitor's animation rate at release.
    if (!EnsureAnimationClockRunning(hwnd))
    {
        Wh_Log(L"Failed to continue free wobble animation");
        RetireAnimationSlot(slotIndex);
        return;
    }
    if (transitionReplayFlags != 0)
    {
        // The native transition hook often fires before MOVESIZEEND. Replay it
        // once after release so the clean state pulse binds to the new visual.
        QueueNativeWindowTransition(hwnd, transitionReplayFlags);
    }
}

static int FindObservedWindowState(HWND hwnd)
{
    for (int i = 0; i < MAX_OBSERVED_WINDOWS; i++)
    {
        if (g_observedWindows[i].hwnd == hwnd)
        {
            return i;
        }
    }
    return -1;
}

static void ForgetObservedWindowState(HWND hwnd)
{
    int index = FindObservedWindowState(hwnd);
    if (index >= 0)
    {
        g_observedWindows[index] = {};
    }
}

static int RememberObservedWindowState(HWND hwnd)
{
    if (!IsEligibleWindowForStateThrob(hwnd))
    {
        return -1;
    }
    RECT rect = {};
    if (!GetWindowRect(hwnd, &rect))
    {
        return -1;
    }
    int index = FindObservedWindowState(hwnd);
    if (index < 0)
    {
        ULONGLONG oldestTimestamp = ULLONG_MAX;
        for (int i = 0; i < MAX_OBSERVED_WINDOWS; i++)
        {
            if (!g_observedWindows[i].hwnd)
            {
                index = i;
                break;
            }
            if (g_observedWindows[i].lastSeen < oldestTimestamp)
            {
                oldestTimestamp = g_observedWindows[i].lastSeen;
                index = i;
            }
        }
    }
    if (index < 0)
    {
        return -1;
    }
    g_observedWindows[index] = {
        hwnd, IsZoomed(hwnd) != FALSE, IsIconic(hwnd) != FALSE,
        IsApproximatelySnapLayoutTarget(rect), false, false,
        rect, GetTickCount64(), 0, 0, 0, 0};
    return index;
}

static int EnsureObservedWindowState(HWND hwnd)
{
    int index = FindObservedWindowState(hwnd);
    return index >= 0 ? index : RememberObservedWindowState(hwnd);
}

static void CancelWindowStateThrobForWindow(HWND hwnd)
{
    int slotIndex = FindAnimationSlotMatching(
        [hwnd](const WindowAnimationSlot& slot, int)
        { return slot.active && slot.hwnd == hwnd && slot.windowStateThrob && !slot.dragging; });
    if (slotIndex >= 0)
    {
        RetireAnimationSlot(slotIndex);
    }
}

static void ResetObservedSnapStateForInteractiveMove(HWND hwnd)
{
    int index = EnsureObservedWindowState(hwnd);
    if (index < 0)
    {
        return;
    }
    ObservedWindowState& observed = g_observedWindows[index];
    observed.snapped = false;
    observed.snapTransitionDeadline = 0;
    observed.nativeTransitionPending = false;
    observed.nativeTransitionDeadline = 0;
    observed.nativeTransitionStartedAt = 0;
    observed.lastSeen = GetTickCount64();
}

static bool ShouldSuppressWindowStateThrob(HWND hwnd)
{
    int index = FindObservedWindowState(hwnd);
    if (index < 0)
    {
        return false;
    }
    const ObservedWindowState& observed = g_observedWindows[index];
    return observed.iconic || GetTickCount64() <= observed.suppressStateThrobUntil;
}

static void HandleMinimizeLifecycle(HWND hwnd, bool minimizing)
{
    if (!hwnd || !IsWindow(hwnd))
    {
        return;
    }
    int index = FindObservedWindowState(hwnd);
    if (index < 0)
    {
        RememberObservedWindowState(hwnd);
        index = FindObservedWindowState(hwnd);
    }
    if (index < 0)
    {
        return;
    }
    ULONGLONG now = GetTickCount64();
    ObservedWindowState& observed = g_observedWindows[index];
    observed.iconic = minimizing;
    observed.nativeTransitionPending = false;
    observed.nativeTransitionDeadline = 0;
    observed.snapTransitionDeadline = 0;
    observed.suppressStateThrobUntil = now + 500;
    observed.lastSeen = now;
    if (!minimizing)
    {
        // Sync taskbar restore without triggering maximize wobble.
        observed.zoomed = IsZoomed(hwnd) != FALSE;
        RECT rect = {};
        if (GetWindowRect(hwnd, &rect))
        {
            observed.rect = rect;
            observed.snapped = !observed.zoomed && IsApproximatelySnapLayoutTarget(rect);
        }
    }
    // Cancel only a transition pulse racing taskbar restore.
    CancelWindowStateThrobForWindow(hwnd);
    if (!minimizing)
    {
        Wh_Log(L"TASKBAR RESTORE: window-state wobble suppressed "
               L"for HWND=%p",
               hwnd);
    }
}

static void MarkObservedWindowTransitionPending(HWND hwnd, bool expectedZoomed)
{
    int index = EnsureObservedWindowState(hwnd);
    if (index < 0)
    {
        return;
    }
    ULONGLONG now = GetTickCount64();
    g_observedWindows[index].nativeTransitionPending = true;
    g_observedWindows[index].expectedZoomed = expectedZoomed;
    g_observedWindows[index].nativeTransitionDeadline = now + 750;
    g_observedWindows[index].nativeTransitionStartedAt = 0;
    g_observedWindows[index].lastSeen = now;
}

static void MarkObservedSnapTransition(HWND hwnd, bool transitionStarted)
{
    int index = EnsureObservedWindowState(hwnd);
    if (index < 0)
    {
        return;
    }
    ULONGLONG now = GetTickCount64();
    ObservedWindowState& observed = g_observedWindows[index];
    // Suppress duplicate native Snap notifications briefly.
    observed.snapTransitionDeadline = now + 300;
    // Consume the IsZoomed edge as part of this Snap pulse.
    observed.nativeTransitionPending = true;
    observed.expectedZoomed = false;
    observed.nativeTransitionDeadline = now + 750;
    observed.nativeTransitionStartedAt = transitionStarted ? now : 0;
    observed.lastSeen = now;
}

static void StartWindowStateThrob(HWND hwnd, bool maximizing, bool snapTransition = false,
                                  Vec2 direction = {})
{
    if (!hwnd || g_realDragging.load(std::memory_order_relaxed) ||
        !IsEligibleWindowForStateThrob(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
        ShouldSuppressWindowStateThrob(hwnd))
    {
        return;
    }
    RECT rect = {};
    if (!GetWindowRect(hwnd, &rect))
    {
        return;
    }
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }
    WobblySettings settings = GetSettingsSnapshot();
    if (!settings.windowStateWobbleEnabled)
    {
        return;
    }
    Vec2 centre = {static_cast<double>(width) * 0.5, static_cast<double>(height) * 0.5};
    int slotIndex = AcquireAnimationSlot(hwnd, settings, static_cast<double>(width),
                                         static_cast<double>(height), centre);
    if (slotIndex < 0)
    {
        return;
    }
    bool started = false;
    AcquireSRWLockExclusive(&g_animationSlotsLock);
    WindowAnimationSlot& slot = g_animationSlots[slotIndex];
    if (slot.active && slot.hwnd == hwnd)
    {
        started = ReplaceAnimationWithStateThrob(slot, width, height, maximizing, direction);
    }
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (!started)
    {
        RetireAnimationSlot(slotIndex);
        return;
    }
    if (!EnsureAnimationClockRunning(hwnd))
    {
        RetireAnimationSlot(slotIndex);
        return;
    }
    RequestDwmScenePass();
    Wh_Log(L"WINDOW STATE THROB HWND=%p State=%s Size=%dx%d", hwnd,
           snapTransition ? L"SNAPPED" : (maximizing ? L"MAXIMIZED" : L"RESTORED"), width,
           height);
}

static void HandleNativeWindowTransitionStart(HWND hwnd, LPARAM transitionFlags)
{
    if (!hwnd || !IsWindow(hwnd) || g_realDragging.load(std::memory_order_relaxed))
    {
        return;
    }
    int index = EnsureObservedWindowState(hwnd);
    ULONGLONG now = GetTickCount64();
    if (index >= 0 && (g_observedWindows[index].iconic ||
                       now <= g_observedWindows[index].suppressStateThrobUntil))
    {
        ObservedWindowState& observed = g_observedWindows[index];
        if (!IsIconic(hwnd))
        {
            observed.iconic = false;
            observed.zoomed = IsZoomed(hwnd) != FALSE;
        }
        observed.nativeTransitionPending = false;
        observed.nativeTransitionDeadline = 0;
        observed.lastSeen = now;
        CancelWindowStateThrobForWindow(hwnd);
        return;
    }
    bool targetIsWorkArea = (transitionFlags & NATIVE_TRANSITION_TARGET_IS_WORK_AREA) != 0;
    bool sourceIsWorkArea = (transitionFlags & NATIVE_TRANSITION_SOURCE_IS_WORK_AREA) != 0;
    bool windowIsZoomed = (transitionFlags & NATIVE_TRANSITION_WINDOW_IS_ZOOMED) != 0;
    bool targetIsSnapLayout = (transitionFlags & NATIVE_TRANSITION_TARGET_IS_SNAP_LAYOUT) != 0;
    if (targetIsSnapLayout)
    {
        if (index >= 0 && now < g_observedWindows[index].snapTransitionDeadline &&
            g_observedWindows[index].nativeTransitionStartedAt != 0)
        {
            return;
        }
        // Re-seed an early interactive pulse on the native transition visual.
        MarkObservedSnapTransition(hwnd, true);
        StartWindowStateThrob(hwnd, true, true, DecodeWindowTransitionDirection(transitionFlags));
        return;
    }
    bool previouslyZoomed = index >= 0 && g_observedWindows[index].zoomed;
    bool maximizing = false;
    bool recognizedStateTransition = false;
    if (index >= 0 && windowIsZoomed != previouslyZoomed)
    {
        // Prefer uDWM's published logical state over transitional geometry.
        maximizing = windowIsZoomed;
        recognizedStateTransition = true;
    }
    else if (targetIsWorkArea)
    {
        maximizing = true;
        recognizedStateTransition = true;
    }
    else if (sourceIsWorkArea || previouslyZoomed)
    {
        maximizing = false;
        recognizedStateTransition = true;
    }
    else if (windowIsZoomed)
    {
        // Accept early IsZoomed changes before transitional geometry settles.
        maximizing = true;
        recognizedStateTransition = true;
    }
    if (!recognizedStateTransition)
    {
        // Leave Snap-out and cross-monitor placement as ordinary motion.
        return;
    }
    if (index >= 0)
    {
        ObservedWindowState& observed = g_observedWindows[index];
        // Multiple uDWM hooks can describe two phases of the same transition
        // with opposite flags. Keep the first pulse instead of replacing it.
        if (observed.nativeTransitionStartedAt != 0 &&
            now - observed.nativeTransitionStartedAt < 120)
        {
            return;
        }
        if (observed.nativeTransitionPending && observed.expectedZoomed == maximizing &&
            now < observed.nativeTransitionDeadline && observed.nativeTransitionStartedAt != 0)
        {
            return;
        }
        observed.nativeTransitionPending = true;
        observed.expectedZoomed = maximizing;
        observed.nativeTransitionDeadline = now + 750;
        observed.nativeTransitionStartedAt = now;
        observed.lastSeen = now;
    }
    StartWindowStateThrob(hwnd, maximizing);
}

static void HandleObservedWindowLocationChange(HWND hwnd, LONG idObject, LONG idChild)
{
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
    {
        return;
    }
    int index = FindObservedWindowState(hwnd);
    if (index < 0)
    {
        RememberObservedWindowState(hwnd);
        return;
    }
    if (!IsEligibleWindowForStateThrob(hwnd)) return;
    bool zoomed = IsZoomed(hwnd) != FALSE;
    bool iconic = IsIconic(hwnd) != FALSE;
    RECT rect = {};
    bool hasRect = GetWindowRect(hwnd, &rect) != FALSE;
    ULONGLONG now = GetTickCount64();
    ObservedWindowState& observed = g_observedWindows[index];
    RECT previousObservedRect = observed.rect;
    bool wasIconic = observed.iconic;
    if (iconic)
    {
        // Preserve zoom state while a window is minimized.
        observed.iconic = true;
        observed.nativeTransitionPending = false;
        observed.nativeTransitionDeadline = 0;
        observed.lastSeen = now;
        return;
    }
    if (wasIconic || now <= observed.suppressStateThrobUntil)
    {
        observed.iconic = false;
        observed.zoomed = zoomed;
        observed.nativeTransitionPending = false;
        observed.nativeTransitionDeadline = 0;
        observed.lastSeen = now;
        if (wasIconic)
        {
            observed.suppressStateThrobUntil = now + 500;
        }
        if (hasRect)
        {
            observed.rect = rect;
            observed.snapped = !zoomed && IsApproximatelySnapLayoutTarget(rect);
        }
        CancelWindowStateThrobForWindow(hwnd);
        return;
    }
    observed.iconic = false;
    bool zoomStateChanged = observed.zoomed != zoomed;
    bool realDragging = g_realDragging.load(std::memory_order_relaxed);
    bool rectChanged = hasRect && !EqualRect(&rect, &observed.rect);
    // Skip fallback work when neither geometry nor state changed.
    if (!g_finalizingMoveSize && !rectChanged && !zoomStateChanged &&
        !observed.nativeTransitionPending)
    {
        observed.lastSeen = now;
        return;
    }
    if (realDragging && !g_finalizingMoveSize)
    {
        // Defer Snap classification until the final drag geometry.
        if (hasRect)
        {
            observed.rect = rect;
        }
        observed.zoomed = zoomed;
        observed.nativeTransitionPending = false;
        observed.nativeTransitionDeadline = 0;
        observed.lastSeen = now;
        return;
    }
    bool snapTargetChanged = false;
    if (hasRect)
    {
        bool snapped = !zoomed && IsApproximatelySnapLayoutTarget(rect);
        snapTargetChanged = snapped && (!observed.snapped || rectChanged);
        observed.snapped = snapped;
        observed.rect = rect;
    }
    if (realDragging)
    {
        // Use final geometry when private Snap hooks are absent.
        bool finalGeometryChanged = hasRect && !EqualRect(&rect, &g_realDraggedWindowRect);
        if (observed.snapped && finalGeometryChanged && !g_realResizing &&
            g_realDraggedWindow.load(std::memory_order_relaxed) == hwnd)
        {
            g_pendingInteractiveTransitionFlags.store(
                NATIVE_TRANSITION_TARGET_IS_SNAP_LAYOUT |
                    EncodeWindowTransitionDirection(g_realDraggedWindowRect, rect),
                std::memory_order_relaxed);
            g_pendingInteractiveTransitionWindow.store(hwnd, std::memory_order_release);
        }
        observed.zoomed = zoomed;
        observed.nativeTransitionPending = false;
        observed.nativeTransitionDeadline = 0;
        observed.lastSeen = now;
        return;
    }
    if (observed.nativeTransitionPending)
    {
        if (zoomed == observed.expectedZoomed)
        {
            // Consume the state finalized after an early native pulse.
            observed.zoomed = zoomed;
            observed.nativeTransitionPending = false;
            observed.lastSeen = now;
            return;
        }
        if (now < observed.nativeTransitionDeadline)
        {
            observed.lastSeen = now;
            return;
        }
        observed.nativeTransitionPending = false;
    }
    observed.zoomed = zoomed;
    observed.lastSeen = now;
    if (snapTargetChanged)
    {
        if (now >= observed.snapTransitionDeadline)
        {
            MarkObservedSnapTransition(hwnd, true);
            StartWindowStateThrob(hwnd, true, true,
                                  DecodeWindowTransitionDirection(
                                      EncodeWindowTransitionDirection(previousObservedRect, rect)));
        }
        // Snap-in supersedes a simultaneous restore edge.
        return;
    }
    if (zoomStateChanged)
    {
        StartWindowStateThrob(hwnd, zoomed);
    }
}

static bool IsShellCloakedWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !g_dwmGetWindowAttribute)
    {
        return false;
    }
    constexpr DWORD dwmwaCloaked = 14;
    constexpr DWORD dwmCloakedByShell = 0x00000002;
    DWORD cloaked = 0;
    return SUCCEEDED(g_dwmGetWindowAttribute(hwnd, dwmwaCloaked, &cloaked, sizeof(cloaked))) &&
           (cloaked & dwmCloakedByShell) != 0;
}

static bool IsDesktopHiddenAppWindow(HWND hwnd)
{
    return IsEligibleWindowForStateThrob(hwnd) && !IsIconic(hwnd) &&
           IsShellCloakedWindow(hwnd);
}

static void RefreshDesktopVisibility()
{
    bool resetAll = g_systemDesktopResetPending;
    g_systemDesktopResetPending = false;
    struct ActiveWindow
    {
        int slotIndex;
        HWND hwnd;
    } activeWindows[MAX_ANIMATION_SLOTS] = {};
    int activeCount = 0;
    AcquireSRWLockShared(&g_animationSlotsLock);
    for (int i = 0; i < MAX_ANIMATION_SLOTS; i++)
    {
        if (g_animationSlots[i].active)
        {
            activeWindows[activeCount++] = {i, g_animationSlots[i].hwnd};
        }
    }
    ReleaseSRWLockShared(&g_animationSlotsLock);
    int retiredCount = 0;
    for (int i = 0; i < activeCount; i++)
    {
        if (resetAll || IsShellCloakedWindow(activeWindows[i].hwnd))
        {
            RetireAnimationSlot(activeWindows[i].slotIndex, activeWindows[i].hwnd);
            retiredCount++;
        }
    }
    // Old desktop objects can remain valid while pointing at a hidden visual.
    // Force the delayed backfill to read the active CWindowData object graph.
    ClearAllDwmWindowMappings();
    if (resetAll)
    {
        ResetDragInputState();
        for (ObservedWindowState& observed : g_observedWindows)
        {
            observed = {};
        }
        g_pendingMaximizedStateWindow.store(nullptr, std::memory_order_release);
    }
    else
    {
        for (ObservedWindowState& observed : g_observedWindows)
        {
            if (IsShellCloakedWindow(observed.hwnd))
            {
                observed = {};
            }
        }
    }
    RememberObservedWindowState(GetForegroundWindow());
    QueueExistingWindowBackfill();
    Wh_Log(L"Desktop visibility changed: mode=%s retiredAnimations=%d",
           resetAll ? L"system-desktop-reset" : L"shell-cloak", retiredCount);
}

static void ScheduleDesktopVisibilityRefresh(bool resetAll = false)
{
    if (g_unloading.load(std::memory_order_acquire))
    {
        return;
    }
    g_systemDesktopResetPending |= resetAll;
    g_desktopVisibilityDeadline = GetTickCount64() + 300;
    if (g_desktopVisibilityTimer)
    {
        KillTimer(nullptr, g_desktopVisibilityTimer);
    }
    g_desktopVisibilityTimer = SetTimer(nullptr, 0, 300, nullptr);
    if (!g_desktopVisibilityTimer)
    {
        g_desktopVisibilityDeadline = 0;
        RefreshDesktopVisibility();
    }
}

static void CALLBACK WinEventCallback(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                      LONG idChild, DWORD, DWORD)
{
    if (g_unloading.load(std::memory_order_acquire))
    {
        return;
    }
    if (event == EVENT_OBJECT_LOCATIONCHANGE &&
        (idObject != OBJID_WINDOW || idChild != CHILDID_SELF))
    {
        return;
    }
    switch (event)
    {
    case EVENT_SYSTEM_MOVESIZESTART:
    {
        HandleMoveSizeStart(hwnd);
        break;
    }
    case EVENT_OBJECT_LOCATIONCHANGE:
    {
        HandleObservedWindowLocationChange(hwnd, idObject, idChild);
        HandleLocationChange(hwnd, idObject, idChild);
        break;
    }
    case EVENT_SYSTEM_MOVESIZEEND:
    {
        HandleMoveSizeEnd(hwnd);
        break;
    }
    case EVENT_SYSTEM_FOREGROUND:
    {
        HWND previousForeground = g_lastForegroundWindow;
        g_lastForegroundWindow = hwnd;
        if (previousForeground && previousForeground != hwnd &&
            IsDesktopHiddenAppWindow(previousForeground))
        {
            ScheduleDesktopVisibilityRefresh();
        }
        HandleObservedWindowLocationChange(hwnd, OBJID_WINDOW, CHILDID_SELF);
        break;
    }
    case EVENT_SYSTEM_MINIMIZESTART:
    {
        HandleMinimizeLifecycle(hwnd, true);
        break;
    }
    case EVENT_SYSTEM_MINIMIZEEND:
    {
        HandleMinimizeLifecycle(hwnd, false);
        break;
    }
    case EVENT_OBJECT_DESTROY:
    {
        if (idObject == OBJID_WINDOW)
        {
            ForgetObservedWindowState(hwnd);
        }
        break;
    }
    case EVENT_OBJECT_CLOAKED:
    {
        if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF &&
            IsDesktopHiddenAppWindow(hwnd))
        {
            ScheduleDesktopVisibilityRefresh();
        }
        break;
    }
    case EVENT_SYSTEM_DESKTOPSWITCH:
    {
        // This is a Win32 input-desktop transition (for example lock/UAC),
        // not a Windows virtual desktop notification. DWM can replace its
        // scene objects across it, so invalidate every animation on return.
        g_lastForegroundWindow = hwnd ? hwnd : GetForegroundWindow();
        ScheduleDesktopVisibilityRefresh(true);
        break;
    }
    default:
        break;
    }
}

static bool InitializeWindowEventHooks()
{
    struct HookSpec
    {
        DWORD first;
        DWORD last;
        DWORD flags;
        const wchar_t* name;
    };
    constexpr DWORD standardFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    // Keep DWM-originated cloak/system-desktop events; ordinary events skip our process.
    const HookSpec specs[WINDOW_EVENT_HOOK_COUNT] = {
        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, standardFlags, L"move/size"},
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, standardFlags, L"location"},
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, standardFlags, L"foreground"},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, standardFlags, L"minimize"},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, standardFlags, L"destroy"},
        {EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, WINEVENT_OUTOFCONTEXT,
         L"system desktop"},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_CLOAKED, WINEVENT_OUTOFCONTEXT, L"window cloak"},
    };
    for (int i = 0; i < WINDOW_EVENT_HOOK_COUNT; i++)
    {
        const HookSpec& spec = specs[i];
        g_windowEventHooks[i] =
            SetWinEventHook(spec.first, spec.last, nullptr, WinEventCallback, 0, 0, spec.flags);
        if (g_windowEventHooks[i])
        {
            continue;
        }
        Wh_Log(L"Failed to create %s WinEvent hook", spec.name);
        while (--i >= 0)
        {
            UnhookWinEvent(g_windowEventHooks[i]);
            g_windowEventHooks[i] = nullptr;
        }
        return false;
    }
    g_lastForegroundWindow = GetForegroundWindow();
    RememberObservedWindowState(g_lastForegroundWindow);
    Wh_Log(L"Window event hooks initialized");
    return true;
}

static void UninitializeWindowEventHooks()
{
    if (g_desktopVisibilityTimer)
    {
        KillTimer(nullptr, g_desktopVisibilityTimer);
        g_desktopVisibilityTimer = 0;
    }
    g_desktopVisibilityDeadline = 0;
    g_systemDesktopResetPending = false;
    ResetExistingWindowBackfill();
    StopAllAnimations();
    for (HWINEVENTHOOK& hook : g_windowEventHooks)
    {
        if (hook)
        {
            UnhookWinEvent(hook);
            hook = nullptr;
        }
    }
    for (int i = 0; i < MAX_OBSERVED_WINDOWS; i++)
    {
        g_observedWindows[i] = {};
    }
    g_lastForegroundWindow = nullptr;
    Wh_Log(L"Window event hooks removed");
}

static DWORD WINAPI WindowEventThreadProc(LPVOID)
{
    Wh_Log(L"Window event thread starting");
    // Force creation of this thread's message queue.
    MSG message = {};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_eventThreadMessageTarget.store(GetCurrentThreadId(), std::memory_order_release);
    g_animationTimer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_animationTimer)
    {
        g_animationTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    if (!g_animationTimer || !QueryPerformanceFrequency(&g_animationFrequency))
    {
        Wh_Log(L"Window event thread: animation clock initialization failed");
        CloseKernelHandle(g_animationTimer);
        if (g_eventThreadReady)
        {
            SetEvent(g_eventThreadReady);
        }
        g_eventThreadMessageTarget.store(0, std::memory_order_release);
        return 1;
    }
    DisableAnimationThreadPowerThrottling();
    Wh_Log(L"Animation clock: display-rate-adapted high-resolution timer, EcoQoS disabled");
    if (!InitializeWindowEventHooks())
    {
        Wh_Log(L"Window event thread: hook initialization failed");
        if (g_eventThreadReady)
        {
            SetEvent(g_eventThreadReady);
        }
        CloseKernelHandle(g_animationTimer);
        g_eventThreadMessageTarget.store(0, std::memory_order_release);
        return 1;
    }
    if (g_eventThreadReady)
    {
        SetEvent(g_eventThreadReady);
    }
    Wh_Log(L"Window event thread ready");
    bool running = true;
    HANDLE waitHandles[] = {g_animationTimer, g_eventThreadStop};
    ULONGLONG nextNativeMeshWakeAt = 0;
    while (running)
    {
        bool frameDue = false;
        DWORD waitTimeout = INFINITE;
        if (g_nativeMeshActive.load(std::memory_order_acquire) &&
            g_nativeMeshStopRequested.load(std::memory_order_acquire))
        {
            ULONGLONG now = GetTickCount64();
            if (!nextNativeMeshWakeAt || now >= nextNativeMeshWakeAt)
            {
                RequestDwmScenePass();
                nextNativeMeshWakeAt = now + 100;
            }
            waitTimeout = static_cast<DWORD>(
                std::max<ULONGLONG>(1, nextNativeMeshWakeAt - now));
        }
        else
        {
            nextNativeMeshWakeAt = 0;
        }
        DWORD waitResult = MsgWaitForMultipleObjectsEx(
            ARRAYSIZE(waitHandles), waitHandles, waitTimeout, QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_OBJECT_0)
        {
            frameDue = true;
        }
        else if (waitResult == WAIT_TIMEOUT)
        {
            continue;
        }
        else if (waitResult == WAIT_OBJECT_0 + 1)
        {
            running = false;
            continue;
        }
        else if (waitResult != WAIT_OBJECT_0 + ARRAYSIZE(waitHandles))
        {
            Wh_Log(L"Window event thread wait failed: %u", GetLastError());
            break;
        }
        // Bound message work so location floods cannot starve animation.
        int processedMessages = 0;
        LARGE_INTEGER messageBatchStart = {};
        QueryPerformanceCounter(&messageBatchStart);
        while (processedMessages < 16 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            processedMessages++;
            if (message.message == WM_QUIT)
            {
                running = false;
                break;
            }
            if (message.message == WM_TIMER &&
                message.wParam == g_desktopVisibilityTimer)
            {
                KillTimer(nullptr, g_desktopVisibilityTimer);
                g_desktopVisibilityTimer = 0;
                ULONGLONG now = GetTickCount64();
                if (now < g_desktopVisibilityDeadline)
                {
                    UINT remaining = static_cast<UINT>(
                        std::max<ULONGLONG>(1, g_desktopVisibilityDeadline - now));
                    g_desktopVisibilityTimer = SetTimer(nullptr, 0, remaining, nullptr);
                    if (g_desktopVisibilityTimer)
                    {
                        continue;
                    }
                }
                g_desktopVisibilityDeadline = 0;
                RefreshDesktopVisibility();
                continue;
            }
            if (message.message == WM_WOBBLY_MAXIMIZED_CHANGE)
            {
                // Clear the gate before consuming the latest coalesced HWND.
                g_maximizedStateCheckQueued.store(false, std::memory_order_release);
                HWND maximizedStateWindow =
                    g_pendingMaximizedStateWindow.exchange(nullptr, std::memory_order_acq_rel);
                HandleObservedWindowLocationChange(maximizedStateWindow, OBJID_WINDOW,
                                                   CHILDID_SELF);
                continue;
            }
            if (message.message == WM_WOBBLY_NATIVE_WINDOW_TRANSITION)
            {
                HWND transitionWindow = reinterpret_cast<HWND>(message.wParam);
                if (g_realDragging.load(std::memory_order_relaxed) &&
                    g_realDraggedWindow.load(std::memory_order_relaxed) == transitionWindow)
                {
                    HandleLocationChange(transitionWindow, OBJID_WINDOW, CHILDID_SELF);
                }
                else
                {
                    HandleNativeWindowTransitionStart(transitionWindow, message.lParam);
                }
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (g_animationFrequency.QuadPart > 0 && messageBatchStart.QuadPart > 0)
            {
                LARGE_INTEGER messageBatchNow = {};
                if (QueryPerformanceCounter(&messageBatchNow) &&
                    messageBatchNow.QuadPart - messageBatchStart.QuadPart >=
                        g_animationFrequency.QuadPart / 1000)
                {
                    break;
                }
            }
        }
        if (running && frameDue && g_animationClockArmed)
        {
            UpdateAnimationFrame();
        }
    }
    Wh_Log(L"Window event thread stopping");
    g_eventThreadMessageTarget.store(0, std::memory_order_release);
    g_pendingMaximizedStateWindow.store(nullptr, std::memory_order_release);
    g_maximizedStateCheckQueued.store(false, std::memory_order_release);
    UninitializeWindowEventHooks();
    CloseKernelHandle(g_animationTimer);
    g_animationFrequency = {};
    return 0;
}

static void StopWindowEventThread();

static bool StartWindowEventThread()
{
    g_eventThreadStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_eventThreadStop)
    {
        Wh_Log(L"Failed to create event thread stop event");
        return false;
    }
    g_eventThreadReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_eventThreadReady)
    {
        Wh_Log(L"Failed to create event thread ready event");
        CloseKernelHandle(g_eventThreadStop);
        return false;
    }
    g_eventThread = CreateThread(nullptr, 0, WindowEventThreadProc, nullptr, 0, nullptr);
    if (!g_eventThread)
    {
        Wh_Log(L"Failed to create window event thread");
        CloseKernelHandle(g_eventThreadReady);
        CloseKernelHandle(g_eventThreadStop);
        return false;
    }
    DWORD waitResult = WaitForSingleObject(g_eventThreadReady, 3000);
    if (waitResult != WAIT_OBJECT_0)
    {
        Wh_Log(L"Window event thread didn't become ready");
        StopWindowEventThread();
        return false;
    }
    return g_windowEventHooks[MOVE_SIZE_HOOK] && g_windowEventHooks[LOCATION_HOOK];
}

static void StopWindowEventThread()
{
    ResetExistingWindowBackfill();
    g_eventThreadMessageTarget.store(0, std::memory_order_release);
    if (g_eventThreadStop)
    {
        SetEvent(g_eventThreadStop);
    }
    if (g_eventThread)
    {
        DWORD waitResult = WaitForSingleObject(g_eventThread, INFINITE);
        if (waitResult != WAIT_OBJECT_0)
        {
            Wh_Log(L"Failed waiting for window event thread");
        }
        CloseKernelHandle(g_eventThread);
    }
    CloseKernelHandle(g_eventThreadReady);
    CloseKernelHandle(g_eventThreadStop);
}

static WobblySettings GetSettingsSnapshot()
{
    AcquireSRWLockShared(&g_settingsLock);
    WobblySettings settings = g_settings;
    ReleaseSRWLockShared(&g_settingsLock);
    return settings;
}

static void LoadSettings()
{
    auto wobblinessPresetSetting = WindhawkUtils::StringSetting::make(L"WobblinessPreset");
    PCWSTR wobblinessPreset = wobblinessPresetSetting.get();
    int wobbliness = 2;
    if (wobblinessPreset[0] >= L'0' && wobblinessPreset[0] <= L'4' &&
        wobblinessPreset[1] == L'\0')
    {
        wobbliness = wobblinessPreset[0] - L'0';
    }
    WobblySettings settings = PHYSICS_PRESETS[wobbliness];
    auto rendererSetting = WindhawkUtils::StringSetting::make(L"Renderer");
    std::wstring_view rendererValue(rendererSetting.get());
    settings.rendererMode = rendererValue == L"external-gpu"
                                ? RendererMode::ExternalGpu
                                : rendererValue == L"mesh"
                                      ? RendererMode::Mesh
                                      : RendererMode::Affine;
    g_meshBridgeEnabled.store(
        settings.rendererMode == RendererMode::ExternalGpu,
        std::memory_order_release);
    bool advancedMode = Wh_GetIntSetting(L"AdvancedMode.enable") != 0;
    settings.resizeWobbleEnabled = Wh_GetIntSetting(L"EnableResizeWobble") != 0;
    settings.windowStateWobbleEnabled =
        Wh_GetIntSetting(L"EnableWindowStateWobble") != 0;
    if (advancedMode)
    {
        settings.stiffness =
            static_cast<double>(Wh_GetIntSetting(L"AdvancedMode.Stiffness"));
        settings.drag = static_cast<double>(Wh_GetIntSetting(L"AdvancedMode.Drag"));
        settings.moveFactor =
            static_cast<double>(Wh_GetIntSetting(L"AdvancedMode.MoveFactor"));
    }
    // Clamp imported and current settings at the boundary.
    settings.stiffness = std::clamp(settings.stiffness, 1.0, 100.0);
    settings.drag = std::clamp(settings.drag, 1.0, 100.0);
    settings.moveFactor = std::clamp(settings.moveFactor, 1.0, 25.0);
    AcquireSRWLockExclusive(&g_settingsLock);
    g_settings = settings;
    ReleaseSRWLockExclusive(&g_settingsLock);
    if (settings.rendererMode != RendererMode::Mesh &&
        g_nativeMeshActive.load(std::memory_order_acquire))
    {
        g_nativeMeshStopRequested.store(true, std::memory_order_release);
        RequestDwmScenePass();
    }
    Wh_Log(L"Settings: "
           L"WobblinessPreset=%d, "
           L"Advanced=%d, "
           L"Renderer=%s, "
           L"ResizeWobble=%d, "
           L"WindowStateWobble=%d, "
           L"Stiffness=%.2f, "
           L"Drag=%.2f, "
           L"MoveFactor=%.2f",
           wobbliness, advancedMode,
           settings.rendererMode == RendererMode::ExternalGpu ? L"external-gpu" :
               settings.rendererMode == RendererMode::Mesh ? L"mesh" : L"affine",
           settings.resizeWobbleEnabled,
           settings.windowStateWobbleEnabled, settings.stiffness, settings.drag,
           settings.moveFactor);
}

static bool HasRecentDwmCrashLoop()
{
    // Check once at load, before installing any DWM hooks.
    EVT_HANDLE query = EvtQuery(
        nullptr, L"Application",
        L"*[System[Provider[@Name='Dwminit'] and Level=3 and "
        L"TimeCreated[timediff(@SystemTime) <= 60000]]]",
        EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!query)
    {
        Wh_Log(L"DWM crash-loop check unavailable: EvtQuery error=%u", GetLastError());
        return false;
    }
    EVT_HANDLE warnings[2] = {};
    DWORD count = 0;
    BOOL success = EvtNext(query, ARRAYSIZE(warnings), warnings, 1000, 0, &count);
    if (!success && GetLastError() != ERROR_NO_MORE_ITEMS)
    {
        Wh_Log(L"DWM crash-loop check unavailable: EvtNext error=%u", GetLastError());
    }
    for (DWORD i = 0; i < count; i++) EvtClose(warnings[i]);
    EvtClose(query);
    return success && count >= ARRAYSIZE(warnings);
}

namespace mesh_bridge {
using Payload = MeshBridgePayload;
using Slot = MeshBridgeSlot;
using Data = MeshBridgeData;
constexpr const wchar_t* name = MESH_BRIDGE_NAME;
constexpr DWORD magic = MESH_BRIDGE_MAGIC;
constexpr DWORD version = 1;
constexpr unsigned slot_count = MAX_ANIMATION_SLOTS;
}
using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

constexpr UINT kFrameMessage = WM_APP + 1;
constexpr UINT kGrid = 20;
constexpr char kShaders[] = R"(
cbuffer Bend : register(b0) { float4 options; float4 controls[16]; };
struct Vertex { float2 position : POSITION; float2 uv : TEXCOORD0; };
struct Pixel { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Pixel VSMain(Vertex input) {
    Pixel output;
    float2 p = input.position;
    float u = input.uv.x;
    float v = input.uv.y;
    float4 bx = float4(pow(1-u, 3), 3*u*pow(1-u, 2),
                       3*u*u*(1-u), u*u*u);
    float4 by = float4(pow(1-v, 3), 3*v*pow(1-v, 2),
                       3*v*v*(1-v), v*v*v);
    float2 displacement = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            displacement += controls[y * 4 + x].xy * bx[x] * by[y];
    p += displacement;
    output.position = float4(p * options.w, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}
Texture2D windowImage : register(t0);
SamplerState linearSampler : register(s0);
float4 PSMain(Pixel input) : SV_TARGET {
    return windowImage.Sample(linearSampler, input.uv);
}
)";

struct Vertex {
    float x, y, u, v;
};

struct Bend {
    float options[4];
    float controls[16][4];
};

bool ReadSlot(const mesh_bridge::Slot& slot, mesh_bridge::Payload& payload) {
    bool consistent = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        LONG before = slot.sequence;
        if (before & 1) continue;
        MemoryBarrier();
        payload = slot.payload;
        MemoryBarrier();
        LONG after = slot.sequence;
        if (before == after && !(after & 1)) {
            consistent = true;
            break;
        }
    }
    if (!consistent || !payload.active || !payload.hwnd) return false;
    ULONGLONG now = GetTickCount64();
    return payload.timestamp <= now && now - payload.timestamp <= 500;
}

struct GpuRuntime {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> compositionDevice;
    ComPtr<ID3DBlob> vertexBlob, pixelBlob;
    IDirect3DDevice captureDevice{nullptr};
    winrt::com_ptr<IGraphicsCaptureItemInterop> captureInterop;

    GpuRuntime() {
        constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL level;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
            &device, &level, &context);
        if (FAILED(hr)) {
            Wh_Log(L"GPU mesh renderer: hardware D3D11 unavailable, using WARP");
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
                nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                &device, &level, &context));
        }
        winrt::check_hresult(device.As(&dxgi));
        ComPtr<IDXGIAdapter> adapter;
        winrt::check_hresult(dxgi->GetAdapter(&adapter));
        winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)));
        winrt::check_hresult(DCompositionCreateDevice(
            dxgi.Get(), IID_PPV_ARGS(&compositionDevice)));

        auto compile = [&](const char* entry, const char* profile,
                           ID3DBlob** output) {
            ComPtr<ID3DBlob> errors;
            HRESULT result = D3DCompile(kShaders, sizeof(kShaders) - 1, nullptr,
                nullptr, nullptr, entry, profile, 0, 0, output, &errors);
            if (FAILED(result) && errors)
                Wh_Log(L"GPU mesh shader compile failed: %S",
                       static_cast<const char*>(errors->GetBufferPointer()));
            winrt::check_hresult(result);
        };
        compile("VSMain", "vs_4_0", &vertexBlob);
        compile("PSMain", "ps_4_0", &pixelBlob);

        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgi.Get(), inspectable.put()));
        captureDevice = inspectable.as<IDirect3DDevice>();
        captureInterop = winrt::get_activation_factory<GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        Wh_Log(L"GPU mesh renderer: GPU and capture resources ready");
    }
};

class Preview {
public:
    Preview(HWND target, GpuRuntime& runtime)
        : target_(target), runtime_(runtime), device_(runtime.device),
          context_(runtime.context), compositionDevice_(runtime.compositionDevice) {
        try {
            CreateWindowAndDevice();
            StartCapture();
            InvalidateRect(window_, nullptr, FALSE);
        } catch (...) {
            StopCapture();
            if (window_ && IsWindow(window_)) DestroyWindow(window_);
            throw;
        }
    }

    ~Preview() {
        StopCapture();
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        if (bridge_) UnmapViewOfFile(bridge_);
        if (bridgeHandle_) CloseHandle(bridgeHandle_);
    }

    HWND window() const { return window_; }
    bool failed() const { return failed_; }

    void SuspendCapture() {
        if (suspended_) return;
        KillTimer(window_, 1);
        if (session_) session_.Close();
        session_ = nullptr;
        suspended_ = true;
        if (overlayVisible_) ShowWindow(window_, SW_HIDE);
        overlayVisible_ = false;
        Wh_Log(L"GPU mesh capture paused: HWND=%p", target_);
    }

    void ResumeCapture() {
        if (!suspended_) return;
        if (!IsWindow(target_) || !pool_ || !item_)
            winrt::throw_hresult(E_INVALIDARG);
        HMONITOR current = MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST);
        if (captureMonitor_ && current != captureMonitor_) {
            Wh_Log(L"GPU mesh cached capture monitor changed: HWND=%p from=%p to=%p",
                   target_, captureMonitor_, current);
            winrt::throw_hresult(E_FAIL);
        }
        while (auto frame = pool_.TryGetNextFrame()) frame.Close();
        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(false);
        session_.StartCapture();
        suspended_ = false;
        renderedLogged_ = false;
        SetTimer(window_, 1, 16, nullptr);
        Wh_Log(L"GPU mesh capture resumed: HWND=%p", target_);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
        auto* self = reinterpret_cast<Preview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Preview*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            switch (message) {
            case kFrameMessage:
                try { self->ReceiveFrames(); }
                catch (winrt::hresult_error const& error) {
                    Wh_Log(L"GPU mesh capture failed: HWND=%p HRESULT=0x%08X %s",
                           self->target_, static_cast<unsigned>(error.code()),
                           error.message().c_str());
                    self->failed_ = true;
                    self->StopCapture();
                }
                catch (...) {
                    Wh_Log(L"GPU mesh capture failed: HWND=%p", self->target_);
                    self->failed_ = true;
                    self->StopCapture();
                }
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT paint;
                BeginPaint(window, &paint);
                try { self->Render(); }
                catch (winrt::hresult_error const& error) {
                    Wh_Log(L"GPU mesh render failed: HWND=%p HRESULT=0x%08X %s",
                           self->target_, static_cast<unsigned>(error.code()),
                           error.message().c_str());
                    self->failed_ = true;
                    self->StopCapture();
                }
                catch (...) {
                    Wh_Log(L"GPU mesh render failed: HWND=%p", self->target_);
                    self->failed_ = true;
                    self->StopCapture();
                }
                EndPaint(window, &paint);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            case WM_TIMER:
                if (wparam == 1) InvalidateRect(window, nullptr, FALSE);
                return 0;
            case WM_SIZE:
                if (self->swapChain_ && LOWORD(lparam) && HIWORD(lparam))
                    self->Resize(LOWORD(lparam), HIWORD(lparam));
                return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                break;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void CreateWindowAndDevice() {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"WobblyWgcMeshPreview";
        if (!RegisterClassW(&windowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            winrt::throw_last_error();
        DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                        WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP;
        window_ = CreateWindowExW(exStyle, windowClass.lpszClassName,
                                  L"Wobbly GPU mesh overlay", WS_POPUP,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
                                  nullptr, nullptr, windowClass.hInstance, this);
        if (!window_) winrt::throw_last_error();
        if (!SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA))
            winrt::throw_last_error();

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = 1;
        description.Height = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        winrt::check_hresult(runtime_.factory->CreateSwapChainForComposition(
            device_.Get(), &description, nullptr, &swapChain_));
        winrt::check_hresult(compositionDevice_->CreateTargetForHwnd(
            window_, TRUE, &compositionTarget_));
        winrt::check_hresult(compositionDevice_->CreateVisual(&compositionVisual_));
        winrt::check_hresult(compositionVisual_->SetContent(swapChain_.Get()));
        winrt::check_hresult(compositionTarget_->SetRoot(compositionVisual_.Get()));
        winrt::check_hresult(compositionDevice_->Commit());
        CreateRenderTarget();
        CreatePipeline();
        UpdateOverlayBounds();
        SetTimer(window_, 1, 16, nullptr);
    }

    void CreateRenderTarget() {
        ComPtr<ID3D11Texture2D> backBuffer;
        winrt::check_hresult(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
        winrt::check_hresult(device_->CreateRenderTargetView(backBuffer.Get(),
                                                             nullptr, &renderTarget_));
    }

    void Resize(UINT width, UINT height) {
        renderTarget_.Reset();
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        winrt::check_hresult(swapChain_->ResizeBuffers(0, width, height,
                                                         DXGI_FORMAT_UNKNOWN, 0));
        CreateRenderTarget();
    }

    bool UpdateOverlayBounds(double physicsX = 0.0, double physicsY = 0.0) {
        if (!target_ || !IsWindow(target_)) {
            if (overlayVisible_) ShowWindow(window_, SW_HIDE);
            overlayVisible_ = false;
            return false;
        }
        DWORD cloaked = 0;
        DwmGetWindowAttribute(target_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (!IsWindowVisible(target_) || IsIconic(target_) || cloaked) {
            if (overlayVisible_) ShowWindow(window_, SW_HIDE);
            overlayVisible_ = false;
            return false;
        }
        RECT windowBounds{};
        if (!GetWindowRect(target_, &windowBounds))
            return false;
        if (!haveFrameInsets_) {
            RECT visibleBounds{};
            if (SUCCEEDED(DwmGetWindowAttribute(target_,
                    DWMWA_EXTENDED_FRAME_BOUNDS,
                    &visibleBounds, sizeof(visibleBounds))) &&
                visibleBounds.right > visibleBounds.left &&
                visibleBounds.bottom > visibleBounds.top) {
                frameInsets_ = {visibleBounds.left - windowBounds.left,
                                visibleBounds.top - windowBounds.top,
                                visibleBounds.right - windowBounds.right,
                                visibleBounds.bottom - windowBounds.bottom};
            }
            haveFrameInsets_ = true;
        }
        RECT source{windowBounds.left + frameInsets_.left,
                    windowBounds.top + frameInsets_.top,
                    windowBounds.right + frameInsets_.right,
                    windowBounds.bottom + frameInsets_.bottom};
        const int width = source.right - source.left;
        const int height = source.bottom - source.top;
        if (width < 1 || height < 1 || width > 16000 || height > 16000)
            return false;
        const int canvasWidth = width * 5 / 4;
        const int canvasHeight = height * 5 / 4;
        RECT desired{source.left - (canvasWidth - width) / 2,
                     source.top - (canvasHeight - height) / 2,
                     source.left - (canvasWidth - width) / 2 + canvasWidth,
                     source.top - (canvasHeight - height) / 2 + canvasHeight};
        const LONG shiftX = static_cast<LONG>(std::lround(physicsX));
        const LONG shiftY = static_cast<LONG>(std::lround(physicsY));
        OffsetRect(&desired, shiftX, shiftY);
        if (!EqualRect(&desired, &overlayBounds_)) {
            if (!SetWindowPos(window_, nullptr, desired.left, desired.top,
                              canvasWidth, canvasHeight,
                              SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER))
                return false;
            overlayBounds_ = desired;
        }
        if (!overlayVisible_) {
            ShowWindow(window_, SW_SHOWNOACTIVATE);
            overlayVisible_ = true;
            Wh_Log(L"GPU mesh overlay visible: HWND=%p source=%dx%d canvas=%dx%d capture=%ux%u",
                   target_, width, height, canvasWidth, canvasHeight,
                   textureWidth_, textureHeight_);
        }
        return true;
    }

    void CreatePipeline() {
        winrt::check_hresult(device_->CreateVertexShader(
            runtime_.vertexBlob->GetBufferPointer(),
            runtime_.vertexBlob->GetBufferSize(), nullptr, &vertexShader_));
        winrt::check_hresult(device_->CreatePixelShader(
            runtime_.pixelBlob->GetBufferPointer(),
            runtime_.pixelBlob->GetBufferSize(), nullptr, &pixelShader_));
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        winrt::check_hresult(device_->CreateInputLayout(layout, 2,
            runtime_.vertexBlob->GetBufferPointer(),
            runtime_.vertexBlob->GetBufferSize(), &inputLayout_));

        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        vertices.reserve((kGrid + 1) * (kGrid + 1));
        indices.reserve(kGrid * kGrid * 6);
        for (UINT y = 0; y <= kGrid; ++y)
            for (UINT x = 0; x <= kGrid; ++x) {
                float u = static_cast<float>(x) / kGrid;
                float v = static_cast<float>(y) / kGrid;
                vertices.push_back({u * 1.8f - 0.9f, 0.9f - v * 1.8f, u, v});
            }
        for (UINT y = 0; y < kGrid; ++y)
            for (UINT x = 0; x < kGrid; ++x) {
                uint16_t a = static_cast<uint16_t>(y * (kGrid + 1) + x);
                uint16_t b = static_cast<uint16_t>(a + 1);
                uint16_t c = static_cast<uint16_t>(a + kGrid + 1);
                uint16_t d = static_cast<uint16_t>(c + 1);
                indices.insert(indices.end(), {a, b, c, b, d, c});
            }
        indexCount_ = static_cast<UINT>(indices.size());
        D3D11_BUFFER_DESC buffer{};
        buffer.Usage = D3D11_USAGE_IMMUTABLE;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        D3D11_SUBRESOURCE_DATA data{vertices.data()};
        winrt::check_hresult(device_->CreateBuffer(&buffer, &data, &vertexBuffer_));
        buffer.BindFlags = D3D11_BIND_INDEX_BUFFER;
        buffer.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t));
        data.pSysMem = indices.data();
        winrt::check_hresult(device_->CreateBuffer(&buffer, &data, &indexBuffer_));
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.ByteWidth = sizeof(Bend);
        winrt::check_hresult(device_->CreateBuffer(&buffer, nullptr, &bendBuffer_));

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        winrt::check_hresult(device_->CreateSamplerState(&sampler, &sampler_));
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        winrt::check_hresult(device_->CreateBlendState(&blend, &blendState_));
    }

    void StartCapture() {
        winrt::check_hresult(runtime_.captureInterop->CreateForWindow(
            target_, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item_)));
        captureDevice_ = runtime_.captureDevice;
        frameSize_ = item_.Size();
        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            captureDevice_, DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, frameSize_);
        frameToken_ = pool_.FrameArrived([window = window_](auto const&, auto const&) {
            PostMessageW(window, kFrameMessage, 0, 0);
        });
        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(false);
        session_.StartCapture();
        captureMonitor_ = MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST);
        pendingMonitor_ = nullptr;
        Wh_Log(L"GPU mesh capture started: HWND=%p size=%dx%d monitor=%p dpi=%u",
               target_, frameSize_.Width, frameSize_.Height,
               captureMonitor_, GetDpiForWindow(target_));
    }

    void RefreshCaptureAfterMonitorChange() {
        if (!IsWindow(target_)) return;
        HMONITOR current = MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST);
        if (!current || !captureMonitor_ || current == captureMonitor_) {
            pendingMonitor_ = nullptr;
            return;
        }
        ULONGLONG now = GetTickCount64();
        if (current != pendingMonitor_) {
            pendingMonitor_ = current;
            pendingMonitorSince_ = now;
            Wh_Log(L"GPU mesh monitor transition: HWND=%p from=%p to=%p",
                   target_, captureMonitor_, current);
            return;
        }
        ULONGLONG stableSince = std::max(pendingMonitorSince_,
                                         lastFrameSizeChangeAt_);
        if (now - stableSince < 250) return;

        HMONITOR previous = captureMonitor_;
        if (pool_) pool_.FrameArrived(frameToken_);
        if (session_) session_.Close();
        if (pool_) pool_.Close();
        session_ = nullptr;
        pool_ = nullptr;
        item_ = nullptr;
        captureDevice_ = nullptr;
        haveFrameInsets_ = false;
        StartCapture();
        renderedLogged_ = false;
        Wh_Log(L"GPU mesh capture refreshed for monitor: HWND=%p from=%p to=%p "
               L"wait=%llums",
               target_, previous, captureMonitor_, now - pendingMonitorSince_);
    }

    void StopCapture() noexcept {
        try {
            if (pool_) pool_.FrameArrived(frameToken_);
            if (session_) session_.Close();
            if (pool_) pool_.Close();
        } catch (...) {
        }
        session_ = nullptr;
        pool_ = nullptr;
        item_ = nullptr;
        captureDevice_ = nullptr;
        ID3D11ShaderResourceView* empty = nullptr;
        if (context_) context_->PSSetShaderResources(0, 1, &empty);
        capturedView_.Reset();
        capturedTexture_.Reset();
        textureWidth_ = textureHeight_ = 0;
        haveFrameInsets_ = false;
        overlayBounds_ = {};
        captureMonitor_ = nullptr;
        pendingMonitor_ = nullptr;
        pendingMonitorSince_ = 0;
        lastFrameSizeChangeAt_ = 0;
        if (overlayVisible_) ShowWindow(window_, SW_HIDE);
        overlayVisible_ = false;
    }

    void ReceiveFrames() {
        if (!pool_) return;
        if (suspended_) {
            while (auto frame = pool_.TryGetNextFrame()) frame.Close();
            return;
        }
        bool resize = false;
        while (auto frame = pool_.TryGetNextFrame()) {
            auto size = frame.ContentSize();
            auto access = frame.Surface().as<
                IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> source;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            if (!capturedTexture_ || desc.Width != textureWidth_ ||
                desc.Height != textureHeight_) {
                lastFrameSizeChangeAt_ = GetTickCount64();
                textureWidth_ = desc.Width;
                textureHeight_ = desc.Height;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                capturedView_.Reset();
                capturedTexture_.Reset();
                winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr,
                                                               &capturedTexture_));
                winrt::check_hresult(device_->CreateShaderResourceView(
                    capturedTexture_.Get(), nullptr, &capturedView_));
            }
            context_->CopyResource(capturedTexture_.Get(), source.Get());
            frame.Close();
            if (size.Width > 0 && size.Height > 0 &&
                (size.Width != frameSize_.Width || size.Height != frameSize_.Height)) {
                lastFrameSizeChangeAt_ = GetTickCount64();
                frameSize_ = size;
                resize = true;
            }
        }
        if (resize) pool_.Recreate(captureDevice_,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, frameSize_);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void TryOpenBridge() {
        if (bridge_ || GetTickCount64() - lastBridgeOpenAttempt_ < 1000) return;
        lastBridgeOpenAttempt_ = GetTickCount64();
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mesh_bridge::name);
        if (!mapping) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != lastBridgeOpenError_)
                Wh_Log(L"GPU mesh bridge open failed: %u", error);
            lastBridgeOpenError_ = error;
            return;
        }
        lastBridgeOpenError_ = ERROR_SUCCESS;
        auto* data = static_cast<const mesh_bridge::Data*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(mesh_bridge::Data)));
        if (!data) {
            CloseHandle(mapping);
            return;
        }
        bridgeHandle_ = mapping;
        bridge_ = data;
    }

    bool BridgeReady() {
        TryOpenBridge();
        return bridge_ && bridge_->magic == mesh_bridge::magic &&
            bridge_->version == mesh_bridge::version &&
            bridge_->size == sizeof(mesh_bridge::Data);
    }

    bool ReadPhysics(mesh_bridge::Payload& payload) {
        if (!BridgeReady()) return false;
        for (auto const& slot : bridge_->slots) {
            mesh_bridge::Payload copy{};
            if (!ReadSlot(slot, copy) ||
                copy.hwnd != reinterpret_cast<UINT_PTR>(target_)) continue;
            payload = copy;
            return true;
        }
        return false;
    }

    bool MakePhysicsBend(Bend& bend, double& translationX,
                         double& translationY) {
        mesh_bridge::Payload snapshot{};
        if (!ReadPhysics(snapshot)) return false;
        if (!std::isfinite(snapshot.width) || !std::isfinite(snapshot.height) ||
            snapshot.width < 1 || snapshot.height < 1 ||
            snapshot.width > 20000 || snapshot.height > 20000)
            return false;
        double baseX = 0, baseY = 0, centerX = 0, centerY = 0;
        for (auto const& point : snapshot.points) {
            if (!std::isfinite(point.baseX) || !std::isfinite(point.baseY) ||
                !std::isfinite(point.x) || !std::isfinite(point.y))
                return false;
            baseX += point.baseX;
            baseY += point.baseY;
            centerX += point.x;
            centerY += point.y;
        }
        baseX /= 16; baseY /= 16;
        centerX /= 16; centerY /= 16;
        translationX = centerX - baseX;
        translationY = centerY - baseY;
        if (!std::isfinite(translationX) || !std::isfinite(translationY))
            return false;
        translationX = std::clamp(translationX, -4096.0, 4096.0);
        translationY = std::clamp(translationY, -4096.0, 4096.0);
        double denomX = 0, denomY = 0, xx = 0, xy = 0, yx = 0, yy = 0;
        for (auto const& point : snapshot.points) {
            double bx = point.baseX - baseX;
            double by = point.baseY - baseY;
            double px = point.x - centerX;
            double py = point.y - centerY;
            denomX += bx * bx;
            denomY += by * by;
            xx += bx * px;
            xy += bx * py;
            yx += by * px;
            yy += by * py;
        }
        if (denomX < 1 || denomY < 1) return false;
        double m11 = xx / denomX, m12 = xy / denomX;
        double m21 = yx / denomY, m22 = yy / denomY;
        for (int i = 0; i < 16; ++i) {
            auto const& point = snapshot.points[i];
            double bx = point.baseX - baseX;
            double by = point.baseY - baseY;
            double dx = (point.x - centerX) - m11 * bx - m21 * by;
            double dy = (point.y - centerY) - m12 * bx - m22 * by;
            bend.controls[i][0] = static_cast<float>(
                std::clamp(1.8 * dx / snapshot.width, -0.25, 0.25));
            bend.controls[i][1] = static_cast<float>(
                std::clamp(-1.8 * dy / snapshot.height, -0.25, 0.25));
        }
        return true;
    }

    void Render() {
        if (suspended_ || !renderTarget_) return;
        RefreshCaptureAfterMonitorChange();
        if (!capturedView_) return;
        Bend bend{};
        double translationX = 0.0, translationY = 0.0;
        bend.options[3] = 8.0f / 9.0f;
        bridgeActive_ = MakePhysicsBend(bend, translationX, translationY);
        if (!UpdateOverlayBounds(bridgeActive_ ? translationX : 0.0,
                                 bridgeActive_ ? translationY : 0.0))
            return;
        RECT rect{};
        GetClientRect(window_, &rect);
        if (rect.right <= 0 || rect.bottom <= 0) return;
        context_->UpdateSubresource(bendBuffer_.Get(), 0, nullptr, &bend, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(rect.right),
                                static_cast<float>(rect.bottom), 0, 1};
        context_->RSSetViewports(1, &viewport);
        const float clear[] = {0, 0, 0, 0};
        context_->ClearRenderTargetView(renderTarget_.Get(), clear);
        ID3D11RenderTargetView* target = renderTarget_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->OMSetBlendState(blendState_.Get(), nullptr, 0xFFFFFFFF);
        UINT stride = sizeof(Vertex), offset = 0;
        ID3D11Buffer* vertices = vertexBuffer_.Get();
        context_->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
        context_->IASetIndexBuffer(indexBuffer_.Get(), DXGI_FORMAT_R16_UINT, 0);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        ID3D11Buffer* constants = bendBuffer_.Get();
        context_->VSSetConstantBuffers(0, 1, &constants);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* image = capturedView_.Get();
        context_->PSSetShaderResources(0, 1, &image);
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        if (bridgeActive_) context_->DrawIndexed(indexCount_, 0, 0);
        swapChain_->Present(1, 0);
        if (bridgeActive_ && !renderedLogged_) {
            renderedLogged_ = true;
            Wh_Log(L"GPU mesh first warped frame: HWND=%p vertices=%u indices=%u",
                   target_, (kGrid + 1) * (kGrid + 1), indexCount_);
        }
    }

    HWND target_ = nullptr;
    GpuRuntime& runtime_;
    HWND window_ = nullptr;
    bool failed_ = false;
    bool suspended_ = false;
    bool overlayVisible_ = false;
    RECT overlayBounds_{};
    RECT frameInsets_{};
    bool haveFrameInsets_ = false;
    bool bridgeActive_ = false;
    bool renderedLogged_ = false;
    HMONITOR captureMonitor_ = nullptr;
    HMONITOR pendingMonitor_ = nullptr;
    ULONGLONG pendingMonitorSince_ = 0;
    ULONGLONG lastFrameSizeChangeAt_ = 0;
    HANDLE bridgeHandle_ = nullptr;
    const mesh_bridge::Data* bridge_ = nullptr;
    ULONGLONG lastBridgeOpenAttempt_ = 0;
    DWORD lastBridgeOpenError_ = ERROR_SUCCESS;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> compositionVisual_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_, indexBuffer_, bendBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11Texture2D> capturedTexture_;
    ComPtr<ID3D11ShaderResourceView> capturedView_;
    UINT textureWidth_ = 0, textureHeight_ = 0, indexCount_ = 0;
    GraphicsCaptureItem item_{nullptr};
    IDirect3DDevice captureDevice_{nullptr};
    Direct3D11CaptureFramePool pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    winrt::event_token frameToken_{};
    winrt::Windows::Graphics::SizeInt32 frameSize_{};
};

class BridgeScanner {
public:
    ~BridgeScanner() {
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
    }

    std::vector<HWND> ActiveWindows() {
        if (!data_) {
            ULONGLONG now = GetTickCount64();
            if (now - lastOpenAttempt_ < 1000) return {};
            lastOpenAttempt_ = now;
            mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, mesh_bridge::name);
            if (!mapping_) return {};
            data_ = static_cast<const mesh_bridge::Data*>(MapViewOfFile(
                mapping_, FILE_MAP_READ, 0, 0, sizeof(mesh_bridge::Data)));
            if (!data_) {
                CloseHandle(mapping_);
                mapping_ = nullptr;
                return {};
            }
        }
        if (data_->magic != mesh_bridge::magic ||
            data_->version != mesh_bridge::version ||
            data_->size != sizeof(mesh_bridge::Data)) return {};
        std::vector<HWND> windows;
        windows.reserve(mesh_bridge::slot_count);
        for (auto const& slot : data_->slots) {
            mesh_bridge::Payload payload{};
            if (!ReadSlot(slot, payload)) continue;
            HWND hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(payload.hwnd));
            if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) continue;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (!cloaked && std::find(windows.begin(), windows.end(), hwnd) == windows.end())
                windows.push_back(hwnd);
        }
        return windows;
    }

private:
    HANDLE mapping_ = nullptr;
    const mesh_bridge::Data* data_ = nullptr;
    ULONGLONG lastOpenAttempt_ = 0;
};

struct ActiveView {
    std::unique_ptr<Preview> preview;
    ULONGLONG lastSeen;
};

int RunIntegrated(HANDLE stopEvent) {
    GpuRuntime runtime;
    BridgeScanner scanner;
    std::unordered_map<HWND, ActiveView> views;
    std::unordered_map<HWND, ULONGLONG> retryAfter;
    std::unique_ptr<Preview> cached;
    HWND cachedHwnd = nullptr;
    DWORD cachedPid = 0, cachedThread = 0;
    ULONGLONG cachedSince = 0;
    HWND lastRaised = nullptr;
    ULONGLONG lastScan = 0;
    for (;;) {
        DWORD wait = MsgWaitForMultipleObjectsEx(
            1, &stopEvent, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) return 0;
        if (wait == WAIT_FAILED) winrt::throw_last_error();
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ULONGLONG now = GetTickCount64();
        if (now - lastScan < 16) continue;
        lastScan = now;
        if (cached && (now - cachedSince > 10000 || !IsWindow(cachedHwnd)))
            cached.reset();
        auto active = scanner.ActiveWindows();
        for (HWND hwnd : active) {
            auto existing = views.find(hwnd);
            if (existing != views.end()) {
                existing->second.lastSeen = now;
                continue;
            }
            if (now < retryAfter[hwnd]) continue;
            try {
                ULONGLONG setupStarted = GetTickCount64();
                std::unique_ptr<Preview> preview;
                bool reused = false;
                DWORD pid = 0;
                DWORD thread = GetWindowThreadProcessId(hwnd, &pid);
                if (cached && cachedHwnd == hwnd && cachedPid == pid &&
                    cachedThread == thread) {
                    preview = std::move(cached);
                    try {
                        preview->ResumeCapture();
                        reused = true;
                    } catch (...) {
                        Wh_Log(L"GPU mesh cached capture failed: HWND=%p; rebuilding", hwnd);
                        preview.reset();
                    }
                }
                if (!preview) preview = std::make_unique<Preview>(hwnd, runtime);
                views.emplace(hwnd, ActiveView{std::move(preview), now});
                retryAfter.erase(hwnd);
                Wh_Log(L"GPU mesh following HWND=%p activeViews=%zu reused=%d setup=%llums",
                       hwnd, views.size(), reused, GetTickCount64() - setupStarted);
            } catch (winrt::hresult_error const& error) {
                retryAfter[hwnd] = now + 2000;
                Wh_Log(L"GPU mesh capture start failed HWND=%p HRESULT=0x%08X %s",
                       hwnd, static_cast<unsigned>(error.code()),
                       error.message().c_str());
            } catch (...) {
                retryAfter[hwnd] = now + 2000;
                Wh_Log(L"GPU mesh capture start failed HWND=%p", hwnd);
            }
        }
        for (auto it = views.begin(); it != views.end();) {
            if (it->second.preview->failed() || !IsWindow(it->first)) {
                if (it->second.preview->failed()) retryAfter[it->first] = now + 2000;
                it = views.erase(it);
            } else if (now - it->second.lastSeen > 750) {
                HWND hwnd = it->first;
                try {
                    it->second.preview->SuspendCapture();
                    cachedThread = GetWindowThreadProcessId(hwnd, &cachedPid);
                    cachedHwnd = hwnd;
                    cachedSince = now;
                    cached = std::move(it->second.preview);
                    Wh_Log(L"GPU mesh stopped following HWND=%p; resources cached", hwnd);
                } catch (...) {
                    Wh_Log(L"GPU mesh capture pause failed: HWND=%p", hwnd);
                }
                it = views.erase(it);
            } else {
                ++it;
            }
        }
        HWND foreground = GetForegroundWindow();
        auto front = views.find(foreground);
        if (front != views.end() && lastRaised != foreground) {
            SetWindowPos(front->second.preview->window(), HWND_TOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            lastRaised = foreground;
        } else if (front == views.end()) lastRaised = nullptr;
    }
}

HANDLE g_gpuRendererStop = nullptr;
HANDLE g_gpuRendererThread = nullptr;

static DWORD WINAPI GpuRendererThreadProc(void*)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try
    {
        struct Apartment {
            Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
            ~Apartment() { winrt::uninit_apartment(); }
        } apartment;
        Wh_Log(L"GPU mesh renderer: started");
        RunIntegrated(g_gpuRendererStop);
        Wh_Log(L"GPU mesh renderer: stopped");
    }
    catch (const winrt::hresult_error& error)
    {
        Wh_Log(L"GPU mesh renderer: failed HRESULT=0x%08X %s",
               static_cast<unsigned int>(error.code()), error.message().c_str());
    }
    catch (const std::exception& error)
    {
        Wh_Log(L"GPU mesh renderer: failed %S", error.what());
    }
    catch (...)
    {
        Wh_Log(L"GPU mesh renderer: unexpected failure");
    }
    return 0;
}

static void StopGpuRenderer()
{
    if (g_gpuRendererStop) SetEvent(g_gpuRendererStop);
    if (g_gpuRendererThread)
    {
        DWORD result = WaitForSingleObject(g_gpuRendererThread, 3000);
        if (result == WAIT_TIMEOUT)
        {
            Wh_Log(L"GPU mesh renderer: stop timed out; tool process will exit");
            return;
        }
        CloseHandle(g_gpuRendererThread);
        g_gpuRendererThread = nullptr;
    }
    if (g_gpuRendererStop)
    {
        CloseHandle(g_gpuRendererStop);
        g_gpuRendererStop = nullptr;
    }
}

static bool GpuRendererSelected()
{
    auto rendererSetting = WindhawkUtils::StringSetting::make(L"Renderer");
    return std::wcscmp(rendererSetting.get(), L"external-gpu") == 0;
}

static void UpdateGpuRenderer()
{
    if (!GpuRendererSelected())
    {
        StopGpuRenderer();
        return;
    }
    if (g_gpuRendererThread &&
        WaitForSingleObject(g_gpuRendererThread, 0) == WAIT_TIMEOUT)
        return;
    StopGpuRenderer();
    g_gpuRendererStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_gpuRendererStop) return;
    g_gpuRendererThread = CreateThread(
        nullptr, 0, GpuRendererThreadProc, nullptr, 0, nullptr);
    if (!g_gpuRendererThread) StopGpuRenderer();
}

enum class HostProcess { Dwm, GpuToolLauncher, GpuToolChild };
HostProcess g_hostProcess = HostProcess::Dwm;
HANDLE g_gpuToolMutex = nullptr;
HANDLE g_gpuToolProcess = nullptr;

static bool IsCurrentGpuToolChild(bool* isAnyToolChild)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool current = false;
    *isAnyToolChild = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!_wcsicmp(argv[i], L"-service") ||
            !_wcsicmp(argv[i], L"-service-start") ||
            !_wcsicmp(argv[i], L"-service-stop"))
        {
            *isAnyToolChild = true;
            break;
        }
        if (!_wcsicmp(argv[i], L"-tool-mod"))
        {
            *isAnyToolChild = true;
            current = i + 1 < argc && !_wcsicmp(argv[i + 1], WH_MOD_ID);
            break;
        }
    }
    LocalFree(argv);
    return current;
}

static void WINAPI GpuToolEntryPointHook()
{
    ExitThread(0);
}

static bool HookGpuToolEntryPoint()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        !nt->OptionalHeader.AddressOfEntryPoint)
        return false;
    return Wh_SetFunctionHook(base + nt->OptionalHeader.AddressOfEntryPoint,
                              reinterpret_cast<void*>(GpuToolEntryPointHook),
                              nullptr);
}

static void StartGpuToolProcess()
{
    if (!GpuRendererSelected()) return;
    if (g_gpuToolProcess)
    {
        if (WaitForSingleObject(g_gpuToolProcess, 0) == WAIT_TIMEOUT) return;
        CloseHandle(g_gpuToolProcess);
        g_gpuToolProcess = nullptr;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path))
    {
        Wh_Log(L"GPU companion launcher: cannot find host executable");
        return;
    }
    std::wstring command = L"\"";
    command += path;
    command += L"\" -tool-mod \"";
    command += WH_MOD_ID;
    command += L"\"";
    HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
    if (!kernel) kernel = GetModuleHandleW(L"kernel32.dll");
    using CreateProcessInternalW_t = BOOL (WINAPI*)(
        HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
        LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
        LPSTARTUPINFOW, LPPROCESS_INFORMATION, PHANDLE);
    auto createProcess = kernel ? reinterpret_cast<CreateProcessInternalW_t>(
        GetProcAddress(kernel, "CreateProcessInternalW")) : nullptr;
    if (!createProcess)
    {
        Wh_Log(L"GPU companion launcher: CreateProcessInternalW unavailable");
        return;
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_FORCEOFFFEEDBACK;
    PROCESS_INFORMATION process = {};
    if (!createProcess(nullptr, path, command.data(), nullptr, nullptr,
                       FALSE, NORMAL_PRIORITY_CLASS, nullptr, nullptr,
                       &startup, &process, nullptr))
    {
        Wh_Log(L"GPU companion launcher: process creation failed error=%u",
               GetLastError());
        return;
    }
    Wh_Log(L"GPU companion launcher: started PID=%u", process.dwProcessId);
    CloseHandle(process.hThread);
    g_gpuToolProcess = process.hProcess;
}

}  // namespace

BOOL Wh_ModInit()
{
    wchar_t processPath[MAX_PATH] = {};
    DWORD processPathLength = GetModuleFileNameW(
        nullptr, processPath, ARRAYSIZE(processPath));
    if (!processPathLength || processPathLength >= ARRAYSIZE(processPath))
        return FALSE;
    const wchar_t* processName = wcsrchr(processPath, L'\\');
    processName = processName ? processName + 1 : processPath;
    if (!_wcsicmp(processName, L"windhawk.exe"))
    {
        DWORD sessionId = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) ||
            sessionId == 0)
            return FALSE;
        bool anyToolChild = false;
        bool thisToolChild = IsCurrentGpuToolChild(&anyToolChild);
        if (anyToolChild && !thisToolChild) return FALSE;
        g_hostProcess = thisToolChild ? HostProcess::GpuToolChild
                                     : HostProcess::GpuToolLauncher;
        if (!thisToolChild) return TRUE;
        g_gpuToolMutex = CreateMutexW(
            nullptr, TRUE, L"Local\\Windhawk.WobblyWindows.GpuTool.v1");
        if (!g_gpuToolMutex || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            Wh_Log(L"GPU companion: another instance is running");
            ExitProcess(1);
        }
        if (!HookGpuToolEntryPoint())
        {
            Wh_Log(L"GPU companion: host entry point hook failed");
            CloseHandle(g_gpuToolMutex);
            g_gpuToolMutex = nullptr;
            return FALSE;
        }
        Wh_Log(L"GPU companion: dedicated process initialized");
        return TRUE;
    }
    if (_wcsicmp(processName, L"dwm.exe")) return FALSE;
    if (HasRecentDwmCrashLoop())
    {
        Wh_Log(L"Refusing to load: repeated recent Dwminit warnings; "
               L"disable the mod and investigate DWM stability");
        return FALSE;
    }
    g_unloading.store(false, std::memory_order_release);
    g_nativeMeshLastAttemptHwnd = nullptr;
    g_nativeMeshLastAttemptAt = 0;
    g_nativeMeshActive.store(false, std::memory_order_release);
    g_nativeMeshStopRequested.store(false, std::memory_order_release);
    g_nativeMeshHwnd.store(nullptr, std::memory_order_release);
    g_dwmSceneWakeMessage =
        RegisterWindowMessageW(L"Windhawk.WobblyWindows.DwmSceneWake");
    if (g_dwmSceneWakeMessage == 0)
    {
        Wh_Log(L"Failed to register DWM scene wake message");
        return FALSE;
    }
    g_desktopManagerThreadIdOffset = SIZE_MAX;
    g_topLevelWindow3DWindowDataOffset = SIZE_MAX;
    g_dwmSceneThreadId.store(0, std::memory_order_release);
    g_dwmCompositor.store(nullptr, std::memory_order_release);
    g_desktopManager.store(nullptr, std::memory_order_release);
    g_proxyCreationDisabled.store(false, std::memory_order_release);
    g_dwmThreadMismatchLogged.store(false, std::memory_order_release);
    g_dwmObjectDiscoveryFailureCount.store(0, std::memory_order_release);
    g_sceneWakeScheduled.store(false, std::memory_order_release);
    g_sceneWakeOutstanding.store(0, std::memory_order_release);
    g_sceneWakeAwaitingNativeTimeline.store(false, std::memory_order_release);
    LARGE_INTEGER wakeTokenCounter = {};
    QueryPerformanceCounter(&wakeTokenCounter);
    UINT_PTR wakeTokenSeed = static_cast<UINT_PTR>(wakeTokenCounter.QuadPart) ^
                             static_cast<UINT_PTR>(GetTickCount64()) ^
                             reinterpret_cast<UINT_PTR>(&g_dwmSceneWakeToken);
    INT_PTR wakeToken = static_cast<INT_PTR>(wakeTokenSeed);
    g_dwmSceneWakeToken.store(wakeToken ? wakeToken : 1, std::memory_order_release);
    g_sceneRequestedSerial.store(0, std::memory_order_release);
    g_sceneSubmittedSerial.store(0, std::memory_order_release);
    g_sceneWakePostTimestamp.store(0, std::memory_order_release);
    g_lastNativeTimelineTimestamp.store(0, std::memory_order_release);
    g_sceneWakeStallStartedAt.store(0, std::memory_order_release);
    g_sceneWakeStalled.store(false, std::memory_order_release);
    g_sceneRecoveryCleanupPending.store(false, std::memory_order_release);
    g_scenePassCounter.store(0, std::memory_order_release);
    g_abandonedProxyCount.store(0, std::memory_order_release);
    g_sceneOwnershipResetPending.store(false, std::memory_order_release);
    g_lastBindPrerequisiteLog.store(0, std::memory_order_release);
    ResetExistingWindowBackfill();
    g_lastObservedScenePassCounter = 0;
    g_lastSceneProgressTimestamp = 0;
    Wh_Log(L"Initializing version " WH_MOD_VERSION);
    InitializeDpiSupport();
    LoadSettings();
    InitializeMeshBridge();
    if (!InitializeDwmHooks())
    {
        Wh_Log(L"Failed to initialize DWM hooks");
        UninitializeDpiSupport();
        CloseMeshBridge();
        return FALSE;
    }
    if (!StartWindowEventThread())
    {
        Wh_Log(L"Failed to initialize event thread");
        StopWindowEventThread();
        UninitializeDpiSupport();
        CloseMeshBridge();
        return FALSE;
    }
    Wh_Log(L"Initialized successfully");
    return TRUE;
}

void Wh_ModAfterInit()
{
    if (g_hostProcess == HostProcess::GpuToolLauncher)
    {
        StartGpuToolProcess();
        return;
    }
    if (g_hostProcess == HostProcess::GpuToolChild)
    {
        UpdateGpuRenderer();
        return;
    }
    // Windhawk activates detours after Wh_ModInit returns. Queue the first
    // scene pass only now so already-open windows cannot miss the wake.
    QueueExistingWindowBackfill();
}

void Wh_ModSettingsChanged()
{
    if (g_hostProcess == HostProcess::GpuToolLauncher)
    {
        StartGpuToolProcess();
        return;
    }
    if (g_hostProcess == HostProcess::GpuToolChild)
    {
        UpdateGpuRenderer();
        if (!GpuRendererSelected())
            ExitProcess(0);
        return;
    }
    Wh_Log(L"Settings changed");
    LoadSettings();
    InitializeMeshBridge();
}

void Wh_ModBeforeUninit()
{
    if (g_hostProcess == HostProcess::GpuToolLauncher)
    {
        if (g_gpuToolProcess)
        {
            CloseHandle(g_gpuToolProcess);
            g_gpuToolProcess = nullptr;
        }
        return;
    }
    if (g_hostProcess == HostProcess::GpuToolChild)
    {
        StopGpuRenderer();
        return;
    }
    g_unloading.store(true, std::memory_order_release);
    Wh_Log(L"Preparing to unload");
    // Restore scene resources before Windhawk removes the hooks.
    StopWindowEventThread();
    CloseMeshBridge();
    UninitializeDpiSupport();
}

void Wh_ModUninit()
{
    if (g_hostProcess != HostProcess::GpuToolChild) return;
    if (g_gpuToolMutex)
    {
        CloseHandle(g_gpuToolMutex);
        g_gpuToolMutex = nullptr;
    }
    ExitProcess(0);
}

