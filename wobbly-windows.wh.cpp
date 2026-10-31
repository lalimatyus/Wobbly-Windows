// ==WindhawkMod==
// @id              wobbly-windows
// @name            Wobbly Windows
// @description     The classic Compiz/KDE Plasma style Wobbly Windows effect for Windows 11!
// @version         0.336
// @author          lalimatyus
// @github          https://github.com/lalimatyus
// @include         dwm.exe
// @architecture    amd64
// @compilerOptions -lwevtapi -luser32
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
* Uses a 4x4 spring simulation with Affine and smooth Bezier Mesh rendering

**Mesh Renderer** is an experimental in-process DWM path on the validated 23H2
build. Other builds use the Affine fallback. It bends a live window image with
20x20 Bezier interpolation, including its shadow, Mica and acrylic. Backdrop
materials are sampled for the original window and then deform with the image;
the original backdrop follows the mesh center, while fully nonlinear background
sampling is still experimental. While the
mesh is visible, the original remains nearly transparent to keep DWM's native
backdrop calculation active and is restored when the mesh detaches.
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
  $description: Mesh Renderer bends a live DWM window image on the validated 23H2 build. Unsupported builds use Affine.
  $options:
  - "affine": "Affine (stable)"
  - "mesh": "Mesh Renderer (experimental)"

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
#include <winevt.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <windhawk_utils.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{

// Physical coordinate scope begin: shared with the DPI regression test.
class ScopedDpiContext
{
public:
    explicit ScopedDpiContext(DPI_AWARENESS_CONTEXT desired)
        : previous_(desired ? SetThreadDpiAwarenessContext(desired) : nullptr),
          valid_(!desired || previous_ != nullptr) {}
    ~ScopedDpiContext()
    {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
    ScopedDpiContext(const ScopedDpiContext&) = delete;
    ScopedDpiContext& operator=(const ScopedDpiContext&) = delete;
    bool valid() const { return valid_; }
private:
    DPI_AWARENESS_CONTEXT previous_;
    bool valid_;
};
// Physical coordinate scope end.

// Invariants: private visual calls run only on the verified scene owner.
// Cached window pointers are invalidated by destructor hooks; page probes check
// accessibility, not lifetime. Slot pins protect proxies across unlocked calls,
// and generations reject stale results. Each slot owns one factory proxy ref;
// visuals bind its underlying composition resource, not the wrapper itself.

enum class RendererMode : unsigned char
{
    Affine,
    Mesh,
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

static WobblySettings GetSettingsSnapshot();

static bool ReadWindowRectForRenderer(HWND hwnd, RECT* bounds)
{
    if (GetSettingsSnapshot().rendererMode != RendererMode::Mesh)
        return GetWindowRect(hwnd, bounds) != FALSE;
    const auto windowContext = GetWindowDpiAwarenessContext(hwnd);
    if (!windowContext) return false;
    // Read in the HWND's own units, then explicitly convert BOTH corners.
    // A PMv2 caller scope alone still yielded virtualized bounds inside DWM.
    ScopedDpiContext coordinates(windowContext);
    RECT localBounds = {};
    if (!coordinates.valid() || !GetWindowRect(hwnd, &localBounds)) return false;
    if (GetAwarenessFromDpiAwarenessContext(windowContext) != DPI_AWARENESS_PER_MONITOR_AWARE)
    {
        POINT topLeft = {localBounds.left, localBounds.top};
        POINT bottomRight = {localBounds.right, localBounds.bottom};
        if (!LogicalToPhysicalPointForPerMonitorDPI(hwnd, &topLeft) ||
            !LogicalToPhysicalPointForPerMonitorDPI(hwnd, &bottomRight)) return false;
        localBounds = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    }
    if (localBounds.right <= localBounds.left || localBounds.bottom <= localBounds.top)
        return false;
    *bounds = localBounds;
    return true;
}

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
bool g_dragUsesPhysicalCoordinates = false; // Event thread only.
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

// Native mesh image and geometry types; private calls require the verified ABI.
struct NativeMeshRectF
{
    float left, top, right, bottom;
};
struct NativeMeshSizeD
{
    double width, height;
};
struct NativeMeshSizeF
{
    float width, height;
};
struct NativeMeshPoint3F
{
    float x, y, z;
};
struct NativeMeshPoint2D
{
    double x, y;
};
using NativeMeshCreateImage_t = long(__cdecl*)(void*, void**);
using NativeMeshUpdateImageD_t = long(__cdecl*)(
    void*, const NativeMeshRectF&, const NativeMeshSizeD&, const void*,
    const void*, void*, int);
using NativeMeshUpdateImageF_t = long(__cdecl*)(
    void*, const NativeMeshRectF&, const NativeMeshSizeF&, const void*,
    const void*, void*, int);
using NativeMeshCreateVisual_t = long(__cdecl*)(void**);
using NativeMeshAddInstruction_t = long(__cdecl*)(void*, void*);
using NativeMeshUpdateRenderData_t = long(__cdecl*)(void*);
using NativeMeshInsertRelative_t = long(__cdecl*)(
    void*, void*, void*, bool, bool);
using NativeMeshRemoveFromParent_t = long(__cdecl*)(void*);
using NativeMeshCreateMesh_t = long(__cdecl*)(void*, void**);
using NativeMeshUpdateMesh_t = long(__cdecl*)(
    void*, int, const NativeMeshPoint3F*, const NativeMeshPoint2D*,
    unsigned int, const unsigned int*, unsigned int);
using NativeMeshUpdateGroup_t = long(__cdecl*)(void*, void*);
// Its declared second argument is CBitmapSourceProxy*. The exact 23H2
// implementation only retains the base object and serializes its resource ID;
// the receiving core path accepts CImageSource. Other builds stay disabled.
using NativeMeshCreateMeshInstruction_t = long(__cdecl*)(void*, void*, void**);
using NativeMeshGetVisual_t = void*(__cdecl*)(void*);
NativeMeshCreateImage_t g_nativeMeshCreateImage = nullptr;
NativeMeshUpdateImageD_t g_nativeMeshUpdateImageD = nullptr;
NativeMeshUpdateImageF_t g_nativeMeshUpdateImageF = nullptr;
NativeMeshCreateVisual_t g_nativeMeshCreateVisual = nullptr;
NativeMeshAddInstruction_t g_nativeMeshAddInstruction = nullptr;
NativeMeshUpdateRenderData_t g_nativeMeshUpdateRenderData = nullptr;
NativeMeshInsertRelative_t g_nativeMeshInsertRelative = nullptr;
NativeMeshRemoveFromParent_t g_nativeMeshRemoveFromParent = nullptr;
void* g_nativeMeshCollectionRemove = nullptr;
void* g_nativeMeshCollectionVtable = nullptr;
void* g_nativeBackdropVisualVtable = nullptr;
std::atomic_bool g_nativeMeshActive = false;
std::atomic_bool g_nativeMeshStopRequested = false;
std::atomic<HWND> g_nativeMeshHwnd = nullptr;
using NativeCapturedRepresentationCreate_t = long(__cdecl*)(unsigned, const void*, void*, unsigned, int, void**);
using NativeCapturedRepresentationMethod_t = long(__cdecl*)(void*);
using NativeVisualSetOpacity_t = void(__cdecl*)(void*, double);
NativeCapturedRepresentationCreate_t g_nativeCapturedRepresentationCreate = nullptr;
NativeCapturedRepresentationMethod_t g_nativeCapturedRepresentationEnsureRoot = nullptr;
NativeCapturedRepresentationMethod_t g_nativeCapturedRepresentationValidate = nullptr;
NativeVisualSetOpacity_t g_nativeVisualSetOpacity = nullptr;
void* g_nativeCapturedRepresentationVtable = nullptr;
// Native listener interface with three verified no-op callbacks.
void* g_nativeCaptureListenerVtable = nullptr;
void* g_nativeCaptureCriticalSection = nullptr;
bool g_nativeCapturedSourceReady = false;
std::atomic_bool g_nativeMeshSourcePending = false;
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
    void* imageSourceProxy = nullptr;
    void* capturedRepresentation = nullptr;
    void* sourceOwner = nullptr;
    void* sourceTransform = nullptr;
    bool sourceMotionApplied = false;
    double sourceMotionX = 0.0, sourceMotionY = 0.0;
    ULONGLONG attachedAt = 0;
    std::uint64_t materialKey = 0;
    bool originalHidden = false;
    double originalOpacity = 1.0;
    bool backdropSource = false;
    int sourcePadding = 0;
    HWND hwnd = nullptr;
    int width = 0;
    int height = 0;
    bool detachFailureLogged = false;
    ULONGLONG meshGeneration = 0;
    ULONGLONG submittedMeshRevision = 0;
    unsigned int meshUpdateCount = 0;
};
NativeMeshState g_nativeMeshState; // DWM scene thread only.
NativeMeshCreateMesh_t g_nativeMeshCreateMesh = nullptr;
NativeMeshUpdateMesh_t g_nativeMeshUpdateMesh = nullptr;
NativeMeshCreateMesh_t g_nativeMeshCreateGroup = nullptr;
NativeMeshUpdateGroup_t g_nativeMeshUpdateGroup = nullptr;
NativeMeshCreateMeshInstruction_t g_nativeMeshCreateMeshInstruction = nullptr;
NativeMeshGetVisual_t g_nativeMeshGetVisualParent = nullptr;
NativeMeshGetVisual_t g_nativeMeshGetVisualProxy = nullptr;
void* g_nativeMeshImageVtableSymbol = nullptr;
std::atomic<void*> g_nativeMeshImageVtable = nullptr;
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

static void* ReadPointerMember(const void* object, size_t offset)
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
    bool hasSourceRect = ReadWindowRectForRenderer(hwnd, &sourceRect) != FALSE;
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
         &g_nativeMeshCreateImage, nullptr, true},
        {{L"public: static long __cdecl CSecondaryWindowRepresentation::Create("
           L"struct TMILFlagsEnum<enum SWRUsage::FlagsEnum>,"
           L"struct ISecondaryWindowRepresentationChangedListener *,class CWindowData *,"
           L"unsigned int,enum DEVICE_SCALE_FACTOR,class CSecondaryWindowRepresentation * *)"},
         &g_nativeCapturedRepresentationCreate, nullptr, true},
        {{L"public: long __cdecl CSecondaryWindowRepresentation::EnsureOwnedWindowVisual(void)"},
         &g_nativeCapturedRepresentationEnsureRoot, nullptr, true},
        {{L"public: long __cdecl CSecondaryWindowRepresentation::Validate(void)"},
         &g_nativeCapturedRepresentationValidate, nullptr, true},
        {{L"const CSecondaryWindowRepresentation::`vftable'{for `CBaseObject'}"},
         &g_nativeCapturedRepresentationVtable, nullptr, true},
        {{L"const CSWRListener::`vftable'{for `ISecondaryWindowRepresentationChangedListener'}"},
         &g_nativeCaptureListenerVtable, nullptr, true},
        {{L"public: static class CDwmCS CDesktopManager::s_csDwmInstance"},
         &g_nativeCaptureCriticalSection, nullptr, true},
        {{L"public: virtual void __cdecl CVisual::SetOpacity(double)"},
         &g_nativeVisualSetOpacity, nullptr, true},
        {{L"public: long __cdecl CCachedVisualImageProxy::Update("
           L"struct MilRectF const &,struct MilSizeD const &,"
           L"class CRectResourceProxy const *,"
           L"class CSizeResourceProxy const *,class CVisualProxy *,"
           L"enum MilBrushMappingMode::Enum)"},
         &g_nativeMeshUpdateImageD, nullptr, true},
        {{L"public: long __cdecl CCachedVisualImageProxy::Update("
           L"struct D2D_RECT_F const &,struct D2D_SIZE_F const &,"
           L"class CRectResourceProxy const *,"
           L"class CSizeResourceProxy const *,class CVisualProxy *,"
           L"enum MilBrushMappingMode::Enum)"},
         &g_nativeMeshUpdateImageF, nullptr, true},
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
        {{L"const CWindowBackgroundTreatmentVisual::`vftable'"},
         &g_nativeBackdropVisualVtable, nullptr, true},
        {{L"public: long __cdecl CCompositor::CreateMeshGeometry2dProxy("
           L"class CMeshGeometry2dProxy * *)",
          L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CMeshGeometry2dProxy>(class CMeshGeometry2dProxy * *)"},
         &g_nativeMeshCreateMesh, nullptr, true},
        {{L"public: long __cdecl CMeshGeometry2dProxy::Update("
           L"int,struct MilPoint3F const *,struct MilPoint2D const *,"
           L"unsigned int,unsigned int const *,unsigned int)"},
         &g_nativeMeshUpdateMesh, nullptr, true},
        {{L"public: long __cdecl CCompositor::CreateGeometry2dGroupProxy("
           L"class CGeometry2dGroupProxy * *)",
          L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CGeometry2dGroupProxy>(class CGeometry2dGroupProxy * *)"},
         &g_nativeMeshCreateGroup, nullptr, true},
        {{L"public: long __cdecl CGeometry2dGroupProxy::Update("
           L"class CMeshGeometry2dProxy const *)"},
         &g_nativeMeshUpdateGroup, nullptr, true},
        {{L"public: static long __cdecl CDrawMesh2DInstruction::Create("
           L"class CGeometry2dGroupProxy *,class CBitmapSourceProxy *,"
           L"class CDrawMesh2DInstruction * *)"},
         &g_nativeMeshCreateMeshInstruction, nullptr, true},
        {{L"public: virtual class CVisual * __cdecl "
           L"CVisual::GetTransformParent(void)const",
          L"public: virtual class CVisual * __cdecl "
           L"CVisual::GetTransformParent(void)const "},
         &g_nativeMeshGetVisualParent, nullptr, true},
        {{L"public: virtual class CVisualProxy * __cdecl "
           L"CVisual::GetVisualProxyForStructure(void)"},
         &g_nativeMeshGetVisualProxy, nullptr, true},
        {{L"const CCachedVisualImageProxy::`vftable'"},
         &g_nativeMeshImageVtableSymbol, nullptr, true},

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
    keepValid(g_nativeMeshCreateImage);
    keepValid(g_nativeCapturedRepresentationCreate);
    keepValid(g_nativeCapturedRepresentationEnsureRoot);
    keepValid(g_nativeCapturedRepresentationValidate);
    keepValid(g_nativeVisualSetOpacity);
    keepValid(g_nativeMeshUpdateImageD);
    keepValid(g_nativeMeshUpdateImageF);
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
    if (!IsDwmImageAddress(g_nativeBackdropVisualVtable, sizeof(void*)))
        g_nativeBackdropVisualVtable = nullptr;
    keepValid(g_nativeMeshCreateMesh);
    keepValid(g_nativeMeshUpdateMesh);
    keepValid(g_nativeMeshCreateGroup);
    keepValid(g_nativeMeshUpdateGroup);
    keepValid(g_nativeMeshCreateMeshInstruction);
    keepValid(g_nativeMeshGetVisualParent);
    keepValid(g_nativeMeshGetVisualProxy);
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
    cacheVtableSymbol(g_nativeMeshImageVtableSymbol, g_nativeMeshImageVtable);
    const auto nativeBase = reinterpret_cast<uintptr_t>(g_dwmModuleLayout.imageBegin);
    g_nativeCapturedSourceReady =
        g_dwmModuleLayout.timeDateStamp == 0xE19C3234 && g_dwmModuleLayout.sizeOfImage == 0x16A000 &&
        reinterpret_cast<uintptr_t>(g_nativeCapturedRepresentationCreate) == nativeBase + 0x33C30 &&
        reinterpret_cast<uintptr_t>(g_nativeCapturedRepresentationEnsureRoot) == nativeBase + 0x4D13C &&
        reinterpret_cast<uintptr_t>(g_nativeCapturedRepresentationValidate) == nativeBase + 0x33F2C &&
        reinterpret_cast<uintptr_t>(g_nativeCapturedRepresentationVtable) == nativeBase + 0x113CD0 &&
        reinterpret_cast<uintptr_t>(g_nativeCaptureListenerVtable) == nativeBase + 0x112310 &&
        reinterpret_cast<uintptr_t>(g_nativeCaptureCriticalSection) == nativeBase + 0x14BE80 &&
        IsReadableMemory(g_nativeCaptureCriticalSection, sizeof(CRITICAL_SECTION)) &&
        IsDwmImageAddress(g_nativeCapturedRepresentationVtable, sizeof(void*)) &&
        IsDwmImageAddress(g_nativeCaptureListenerVtable, sizeof(void*) * 3);
    if (g_nativeCapturedSourceReady)
    {
        const uintptr_t callbacks[] = {0x59100, 0x590C0, 0x590B0};
        for (unsigned i = 0; i < ARRAYSIZE(callbacks); ++i)
        {
            const auto* callback = static_cast<const BYTE*>(g_dwmModuleLayout.imageBegin) + callbacks[i];
            if (static_cast<void**>(g_nativeCaptureListenerVtable)[i] != callback ||
                !IsDwmImageAddress(callback, 3) || callback[0] != 0xC2 || callback[1] || callback[2])
                g_nativeCapturedSourceReady = false;
        }
    }
    if (reinterpret_cast<uintptr_t>(g_nativeVisualSetOpacity) != nativeBase + 0x41F60)
        g_nativeVisualSetOpacity = nullptr;
    Wh_Log(L"NATIVE CAPTURE SOURCE: ownedRepresentation=%s opacityControl=%s",
           g_nativeCapturedSourceReady ? L"available" : L"unavailable",
           g_nativeCapturedSourceReady && g_nativeVisualSetOpacity ? L"available" : L"unavailable");
    Wh_Log(L"NATIVE MESH SOURCE: create=%d update=%s imageVtable=%p",
           g_nativeMeshCreateImage != nullptr,
           g_nativeMeshUpdateImageD ? L"double" :
               (g_nativeMeshUpdateImageF ? L"float" : L"unavailable"),
           g_nativeMeshImageVtable.load(std::memory_order_acquire));
    Wh_Log(L"NATIVE MESH BRIDGE: mesh=%d update=%d group=%d "
           L"groupUpdate=%d meshFactory=%d cachedImageMesh=experimental "
           L"getParent=%d getProxy=%d",
           g_nativeMeshCreateMesh != nullptr,
           g_nativeMeshUpdateMesh != nullptr,
           g_nativeMeshCreateGroup != nullptr,
           g_nativeMeshUpdateGroup != nullptr,
           g_nativeMeshCreateMeshInstruction != nullptr,
           g_nativeMeshGetVisualParent != nullptr,
           g_nativeMeshGetVisualProxy != nullptr);
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
        !g_topLevelWindowGetRootVisual ||
        !g_nativeMeshGetVisualParent || !g_nativeMeshGetVisualProxy ||
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
    if (!parent || parent != g_nativeMeshGetVisualParent(root) ||
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

// Core material bridge: private layouts are gated by the exact module image.
using CoreExecuteBlur_t = long(__cdecl*)(void*, void*, const void*, const void*, void*);
using CoreIntermediateBrush_t = long(__cdecl*)(void*, unsigned int, const void*, void**);
CoreExecuteBlur_t g_coreExecuteBlurFunction = nullptr, g_coreExecuteBlurOriginal = nullptr;
CoreIntermediateBrush_t g_coreIntermediateBrushFunction = nullptr, g_coreIntermediateBrushOriginal = nullptr;
std::atomic<ULONGLONG> g_coreMaterialPipelineLogAt = 0;
using CoreOwnedImageCreate_t = long(__cdecl*)(void*, const void*, const void*, bool, void**);
using CoreOwnedImageValidate_t = long(__cdecl*)(void*, void*, const void*, bool, bool*);
using CoreOwnedImageRelease_t = unsigned long(__cdecl*)(void*);
using CoreOwnedStorageDestroy_t = void(__cdecl*)(void*);
CoreOwnedImageCreate_t g_coreOwnedImageCreateFunction = nullptr;
CoreOwnedImageValidate_t g_coreOwnedImageValidateFunction = nullptr;
CoreOwnedImageRelease_t g_coreOwnedImageReleaseFunction = nullptr;
CoreOwnedStorageDestroy_t g_coreOwnedBitmapDestroyFunction = nullptr, g_coreOwnedInputDestroyFunction = nullptr;
std::atomic_bool g_coreOwnedBackdropProbeReady = false;
std::atomic<ULONGLONG> g_coreOwnedBackdropProbeEpoch = UINT64_MAX;
using CoreMaterialInput_t = long(__cdecl*)(void*, const void*, void*);
using CoreImageUpdate_t = long(__cdecl*)(void*, void*, const void*);
using CoreImageRealize_t = long(__cdecl*)(void*, const void*, void**);
using CoreDrawVisualTree_t = long(__cdecl*)(void*, const void*, const void*, const void*, int, bool, void*);
using CoreFindTreeData_t = void*(__cdecl*)(const void*, const void*);
using CoreCurrentVisual_t = void*(__cdecl*)(const void*);
using CoreBackdropEffectInput_t = void*(__cdecl*)(void*, void*);
using CoreCurrentTargetInfo_t = const void*(__cdecl*)(const void*);
using CoreBackdropRealization_t = bool(__cdecl*)(const void*, const void*);
CoreMaterialInput_t g_coreBviValidationFunction = nullptr, g_coreBviValidationOriginal = nullptr;
CoreImageUpdate_t g_coreImageUpdateFunction = nullptr, g_coreImageUpdateOriginal = nullptr;
CoreImageRealize_t g_coreImageRealizeFunction = nullptr, g_coreImageRealizeOriginal = nullptr;
CoreDrawVisualTree_t g_coreDrawVisualTreeFunction = nullptr, g_coreDrawVisualTreeOriginal = nullptr;
CoreFindTreeData_t g_coreFindTreeDataFunction = nullptr;
CoreCurrentVisual_t g_coreCurrentVisualFunction = nullptr;
CoreBackdropEffectInput_t g_coreBackdropEffectInputFunction = nullptr, g_coreBackdropEffectInputOriginal = nullptr;
CoreCurrentTargetInfo_t g_coreCurrentTargetInfoFunction = nullptr;
CoreBackdropRealization_t g_coreBackdropRealizationFunction = nullptr;
void* g_coreBackdropImageVtable = nullptr, *g_coreCachedImageVtable = nullptr;
void* g_coreDesktopTreeVtable = nullptr, *g_coreVisualTreeVtable = nullptr, *g_coreSubVisualTreeVtable = nullptr;
void* g_coreDesktopTreeDataVtable = nullptr;
uintptr_t g_coreMaterialModuleBase = 0;
bool g_coreBackdropDetailReady = false, g_coreBackdropSceneReady = false, g_coreBackdropDesktopReady = false;
std::atomic_bool g_coreMaterialBridgeReady = false, g_coreBackdropBridgeReady = false;
std::atomic_bool g_coreBackdropBridgeLogged = false, g_coreBackdropReuseLogged = false;
std::atomic<ULONGLONG> g_coreBackdropFallbackLogAt = 0;
SRWLOCK g_coreMaterialTargetLock = SRWLOCK_INIT;
std::atomic<std::uint64_t> g_coreMaterialTargetKey = 0, g_coreMaterialReadyKey = 0;
std::atomic<ULONGLONG> g_coreMaterialTargetEpoch = 0;
std::atomic<HWND> g_coreMaterialTargetHwnd = nullptr;
std::atomic<void*> g_coreMaterialSourceRoot = nullptr, g_coreMaterialSourceImage = nullptr, g_coreMaterialSourceTree = nullptr;

static unsigned int NativeProxyResourceId(void* proxy)
{
    const auto* resource = static_cast<const BYTE*>(ReadPointerMember(proxy, 0x10));
    unsigned int id = 0;
    if (resource && IsReadableMemory(resource + 0x18, sizeof(id)))
        std::memcpy(&id, resource + 0x18, sizeof(id));
    return id;
}


static void ClearCoreMaterialTarget()
{
    AcquireSRWLockExclusive(&g_coreMaterialTargetLock);
    g_coreMaterialTargetKey.store(0, std::memory_order_release);
    g_coreMaterialReadyKey.store(0, std::memory_order_release);
    g_coreMaterialSourceRoot.store(nullptr, std::memory_order_release);
    g_coreMaterialSourceImage.store(nullptr, std::memory_order_release);
    g_coreMaterialSourceTree.store(nullptr, std::memory_order_release);
    g_coreMaterialTargetHwnd.store(nullptr, std::memory_order_release);
    g_coreBackdropBridgeLogged.store(false, std::memory_order_relaxed);
    g_coreBackdropReuseLogged.store(false, std::memory_order_relaxed);
    g_coreBackdropFallbackLogAt.store(0, std::memory_order_relaxed);
    g_coreMaterialPipelineLogAt.store(0, std::memory_order_relaxed);
    g_coreOwnedBackdropProbeEpoch.store(UINT64_MAX, std::memory_order_relaxed);
    g_coreMaterialTargetEpoch.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_coreMaterialTargetLock);
}

static void SetCoreMaterialTarget(void* image, void* sourceProxy, HWND hwnd)
{
    ClearCoreMaterialTarget();
    if (!g_coreMaterialBridgeReady.load(std::memory_order_acquire)) return;
    const unsigned imageId = NativeProxyResourceId(image), sourceId = NativeProxyResourceId(sourceProxy);
    if (!imageId || !sourceId) return;
    AcquireSRWLockExclusive(&g_coreMaterialTargetLock);
    g_coreMaterialTargetHwnd.store(hwnd, std::memory_order_relaxed);
    g_coreMaterialTargetKey.store((std::uint64_t{sourceId} << 32) | imageId, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_coreMaterialTargetLock);
}

static long __cdecl CoreImageUpdateHook(void* image, void* table, const void* command)
{
    const auto epoch = g_coreMaterialTargetEpoch.load(std::memory_order_acquire);
    const long result = g_coreImageUpdateOriginal(image, table, command);
    if (result < 0 || !g_coreMaterialBridgeReady.load(std::memory_order_acquire) ||
        g_unloading.load(std::memory_order_acquire)) return result;
    const auto key = g_coreMaterialTargetKey.load(std::memory_order_acquire);
    if (!key || !IsReadableMemory(command, 0x34) ||
        ReadPointerMember(image, 0) != g_coreCachedImageVtable) return result;
    unsigned imageId = 0, rootId = 0;
    std::memcpy(&imageId, static_cast<const BYTE*>(command) + 4, 4);
    std::memcpy(&rootId, static_cast<const BYTE*>(command) + 0x30, 4);
    if (((std::uint64_t{rootId} << 32) | imageId) != key) return result;
    // ProcessUpdate receives primary CImageSource, not IBitmapRealization (+48).
    void* root = ReadPointerMember(image, 0x90);
    AcquireSRWLockExclusive(&g_coreMaterialTargetLock);
    if (root && key == g_coreMaterialTargetKey.load(std::memory_order_relaxed) &&
        epoch == g_coreMaterialTargetEpoch.load(std::memory_order_relaxed))
    {
        if (root != g_coreMaterialSourceRoot.load(std::memory_order_relaxed) ||
            image != g_coreMaterialSourceImage.load(std::memory_order_relaxed))
        {
            g_coreMaterialReadyKey.store(0, std::memory_order_release);
            g_coreMaterialTargetEpoch.fetch_add(1, std::memory_order_release);
        }
        g_coreMaterialSourceRoot.store(root, std::memory_order_release);
        g_coreMaterialSourceImage.store(image, std::memory_order_release);
        g_coreMaterialSourceTree.store(ReadPointerMember(image, 0xB8), std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_coreMaterialTargetLock);
    return result;
}

static bool IsCoreMaterialVisual(const void* visual);

struct CoreBackdropSceneSnapshot
{
    void* treeRoot = nullptr;
    bool frameKnown = false;
    std::uint64_t currentFrame = 0, lastUsedFrame = 0, frameAge = UINT64_MAX;
    int effectReady = -1; // Raw native CanUseAsEffectInput gate, not proof of blurred pixels.
};

static CoreBackdropSceneSnapshot ReadCoreBackdropScene(void* data, void* image,
                                                       uintptr_t first)
{
    CoreBackdropSceneSnapshot scene;
    if (!g_coreBackdropSceneReady) return scene;
    void* tree = ReadPointerMember(reinterpret_cast<void*>(first + 8), 0);
    void* vtable = ReadPointerMember(tree, 0);
    if (vtable && (vtable == g_coreDesktopTreeVtable || vtable == g_coreVisualTreeVtable ||
                   vtable == g_coreSubVisualTreeVtable))
    {
        scene.treeRoot = ReadPointerMember(tree, 0x40);
    }
    // Native GetBackdropVisualImage stamps +7C0 from CTreeData's composition
    // frame. Read that counter here without calling the mutating getter.
    if (IsReadableMemory(static_cast<const BYTE*>(image) + 0x7A0, 1))
        scene.effectReady = *(static_cast<const BYTE*>(image) + 0x7A0);
    void* composition = ReadPointerMember(ReadPointerMember(data, 0x108), 0x10);
    if (composition && IsReadableMemory(static_cast<const BYTE*>(composition) + 0x1F0, 8) &&
        IsReadableMemory(static_cast<const BYTE*>(image) + 0x7C0, 8))
    {
        std::memcpy(&scene.currentFrame, static_cast<const BYTE*>(composition) + 0x1F0, 8);
        std::memcpy(&scene.lastUsedFrame, static_cast<const BYTE*>(image) + 0x7C0, 8);
        scene.frameKnown = true;
        if (scene.lastUsedFrame <= scene.currentFrame)
            scene.frameAge = scene.currentFrame - scene.lastUsedFrame;
    }
    return scene;
}

struct CoreBackdropSnapshot
{
    bool valid = false;
    unsigned images = 0;
    void* currentVisual = nullptr;
    void* currentTree = nullptr;
    void* image = nullptr;
    void* root = nullptr;
    unsigned alternateCount = 0;
    bool desktopValid = false;
    CoreBackdropSceneSnapshot scene;
    struct Alternate {
        void* image; void* root; void* tree;
        CoreBackdropSceneSnapshot scene;
    } alternates[4] = {};
};

static bool ReadCoreBackdropRegistry(void* data, void**& images, unsigned& count)
{
    const auto first = reinterpret_cast<uintptr_t>(ReadPointerMember(data, 0xA8));
    const auto last = reinterpret_cast<uintptr_t>(ReadPointerMember(data, 0xB0));
    if (last < first || (last - first) % sizeof(void*) || last - first > 32 * sizeof(void*) ||
        (last != first && (!first || !IsReadableMemory(reinterpret_cast<void*>(first), last - first))))
        return false;
    images = reinterpret_cast<void**>(first);
    count = static_cast<unsigned>((last - first) / sizeof(void*));
    return true;
}

static void CollectCoreBackdropAlternates(void* currentData, CoreBackdropSnapshot& hit)
{
    auto collect = [&](void* data, bool desktop = false)
    {
        void** images = nullptr;
        unsigned count = 0;
        if (!ReadCoreBackdropRegistry(data, images, count)) { return; }
        if (desktop) { hit.desktopValid = true; }
        for (unsigned i = 0; i < count; ++i)
        {
            void* image = images[i];
            if (image == hit.image || ReadPointerMember(image, 0) != g_coreBackdropImageVtable) continue;
            void* root = ReadPointerMember(image, 0x90);
            if (!root || root == hit.root) continue;
            const auto first = reinterpret_cast<uintptr_t>(ReadPointerMember(image, 0x700));
            const auto last = reinterpret_cast<uintptr_t>(ReadPointerMember(image, 0x708));
            if (!first || last <= first || (last - first) % 16 || last - first > 32 * 16 ||
                !IsReadableMemory(reinterpret_cast<void*>(first), last - first))
                { continue; }
            if (desktop)
            {
                void* tree = ReadPointerMember(reinterpret_cast<void*>(last - 8), 0);
                if (ReadPointerMember(tree, 0) != g_coreDesktopTreeVtable ||
                    g_coreFindTreeDataFunction(hit.currentVisual, tree) != data)
                    { continue; }
            }
            bool duplicate = false;
            for (unsigned j = 0; j < hit.alternateCount; ++j)
                duplicate |= hit.alternates[j].image == image;
            if (duplicate) continue;
            if (hit.alternateCount == ARRAYSIZE(hit.alternates)) { continue; }
            hit.alternates[hit.alternateCount++] = {
                image, root, ReadPointerMember(reinterpret_cast<void*>(first + 8), 0),
                ReadCoreBackdropScene(data, image, first)};
        }
    };
    // FindTreeData's IsDesktop branch uses embedded CDesktopTreeData, not the
    // property-1 association list. Prefer this registry over old copies.
    void* desktopData = static_cast<BYTE*>(hit.currentVisual) + 0x150;
    if (g_coreBackdropDesktopReady && desktopData != currentData &&
        IsReadableMemory(desktopData, 0x110) &&
        ReadPointerMember(desktopData, 0) == g_coreDesktopTreeDataVtable &&
        ReadPointerMember(desktopData, 0x108) == hit.currentVisual)
        collect(desktopData, true);

}

static CoreBackdropSnapshot ReadCoreBackdropSnapshot(void* context)
{
    CoreBackdropSnapshot hit;
    if (!g_coreBackdropDetailReady || !context) return hit;
    // Exact native ValidateBVIEffectInputForRender / GetBackdropVisualImage
    // layouts. Borrow only for this callback; never retain or transfer images.
    void* path = ReadPointerMember(context, 0x1F90);
    const auto begin = reinterpret_cast<uintptr_t>(ReadPointerMember(path, 0));
    const auto end = reinterpret_cast<uintptr_t>(ReadPointerMember(path, 8));
    if (!begin || end <= begin || (end - begin) % 16 || end - begin > 32 * 16 ||
        !IsReadableMemory(reinterpret_cast<void*>(begin), end - begin)) return hit;
    hit.currentTree = ReadPointerMember(reinterpret_cast<void*>(end - 8), 0);
    hit.currentVisual = g_coreCurrentVisualFunction(static_cast<const BYTE*>(context) + 0x18);
    if (!hit.currentTree || !IsCoreMaterialVisual(hit.currentVisual)) return hit;
    void* data = g_coreFindTreeDataFunction(hit.currentVisual, hit.currentTree);
    if (!data) return hit;
    void** images = nullptr;
    if (!ReadCoreBackdropRegistry(data, images, hit.images)) return hit;
    hit.valid = true;
    for (unsigned i = 0; i < hit.images; ++i)
    {
        void* image = images[i];
        if (ReadPointerMember(image, 0) != g_coreBackdropImageVtable) continue;
        const auto imageBegin = reinterpret_cast<uintptr_t>(ReadPointerMember(image, 0x700));
        const auto imageEnd = reinterpret_cast<uintptr_t>(ReadPointerMember(image, 0x708));
        if (!imageBegin || imageEnd < imageBegin || imageEnd - imageBegin != end - begin ||
            !IsReadableMemory(reinterpret_cast<void*>(imageBegin), imageEnd - imageBegin) ||
            std::memcmp(reinterpret_cast<void*>(begin), reinterpret_cast<void*>(imageBegin), end - begin))
            continue;
        hit.image = image;
        hit.root = ReadPointerMember(image, 0x90);
        hit.scene = ReadCoreBackdropScene(data, image, imageBegin);
        break;
    }
    CollectCoreBackdropAlternates(data, hit);
    return hit;
}

// Only metadata and call-scoped identities are observed. Native EffectInput
// objects keep their original ownership; no resource or effect is substituted.
struct CoreMaterialInputIdentity
{
    void* bitmap = nullptr;
    void* brush = nullptr;
};

struct CoreMaterialInputMetadata
{
    bool readable = false;
    float transform[9] = {}, bounds[4] = {};
    unsigned flags = 0;
    uintptr_t brushType = 0;
};

struct CoreMaterialObservation
{
    ULONGLONG bridgeEpoch = 0;
    void* bridgeContext = nullptr; // Borrowed only within this DrawVisualTree.
    void* drawContext = nullptr;
    unsigned fallbacks = 0;
    const wchar_t* fallbackReason = L"none";
    int fallbackRenderingValid = -1;
    CoreMaterialInputIdentity materialInputs[8] = {};
    unsigned inputCount = 0, blurCalls = 0, materialBlurCalls = 0;
    unsigned intermediateCalls = 0, materialIntermediateCalls = 0;
    CoreMaterialInputMetadata blurInput, blurOutput;
};
thread_local CoreMaterialObservation* g_coreMaterialObservation = nullptr;
thread_local unsigned g_coreMaterialRealizeDepth = 0;
thread_local void* g_coreMaterialCurrentImage = nullptr;
thread_local bool g_coreBackdropBridgeInCall = false;

static CoreMaterialObservation* CurrentCoreMaterialObservation()
{
    auto* observation = g_coreMaterialObservation;
    return observation && !g_unloading.load(std::memory_order_acquire) &&
        g_coreMaterialBridgeReady.load(std::memory_order_acquire) &&
        g_coreMaterialCurrentImage &&
        g_coreMaterialCurrentImage == g_coreMaterialSourceImage.load(std::memory_order_acquire) &&
        observation->bridgeEpoch == g_coreMaterialTargetEpoch.load(std::memory_order_acquire)
        ? observation : nullptr;
}

static CoreMaterialInputIdentity ReadCoreMaterialInputIdentity(const void* input)
{
    return {ReadPointerMember(input, 0), ReadPointerMember(input, 8)};
}

static bool IsCoreMaterialInput(const CoreMaterialObservation& observation,
                                CoreMaterialInputIdentity input)
{
    if (!input.bitmap && !input.brush) return false;
    for (unsigned i = 0; i < observation.inputCount; ++i)
        if (observation.materialInputs[i].bitmap == input.bitmap &&
            observation.materialInputs[i].brush == input.brush) return true;
    return false;
}

static void RememberCoreMaterialInput(CoreMaterialObservation& observation,
                                      CoreMaterialInputIdentity input)
{
    if ((!input.bitmap && !input.brush) || IsCoreMaterialInput(observation, input) ||
        observation.inputCount == ARRAYSIZE(observation.materialInputs)) return;
    observation.materialInputs[observation.inputCount++] = input;
}

static CoreMaterialInputMetadata ReadCoreMaterialInputMetadata(const void* input)
{
    CoreMaterialInputMetadata metadata;
    if (!IsReadableMemory(input, 0x68) || static_cast<const BYTE*>(input)[0x2C] != 1) return metadata;
    const auto* bytes = static_cast<const BYTE*>(input);
    // Native EffectInput copy/assignment: two managed inputs at +0/+8,
    // 3x3 mapping at +30, bounds at +54, four property bytes at +64.
    std::memcpy(metadata.transform, bytes + 0x30, sizeof(metadata.transform));
    std::memcpy(metadata.bounds, bytes + 0x54, sizeof(metadata.bounds));
    std::memcpy(&metadata.flags, bytes + 0x64, sizeof(metadata.flags));
    const auto type = reinterpret_cast<uintptr_t>(ReadPointerMember(ReadPointerMember(input, 8), 0));
    if (type >= g_coreMaterialModuleBase && type - g_coreMaterialModuleBase < 0x41A000)
        metadata.brushType = type - g_coreMaterialModuleBase;
    metadata.readable = true;
    return metadata;
}

static long __cdecl CoreExecuteBlurHook(void* technique, void* context,
                                       const void* input, const void* amount, void* output)
{
    auto* observation = CurrentCoreMaterialObservation();
    const bool material = observation &&
        IsCoreMaterialInput(*observation, ReadCoreMaterialInputIdentity(input));
    // Read before forwarding: native callers may alias input and output.
    const auto metadata = material ? ReadCoreMaterialInputMetadata(input)
                                   : CoreMaterialInputMetadata{};
    const long result = g_coreExecuteBlurOriginal(technique, context, input, amount, output);
    if (observation && observation == CurrentCoreMaterialObservation())
    {
        ++observation->blurCalls;
        if (material)
        {
            ++observation->materialBlurCalls;
            observation->blurInput = metadata;
            observation->blurOutput = {};
            if (result >= 0)
            {
                observation->blurOutput = ReadCoreMaterialInputMetadata(output);
                RememberCoreMaterialInput(*observation, ReadCoreMaterialInputIdentity(output));
            }
        }
    }
    return result;
}

static long __cdecl CoreIntermediateBrushHook(void* technique, unsigned int index,
                                             const void* input, void** output)
{
    auto* observation = CurrentCoreMaterialObservation();
    const bool material = observation &&
        IsCoreMaterialInput(*observation, ReadCoreMaterialInputIdentity(input));
    const long result = g_coreIntermediateBrushOriginal(technique, index, input, output);
    if (observation && observation == CurrentCoreMaterialObservation())
    {
        ++observation->intermediateCalls;
        if (material) ++observation->materialIntermediateCalls;
    }
    return result;
}

static long __cdecl CoreImageRealizeHook(void*, const void*, void**);

struct CoreOwnedBackdropProbeScope
{
    CoreMaterialObservation* previous = g_coreMaterialObservation;
    void* image = nullptr;
    void* bitmap = nullptr;
    alignas(16) BYTE input[0x68] = {};
    bool inputConstructed = false;

    CoreOwnedBackdropProbeScope() { g_coreMaterialObservation = nullptr; }
    CoreOwnedBackdropProbeScope(const CoreOwnedBackdropProbeScope&) = delete;
    CoreOwnedBackdropProbeScope& operator=(const CoreOwnedBackdropProbeScope&) = delete;
    ~CoreOwnedBackdropProbeScope()
    {
        // These are native managed destructors, called on the producing thread.
        if (inputConstructed) g_coreOwnedInputDestroyFunction(input);
        if (bitmap) g_coreOwnedBitmapDestroyFunction(&bitmap);
        if (image) g_coreOwnedImageReleaseFunction(image);
        g_coreMaterialObservation = previous;
    }
};

static bool CoreOwnedProbeCurrent(const CoreMaterialObservation& observation)
{
    return g_coreOwnedBackdropProbeReady.load(std::memory_order_acquire) &&
        !g_unloading.load(std::memory_order_acquire) &&
        observation.bridgeEpoch == g_coreMaterialTargetEpoch.load(std::memory_order_acquire) &&
        g_coreMaterialCurrentImage &&
        g_coreMaterialCurrentImage == g_coreMaterialSourceImage.load(std::memory_order_acquire);
}

static void RunCoreOwnedBackdropProbe(CoreMaterialObservation& observation,
                                      void* candidate, void* owner, const void* referenceInput)
{
    if (!CoreOwnedProbeCurrent(observation) || !IsCoreMaterialVisual(owner) ||
        ReadPointerMember(candidate, 0) != g_coreBackdropImageVtable ||
        !IsReadableMemory(static_cast<const BYTE*>(candidate) + 0x7A1, 1)) return;
    auto* path = static_cast<BYTE*>(candidate) + 0x700;
    const auto first = reinterpret_cast<uintptr_t>(ReadPointerMember(path, 0));
    const auto last = reinterpret_cast<uintptr_t>(ReadPointerMember(path, 8));
    const auto capacity = reinterpret_cast<uintptr_t>(ReadPointerMember(path, 16));
    if (!first || last <= first || capacity < last || (last - first) % 16 ||
        last - first > 32 * 16 || !IsReadableMemory(reinterpret_cast<void*>(first), last - first)) return;
    void* tree = ReadPointerMember(reinterpret_cast<void*>(last - 8), 0);
    void* root = ReadPointerMember(candidate, 0x90);
    void* composition = ReadPointerMember(owner, 0x10);
    const auto rootType = reinterpret_cast<uintptr_t>(ReadPointerMember(root, 0));
    if (ReadPointerMember(tree, 0) != g_coreDesktopTreeVtable ||
        ReadPointerMember(tree, 0x40) != root || rootType < g_coreMaterialModuleBase ||
        rootType - g_coreMaterialModuleBase >= 0x41A000 ||
        !composition || composition != ReadPointerMember(root, 0x10) ||
        composition != ReadPointerMember(reinterpret_cast<void*>(g_coreMaterialModuleBase + 0x3E5930), 0)) return;
    float bounds[4];
    const auto* sourceBounds = static_cast<const BYTE*>(candidate) + 0x60;
    if (!IsReadableMemory(sourceBounds, sizeof(bounds))) return;
    std::memcpy(bounds, sourceBounds, sizeof(bounds));
    for (float value : bounds) if (!std::isfinite(value)) return;
    const double width = static_cast<double>(bounds[2]) - bounds[0];
    const double height = static_cast<double>(bounds[3]) - bounds[1];
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || width * height > 33554432) return;
    const BYTE nativeMode = *(static_cast<const BYTE*>(candidate) + 0x7A1);
    const void* target = g_coreCurrentTargetInfoFunction(
        static_cast<const BYTE*>(observation.bridgeContext) + 0x18);
    if (nativeMode > 1 || !IsReadableMemory(target, 0x18)) return;
    auto tested = g_coreOwnedBackdropProbeEpoch.load(std::memory_order_relaxed);
    if (tested == observation.bridgeEpoch ||
        !g_coreOwnedBackdropProbeEpoch.compare_exchange_strong(tested, observation.bridgeEpoch,
                                                              std::memory_order_relaxed) ||
        !CoreOwnedProbeCurrent(observation)) return;

    const wchar_t* stage = L"Create";
    long result = E_FAIL;
    bool exactType = false, rootMatch = false, bitmapValid = false;
    bool comparableMapping = false;
    uintptr_t bitmapType = 0;
    CoreMaterialInputMetadata ownMetadata;
    {
        CoreOwnedBackdropProbeScope scope;
        Wh_Log(L"MESH OWN BACKDROP STAGE: Create HWND=%p",
               g_coreMaterialTargetHwnd.load(std::memory_order_acquire));
        result = g_coreOwnedImageCreateFunction(composition, owner, path, nativeMode != 0, &scope.image);
        if (result >= 0 && !scope.image) result = E_UNEXPECTED;
        if (result >= 0)
        {
            exactType = ReadPointerMember(scope.image, 0) == g_coreBackdropImageVtable;
            if (!exactType) result = E_NOINTERFACE;
        }
        if (result >= 0 && !CoreOwnedProbeCurrent(observation)) result = E_ABORT;
        if (result >= 0)
        {
            stage = L"Validate";
            bool changed = false;
            Wh_Log(L"MESH OWN BACKDROP STAGE: Validate HWND=%p",
                   g_coreMaterialTargetHwnd.load(std::memory_order_acquire));
            result = g_coreOwnedImageValidateFunction(scope.image, root, bounds, false, &changed);
            rootMatch = ReadPointerMember(scope.image, 0x90) == root;
            if (result >= 0 && !rootMatch) result = E_UNEXPECTED;
        }
        if (result >= 0 && !CoreOwnedProbeCurrent(observation)) result = E_ABORT;
        if (result >= 0)
        {
            stage = L"Realize";
            Wh_Log(L"MESH OWN BACKDROP STAGE: Realize HWND=%p",
                   g_coreMaterialTargetHwnd.load(std::memory_order_acquire));
            // Use the wrapper too: nested rendering must have its own identity.
            result = CoreImageRealizeHook(scope.image, target, &scope.bitmap);
            if (result >= 0 && !scope.bitmap) result = E_UNEXPECTED;
            const auto vtable = reinterpret_cast<uintptr_t>(ReadPointerMember(scope.bitmap, 0));
            if (vtable >= g_coreMaterialModuleBase && vtable - g_coreMaterialModuleBase < 0x41A000)
                bitmapType = vtable - g_coreMaterialModuleBase;
        }
        if (result >= 0 && !CoreOwnedProbeCurrent(observation)) result = E_ABORT;
        if (result >= 0)
        {
            bitmapValid = g_coreBackdropRealizationFunction(scope.image, target);
            stage = L"InspectInput";
            // Construct and inspect an unattached input. Its projection is NOT
            // assumed ready and it is never returned to the window's renderer.
            g_coreBackdropEffectInputOriginal(scope.image, scope.input);
            scope.inputConstructed = true;
            ownMetadata = ReadCoreMaterialInputMetadata(scope.input);
            const auto reference = ReadCoreMaterialInputMetadata(referenceInput);
            comparableMapping = ownMetadata.readable && reference.readable &&
                !std::memcmp(ownMetadata.transform, reference.transform, sizeof(reference.transform)) &&
                !std::memcmp(ownMetadata.bounds, reference.bounds, sizeof(reference.bounds));
        }
    }
    Wh_Log(L"MESH OWN BACKDROP PROBE: HWND=%p stage=%s result=0x%08X exactType=%d "
           L"rootMatch=%d bitmapValid=%d bitmapType=0x%llX mappingMatch=%d "
           L"sourceBounds=(%.1f,%.1f,%.1f,%.1f) ownMap=(%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g) "
           L"attached=0 renderer=unchanged",
           g_coreMaterialTargetHwnd.load(std::memory_order_acquire), stage, result,
           exactType, rootMatch, bitmapValid, static_cast<unsigned long long>(bitmapType), comparableMapping,
           bounds[0], bounds[1], bounds[2], bounds[3],
           ownMetadata.transform[0], ownMetadata.transform[1], ownMetadata.transform[2],
           ownMetadata.transform[3], ownMetadata.transform[4], ownMetadata.transform[5],
           ownMetadata.transform[6], ownMetadata.transform[7], ownMetadata.transform[8]);
}

static void* __cdecl CoreBackdropEffectInputHook(void* image, void* output)
{
    auto* observation = g_coreMaterialObservation;
    if (!g_coreBackdropBridgeReady.load(std::memory_order_acquire) ||
        !g_coreMaterialBridgeReady.load(std::memory_order_acquire) ||
        g_unloading.load(std::memory_order_acquire) || !output || !observation ||
        g_coreBackdropBridgeInCall || !g_coreMaterialCurrentImage ||
        g_coreMaterialCurrentImage != g_coreMaterialSourceImage.load(std::memory_order_acquire) ||
        observation->bridgeEpoch != g_coreMaterialTargetEpoch.load(std::memory_order_acquire))
        return g_coreBackdropEffectInputOriginal(image, output);

    const auto hit = observation->bridgeContext
        ? ReadCoreBackdropSnapshot(observation->bridgeContext) : CoreBackdropSnapshot{};
    const wchar_t* reason = observation->bridgeContext ? L"SourcePath" : L"MissingContext";
    void* selected = nullptr;
    std::uint64_t lastUseAge = 0;
    void* notReadySource = nullptr; // Borrowed only within this callback.
    if (hit.valid && hit.image == image && hit.desktopValid && hit.scene.frameKnown &&
        hit.root == g_coreMaterialSourceRoot.load(std::memory_order_acquire))
    {
        reason = L"DesktopUnavailable";
        for (unsigned i = 0; i < hit.alternateCount; ++i)
        {
            const auto& candidate = hit.alternates[i];
            const auto& scene = candidate.scene;
            if (ReadPointerMember(candidate.tree, 0) != g_coreDesktopTreeVtable) continue;
            if (scene.treeRoot != candidate.root)
                { reason = L"DesktopRoot"; continue; }
            if (!scene.frameKnown || scene.currentFrame != hit.scene.currentFrame ||
                scene.lastUsedFrame > scene.currentFrame)
                { reason = L"DesktopClock"; continue; }
            if (scene.effectReady != 1)
                { reason = L"EffectNotReady"; notReadySource = candidate.image; continue; }
            if (selected) { selected = nullptr; reason = L"AmbiguousDesktop"; break; }
            selected = candidate.image;
            lastUseAge = scene.frameAge;
        }
    }
    if (selected)
    {
        const void* targetInfo = g_coreCurrentTargetInfoFunction(
            static_cast<const BYTE*>(observation->bridgeContext) + 0x18);
        // +7C0 is a last-use stamp, not the age of rendered pixels. Native
        // HasValidRenderingRealization also checks RequiresRendering, the target,
        // device status and cache invalidation; idle caches can remain valid.
        if (!targetInfo || !g_coreBackdropRealizationFunction(selected, targetInfo))
        {
            selected = nullptr;
            reason = L"RenderTargetNotReady";
        }
    }
    if (observation->bridgeEpoch != g_coreMaterialTargetEpoch.load(std::memory_order_acquire) ||
        g_unloading.load(std::memory_order_acquire))
    {
        selected = nullptr;
        reason = L"TargetRetired";
    }
    if (!selected)
    {
        observation->fallbackRenderingValid = -1;
        const auto lastLog = g_coreBackdropFallbackLogAt.load(std::memory_order_relaxed);
        if (notReadySource && reason == std::wstring_view(L"EffectNotReady") &&
            (!lastLog || GetTickCount64() - lastLog >= 1000) &&
            observation->bridgeEpoch == g_coreMaterialTargetEpoch.load(std::memory_order_acquire))
        {
            const void* targetInfo = g_coreCurrentTargetInfoFunction(
                static_cast<const BYTE*>(observation->bridgeContext) + 0x18);
            observation->fallbackRenderingValid = targetInfo
                ? static_cast<int>(g_coreBackdropRealizationFunction(notReadySource, targetInfo)) : -1;
        }
        ++observation->fallbacks;
        observation->fallbackReason = reason;
        void* result = g_coreBackdropEffectInputOriginal(image, output);
        if (notReadySource && reason == std::wstring_view(L"EffectNotReady"))
            RunCoreOwnedBackdropProbe(*observation, notReadySource, hit.currentVisual, result);
        return result;
    }

    // The native factory constructs EffectInput and owns its normal references.
    // No structure copy, context-field swap or client/server image cast occurs.
    g_coreBackdropBridgeInCall = true;
    void* result = g_coreBackdropEffectInputOriginal(selected, output);
    g_coreBackdropBridgeInCall = false;
    if (observation == CurrentCoreMaterialObservation())
        RememberCoreMaterialInput(*observation, ReadCoreMaterialInputIdentity(result));
    if (!g_coreBackdropBridgeLogged.load(std::memory_order_relaxed) &&
        !g_coreBackdropBridgeLogged.exchange(true, std::memory_order_relaxed))
        Wh_Log(L"MESH ACRYLIC ACTIVE: HWND=%p nativeDesktopInput=1",
               g_coreMaterialTargetHwnd.load(std::memory_order_acquire));
    if (lastUseAge > 1 && !g_coreBackdropReuseLogged.load(std::memory_order_relaxed) &&
        !g_coreBackdropReuseLogged.exchange(true, std::memory_order_relaxed))
        Wh_Log(L"MESH ACRYLIC CACHE REUSE: HWND=%p lastUseAge=%llu nativeValid=1",
               g_coreMaterialTargetHwnd.load(std::memory_order_acquire),
               static_cast<unsigned long long>(lastUseAge));
    return result;
}


static bool IsCoreMaterialVisual(const void* visual)
{
    if (!g_coreMaterialModuleBase || !IsReadableMemory(visual, 0xF0)) return false;
    const uintptr_t vtable = reinterpret_cast<uintptr_t>(ReadPointerMember(visual, 0));
    if (vtable < g_coreMaterialModuleBase ||
        vtable - g_coreMaterialModuleBase >= 0x41A000) return false;
    const auto* properties = static_cast<const BYTE*>(ReadPointerMember(visual, 0xE8));
    unsigned flags = 0, count = 0;
    if (!IsReadableMemory(properties, 8)) return false;
    std::memcpy(&flags, properties, 4);
    std::memcpy(&count, properties + 4, 4);
    if (count > 64 || !IsReadableMemory(properties, ((8 + count + 7) & ~size_t{7}) + count * sizeof(void*)))
        return false;
    if ((flags & 0x200000) && !std::memchr(properties + 8, 11, count)) return false;
    if ((flags & 0x100000) && !std::memchr(properties + 8, 12, count)) return false;
    return true;
}


static long __cdecl CoreBviValidationHook(void* context, const void* bounds, void* flag)
{
    const long result = g_coreBviValidationOriginal(context, bounds, flag);
    if (result >= 0)
        if (auto* observation = CurrentCoreMaterialObservation();
            observation && observation->drawContext == context)
            observation->bridgeContext = context;
    return result;
}

static long __cdecl CoreDrawVisualTreeHook(void* context, const void* tree, const void* bounds,
                                          const void* occlusion, int clearMode, bool drawFlag, void* root)
{
    bool observe = tree && g_coreMaterialBridgeReady.load(std::memory_order_acquire) &&
        !g_unloading.load(std::memory_order_acquire) &&
        tree == g_coreMaterialSourceTree.load(std::memory_order_acquire);
    ULONGLONG epoch = 0;
    if (observe)
    {
        AcquireSRWLockShared(&g_coreMaterialTargetLock);
        observe = g_coreMaterialTargetKey.load(std::memory_order_relaxed) &&
                  tree == g_coreMaterialSourceTree.load(std::memory_order_relaxed);
        epoch = g_coreMaterialTargetEpoch.load(std::memory_order_relaxed);
        ReleaseSRWLockShared(&g_coreMaterialTargetLock);
    }
    if (!observe)
    {
        // A nested foreign tree cannot publish its temporary context into the
        // outer source observation, even if it reuses the same drawing object.
        auto* previous = g_coreMaterialObservation;
        g_coreMaterialObservation = nullptr;
        const long result = g_coreDrawVisualTreeOriginal(context, tree, bounds, occlusion, clearMode, drawFlag, root);
        g_coreMaterialObservation = previous;
        return result;
    }
    CoreMaterialObservation observation;
    observation.bridgeEpoch = epoch;
    observation.drawContext = context;
    auto* previous = g_coreMaterialObservation;
    g_coreMaterialObservation = &observation;
    const long result = g_coreDrawVisualTreeOriginal(context, tree, bounds, occlusion, clearMode, drawFlag, root);
    g_coreMaterialObservation = previous;
    if (epoch == g_coreMaterialTargetEpoch.load(std::memory_order_acquire) &&
        !g_unloading.load(std::memory_order_acquire) &&
        (observation.inputCount || observation.fallbacks))
    {
        const auto now = GetTickCount64();
        auto last = g_coreMaterialPipelineLogAt.load(std::memory_order_relaxed);
        if ((!last || now - last >= 1000) &&
            g_coreMaterialPipelineLogAt.compare_exchange_strong(last, now, std::memory_order_relaxed))
        {
            const auto& in = observation.blurInput;
            const auto& out = observation.blurOutput;
            Wh_Log(L"MESH ACRYLIC PIPELINE: HWND=%p inputs=%u blur=%u matchedBlur=%u "
                   L"intermediate=%u matchedIntermediate=%u fallback=%u "
                   L"inputMetadata=%d outputMetadata=%d outputRealization=0x%llX "
                   L"inputBounds=(%.1f,%.1f,%.1f,%.1f) "
                   L"inputMap=(%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g) "
                   L"outputFlags=0x%X readOnly=1",
                   g_coreMaterialTargetHwnd.load(std::memory_order_acquire),
                   observation.inputCount, observation.blurCalls, observation.materialBlurCalls,
                   observation.intermediateCalls, observation.materialIntermediateCalls,
                   observation.fallbacks, in.readable, out.readable,
                   static_cast<unsigned long long>(out.brushType),
                   in.bounds[0], in.bounds[1], in.bounds[2], in.bounds[3],
                   in.transform[0], in.transform[1], in.transform[2], in.transform[3],
                   in.transform[4], in.transform[5], in.transform[6], in.transform[7], in.transform[8],
                   out.flags);
        }
    }
    if (observation.fallbacks && epoch == g_coreMaterialTargetEpoch.load(std::memory_order_acquire) &&
        !g_unloading.load(std::memory_order_acquire))
    {
        const auto now = GetTickCount64();
        auto last = g_coreBackdropFallbackLogAt.load(std::memory_order_relaxed);
        if ((!last || now - last >= 1000) &&
            g_coreBackdropFallbackLogAt.compare_exchange_strong(last, now, std::memory_order_relaxed))
            Wh_Log(L"MESH ACRYLIC FALLBACK: HWND=%p reason=%s calls=%u renderingValid=%d",
                   g_coreMaterialTargetHwnd.load(std::memory_order_acquire),
                   observation.fallbackReason, observation.fallbacks, observation.fallbackRenderingValid);
    }
    return result;
}

static long __cdecl CoreImageRealizeHook(void* image, const void* targetInfo, void** bitmap)
{
    bool observe = g_coreMaterialBridgeReady.load(std::memory_order_acquire) &&
        !g_unloading.load(std::memory_order_acquire) &&
        image == g_coreMaterialSourceImage.load(std::memory_order_acquire);
    ULONGLONG epoch = 0;
    std::uint64_t key = 0;
    if (observe)
    {
        AcquireSRWLockShared(&g_coreMaterialTargetLock);
        key = g_coreMaterialTargetKey.load(std::memory_order_relaxed);
        observe = key && image == g_coreMaterialSourceImage.load(std::memory_order_relaxed);
        epoch = g_coreMaterialTargetEpoch.load(std::memory_order_relaxed);
        ReleaseSRWLockShared(&g_coreMaterialTargetLock);
    }
    if (!observe && !g_coreMaterialRealizeDepth)
        return g_coreImageRealizeOriginal(image, targetInfo, bitmap);
    // Nested realizations must expose their own identity to prevent recursion.
    void* previousImage = g_coreMaterialCurrentImage;
    g_coreMaterialCurrentImage = image;
    ++g_coreMaterialRealizeDepth;
    const long result = g_coreImageRealizeOriginal(image, targetInfo, bitmap);
    --g_coreMaterialRealizeDepth;
    g_coreMaterialCurrentImage = previousImage;
    bool lostBitmap = false;
    if (observe)
    {
        AcquireSRWLockExclusive(&g_coreMaterialTargetLock);
        if (epoch == g_coreMaterialTargetEpoch.load(std::memory_order_relaxed) &&
            key == g_coreMaterialTargetKey.load(std::memory_order_relaxed) &&
            image == g_coreMaterialSourceImage.load(std::memory_order_relaxed))
        {
            g_coreMaterialSourceTree.store(ReadPointerMember(image, 0xB8), std::memory_order_release);
            const bool available = result >= 0 && ReadPointerMember(bitmap, 0);
            const auto previousReady = g_coreMaterialReadyKey.exchange(available ? key : 0,
                                                                       std::memory_order_acq_rel);
            lostBitmap = !available && previousReady == key;
        }
        ReleaseSRWLockExclusive(&g_coreMaterialTargetLock);
    }
    if (lostBitmap) RequestDwmScenePass();
    return result;
}

static void InitializeCoreMaterialBridge()
{
    HMODULE core = GetModuleHandleW(L"dwmcore.dll");
    if (sizeof(void*) != 8 || !NativeMeshAbiVerified() ||
        !IsReadableMemory(core, sizeof(IMAGE_DOS_HEADER)))
        return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(core);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x1000)
        return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(core) + dos->e_lfanew);
    if (!IsReadableMemory(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    if (nt->FileHeader.TimeDateStamp != 0x610B3AB4 || nt->OptionalHeader.SizeOfImage != 0x41A000)
    {
        Wh_Log(L"MESH MATERIAL BRIDGE: unavailable coreTimestamp=0x%08X coreSize=0x%X",
               nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return;
    }
    WindhawkUtils::SYMBOL_HOOK dwmcoreDllHooks[] = {
        {{L"public: static long __cdecl CBackdropVisualImage::Create(class CComposition *,"
           L"class CVisual const *,class CVisualTreePath const &,bool,class CBackdropVisualImage * *)"},
         &g_coreOwnedImageCreateFunction, nullptr, true},
        {{L"public: long __cdecl CBackdropVisualImage::ValidateRootAndSourceRectangle(class CVisual *,"
           L"class TMilRect_<float,struct MilRectF,struct Mil3DRectF,struct MilPointAndSizeF,"
           L"struct RectUniqueness::NotNeeded> const &,bool,bool *)"},
         &g_coreOwnedImageValidateFunction, nullptr, true},
        {{L"protected: unsigned long __cdecl CResource::InternalRelease(void)"},
         &g_coreOwnedImageReleaseFunction, nullptr, true},
        {{L"public: __cdecl wil::com_ptr_t<class IRenderTargetBitmap,struct wil::err_returncode_policy>::"
           L"~com_ptr_t<class IRenderTargetBitmap,struct wil::err_returncode_policy>(void)"},
         &g_coreOwnedBitmapDestroyFunction, nullptr, true},
        {{L"public: __cdecl EffectInput::~EffectInput(void)"},
         &g_coreOwnedInputDestroyFunction, nullptr, true},
        {{L"public: long __cdecl CRenderingTechnique::ExecuteBlur(class CDrawingContext *,"
           L"struct EffectInput const &,struct D2D_VECTOR_2F const &,struct EffectInput *)"},
         &g_coreExecuteBlurFunction, nullptr, true},
        {{L"public: long __cdecl CRenderingTechnique::CreateSurfaceDrawListBrushForIntermediate("
           L"unsigned int,struct EffectInput const &,class CSurfaceDrawListBrush * *)"},
         &g_coreIntermediateBrushFunction, nullptr, true},
        {{L"public: class CTreeData * __cdecl CVisual::FindTreeData(class CVisualTree const *)const",
          L"public: class CTreeData * __cdecl CVisual::FindTreeData(class CVisualTree const *)const "},
         &g_coreFindTreeDataFunction, nullptr, true},
        {{L"public: virtual class CVisual * __cdecl CDrawingContext::GetCurrentVisual(void)const",
          L"public: virtual class CVisual * __cdecl CDrawingContext::GetCurrentVisual(void)const "},
         &g_coreCurrentVisualFunction, nullptr, true},
        {{L"const CBackdropVisualImage::`vftable'{for `CImageSource'}"},
         &g_coreBackdropImageVtable, nullptr, true},
        {{L"const CDesktopTree::`vftable'"}, &g_coreDesktopTreeVtable, nullptr, true},
        {{L"const CVisualTree::`vftable'"}, &g_coreVisualTreeVtable, nullptr, true},
        {{L"const CSubVisualTree::`vftable'"}, &g_coreSubVisualTreeVtable, nullptr, true},
        {{L"const CDesktopTreeData::`vftable'"}, &g_coreDesktopTreeDataVtable, nullptr, true},
        {{L"public: struct EffectInput __cdecl CBackdropVisualImage::GenerateEffectInput(void)"},
         &g_coreBackdropEffectInputFunction, nullptr, true},
        {{L"public: virtual class RenderTargetInfo const & __cdecl CDrawingContext::GetCurrentRenderTargetInfo(void)const",
          L"public: virtual class RenderTargetInfo const & __cdecl CDrawingContext::GetCurrentRenderTargetInfo(void)const "},
         &g_coreCurrentTargetInfoFunction, nullptr, true},
        {{L"public: bool __cdecl CBackdropVisualImage::HasValidRenderingRealization(class RenderTargetInfo const &)const",
          L"public: bool __cdecl CBackdropVisualImage::HasValidRenderingRealization(class RenderTargetInfo const &)const "},
         &g_coreBackdropRealizationFunction, nullptr, true},
        {{L"public: long __cdecl CDrawingContext::ValidateBVIEffectInputForRender("
           L"class TMilRect_<float,struct MilRectF,struct Mil3DRectF,struct MilPointAndSizeF,"
           L"struct RectUniqueness::NotNeeded> const &,bool *)const",
          L"public: long __cdecl CDrawingContext::ValidateBVIEffectInputForRender("
           L"class TMilRect_<float,struct MilRectF,struct Mil3DRectF,struct MilPointAndSizeF,"
           L"struct RectUniqueness::NotNeeded> const &,bool *)const "},
         &g_coreBviValidationFunction, nullptr, true},
        {{L"public: long __cdecl CDrawingContext::DrawVisualTree("
           L"class CVisualTree const *,class TMil3DRect<float,class TMilRect_<float,"
           L"struct MilRectF,struct Mil3DRectF,struct MilPointAndSizeF,struct RectUniqueness::NotNeeded>,"
           L"struct Mil3DRectF,struct RectUniqueness::NotNeeded> const &,class COcclusionContext const *,"
           L"enum ClearMode::Enum,bool,class CVisual *)"},
         &g_coreDrawVisualTreeFunction, nullptr, true},
        {{L"const CCachedVisualImage::`vftable'{for `CImageSource'}"},
         &g_coreCachedImageVtable, nullptr, true},
        {{L"protected: long __cdecl CCachedVisualImage::ProcessUpdate("
           L"class CResourceTable *,struct tagMILCMD_CACHEDVISUALIMAGE const *)"},
         &g_coreImageUpdateFunction, nullptr, true},
        {{L"protected: long __cdecl CCachedVisualImage::EnsureRenderTargetBitmap("
           L"class RenderTargetInfo const &,class IRenderTargetBitmap * *)"},
         &g_coreImageRealizeFunction, nullptr, true},
    };
    if (!WindhawkUtils::HookSymbols(core, dwmcoreDllHooks, ARRAYSIZE(dwmcoreDllHooks)))
        return;
    const auto base = reinterpret_cast<uintptr_t>(core);
    // PDB resolution plus exact-image RVAs prevent a same-name ABI mismatch.
    if (reinterpret_cast<uintptr_t>(g_coreDrawVisualTreeFunction) != base + 0x87B2C ||
        reinterpret_cast<uintptr_t>(g_coreBviValidationFunction) != base + 0x8850C ||
        reinterpret_cast<uintptr_t>(g_coreCachedImageVtable) != base + 0x2C4350 ||
        reinterpret_cast<uintptr_t>(g_coreImageUpdateFunction) != base + 0xA923C ||
        reinterpret_cast<uintptr_t>(g_coreImageRealizeFunction) != base + 0x3A8B0)
    {
        Wh_Log(L"MESH MATERIAL BRIDGE: unavailable symbol/layout mismatch");
        return;
    }
    g_coreMaterialModuleBase = base;
    g_coreBackdropDetailReady =
        reinterpret_cast<uintptr_t>(g_coreFindTreeDataFunction) == base + 0x88B84 &&
        reinterpret_cast<uintptr_t>(g_coreCurrentVisualFunction) == base + 0xD4950 &&
        reinterpret_cast<uintptr_t>(g_coreBackdropImageVtable) == base + 0x2C30D8 &&
        IsReadableMemory(g_coreBackdropImageVtable, sizeof(void*));
    g_coreBackdropSceneReady = g_coreBackdropDetailReady &&
        reinterpret_cast<uintptr_t>(g_coreDesktopTreeVtable) == base + 0x2C9668 &&
        reinterpret_cast<uintptr_t>(g_coreVisualTreeVtable) == base + 0x2C1C28 &&
        reinterpret_cast<uintptr_t>(g_coreSubVisualTreeVtable) == base + 0x2CCA28;
    g_coreBackdropDesktopReady = g_coreBackdropDetailReady &&
        reinterpret_cast<uintptr_t>(g_coreDesktopTreeVtable) == base + 0x2C9668 &&
        reinterpret_cast<uintptr_t>(g_coreDesktopTreeDataVtable) == base + 0x2C2CD8;
    // Leaf getters can carry private volatile-register state; never detour them.
    const bool ready = g_coreBackdropDetailReady && g_coreBackdropSceneReady && g_coreBackdropDesktopReady &&
        reinterpret_cast<uintptr_t>(g_coreBackdropEffectInputFunction) == base + 0x3122C &&
        reinterpret_cast<uintptr_t>(g_coreCurrentTargetInfoFunction) == base + 0x105760 &&
        reinterpret_cast<uintptr_t>(g_coreBackdropRealizationFunction) == base + 0x4B1E0 &&
        WindhawkUtils::SetFunctionHook(g_coreBviValidationFunction,
            CoreBviValidationHook, &g_coreBviValidationOriginal) &&
        WindhawkUtils::SetFunctionHook(g_coreDrawVisualTreeFunction,
            CoreDrawVisualTreeHook, &g_coreDrawVisualTreeOriginal) &&
        WindhawkUtils::SetFunctionHook(g_coreImageUpdateFunction,
            CoreImageUpdateHook, &g_coreImageUpdateOriginal) &&
        WindhawkUtils::SetFunctionHook(g_coreImageRealizeFunction,
            CoreImageRealizeHook, &g_coreImageRealizeOriginal) &&
        WindhawkUtils::SetFunctionHook(g_coreBackdropEffectInputFunction,
            CoreBackdropEffectInputHook, &g_coreBackdropEffectInputOriginal);
    g_coreMaterialBridgeReady.store(ready, std::memory_order_release);
    g_coreBackdropBridgeReady.store(ready, std::memory_order_release);
    // Two non-leaf observers on this exact image. They always forward every
    // argument and native result, including failures and aliased input/output.
    const BYTE blurPrefix[] = {0x48,0x8B,0xC4,0x55,0x53,0x56,0x57,0x41,0x54,
                              0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x68,0x88};
    const BYTE intermediatePrefix[] = {0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,
                                      0x48,0x89,0x70,0x20,0x55,0x57,0x41,0x56};
    const bool diagnosticAbi = ready &&
        reinterpret_cast<uintptr_t>(g_coreExecuteBlurFunction) == base + 0x37F24 &&
        reinterpret_cast<uintptr_t>(g_coreIntermediateBrushFunction) == base + 0x30190 &&
        IsReadableMemory(reinterpret_cast<void*>(g_coreExecuteBlurFunction), sizeof(blurPrefix)) &&
        IsReadableMemory(reinterpret_cast<void*>(g_coreIntermediateBrushFunction), sizeof(intermediatePrefix)) &&
        !std::memcmp(reinterpret_cast<void*>(g_coreExecuteBlurFunction), blurPrefix, sizeof(blurPrefix)) &&
        !std::memcmp(reinterpret_cast<void*>(g_coreIntermediateBrushFunction), intermediatePrefix, sizeof(intermediatePrefix));
    const bool observeBlur = diagnosticAbi && WindhawkUtils::SetFunctionHook(
        g_coreExecuteBlurFunction, CoreExecuteBlurHook, &g_coreExecuteBlurOriginal);
    const bool observeIntermediate = diagnosticAbi && WindhawkUtils::SetFunctionHook(
        g_coreIntermediateBrushFunction, CoreIntermediateBrushHook, &g_coreIntermediateBrushOriginal);
    g_coreOwnedBackdropProbeReady.store(ready &&
        reinterpret_cast<uintptr_t>(g_coreOwnedImageCreateFunction) == base + 0xA61F4 &&
        reinterpret_cast<uintptr_t>(g_coreOwnedImageValidateFunction) == base + 0x4CA84 &&
        reinterpret_cast<uintptr_t>(g_coreOwnedImageReleaseFunction) == base + 0x78A28 &&
        reinterpret_cast<uintptr_t>(g_coreOwnedBitmapDestroyFunction) == base + 0x343B8 &&
        reinterpret_cast<uintptr_t>(g_coreOwnedInputDestroyFunction) == base + 0x94170,
        std::memory_order_release);
    Wh_Log(L"MESH OWN BACKDROP PROBE: available=%d attached=0",
           g_coreOwnedBackdropProbeReady.load(std::memory_order_acquire));
    Wh_Log(L"MESH MATERIAL PIPELINE OBSERVER: blur=%d intermediate=%d readOnly=1",
           observeBlur, observeIntermediate);
    Wh_Log(L"MESH MATERIAL BRIDGE: %s hooks=5 leafHooks=0", ready ? L"ready" : L"unavailable; using Affine");
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

static size_t NativeVisualVtableRva(void* visual)
{
    void* vtable = ReadPointerMember(visual, 0);
    return IsDwmImageAddress(vtable, sizeof(void*))
        ? reinterpret_cast<uintptr_t>(vtable) -
              reinterpret_cast<uintptr_t>(g_dwmModuleLayout.imageBegin)
        : 0;
}

static bool ReadNativeVisualChildren(void* visual, void**& entries,
                                     unsigned int& count)
{
    entries = nullptr;
    count = 0;
    if (!NativeVisualVtableRva(visual))
        return false;
    auto* collection = static_cast<BYTE*>(visual) + 0x20;
    if (!IsReadableMemory(collection, 0x2C) ||
        ReadPointerMember(collection, 0) != g_nativeMeshCollectionVtable ||
        ReadPointerMember(collection, 8) != visual)
        return false;
    std::memcpy(&count, collection + 0x28, sizeof(count));
    if (count > 256)
        return false;
    if (count)
    {
        entries = static_cast<void**>(ReadPointerMember(collection, 0x10));
        if (!IsReadableMemory(entries, count * sizeof(void*)))
            return false;
    }
    return true;
}

static bool IsNativeBackdropSourceRoot(void* topLevelWindow, void* border,
                                       void* sourceRoot)
{
    if (!g_nativeBackdropVisualVtable || !border || !sourceRoot ||
        ReadPointerMember(sourceRoot, 0) != g_nativeBackdropVisualVtable ||
        ReadPointerMember(border, 0x18) != sourceRoot ||
        ReadPointerMember(sourceRoot, 0x18) != topLevelWindow)
        return false;
    void** children = nullptr;
    unsigned int count = 0;
    return ReadNativeVisualChildren(sourceRoot, children, count) &&
           count == 1 && children[0] == border;
}

static bool IsNativeMeshSourceCurrent(void* topLevelWindow, void* sourceRoot,
                                      void* sourceProxy, bool backdropSource)
{
    void* border = g_topLevelWindowGetRootVisual(topLevelWindow, 0);
    if (backdropSource ? !IsNativeBackdropSourceRoot(topLevelWindow, border, sourceRoot)
                       : border != sourceRoot)
        return false;
    return sourceRoot && NativeVisualVtableRva(sourceRoot) &&
           g_nativeMeshGetVisualProxy(sourceRoot) == sourceProxy;
}

// Native capture source begin: exact 23H2 layout, independently tested below.
// A sub-pixel original contribution keeps native backdrop realization active.
static constexpr double kNativeOriginalOpacity = 1.0 / 65536.0;
class NativeCaptureGuard
{
public:
    explicit NativeCaptureGuard(bool needed)
        : section_(needed && g_nativeCapturedSourceReady
                       ? static_cast<CRITICAL_SECTION*>(g_nativeCaptureCriticalSection) : nullptr),
          acquired_(!needed || (section_ && TryEnterCriticalSection(section_))) {}
    ~NativeCaptureGuard() { if (section_ && acquired_) LeaveCriticalSection(section_); }
    NativeCaptureGuard(const NativeCaptureGuard&) = delete;
    NativeCaptureGuard& operator=(const NativeCaptureGuard&) = delete;
    bool acquired() const { return acquired_; }
private:
    CRITICAL_SECTION* section_;
    bool acquired_;
};

// Callers hold the production capture critical section while using this
// borrowed root/proxy. No lock is acquired by a guessed internal ABI call.
static void* GetNativeCapturedSource(void* capturedRepresentation, void** visual = nullptr)
{
    if (visual) *visual = nullptr;
    if (!g_nativeCapturedSourceReady ||
        ReadPointerMember(capturedRepresentation, 0) != g_nativeCapturedRepresentationVtable ||
        !ReadPointerMember(capturedRepresentation, 0x20))
        return nullptr;
    void* root = ReadPointerMember(capturedRepresentation, 0x30);
    if (!NativeVisualVtableRva(root)) return nullptr;
    if (visual) *visual = root;
    void* proxy = ReadPointerMember(root, 0x10);
    if (!IsVisualProxyPointerValid(proxy)) return nullptr;
    return proxy;
}

static void ReleaseNativeCapturedSource(void* representation)
{
    void* listener = ReadPointerMember(representation, 0xA8);
    const bool ownedListener = listener &&
        ReadPointerMember(listener, 0) == g_nativeCaptureListenerVtable;
    const unsigned long references = g_cBaseObjectRelease(representation);
    // Native callbacks must remain valid even if another native owner retains
    // this captured source. The tiny heap record never points into mod code.
    if (ownedListener && !references) HeapFree(GetProcessHeap(), 0, listener);
}

static long CreateNativeCapturedSource(void* windowData, void** representation,
                                      const wchar_t*& stage)
{
    *representation = nullptr;
    stage = L"Compatibility";
    if (!g_nativeCapturedSourceReady || !windowData) return E_NOTIMPL;
    // Same usage/scale as CapturedWindowSWRVisual. Its normal-window root is
    // initially null; explicitly build and validate the owned native subtree.
    auto* listener = static_cast<void**>(HeapAlloc(GetProcessHeap(), 0, sizeof(void*)));
    if (!listener) return E_OUTOFMEMORY;
    *listener = g_nativeCaptureListenerVtable;
    stage = L"CreateRepresentation";
    long result = g_nativeCapturedRepresentationCreate(
        0xA0, listener, windowData, 0, 100, representation);
    if (result < 0 || !*representation)
    {
        HeapFree(GetProcessHeap(), 0, listener);
        return result < 0 ? result : E_UNEXPECTED;
    }
    if (ReadPointerMember(*representation, 0) != g_nativeCapturedRepresentationVtable ||
        ReadPointerMember(*representation, 0x20) != windowData)
        return E_UNEXPECTED;
    stage = L"EnsureOwnedRoot";
    result = g_nativeCapturedRepresentationEnsureRoot(*representation);
    if (result < 0) return result;
    stage = L"ValidateRepresentation";
    result = g_nativeCapturedRepresentationValidate(*representation);
    if (result < 0) return result;
    stage = L"ResolveSource";
    return GetNativeCapturedSource(*representation) ? S_OK : E_NOINTERFACE;
}

static bool IsNativeCapturedSourceSeparate(void* capturedRepresentation, void* original)
{
    void* root = nullptr;
    if (!GetNativeCapturedSource(capturedRepresentation, &root)) return false;
    auto separatePath = [](void* node, void* forbidden)
    {
        void* visited[16] = {};
        for (unsigned i = 0; node && i < ARRAYSIZE(visited); ++i)
        {
            if (node == forbidden || !NativeVisualVtableRva(node) ||
                std::find(visited, visited + i, node) != visited + i)
                return false;
            visited[i] = node;
            node = ReadPointerMember(node, 0x18);
        }
        return !node;
    };
    return separatePath(root, original) && separatePath(original, root);
}

static bool ReadNativeOpacity(void* visual, double& opacity)
{
    if (!visual || !NativeVisualVtableRva(visual)) return false;
    const auto* field = static_cast<const BYTE*>(visual) + 0xC0;
    if (!IsReadableMemory(field, sizeof(opacity))) return false;
    std::memcpy(&opacity, field, sizeof(opacity));
    return std::isfinite(opacity) && opacity >= 0.0 && opacity <= 1.0;
}

static void RestoreNativeOriginal(NativeMeshState& state)
{
    g_nativeMeshSourcePending.store(false, std::memory_order_release);
    if (!state.originalHidden) return;
    double current = 0.0;
    const bool owned = ReadNativeOpacity(state.root, current) && current == kNativeOriginalOpacity;
    if (owned && g_nativeVisualSetOpacity) g_nativeVisualSetOpacity(state.root, state.originalOpacity);
    state.originalHidden = false;
    Wh_Log(L"MESH ORIGINAL RESTORED: HWND=%p propertyOwned=%d", state.hwnd, owned);
}

static bool MaintainNativeOriginalHide(NativeMeshState& state)
{
    const auto key = state.materialKey;
    if (!key || key != g_coreMaterialTargetKey.load(std::memory_order_acquire)) return false;
    double current = 0.0;
    if (state.originalHidden)
        return g_coreMaterialReadyKey.load(std::memory_order_acquire) == key &&
               ReadNativeOpacity(state.root, current) && current == kNativeOriginalOpacity;
    if (g_coreMaterialReadyKey.load(std::memory_order_acquire) != key)
        return GetTickCount64() - state.attachedAt < 1500;
    if (!state.capturedRepresentation || !g_nativeVisualSetOpacity ||
        !IsNativeCapturedSourceSeparate(state.capturedRepresentation, state.root) ||
        !ReadNativeOpacity(state.root, state.originalOpacity) || state.originalOpacity != 1.0)
        return false;
    state.originalHidden = true;
    g_nativeVisualSetOpacity(state.root, kNativeOriginalOpacity);
    g_nativeMeshSourcePending.store(false, std::memory_order_release);
    Wh_Log(L"MESH ORIGINAL SUPPRESSED: HWND=%p opacity=%.8f source=OwnedRepresentation",
           state.hwnd, kNativeOriginalOpacity);
    return true;
}// Native capture source end.

static bool ResetNativeMeshSourceMotion(NativeMeshState& state)
{
    if (!state.sourceTransform || !state.sourceMotionApplied) return true;
    const MilMatrix3x2D identity = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    if (UpdateMatrixTransformProxy(state.sourceTransform, identity) < 0) return false;
    state.sourceMotionApplied = false;
    state.sourceMotionX = state.sourceMotionY = 0.0;
    return true;
}

static void ReleaseNativeMeshState(NativeMeshState& state)
{
    RestoreNativeOriginal(state);
    if (!ResetNativeMeshSourceMotion(state)) return;
    ClearCoreMaterialTarget();
    if (state.visual) g_cBaseObjectRelease(state.visual);
    if (state.transform) g_cBaseObjectRelease(state.transform);
    if (state.sourceTransform) g_cBaseObjectRelease(state.sourceTransform);
    if (state.instruction) g_cBaseObjectRelease(state.instruction);
    if (state.group) g_cBaseObjectRelease(state.group);
    if (state.mesh) g_cBaseObjectRelease(state.mesh);
    if (state.image) g_cBaseObjectRelease(state.image);
    if (state.capturedRepresentation) ReleaseNativeCapturedSource(state.capturedRepresentation);
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
    g_nativeMeshStopRequested.store(true, std::memory_order_release);
    RestoreNativeOriginal(state);
    if (!ResetNativeMeshSourceMotion(state)) { RequestDwmScenePass(); return false; }
    NativeCaptureGuard captureGuard(state.capturedRepresentation != nullptr);
    if (!captureGuard.acquired()) { RequestDwmScenePass(); return false; }
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
            Wh_Log(L"NATIVE MESH DETACH DEFERRED: HWND=%p reason=%s "
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

static void CalculateBernsteinBasis(double value, double basis[4]);

// Native mesh math begin: shared with the standalone geometry checks.
static constexpr int NATIVE_MESH_MAX_PADDING = 256;

static int NativeMeshShadowPaddingForDpi(unsigned dpi)
{
    // Reserve 96 DIPs around the window, rounded outward to 16 pixels.
    dpi = std::clamp(dpi ? dpi : 96u, 96u, 384u);
    return std::min(NATIVE_MESH_MAX_PADDING, static_cast<int>((dpi + 15u) & ~15u));
}

static constexpr unsigned int NATIVE_MESH_CELLS = 20;
static constexpr unsigned int NATIVE_MESH_RENDER_CELLS = NATIVE_MESH_CELLS + 2;
static constexpr unsigned int NATIVE_MESH_SIDE = NATIVE_MESH_RENDER_CELLS + 1;
static constexpr unsigned int NATIVE_MESH_VERTEX_COUNT =
    NATIVE_MESH_SIDE * NATIVE_MESH_SIDE;
static constexpr unsigned int NATIVE_MESH_INDEX_COUNT =
    NATIVE_MESH_RENDER_CELLS * NATIVE_MESH_RENDER_CELLS * 6;

static double NativeMeshWindowCoordinate(unsigned int index, int length, int padding)
{
    if (index == 0) return -static_cast<double>(padding);
    if (index == NATIVE_MESH_SIDE - 1) return static_cast<double>(length + padding);
    return static_cast<double>(length) * (index - 1) / NATIVE_MESH_CELLS;
}

static void CalculateNativeMeshBasis(double value, double basis[4])
{
    // Continue the edge tangent into the shadow margin instead of extending
    // the cubic polynomial, which can amplify extreme corner deformation.
    if (value < 0.0)
    {
        basis[0] = 1.0 - 3.0 * value; basis[1] = 3.0 * value;
        basis[2] = basis[3] = 0.0;
    }
    else if (value > 1.0)
    {
        basis[0] = basis[1] = 0.0;
        basis[2] = -3.0 * (value - 1.0); basis[3] = 1.0 + 3.0 * (value - 1.0);
    }
    else
    {
        double remaining = 1.0 - value;
        basis[0] = remaining * remaining * remaining;
        basis[1] = 3.0 * value * remaining * remaining;
        basis[2] = 3.0 * value * value * remaining;
        basis[3] = value * value * value;
    }
}

static bool BuildNativeMeshVertices(const Vec2 (&control)[16],
    int width, int height, int padding,
    NativeMeshPoint3F (&positions)[NATIVE_MESH_VERTEX_COUNT],
    NativeMeshPoint2D (&coordinates)[NATIVE_MESH_VERTEX_COUNT])
{
    if (width < 1 || height < 1 || padding < 0 || padding > NATIVE_MESH_MAX_PADDING ||
        width > 8192 - 2 * padding || height > 8192 - 2 * padding)
        return false;
    for (const Vec2& point : control)
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
    for (unsigned int y = 0; y < NATIVE_MESH_SIDE; ++y)
    {
        double sourceY = NativeMeshWindowCoordinate(y, height, padding);
        double basisY[4];
        CalculateNativeMeshBasis(sourceY / height, basisY);
        for (unsigned int x = 0; x < NATIVE_MESH_SIDE; ++x)
        {
            double sourceX = NativeMeshWindowCoordinate(x, width, padding);
            double basisX[4];
            CalculateNativeMeshBasis(sourceX / width, basisX);
            Vec2 position = {};
            for (unsigned int cy = 0; cy < 4; ++cy)
                for (unsigned int cx = 0; cx < 4; ++cx)
                {
                    double weight = basisY[cy] * basisX[cx];
                    const Vec2& point = control[cy * 4 + cx];
                    position.x += weight * point.x;
                    position.y += weight * point.y;
                }
            if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                std::abs(position.x) > FLT_MAX || std::abs(position.y) > FLT_MAX)
                return false;
            unsigned int index = y * NATIVE_MESH_SIDE + x;
            positions[index] = {static_cast<float>(position.x),
                                static_cast<float>(position.y), 0.0f};
            // The realized image starts at (0,0); the window starts at (padding,padding).
            coordinates[index] = {sourceX + padding, sourceY + padding};
        }
    }
    return true;
}

static bool NativeMeshSourceMotion(const NativeMeshPoint3F (&positions)[NATIVE_MESH_VERTEX_COUNT],
                                   int width, int height, double& x, double& y)
{
    const auto& center = positions[(NATIVE_MESH_SIDE / 2) * NATIVE_MESH_SIDE + NATIVE_MESH_SIDE / 2];
    x = static_cast<double>(center.x) - width * 0.5;
    y = static_cast<double>(center.y) - height * 0.5;
    return std::isfinite(x) && std::isfinite(y);
}

static void BuildNativeMeshIndices(unsigned int (&indices)[NATIVE_MESH_INDEX_COUNT])
{
    unsigned int count = 0;
    for (unsigned int y = 0; y < NATIVE_MESH_RENDER_CELLS; ++y)
    {
        for (unsigned int x = 0; x < NATIVE_MESH_RENDER_CELLS; ++x)
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
// Native mesh math end.
static_assert(GRID_WIDTH == 4 && GRID_HEIGHT == 4);

static void UpdateNativeVertexMesh(NativeMeshState& state)
{
    if (!state.mesh || !g_nativeMeshUpdateMesh)
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

    NativeMeshPoint3F positions[NATIVE_MESH_VERTEX_COUNT] = {};
    NativeMeshPoint2D coordinates[NATIVE_MESH_VERTEX_COUNT] = {};
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
        // Preserve the actual physics deformation. These generous bounds only
        // guard the private DWM mesh API against corrupted coordinates.
        const double limitX = std::max(4096.0, physics.width * 4.0);
        const double limitY = std::max(4096.0, physics.height * 4.0);
        double dx = std::clamp(point.position.x - point.basePosition.x,
                               -limitX, limitX);
        double dy = std::clamp(point.position.y - point.basePosition.y,
                               -limitY, limitY);
        maximumDisplacement = std::max(maximumDisplacement,
                                        std::hypot(dx, dy));
        control[i] = {(point.basePosition.x + dx) * scaleX,
                      (point.basePosition.y + dy) * scaleY};
    }
    if (!BuildNativeMeshVertices(control, state.width, state.height,
                                  state.sourcePadding, positions, coordinates))
    {
        DetachNativeMesh(L"mesh-vertices-invalid");
        return;
    }
    BuildNativeMeshIndices(indices);
    if (state.sourceTransform)
    {
        double x = 0.0, y = 0.0;
        if (!NativeMeshSourceMotion(positions, state.width, state.height, x, y))
        {
            DetachNativeMesh(L"source-motion-invalid");
            return;
        }
        // This parent is outside the captured border subtree and the mesh's
        // sibling. Follow its physical center without translating either twice.
        if (x != state.sourceMotionX || y != state.sourceMotionY)
        {
            const MilMatrix3x2D matrix = {1.0, 0.0, 0.0, 1.0, x, y};
            state.sourceMotionApplied = true; // Reset even after a partial failed update.
            long motionResult = UpdateMatrixTransformProxy(state.sourceTransform, matrix);
            if (motionResult < 0)
            {
                DetachNativeMesh(L"source-motion-update-failed");
                return;
            }
            state.sourceMotionX = x;
            state.sourceMotionY = y;
            state.sourceMotionApplied = x != 0.0 || y != 0.0;
        }
    }
    long result = g_nativeMeshUpdateMesh(state.mesh, 0, positions,
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
               L"maxDisplacement=%.1f sourceOffset=(%.1f,%.1f)", state.hwnd, state.meshUpdateCount,
               revision, maximumDisplacement, state.sourceMotionX, state.sourceMotionY);
    }
}

static bool GetNativeMeshWindowSize(HWND hwnd, int& width, int& height,
                                    bool logDimensions = false, int* shadowPadding = nullptr)
{
    ScopedDpiContext coordinates(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!coordinates.valid()) return false;
    RECT windowBounds = {};
    if (!ReadWindowRectForRenderer(hwnd, &windowBounds))
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
    if (shadowPadding)
        *shadowPadding = NativeMeshShadowPaddingForDpi(
            monitorDpiX && monitorDpiY ? std::max(monitorDpiX, monitorDpiY) : windowDpi);
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


static long UpdateNativeMeshSourceImage(void* image, void* sourceProxy,
                                         int width, int height, int padding)
{
    if (!image || !IsVisualProxyPointerValid(sourceProxy))
        return E_NOINTERFACE;
    if (padding < 0 || padding > NATIVE_MESH_MAX_PADDING || width < 1 || height < 1 ||
        width > 8192 - 2 * padding || height > 8192 - 2 * padding)
        return E_INVALIDARG;
    NativeMeshRectF rect = {-static_cast<float>(padding), -static_cast<float>(padding),
                             static_cast<float>(width + padding),
                             static_cast<float>(height + padding)};
    constexpr int absoluteMappingMode = 0;
    if (g_nativeMeshUpdateImageD)
    {
        NativeMeshSizeD size = {static_cast<double>(width + 2 * padding),
                                 static_cast<double>(height + 2 * padding)};
        return g_nativeMeshUpdateImageD(image, rect, size, nullptr,
                                         nullptr, sourceProxy,
                                         absoluteMappingMode);
    }
    if (g_nativeMeshUpdateImageF)
    {
        NativeMeshSizeF size = {static_cast<float>(width + 2 * padding),
                                 static_cast<float>(height + 2 * padding)};
        return g_nativeMeshUpdateImageF(image, rect, size, nullptr,
                                         nullptr, sourceProxy,
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
    NativeCaptureGuard captureGuard(state.capturedRepresentation != nullptr);
    if (!captureGuard.acquired()) return;
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
        g_nativeMeshGetVisualProxy(state.root) == state.sourceProxy;
    if (sourceValid && state.capturedRepresentation)
    {
        // The native WindowData listener clears its source on destruction.
        void* windowData = ReadPointerMember(state.capturedRepresentation, 0x20);
        sourceValid = windowData && GetHwndFromWindowData(windowData) == state.hwnd;
        unsigned dirty = 0;
        const auto* flags = static_cast<const BYTE*>(state.capturedRepresentation) + 0x18;
        if (sourceValid && IsReadableMemory(flags, sizeof(dirty)))
        {
            std::memcpy(&dirty, flags, sizeof(dirty));
            if (dirty & 0x1F)
                sourceValid = g_nativeCapturedRepresentationValidate(state.capturedRepresentation) >= 0;
        }
        else sourceValid = false;
        sourceValid = sourceValid &&
            GetNativeCapturedSource(state.capturedRepresentation) == state.imageSourceProxy;
    }
    if (sourceValid && state.backdropSource)
    {
        sourceValid = ReadPointerMember(state.root, 0x18) == state.sourceOwner &&
                      ReadPointerMember(state.visual, 0x18) == state.sourceOwner &&
                      FindNativeMeshSiblingCollection(state.root);
    }
    if (!sourceValid)
        DetachNativeMesh(L"source-changed");
    else
    {
        if (!MaintainNativeOriginalHide(state))
        {
            DetachNativeMesh(L"source-not-ready-or-native-opacity");
            return;
        }
        // A temporarily unavailable HWND rectangle is not a replaced visual.
        // Keep the validated source and retry its bounds on the next scene pass.
        int padding = state.sourcePadding;
        if (!GetNativeMeshWindowSize(state.hwnd, width, height, false, &padding))
        {
            UpdateNativeVertexMesh(state);
            return;
        }
        if ((width != state.width || height != state.height) &&
            std::abs(width - state.width) <= 1 && std::abs(height - state.height) <= 1)
        {
            bool moving = false;
            AcquireSRWLockShared(&g_animationSlotsLock);
            for (const auto& slot : g_animationSlots)
                if (slot.active && slot.hwnd == state.hwnd &&
                    slot.generation == state.meshGeneration)
                {
                    moving = slot.dragging && !slot.mesh.resizing;
                    break;
                }
            ReleaseSRWLockShared(&g_animationSlotsLock);
            if (moving)
            {
                width = state.width;
                height = state.height;
            }
        }
        if (width != state.width || height != state.height || padding != state.sourcePadding)
        {
            long result = UpdateNativeMeshSourceImage(state.image, state.imageSourceProxy,
                                                       width, height, padding);
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
            state.sourcePadding = padding;
            state.submittedMeshRevision = 0;
            Wh_Log(L"NATIVE MESH SOURCE RESIZED: HWND=%p size=%dx%d padding=%d",
                   state.hwnd, width, height, padding);
        }
        UpdateNativeVertexMesh(state);
    }
}

static bool HasAttachableNativeMeshSlot(HWND hwnd)
{
    bool attachable = false;
    AcquireSRWLockShared(&g_animationSlotsLock);
    for (const WindowAnimationSlot& slot : g_animationSlots)
    {
        if (slot.active && slot.hwnd == hwnd &&
            slot.settings.rendererMode == RendererMode::Mesh &&
            !(slot.windowStateThrob && !slot.dragging))
        {
            attachable = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_animationSlotsLock);
    return attachable;
}

static bool TryAttachNativeMesh(HWND hwnd, void* topLevelWindow, void* root,
                                     void* sourceProxy, bool backdropSource,
                                     void* compositor,
                                     void*& image, void*& group,
                                     void*& instruction, void*& mesh,
                                     int width, int height, int sourcePadding,
                                     void*& capturedRepresentation, void* imageSourceProxy)
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
    if (!IsNativeMeshSourceCurrent(topLevelWindow, root, sourceProxy, backdropSource))
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
    pending.imageSourceProxy = imageSourceProxy;
    pending.sourceOwner = ReadPointerMember(root, 0x18);
    pending.backdropSource = backdropSource;
    pending.materialKey = g_coreMaterialTargetKey.load(std::memory_order_acquire);
    pending.sourcePadding = sourcePadding;
    pending.width = width;
    pending.height = height;
    const wchar_t* stage = L"CreateVisual";
    long result = g_nativeMeshCreateVisual(&pending.visual);
    if (result >= 0 && !pending.visual) result = E_UNEXPECTED;
    if (result >= 0)
    {
        stage = L"AddInstruction";
        result = g_nativeMeshAddInstruction(pending.visual, instruction);
    }
    if (result >= 0)
    {
        stage = L"PublishRenderData";
        result = g_nativeMeshUpdateRenderData(pending.visual);
    }
    if (result >= 0)
    {
        stage = L"CreateTransform";
        result = g_createMatrixTransformProxy(compositor, &pending.transform);
        if (result >= 0 && !pending.transform) result = E_UNEXPECTED;
    }
    if (result >= 0)
    {
        stage = L"PositionShape";
        const MilMatrix3x2D matrix = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
        result = UpdateMatrixTransformProxy(pending.transform, matrix);
    }
    if (result >= 0)
    {
        stage = L"ValidateSibling";
        if (!IsWindow(hwnd) || !HasAttachableNativeMeshSlot(hwnd) ||
            FindNativeMeshSiblingCollection(root) != collection ||
            (capturedRepresentation && GetNativeCapturedSource(capturedRepresentation) != imageSourceProxy) ||
            !IsNativeMeshSourceCurrent(topLevelWindow, root, sourceProxy, backdropSource))
            result = E_UNEXPECTED;
    }
    if (result >= 0)
    {
        stage = L"InsertSibling";
        result = g_nativeMeshInsertRelative(collection, pending.visual,
                                            root, true, false);
    }
    bool inserted = pending.visual && ReadPointerMember(pending.visual, 0x18);
    if (result >= 0 && inserted)
    {
        stage = L"VerifySibling";
        if (ReadPointerMember(pending.visual, 0x18) !=
                ReadPointerMember(root, 0x18) ||
            (capturedRepresentation && GetNativeCapturedSource(capturedRepresentation) != imageSourceProxy) ||
            FindNativeMeshSiblingCollection(root) != collection)
            result = E_UNEXPECTED;
    }
    if (result >= 0 && inserted)
    {
        stage = L"BindTransform";
        void* visualProxy = g_nativeMeshGetVisualProxy(pending.visual);
        result = IsVisualProxyPointerValid(visualProxy)
                     ? g_cVisualProxySetTransform(visualProxy, pending.transform)
                     : E_NOINTERFACE;
    }
    if (result >= 0 && inserted && backdropSource)
    {
        stage = L"CreateSourceMotion";
        unsigned usage = 0, representationType = 0;
        if (IsReadableMemory(static_cast<const BYTE*>(capturedRepresentation) + 0x28, sizeof(usage)) &&
            IsReadableMemory(static_cast<const BYTE*>(capturedRepresentation) + 0x48, sizeof(representationType)))
        {
            std::memcpy(&usage, static_cast<const BYTE*>(capturedRepresentation) + 0x28, sizeof(usage));
            std::memcpy(&representationType, static_cast<const BYTE*>(capturedRepresentation) + 0x48, sizeof(representationType));
        }
        if (usage == 0xA0 && representationType != 3)
        {
            result = g_createMatrixTransformProxy(compositor, &pending.sourceTransform);
            if (result >= 0 && !pending.sourceTransform) result = E_UNEXPECTED;
            if (result >= 0)
                result = UpdateMatrixTransformProxy(pending.sourceTransform,
                                                    {1.0, 0.0, 0.0, 1.0, 0.0, 0.0});
            if (result >= 0)
                result = g_cVisualProxySetTransform(sourceProxy, pending.sourceTransform);
        }
    }
    if (result >= 0 && inserted)
    {
        pending.capturedRepresentation = capturedRepresentation;
        capturedRepresentation = nullptr;
        pending.attachedAt = GetTickCount64();
        pending.image = image;
        pending.group = group;
        pending.mesh = mesh;
        pending.instruction = instruction;
        image = group = mesh = instruction = nullptr;
        g_nativeMeshState = pending;
        g_nativeMeshHwnd.store(hwnd, std::memory_order_release);
        g_nativeMeshActive.store(true, std::memory_order_release);
        g_nativeMeshSourcePending.store(true, std::memory_order_release);
        PostThreadMessageW(g_eventThreadMessageTarget.load(std::memory_order_acquire), WM_NULL, 0, 0);
        InvalidateAnimationTransformForNativeMesh(hwnd);
        Wh_Log(L"MESH SOURCE MOTION: HWND=%p enabled=%d pivot=surface-center captureParentExcluded=%d",
               hwnd, pending.sourceTransform != nullptr, backdropSource);
        Wh_Log(L"NATIVE MESH ATTACHED: HWND=%p root=%p "
               L"visual=%p source=%s path=%s duration=until-slot-retirement snapshot=none",
               hwnd, root, pending.visual,
               backdropSource ? L"BackgroundTreatment" : L"WindowBorder",
               L"DrawMesh2D");
        RequestDwmScenePass();
        return true;
    }
    Wh_Log(L"NATIVE MESH ATTACH FAILED: HWND=%p stage=%s "
           L"result=0x%08X inserted=%d", hwnd, stage,
           static_cast<unsigned>(result), inserted);
    if (inserted)
    {
        pending.capturedRepresentation = capturedRepresentation;
        capturedRepresentation = nullptr;
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

static void CreateVisibleNativeMesh(HWND hwnd, void* topLevelWindow, void* sourceRoot,
                                      void* sourceProxy, bool backdropSource,
                                      void* compositor,
                                      void*& image, int width, int height,
                                      int sourcePadding,
                                      void*& capturedRepresentation, void* imageSourceProxy)
{
    // Exact 23H2 ABI only. The instruction stores base-object references and
    // serializes resource IDs; the core DrawMesh2D path consumes CImageSource.
    if (!IsOnDwmSceneThread() ||
        g_dwmModuleLayout.timeDateStamp != 0xE19C3234 ||
        g_dwmModuleLayout.sizeOfImage != 0x16A000 ||
        !g_nativeMeshCreateMesh || !g_nativeMeshUpdateMesh ||
        !g_nativeMeshCreateGroup || !g_nativeMeshUpdateGroup ||
        !g_nativeMeshCreateMeshInstruction || !g_cBaseObjectRelease ||
        !image || width < 160 || height < 100)
    {
        Wh_Log(L"NATIVE VERTEX MESH: skipped HWND=%p", hwnd);
        return;
    }

    NativeMeshPoint3F positions[NATIVE_MESH_VERTEX_COUNT] = {};
    NativeMeshPoint2D coordinates[NATIVE_MESH_VERTEX_COUNT] = {};
    unsigned int indices[NATIVE_MESH_INDEX_COUNT] = {};
    Vec2 control[GRID_POINT_COUNT] = {};
    for (unsigned int y = 0; y < GRID_HEIGHT; ++y)
    {
        for (unsigned int x = 0; x < GRID_WIDTH; ++x)
        {
            control[y * GRID_WIDTH + x] = {
                static_cast<double>(width) * x / (GRID_WIDTH - 1),
                static_cast<double>(height) * y / (GRID_HEIGHT - 1)};
        }
    }
    if (!BuildNativeMeshVertices(control, width, height, sourcePadding, positions, coordinates))
        return;
    BuildNativeMeshIndices(indices);

    void* mesh = nullptr;
    void* group = nullptr;
    void* instruction = nullptr;
    const wchar_t* stage = L"CreateMesh";
    long result = g_nativeMeshCreateMesh(compositor, &mesh);
    if (result >= 0 && !mesh) result = E_UNEXPECTED;
    if (result >= 0)
    {
        stage = L"UpdateMesh";
        result = g_nativeMeshUpdateMesh(mesh, 0, positions, coordinates,
                                         NATIVE_MESH_VERTEX_COUNT, indices,
                                         NATIVE_MESH_INDEX_COUNT);
    }
    if (result >= 0)
    {
        stage = L"CreateGroup";
        result = g_nativeMeshCreateGroup(compositor, &group);
        if (result >= 0 && !group) result = E_UNEXPECTED;
    }
    if (result >= 0)
    {
        stage = L"UpdateGroup";
        result = g_nativeMeshUpdateGroup(group, mesh);
    }
    if (result >= 0)
    {
        stage = L"CreateInstruction";
        result = g_nativeMeshCreateMeshInstruction(group, image, &instruction);
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
        if (TryAttachNativeMesh(hwnd, topLevelWindow, sourceRoot, sourceProxy,
                                     backdropSource,
                                     compositor, image, group, instruction,
                                     mesh, width, height, sourcePadding,
                                     capturedRepresentation, imageSourceProxy))
        {
            Wh_Log(L"NATIVE VERTEX MESH: attached HWND=%p "
                   L"physicsLinked=1 liveSource=1 original=independent-parent-motion", hwnd);
        }
    }
    if (instruction) g_cBaseObjectRelease(instruction);
    if (group) g_cBaseObjectRelease(group);
    if (mesh) g_cBaseObjectRelease(mesh);
}

static void CreateNativeMeshSource(HWND hwnd, void* windowData, void* topLevelWindow,
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
        !g_coreMaterialBridgeReady.load(std::memory_order_acquire) ||
        g_dwmModuleLayout.timeDateStamp != 0xE19C3234 ||
        g_dwmModuleLayout.sizeOfImage != 0x16A000)
        return;
    if (!HasAttachableNativeMeshSlot(hwnd))
        return;
    ULONGLONG now = GetTickCount64();
    if (g_nativeMeshLastAttemptHwnd == hwnd &&
        now - g_nativeMeshLastAttemptAt < 250)
        return;
    g_nativeMeshLastAttemptHwnd = hwnd;
    g_nativeMeshLastAttemptAt = now;
    void* compositor = g_dwmCompositor.load(std::memory_order_acquire);
    void* expectedVtable =
        g_nativeMeshImageVtable.load(std::memory_order_acquire);
    if (!IsDwmObjectPointerValid(compositor, g_compositorVtable) ||
        !expectedVtable || !g_cBaseObjectRelease || !g_nativeMeshCreateImage ||
        (g_nativeMeshUpdateImageD != nullptr) ==
            (g_nativeMeshUpdateImageF != nullptr))
    {
        return;
    }

    void* sourceRoot = g_topLevelWindowGetRootVisual(topLevelWindow, 0);
    if (!IsNativeMeshSourceCurrent(topLevelWindow, sourceRoot, sourceVisualProxy, false))
        return;
    void* treatment = ReadPointerMember(sourceRoot, 0x18);
    bool backdropSource = false;
    if (IsNativeBackdropSourceRoot(topLevelWindow, sourceRoot, treatment))
    {
        void* treatmentProxy = g_nativeMeshGetVisualProxy(treatment);
        if (IsVisualProxyPointerValid(treatmentProxy))
        {
            sourceRoot = treatment;
            sourceVisualProxy = treatmentProxy;
            backdropSource = true;
        }
    }
    Wh_Log(L"NATIVE MESH SOURCE CHOICE: HWND=%p source=%s visual=%p proxy=%p "
           L"host=%p", hwnd,
           backdropSource ? L"BackgroundTreatment" : L"WindowBorder",
           sourceRoot, sourceVisualProxy, ReadPointerMember(sourceRoot, 0x18));
    int width = 0;
    int height = 0;
    int sourcePadding = 0;
    if (!GetNativeMeshWindowSize(hwnd, width, height, true, &sourcePadding))
    {
        return;
    }
    NativeCaptureGuard captureGuard(true);
    if (!captureGuard.acquired()) return;
    void* capturedRepresentation = nullptr;
    void* imageSourceProxy = sourceVisualProxy;
    {
        if (!g_nativeCapturedSourceReady || !windowData || GetHwndFromWindowData(windowData) != hwnd ||
            !IsDwmObjectPointerValid(g_desktopManager.load(std::memory_order_acquire), g_desktopManagerVtable))
        {
            Wh_Log(L"NATIVE CAPTURE SOURCE FAILED: HWND=%p stage=Compatibility", hwnd);
            return;
        }
        const wchar_t* stage = nullptr;
        const long result = CreateNativeCapturedSource(windowData, &capturedRepresentation, stage);
        imageSourceProxy = result >= 0 ? GetNativeCapturedSource(capturedRepresentation) : nullptr;
        void* capturedRoot = nullptr;
        GetNativeCapturedSource(capturedRepresentation, &capturedRoot);
        Wh_Log(L"NATIVE CAPTURE SOURCE RESULT: HWND=%p stage=%s result=0x%08X representation=%p root=%p "
               L"proxy=%p hostRoot=%p separate=%d rootVtableRva=0x%zx proxyVtableRva=0x%zx", hwnd, stage, static_cast<unsigned>(result),
               capturedRepresentation, capturedRoot, imageSourceProxy, sourceRoot,
               capturedRepresentation && IsNativeCapturedSourceSeparate(capturedRepresentation, sourceRoot),
               NativeVisualVtableRva(capturedRoot),
               NativeVisualVtableRva(ReadPointerMember(capturedRoot, 0x10)));
        if (result < 0 || !imageSourceProxy ||
            !IsNativeCapturedSourceSeparate(capturedRepresentation, sourceRoot))
        {
            if (capturedRepresentation) ReleaseNativeCapturedSource(capturedRepresentation);
            return;
        }
    }
    Wh_Log(L"NATIVE SOURCE RECT: HWND=%p rect=(%d,%d,%d,%d) "
           L"realization=%dx%d padding=%d", hwnd,
           -sourcePadding, -sourcePadding,
           width + sourcePadding, height + sourcePadding,
           width + 2 * sourcePadding, height + 2 * sourcePadding, sourcePadding);
    void* image = nullptr;
    const long createResult = g_nativeMeshCreateImage(compositor, &image);
    const bool exactImageType = image && ReadPointerMember(image, 0) == expectedVtable;
    long result = createResult < 0 ? createResult : E_NOINTERFACE;
    if (createResult >= 0 && exactImageType)
    {
        SetCoreMaterialTarget(image, imageSourceProxy, hwnd);
        result = g_coreMaterialTargetKey.load(std::memory_order_acquire)
            ? UpdateNativeMeshSourceImage(image, imageSourceProxy, width, height, sourcePadding)
            : E_NOINTERFACE;
        if (result >= 0)
            CreateVisibleNativeMesh(hwnd, topLevelWindow, sourceRoot, sourceVisualProxy,
                backdropSource, compositor, image, width, height, sourcePadding,
                capturedRepresentation, imageSourceProxy);
    }
    if (result < 0)
        Wh_Log(L"NATIVE MESH SOURCE FAILED: HWND=%p result=0x%08X", hwnd, static_cast<unsigned>(result));
    if (image)
    {
        ClearCoreMaterialTarget();
        if (createResult >= 0 && exactImageType) g_cBaseObjectRelease(image);
    }
    if (capturedRepresentation) ReleaseNativeCapturedSource(capturedRepresentation);
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
            CreateNativeMeshSource(hwnd, windowData, topLevelWindow, topLevelVisualProxy);
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
        mesh.points[i].position.x = mesh.points[i].basePosition.x + displacement[i].x;
        mesh.points[i].position.y = mesh.points[i].basePosition.y + displacement[i].y;
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

static void ContinuePhysicalMeshDrag(WobbleMesh& mesh, double width, double height,
                                      const Vec2& localMouse, const Vec2& windowDelta,
                                      bool rebaseCoordinates)
{
    if (!rebaseCoordinates)
        OffsetMeshPositions(mesh, -windowDelta.x, -windowDelta.y);
    ResizeMeshPreservingDeformation(mesh, width, height);
    UpdateMeshDragOffset(mesh, localMouse);
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

static bool PrepareMeshForGrab(WobbleMesh& mesh, double width, double height,
                                const Vec2& position, bool forceReset, bool physicalMesh)
{
    bool reset = forceReset || mesh.width != width || mesh.height != height ||
                 (physicalMesh && !mesh.active);
    if (physicalMesh && !reset)
    {
        for (int y = 0; y < GRID_HEIGHT && !reset; ++y)
            for (int x = 0; x < GRID_WIDTH; ++x)
            {
                const Vec2& base = mesh.points[GetPointIndex(x, y)].basePosition;
                const double expectedX = width * x / (GRID_WIDTH - 1);
                const double expectedY = height * y / (GRID_HEIGHT - 1);
                if (!std::isfinite(base.x) || !std::isfinite(base.y) ||
                    std::abs(base.x - expectedX) > 0.001 ||
                    std::abs(base.y - expectedY) > 0.001)
                {
                    reset = true;
                    break;
                }
            }
    }
    if (reset) InitializeMesh(mesh, width, height);
    BeginDrag(mesh, position);
    return reset;
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
    g_dragUsesPhysicalCoordinates = false;
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
    bool hasRect = ReadWindowRectForRenderer(hwnd, &rect) != FALSE;
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
                                double height, const Vec2& mousePosition, bool* meshReset = nullptr)
{
    if (meshReset) *meshReset = true;
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
            const bool reset = PrepareMeshForGrab(slot.mesh, width, height, mousePosition,
                resetStateThrob || resetResizeDecay, settings.rendererMode == RendererMode::Mesh);
            if (meshReset) *meshReset = reset;
            if (reset)
            {
                slot.windowStateThrob = false;
                if (resetStateThrob)
                {
                    slot.boundTransitionVisualProxy = nullptr;
                    slot.transitionTransformAttached = false;
                }
            }
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
        const bool reset = PrepareMeshForGrab(slot.mesh, width, height, mousePosition,
            resetStateThrob || slot.mesh.resizing, settings.rendererMode == RendererMode::Mesh);
        if (meshReset) *meshReset = reset;
        if (reset)
        {
            slot.windowStateThrob = false;
        }
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
    WobblySettings activeSettings = GetSettingsSnapshot();
    const bool physicalCoordinates = activeSettings.rendererMode == RendererMode::Mesh;
    ScopedDpiContext coordinates(physicalCoordinates ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 : nullptr);
    if (!coordinates.valid()) return;
    // Reset Snap state so the same side can trigger again.
    ResetObservedSnapStateForInteractiveMove(hwnd);
    g_pendingInteractiveTransitionWindow.store(nullptr, std::memory_order_release);
    g_pendingInteractiveTransitionFlags.store(0, std::memory_order_relaxed);
    RECT rect = {};
    if (!ReadWindowRectForRenderer(hwnd, &rect))
    {
        return;
    }
    POINT mousePosition = {};
    if (!(physicalCoordinates ? GetPhysicalCursorPos(&mousePosition) : GetCursorPos(&mousePosition)))
    {
        return;
    }
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }
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
        !physicalCoordinates && operationTypeKnown && operationResizing
            ? GetResizeVisualCoordinateScale(hwnd, rect) : 1.0;
    if (operationTypeKnown && operationResizing)
    {
        localMousePosition.x *= g_resizeCoordinateScale;
        localMousePosition.y *= g_resizeCoordinateScale;
    }
    double meshWidth = static_cast<double>(width) * g_resizeCoordinateScale;
    double meshHeight = static_cast<double>(height) * g_resizeCoordinateScale;
    bool meshReset = false;
    int slotIndex = AcquireAnimationSlot(hwnd, activeSettings, meshWidth, meshHeight,
                                         localMousePosition, &meshReset);
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
    g_dragUsesPhysicalCoordinates = physicalCoordinates;
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
               activeSettings.rendererMode == RendererMode::Mesh ? L"mesh" : L"affine",
           width, height, g_realResizing, g_moveTypeKnown);
    if (physicalCoordinates)
    {
        WobbleMesh grabbedMesh = {};
        AcquireSRWLockShared(&g_animationSlotsLock);
        grabbedMesh = g_animationSlots[slotIndex].mesh;
        ReleaseSRWLockShared(&g_animationSlotsLock);
        Wh_Log(L"MESH DRAG COORDINATES: HWND=%p rect=(%ld,%ld,%ld,%ld) "
               L"cursor=(%ld,%ld) local=(%.1f,%.1f) normalized=(%.3f,%.3f) dpi=%u space=physical",
               hwnd, rect.left, rect.top, rect.right, rect.bottom,
               mousePosition.x, mousePosition.y, localMousePosition.x, localMousePosition.y,
               localMousePosition.x / meshWidth, localMousePosition.y / meshHeight,
               GetDpiForWindow(hwnd));
        const int pin = grabbedMesh.dragPointIndex;
        if (pin >= 0 && pin < GRID_POINT_COUNT)
            Wh_Log(L"MESH GRAB STATE: HWND=%p reset=%d mesh=(%.1f,%.1f) "
                   L"baseFirst=(%.1f,%.1f) baseLast=(%.1f,%.1f) pin=%d "
                   L"pinBase=(%.1f,%.1f) offset=(%.1f,%.1f)",
                   hwnd, meshReset, grabbedMesh.width, grabbedMesh.height,
                   grabbedMesh.points[0].basePosition.x, grabbedMesh.points[0].basePosition.y,
                   grabbedMesh.points[GRID_POINT_COUNT - 1].basePosition.x,
                   grabbedMesh.points[GRID_POINT_COUNT - 1].basePosition.y, pin,
                   grabbedMesh.points[pin].basePosition.x, grabbedMesh.points[pin].basePosition.y,
                   grabbedMesh.dragOffset.x, grabbedMesh.dragOffset.y);
    }
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
        // The mesh already includes lag and deformation; keep the source transform neutral.
        identityUpdate = nativeMeshActive ||
                         slot.retiring || slot.meshIdentityPending ||
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
    else if (!nativeMeshActive && snapshot.mesh.dragPointIndex >= 0 &&
             snapshot.mesh.dragPointIndex < GRID_POINT_COUNT)
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
    double meshScaleX = 1.0;
    double meshScaleY = 1.0;
    if (nativeMeshActive && !snapshot.mesh.resizing &&
        g_nativeMeshState.hwnd == snapshot.hwnd &&
        g_nativeMeshState.width > 0 && g_nativeMeshState.height > 0)
    {
        // The source and sibling share a physical parent. The mesh stretches
        // its physics coordinates to that parent's pixel bounds; apply the
        // same conversion to the original window's translation only.
        meshScaleX = static_cast<double>(g_nativeMeshState.width) / snapshot.mesh.width;
        meshScaleY = static_cast<double>(g_nativeMeshState.height) / snapshot.mesh.height;
        translationX *= meshScaleX;
        translationY *= meshScaleY;
    }
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
               L"m11=%.4f m12=%.4f m21=%.4f m22=%.4f dx=%.2f dy=%.2f "
               L"nativeMesh=%d scale=(%.3f,%.3f)",
               snapshot.hwnd, snapshot.meshRevision,
               static_cast<unsigned int>(updateResult), matrix.m11, matrix.m12,
               matrix.m21, matrix.m22, matrix.dx, matrix.dy,
               nativeMeshActive, meshScaleX, meshScaleY);
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
    ScopedDpiContext coordinates(g_dragUsesPhysicalCoordinates ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 : nullptr);
    if (!coordinates.valid()) return;
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
    if (!ReadWindowRectForRenderer(hwnd, &rect))
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
            g_resizeCoordinateScale = g_dragUsesPhysicalCoordinates
                ? 1.0 : GetResizeVisualCoordinateScale(hwnd, rect);
            resizeDetectedThisEvent = true;
        }
        g_moveTypeKnown = true;
    }
    POINT mousePosition = {};
    if (!(g_dragUsesPhysicalCoordinates ? GetPhysicalCursorPos(&mousePosition) : GetCursorPos(&mousePosition)))
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
        g_resizeCoordinateScale = g_dragUsesPhysicalCoordinates
            ? 1.0 : GetResizeVisualCoordinateScale(hwnd, rect);
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
    bool physicalRoundingChange = g_dragUsesPhysicalCoordinates && sizeChanged &&
        std::abs(currentWidth - previousWidth) <= 1 &&
        std::abs(currentHeight - previousHeight) <= 1;
    bool dpiReflow = !g_realResizing && sizeChanged && !physicalRoundingChange && !zoomStateChanged &&
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
    else if (g_dragUsesPhysicalCoordinates && !zoomStateChanged &&
             (sizeChanged || rebaseMonitorTransition))
    {
        // Rounded physical bounds and DPI reflow must not reset the springs
        // or select a different grabbed point in the middle of a move.
        ContinuePhysicalMeshDrag(slot.mesh, currentMeshWidth, currentMeshHeight,
                                 localMousePosition, {windowDeltaX, windowDeltaY},
                                 rebaseMonitorTransition && !physicalRoundingChange);
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
    const Vec2 frameMeshSize = {slot.mesh.width, slot.mesh.height};
    const int framePin = slot.mesh.dragPointIndex;
    const Vec2 framePinBase = framePin >= 0 && framePin < GRID_POINT_COUNT
        ? slot.mesh.points[framePin].basePosition : Vec2{};
    const Vec2 framePinPosition = framePin >= 0 && framePin < GRID_POINT_COUNT
        ? slot.mesh.points[framePin].position : Vec2{};
    const Vec2 framePinOffset = slot.mesh.dragOffset;
    ReleaseSRWLockExclusive(&g_animationSlotsLock);
    if (ShouldEmitTimedTrace(g_nextDragTraceAt))
    {
        Wh_Log(L"DRAG TRACE LOCATION: HWND=%p Slot=%d transformAttached=1 "
               L"delta=(%.1f,%.1f) resizing=%d rebase=%d",
               hwnd, slotIndex, windowDeltaX, windowDeltaY,
               g_realResizing, rebaseMonitorTransition);
        if (g_dragUsesPhysicalCoordinates)
            Wh_Log(L"MESH DRAG FRAME: HWND=%p rectSize=%dx%d mesh=(%.1f,%.1f) "
                   L"local=(%.1f,%.1f) pin=%d base=(%.1f,%.1f) point=(%.1f,%.1f) offset=(%.1f,%.1f)",
                   hwnd, currentWidth, currentHeight, frameMeshSize.x, frameMeshSize.y,
                   localMousePosition.x, localMousePosition.y, framePin,
                   framePinBase.x, framePinBase.y, framePinPosition.x, framePinPosition.y,
                   framePinOffset.x, framePinOffset.y);
    }
    g_lastDraggedWindowRect = rect;
    g_lastDraggedWindowZoomed = windowZoomed;
    g_lastMousePosition = mousePosition;
}

static void HandleMoveSizeEnd(HWND hwnd)
{
    ScopedDpiContext coordinates(g_dragUsesPhysicalCoordinates ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 : nullptr);
    if (!coordinates.valid()) return;
    HWND draggedWindow = g_realDraggedWindow.load(std::memory_order_relaxed);
    if (!g_realDragging.load(std::memory_order_relaxed) || hwnd != draggedWindow ||
        g_dragAnimationSlot < 0 || g_dragAnimationSlot >= MAX_ANIMATION_SLOTS)
    {
        return;
    }
    int slotIndex = g_dragAnimationSlot;
    POINT releaseMousePosition = {};
    bool hasReleaseMousePosition = !g_realResizing &&
        (g_dragUsesPhysicalCoordinates ? GetPhysicalCursorPos(&releaseMousePosition)
                                       : GetCursorPos(&releaseMousePosition));
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
    bool hasReleaseRect = ReadWindowRectForRenderer(hwnd, &releaseRect) != FALSE;
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
    if (!ReadWindowRectForRenderer(hwnd, &rect))
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
        if (ReadWindowRectForRenderer(hwnd, &rect))
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
    if (!ReadWindowRectForRenderer(hwnd, &rect))
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
    bool hasRect = ReadWindowRectForRenderer(hwnd, &rect) != FALSE;
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
            (g_nativeMeshStopRequested.load(std::memory_order_acquire) ||
             g_nativeMeshSourcePending.load(std::memory_order_acquire)))
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
    // Migrate saved diagnostic selections to the full Mesh Renderer.
    settings.rendererMode = rendererValue == L"mesh" || rendererValue.starts_with(L"mesh-")
                                ? RendererMode::Mesh : RendererMode::Affine;
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
    if (!InitializeDwmHooks())
    {
        Wh_Log(L"Failed to initialize DWM hooks");
        UninitializeDpiSupport();
        return FALSE;
    }
    InitializeCoreMaterialBridge();
    if (!StartWindowEventThread())
    {
        Wh_Log(L"Failed to initialize event thread");
        StopWindowEventThread();
        UninitializeDpiSupport();
        return FALSE;
    }
    Wh_Log(L"Initialized successfully");
    return TRUE;
}

void Wh_ModAfterInit()
{
    // Windhawk activates detours after Wh_ModInit returns. Queue the first
    // scene pass only now so already-open windows cannot miss the wake.
    QueueExistingWindowBackfill();
}

void Wh_ModSettingsChanged()
{
    Wh_Log(L"Settings changed");
    LoadSettings();
}

void Wh_ModBeforeUninit()
{
    g_unloading.store(true, std::memory_order_release);
    Wh_Log(L"Preparing to unload");
    // Restore scene resources before Windhawk removes the hooks.
    StopWindowEventThread();
    UninitializeDpiSupport();
}
