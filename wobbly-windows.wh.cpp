// ==WindhawkMod==
// @id              wobbly-windows
// @name            Wobbly Windows
// @description     The classic Compiz/KDE Plasma style Wobbly Windows effect for Windows 11!
// @version         0.204
// @author          lalimatyus
// @github          https://github.com/lalimatyus
// @include         dwm.exe
// @architecture    amd64
// @compilerOptions -lwevtapi
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
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <windhawk_utils.h>

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

struct WobblySettings
{
    bool resizeWobbleEnabled;
    bool windowStateWobbleEnabled;
    double stiffness;
    double drag;
    double moveFactor;
};

static constexpr WobblySettings PHYSICS_PRESETS[] = {
    {false, false, 15.0, 80.0, 10.0}, {false, false, 10.0, 85.0, 10.0},
    {false, false, 6.0, 90.0, 10.0},  {false, false, 3.0, 92.0, 20.0},
    {false, false, 1.0, 97.0, 25.0}};

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
using CWindowBorderCloneVisualTree_t =
    long(__cdecl*)(void* pThis, void** clonedVisual, int cloneOptions);
CWindowBorderCloneVisualTree_t g_windowBorderCloneVisualTreeOriginal = nullptr;
using CTopLevelWindowCloneVisualTreeForLivePreview_t =
    long(__cdecl*)(void* pThis, bool includeRenderData,
                   void** clonedTopLevelWindow);
CTopLevelWindowCloneVisualTreeForLivePreview_t
    g_topLevelWindowCloneVisualTreeForLivePreviewOriginal = nullptr;
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
struct D2DPoint3F
{
    float x;
    float y;
    float z;
};
struct MilPoint2DValue
{
    double x;
    double y;
};
static_assert(sizeof(D2DPoint3F) == 12);
static_assert(sizeof(MilPoint2DValue) == 16);
struct MilRectF
{
    float left;
    float top;
    float right;
    float bottom;
};
struct MilSizeD
{
    double width;
    double height;
};
using CMeshGeometry2dProxyUpdate_t = long(__cdecl*)(
    void* pThis, int mode, const D2DPoint3F* positions,
    const MilPoint2DValue* textureCoordinates, unsigned int vertexCount,
    const unsigned int* indices, unsigned int indexCount);
using CCompositorCreateMeshGeometry2dProxy_t = long(__cdecl*)(void* pThis,
                                                               void** meshProxy);
using CCompositorCreateGeometry2dGroupProxy_t = long(__cdecl*)(void* pThis,
                                                                void** groupProxy);
using CCompositorCreateBitmapSourceProxy_t = long(__cdecl*)(void* pThis,
                                                             void** bitmapProxy);
using CCompositorCreateVisualSurfaceProxy_t = long(__cdecl*)(
    void* pThis, void* sharedHandle, void** surfaceProxy);
using CGeometry2dGroupProxyUpdate_t = long(__cdecl*)(void* pThis, void* meshProxy);
using CDrawMesh2DInstructionCreate_t = long(__cdecl*)(void* geometryGroupProxy,
                                                       void* bitmapSourceProxy,
                                                       void** instruction);
using CDrawBitmapInstructionCreate_t = long(__cdecl*)(void* imageProxy,
                                                       void** instruction);
using CDrawTileImageInstructionCreate_t = long(__cdecl*)(
    void* imageProxy, const RECT& sourceRect, const POINT& destinationOffset,
    float opacity, void** instruction);
using CRenderDataVisualAddInstruction_t = long(__cdecl*)(void* pThis,
                                                          void* instruction);
using CRenderDataVisualClearInstructions_t = long(__cdecl*)(void* pThis);
using CRenderDataVisualUpdateRenderData_t = long(__cdecl*)(void* pThis);
using CTopLevelWindow3DEnsureRenderData_t = long(__cdecl*)(void* pThis);
using CVisualProxySetContent_t = long(__cdecl*)(void* pThis,
                                                const void* content);
using CVisualProxyInsertChild_t = long(__cdecl*)(void* pThis, void* child,
                                                 void* reference, bool insertAbove);
using CVisualProxyRemoveChild_t = long(__cdecl*)(void* pThis, void* child);
using CVisualSetContent_t = long(__cdecl*)(void* pThis, void* content);
using CVisualSetParent_t = long(__cdecl*)(void* pThis, void* parent);
using CVisualRemoveSelfFromParent_t = long(__cdecl*)(void* pThis);
using CVisualGetTransformParent_t = void*(__cdecl*)(void* pThis);
using CVisualGetVisualProxyForStructure_t = void*(__cdecl*)(void* pThis);
using CRedirectVisualProxySetRedirectedVisual_t = long(__cdecl*)(void* pThis,
                                                                 void* visual);
using CCompositorCreateCachedVisualImageProxy_t = long(__cdecl*)(void* pThis,
                                                                  void** proxy);
using CCachedVisualImageProxyUpdate_t = long(__cdecl*)(
    void* pThis, const MilRectF& sourceRect, const MilSizeD& size,
    const void* rectAnimation, const void* sizeAnimation, void* visualProxy,
    int mappingMode);
using CCachedVisualImageProxySnapshot_t = long(__cdecl*)(
    void* pThis, const RECT& sourceRect);
using CCachedVisualImageProxyFreeze_t = long(__cdecl*)(void* pThis);
using CRenderDataVisualCreate_t = long(__cdecl*)(void** visual);
CMeshGeometry2dProxyUpdate_t g_meshGeometry2dProxyUpdate = nullptr;
CCompositorCreateMeshGeometry2dProxy_t g_createMeshGeometry2dProxy = nullptr;
CCompositorCreateGeometry2dGroupProxy_t g_createGeometry2dGroupProxy = nullptr;
CCompositorCreateBitmapSourceProxy_t g_createBitmapSourceProxyOriginal = nullptr;
CCompositorCreateVisualSurfaceProxy_t
    g_createVisualSurfaceProxyOriginal = nullptr;
CGeometry2dGroupProxyUpdate_t g_geometry2dGroupProxyUpdate = nullptr;
CDrawMesh2DInstructionCreate_t g_drawMesh2DInstructionCreate = nullptr;
void* g_createTouchDragVisualFunction = nullptr;
void* g_touchDragVisualNotifyFunction = nullptr;
void* g_touchDragVisualStopFunction = nullptr;
void* g_touchDragVisualCreateMeshInstructionFunction = nullptr;
CDrawBitmapInstructionCreate_t g_drawBitmapInstructionCreateOriginal = nullptr;
CDrawTileImageInstructionCreate_t g_drawTileImageInstructionCreateOriginal =
    nullptr;
CRenderDataVisualAddInstruction_t g_renderDataVisualAddInstruction = nullptr;
CRenderDataVisualClearInstructions_t g_renderDataVisualClearInstructions =
    nullptr;
CRenderDataVisualUpdateRenderData_t g_renderDataVisualUpdateRenderData = nullptr;
CTopLevelWindow3DEnsureRenderData_t
    g_topLevelWindow3DEnsureRenderDataOriginal = nullptr;
using CTopLevelWindow3DEnsureSecondaryWindowRepresentation_t =
    long(__cdecl*)(void* pThis, bool forceRecreate);
CTopLevelWindow3DEnsureSecondaryWindowRepresentation_t
    g_topLevelWindow3DEnsureSecondaryWindowRepresentationOriginal = nullptr;
using CTopLevelWindow3DSetParent_t =
    long(__cdecl*)(void* pThis, void* parent);
CTopLevelWindow3DSetParent_t g_topLevelWindow3DSetParentOriginal = nullptr;
using CTopLevelWindow3DShowWindow_t =
    long(__cdecl*)(void* pThis, bool show, bool activate);
CTopLevelWindow3DShowWindow_t g_topLevelWindow3DShowWindowOriginal = nullptr;
size_t g_renderDataInstructionsOffset = SIZE_MAX;
size_t g_renderDataInstructionCountOffset = SIZE_MAX;
size_t g_ensureRenderDataPointerOffsets[16] = {};
unsigned int g_ensureRenderDataPointerOffsetCount = 0;
CVisualProxySetContent_t g_visualProxySetContentOriginal = nullptr;
CVisualProxyInsertChild_t g_visualProxyInsertChildOriginal = nullptr;
CVisualProxyRemoveChild_t g_visualProxyRemoveChildOriginal = nullptr;
CVisualSetContent_t g_visualSetContentOriginal = nullptr;
CVisualSetParent_t g_visualSetParentOriginal = nullptr;
CVisualRemoveSelfFromParent_t g_visualRemoveSelfFromParentOriginal = nullptr;
using CVisualVisibility_t = void(__cdecl*)(void* pThis);
CVisualVisibility_t g_visualHideOriginal = nullptr;
CVisualVisibility_t g_visualUnhideOriginal = nullptr;
using CVisualSetOpacity_t = void(__cdecl*)(void* pThis, double opacity);
CVisualSetOpacity_t g_visualSetOpacityOriginal = nullptr;
CVisualGetTransformParent_t g_visualGetTransformParent = nullptr;
CVisualGetVisualProxyForStructure_t g_visualGetVisualProxyForStructure = nullptr;
void* g_visualCollectionInsertRelativeFunction = nullptr;
CRedirectVisualProxySetRedirectedVisual_t
    g_redirectVisualProxySetRedirectedVisualOriginal = nullptr;
CCompositorCreateCachedVisualImageProxy_t
    g_createCachedVisualImageProxy = nullptr;
CCachedVisualImageProxyUpdate_t g_cachedVisualImageProxyUpdate = nullptr;
CCachedVisualImageProxySnapshot_t g_cachedVisualImageProxySnapshot = nullptr;
CCachedVisualImageProxyFreeze_t g_cachedVisualImageProxyFreeze = nullptr;
CRenderDataVisualCreate_t g_renderDataVisualCreate = nullptr;
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
std::atomic<void*> g_bitmapSourceProxyVtable = nullptr;
std::atomic<void*> g_visualSurfaceProxyVtable = nullptr;
std::atomic<void*> g_cachedVisualImageProxyVtable = nullptr;
std::atomic<void*> g_clientAreaVtable = nullptr;
std::atomic<void*> g_visualCollectionVtable = nullptr;
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
void* g_bitmapSourceProxyVtableSymbol = nullptr;
void* g_visualSurfaceProxyVtableSymbol = nullptr;
void* g_cachedVisualImageProxyVtableSymbol = nullptr;
void* g_clientAreaVtableSymbol = nullptr;
void* g_visualCollectionVtableSymbol = nullptr;
size_t g_desktopManagerCompositorOffset = SIZE_MAX;
size_t g_desktopManagerThreadIdOffset = SIZE_MAX;
size_t g_canvasVisualOwnerOffset = SIZE_MAX;
size_t g_visualProxyOffset = SIZE_MAX;
size_t g_visualParentOffset = SIZE_MAX;
size_t g_visualContentOffset = SIZE_MAX;
size_t g_visualCollectionArrayOffset = SIZE_MAX;
size_t g_visualCollectionCountOffset = SIZE_MAX;
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

static bool IsWritableMemory(void* address, size_t size)
{
    if (!address || !size)
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
        if (!VirtualQuery(reinterpret_cast<void*>(current), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }
        DWORD protection = mbi.Protect & 0xFF;
        if (protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY &&
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

static bool GetRenderDataInstructionList(void* renderVisual,
                                         void*** instructions,
                                         int** countAddress, int* count)
{
    if (instructions)
    {
        *instructions = nullptr;
    }
    if (countAddress)
    {
        *countAddress = nullptr;
    }
    if (count)
    {
        *count = 0;
    }
    if (!renderVisual ||
        g_renderDataInstructionsOffset == SIZE_MAX ||
        g_renderDataInstructionCountOffset == SIZE_MAX)
    {
        return false;
    }
    BYTE* visualBytes = static_cast<BYTE*>(renderVisual);
    int* countField = reinterpret_cast<int*>(
        visualBytes + g_renderDataInstructionCountOffset);
    if (!IsReadableMemory(countField, sizeof(*countField)))
    {
        return false;
    }
    int currentCount = *countField;
    if (currentCount <= 0 || currentCount > 64)
    {
        return false;
    }
    void** array = static_cast<void**>(
        ReadPointerMember(renderVisual, g_renderDataInstructionsOffset));
    if (!IsReadableMemory(array,
                          static_cast<size_t>(currentCount) * sizeof(*array)))
    {
        return false;
    }
    if (instructions)
    {
        *instructions = array;
    }
    if (countAddress)
    {
        *countAddress = countField;
    }
    if (count)
    {
        *count = currentCount;
    }
    return true;
}

static bool FindRenderDataInstructionIndex(void* renderVisual,
                                           void* instruction,
                                           unsigned int* instructionCount,
                                           int* instructionIndex)
{
    if (instructionCount)
    {
        *instructionCount = 0;
    }
    if (instructionIndex)
    {
        *instructionIndex = -1;
    }
    if (!renderVisual || !instruction ||
        g_renderDataInstructionsOffset == SIZE_MAX ||
        g_renderDataInstructionCountOffset == SIZE_MAX)
    {
        return false;
    }
    void** instructions = nullptr;
    int count = 0;
    if (!GetRenderDataInstructionList(renderVisual, &instructions, nullptr,
                                      &count))
    {
        return false;
    }
    if (instructionCount)
    {
        *instructionCount = static_cast<unsigned int>(count);
    }
    for (int index = 0; index < count; index++)
    {
        if (instructions[index] == instruction)
        {
            if (instructionIndex)
            {
                *instructionIndex = index;
            }
            return true;
        }
    }
    return false;
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
std::atomic_bool g_nativeMeshCanaryPending = false;
std::atomic_bool g_nativeMeshCanarySucceeded = false;
// Discovery stays read-only until both the exact owner and a reversible
// replacement transaction are proven. Older slot-replacement experiments
// could leave an identity mesh instruction alive after the mod was unloaded.
static constexpr bool NATIVE_MESH_WRITE_PROBE_ENABLED = false;
static constexpr bool NATIVE_MESH_TRANSACTION_PROBE_ENABLED = true;
std::atomic_bool g_liveBaseImageMeshCanaryStarted = false;
std::atomic_bool g_liveBaseImageMeshCanarySucceeded = false;
std::atomic_bool g_liveBaseImageMeshAnimationLogged = false;
std::atomic<HWND> g_liveBaseImageMeshTargetHwnd = nullptr;
std::atomic_bool g_meshSourceProbePending = false;
std::atomic_bool g_meshSourceProbeCompleted = false;
std::atomic_bool g_cachedVisualImageCanaryCompleted = false;
struct VisibleMeshCanaryState
{
    void* cachedVisual;
    void* meshProxy;
    void* groupProxy;
    void* instruction;
    void* pinnedImageProxy;
    void* renderVisual;
    HWND hwnd;
    ULONGLONG detachAt;
    struct
    {
        void* meshProxy;
        void* groupProxy;
        void* instruction;
    } additionalNativeBindings[3];
    unsigned int nativeBindingCount;
    double width;
    double height;
};
VisibleMeshCanaryState g_visibleMeshCanary = {};
std::atomic_bool g_visibleMeshCanaryActive = false;
std::atomic_bool g_visibleMeshCanaryCleanupRequested = false;
std::atomic<HWND> g_visibleMeshCanaryHwnd = nullptr;
struct NativeRenderSlotProbeState
{
    HWND hwnd;
    void* renderVisual;
    void** instructionArray;
    int* countAddress;
    void* instruction;
    void* imageProxy;
    uintptr_t fingerprint;
    int instructionIndex;
    int instructionCount;
    unsigned int samples;
    unsigned int arrayChanges;
    unsigned int slotChanges;
    unsigned int missingSamples;
};
NativeRenderSlotProbeState g_nativeRenderSlotProbe = {};
SRWLOCK g_nativeRenderSlotProbeLock = SRWLOCK_INIT;
std::atomic_uint g_nativePublishProbeSamples = 0;
std::atomic_uint g_nativePublishProbeChanges = 0;
std::atomic_uint g_nativePublishProbeMissing = 0;
static constexpr unsigned int OBSERVED_VISUAL_PROXY_COUNT = 4096;
static constexpr unsigned int OBSERVED_VISUAL_PROXY_PROBES = 32;
struct ObservedVisualProxy
{
    std::atomic<void*> proxy;
    std::atomic<void*> parent;
    std::atomic<void*> content;
    std::atomic<void*> redirectTarget;
};
ObservedVisualProxy g_observedVisualProxies[OBSERVED_VISUAL_PROXY_COUNT] = {};
ObservedVisualProxy g_observedVisuals[OBSERVED_VISUAL_PROXY_COUNT] = {};
struct ObservedBitmapInstruction
{
    std::atomic<void*> instruction;
    std::atomic<void*> imageProxy;
};
enum class MeshSourceKind
{
    None,
    Bitmap,
    VisualSurface,
    AmbiguousProxy,
};
static MeshSourceKind GetMeshSourceKind(void* object);
static bool ReadBaseImageResourceId(void* imageProxy,
                                    unsigned int* resourceId);
static bool FindUniqueBaseImageInstruction(
    void* renderVisual, unsigned int requiredResourceId,
    void** instruction, void** imageProxy, int* instructionIndex,
    int* instructionCount, unsigned int* candidateCount);
static void ObserveNativeRenderSlotStability(void* renderVisual,
                                             void* windowData, HWND hwnd,
                                             const wchar_t* eventName);
struct ObservedRenderImage
{
    std::atomic<void*> visual;
    std::atomic<void*> instruction;
    std::atomic<void*> imageProxy;
    std::atomic<void*> imageVtable;
    std::atomic<void*> ownerWindowData;
    std::atomic<HWND> ownerHwnd;
    std::atomic<int> sourceKind;
    std::atomic<unsigned int> capturedInstructionCount;
    std::atomic<int> capturedInstructionIndex;
};
ObservedBitmapInstruction
    g_observedBitmapInstructions[OBSERVED_VISUAL_PROXY_COUNT] = {};
ObservedRenderImage g_observedRenderImages[OBSERVED_VISUAL_PROXY_COUNT] = {};
std::atomic<void*>
    g_observedBitmapSourceProxies[OBSERVED_VISUAL_PROXY_COUNT] = {};
std::atomic<void*>
    g_observedVisualSurfaceProxies[OBSERVED_VISUAL_PROXY_COUNT] = {};
std::atomic<unsigned int> g_observedBitmapSourceCreateCount = 0;
std::atomic<unsigned int> g_observedVisualSurfaceCreateCount = 0;
std::atomic<unsigned int> g_observedDrawBitmapCreateCount = 0;
std::atomic<unsigned int> g_observedDrawTileCreateCount = 0;
std::atomic<unsigned int> g_observedImageInstructionMatchedAddCount = 0;
std::atomic<unsigned int> g_ensureRenderDataCallCount = 0;
std::atomic<unsigned int> g_ensureRenderDataMappedCount = 0;
std::atomic<unsigned int> g_ensureRenderDataPopulatedCount = 0;
std::atomic<unsigned int> g_windowBorderCloneCallCount = 0;
std::atomic<unsigned int> g_livePreviewCloneCallCount = 0;
std::atomic<unsigned int> g_secondaryRepresentationCallCount = 0;
std::atomic<unsigned int> g_topLevelWindow3DSetParentCallCount = 0;
std::atomic<unsigned int> g_topLevelWindow3DShowWindowCallCount = 0;
std::atomic<unsigned int> g_trackedVisualVisibilityCallCount = 0;
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
static void RunNativeMeshCanary();
static bool TryInstallLiveBaseImageMeshCanary(void* renderVisual,
                                               void* originalInstruction,
                                               void* imageProxy,
                                               void* windowData, HWND hwnd);
static void InstallRequestedLiveBaseImageMeshCanary();
static long UpdateNativeMeshGeometry(void* meshProxy,
                                      const WobbleMesh* mesh = nullptr,
                                      double identityWidth = 0.0,
                                      double identityHeight = 0.0);
static void MaintainVisibleMeshCanary();
static void RequestVisibleMeshCleanupForHwnd(HWND hwnd);
static long __cdecl VisualProxySetContentHook(void* pThis, const void* content);
static long __cdecl VisualProxyInsertChildHook(void* pThis, void* child,
                                               void* reference, bool insertAbove);
static long __cdecl VisualProxyRemoveChildHook(void* pThis, void* child);
static long __cdecl VisualSetContentHook(void* pThis, void* content);
static long __cdecl VisualSetParentHook(void* pThis, void* parent);
static long __cdecl VisualRemoveSelfFromParentHook(void* pThis);
static long __cdecl DrawBitmapInstructionCreateHook(void* imageProxy,
                                                     void** instruction);
static long __cdecl DrawTileImageInstructionCreateHook(
    void* imageProxy, const RECT& sourceRect, const POINT& destinationOffset,
    float opacity, void** instruction);
static long __cdecl RenderDataVisualAddInstructionHook(void* pThis,
                                                        void* instruction);
static long __cdecl RenderDataVisualUpdateRenderDataHook(void* pThis);
static long __cdecl TopLevelWindow3DEnsureRenderDataHook(void* pThis);
static long __cdecl CreateBitmapSourceProxyHook(void* pThis,
                                                 void** bitmapProxy);
static long __cdecl CreateVisualSurfaceProxyHook(void* pThis,
                                                  void* sharedHandle,
                                                  void** surfaceProxy);
static long __cdecl RedirectVisualProxySetRedirectedVisualHook(void* pThis,
                                                               void* visual);
static bool HasAnyAnimationSlots();
static int GetPointIndex(int x, int y);
static bool ResetMatrixTransformProxy(void* matrixTransformProxy);
static bool CreateMatrixTransformProxy(void** matrixTransformProxy);
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

struct NativeRenderCallSnapshot
{
    void** instructionArray;
    int* countAddress;
    void* instruction;
    void* imageProxy;
    uintptr_t fingerprint;
    int instructionIndex;
    int instructionCount;
    unsigned int candidateCount;
    bool listValid;
    bool unique;
};

static NativeRenderCallSnapshot CaptureNativeRenderCallSnapshot(
    void* renderVisual)
{
    NativeRenderCallSnapshot snapshot = {};
    snapshot.instructionIndex = -1;
    snapshot.listValid = GetRenderDataInstructionList(
        renderVisual, &snapshot.instructionArray, &snapshot.countAddress,
        &snapshot.instructionCount);
    if (!snapshot.listValid)
    {
        return snapshot;
    }
    for (int index = 0; index < snapshot.instructionCount; index++)
    {
        uintptr_t value = reinterpret_cast<uintptr_t>(
            snapshot.instructionArray[index]);
        snapshot.fingerprint ^=
            value + static_cast<uintptr_t>(0x9E3779B9u) +
            (snapshot.fingerprint << 6) + (snapshot.fingerprint >> 2);
    }
    int uniqueInstructionCount = 0;
    snapshot.unique = FindUniqueBaseImageInstruction(
        renderVisual, 0, &snapshot.instruction, &snapshot.imageProxy,
        &snapshot.instructionIndex, &uniqueInstructionCount,
        &snapshot.candidateCount);
    return snapshot;
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

static void ObserveNativeRenderSlotStability(void* renderVisual,
                                             void* windowData, HWND hwnd,
                                             const wchar_t* eventName)
{
    if (!renderVisual || !windowData || !hwnd ||
        g_liveBaseImageMeshTargetHwnd.load(std::memory_order_acquire) != hwnd ||
        GetHwndFromWindowData(windowData) != hwnd)
    {
        return;
    }

    void** instructions = nullptr;
    int* countAddress = nullptr;
    int instructionCount = 0;
    bool listValid = GetRenderDataInstructionList(
        renderVisual, &instructions, &countAddress, &instructionCount);
    uintptr_t fingerprint = 0;
    if (listValid)
    {
        for (int index = 0; index < instructionCount; index++)
        {
            uintptr_t value = reinterpret_cast<uintptr_t>(instructions[index]);
            fingerprint ^= value + static_cast<uintptr_t>(0x9E3779B9u) +
                           (fingerprint << 6) + (fingerprint >> 2);
        }
    }

    void* instruction = nullptr;
    void* imageProxy = nullptr;
    int instructionIndex = -1;
    int uniqueInstructionCount = 0;
    unsigned int candidateCount = 0;
    bool unique = listValid && FindUniqueBaseImageInstruction(
                                   renderVisual, 0, &instruction, &imageProxy,
                                   &instructionIndex, &uniqueInstructionCount,
                                   &candidateCount);

    bool logSample = false;
    bool changed = false;
    NativeRenderSlotProbeState snapshot = {};
    AcquireSRWLockExclusive(&g_nativeRenderSlotProbeLock);
    bool newTarget = g_nativeRenderSlotProbe.hwnd != hwnd ||
                     g_nativeRenderSlotProbe.renderVisual != renderVisual;
    if (newTarget)
    {
        g_nativeRenderSlotProbe = {};
        g_nativeRenderSlotProbe.hwnd = hwnd;
        g_nativeRenderSlotProbe.renderVisual = renderVisual;
    }
    else if (g_nativeRenderSlotProbe.samples)
    {
        bool arrayChanged =
            g_nativeRenderSlotProbe.instructionArray != instructions ||
            g_nativeRenderSlotProbe.countAddress != countAddress ||
            g_nativeRenderSlotProbe.instructionCount != instructionCount;
        bool slotChanged =
            g_nativeRenderSlotProbe.instruction != instruction ||
            g_nativeRenderSlotProbe.imageProxy != imageProxy ||
            g_nativeRenderSlotProbe.instructionIndex != instructionIndex ||
            g_nativeRenderSlotProbe.fingerprint != fingerprint;
        if (arrayChanged)
        {
            g_nativeRenderSlotProbe.arrayChanges++;
        }
        if (slotChanged)
        {
            g_nativeRenderSlotProbe.slotChanges++;
        }
        changed = arrayChanged || slotChanged;
    }
    g_nativeRenderSlotProbe.samples++;
    if (!listValid || !unique)
    {
        g_nativeRenderSlotProbe.missingSamples++;
    }
    g_nativeRenderSlotProbe.instructionArray = instructions;
    g_nativeRenderSlotProbe.countAddress = countAddress;
    g_nativeRenderSlotProbe.instruction = instruction;
    g_nativeRenderSlotProbe.imageProxy = imageProxy;
    g_nativeRenderSlotProbe.fingerprint = fingerprint;
    g_nativeRenderSlotProbe.instructionIndex = instructionIndex;
    g_nativeRenderSlotProbe.instructionCount = instructionCount;
    snapshot = g_nativeRenderSlotProbe;
    logSample = newTarget || changed || snapshot.samples == 4 ||
                snapshot.samples == 12 || snapshot.samples == 32 ||
                ((!listValid || !unique) && snapshot.missingSamples <= 2);
    ReleaseSRWLockExclusive(&g_nativeRenderSlotProbeLock);

    if (logSample)
    {
        Wh_Log(L"True 4x4 native slot stability: event=%s sample=%u "
               L"HWND=%p visual=%p list=%p countField=%p index=%d/%d "
               L"instruction=%p image=%p fingerprint=0x%llX "
               L"candidates=%u listValid=%d unique=%d arrayChanges=%u "
               L"slotChanges=%u missing=%u (read-only)",
               eventName, snapshot.samples, hwnd, renderVisual, instructions,
               countAddress, instructionIndex, instructionCount, instruction,
               imageProxy, static_cast<unsigned long long>(fingerprint),
               candidateCount, listValid, unique, snapshot.arrayChanges,
               snapshot.slotChanges, snapshot.missingSamples);
    }
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

static bool FindRenderDataInstructionLayout(void* function,
                                            size_t* instructionsOffset,
                                            size_t* countOffset)
{
    if (!instructionsOffset || !countOffset ||
        !IsDwmFunctionPointerValid(function))
    {
        return false;
    }
    const std::regex countPattern(
        R"(movsxd\s+\w+,\s*(?:dword ptr )?\[rcx\s*\+\s*0x([0-9a-f]{1,8})\])",
        std::regex_constants::icase);
    const std::regex arrayPattern(
        R"(lea\s+\w+,\s*\[rcx\s*\+\s*0x([0-9a-f]{1,8})\])",
        std::regex_constants::icase);
    size_t candidateCount = SIZE_MAX;
    size_t candidateArray = SIZE_MAX;
    BYTE* instruction = static_cast<BYTE*>(function);
    size_t bytesRead = 0;
    for (int i = 0; i < 48 && bytesRead < 256; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) ||
            !Wh_Disasm(instruction, &result) || result.length == 0)
        {
            break;
        }
        std::string_view text = result.text;
        std::match_results<std::string_view::const_iterator> match;
        if (candidateCount == SIZE_MAX &&
            std::regex_match(text.begin(), text.end(), match, countPattern))
        {
            candidateCount = std::stoull(match[1].str(), nullptr, 16);
        }
        else if (candidateArray == SIZE_MAX &&
                 std::regex_match(text.begin(), text.end(), match,
                                  arrayPattern))
        {
            candidateArray = std::stoull(match[1].str(), nullptr, 16);
        }
        if (candidateArray != SIZE_MAX && candidateCount != SIZE_MAX)
        {
            break;
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }
    if (candidateArray > 0x1000 || candidateCount > 0x1000 ||
        candidateCount != candidateArray + 0x18)
    {
        return false;
    }
    *instructionsOffset = candidateArray;
    *countOffset = candidateCount;
    return true;
}

static bool FindVisualCollectionLayout(void* function, size_t* arrayOffset,
                                       size_t* countOffset)
{
    if (!arrayOffset || !countOffset ||
        !IsDwmFunctionPointerValid(function))
    {
        return false;
    }

    std::string thisAliases[12] = {"rcx"};
    unsigned int aliasCount = 1;
    auto findAlias = [&](const std::string& name) -> int
    {
        for (unsigned int i = 0; i < aliasCount; i++)
        {
            if (thisAliases[i] == name)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    auto removeAlias = [&](const std::string& name)
    {
        int index = findAlias(name);
        if (index >= 0)
        {
            thisAliases[index] = thisAliases[--aliasCount];
        }
    };
    auto addAlias = [&](const std::string& name)
    {
        if (findAlias(name) < 0 && aliasCount < ARRAYSIZE(thisAliases))
        {
            thisAliases[aliasCount++] = name;
        }
    };

    const std::regex movePattern(
        R"(^mov\s+(r[a-z0-9]+),\s*(r[a-z0-9]+)$)",
        std::regex_constants::icase);
    const std::regex countPattern(
        R"(^mov\s+(?:e[a-z0-9]+|r[0-9]+d),\s*(?:dword ptr\s*)?\[(r[a-z0-9]+)\s*\+\s*0x([0-9a-f]{1,8})\]$)",
        std::regex_constants::icase);
    const std::regex arrayPattern(
        R"(^lea\s+(r[a-z0-9]+),\s*\[(r[a-z0-9]+)\s*\+\s*0x([0-9a-f]{1,8})\]$)",
        std::regex_constants::icase);
    size_t countCandidates[8] = {};
    size_t arrayCandidates[8] = {};
    unsigned int countCandidateCount = 0;
    unsigned int arrayCandidateCount = 0;
    auto addCandidate = [](size_t* candidates, unsigned int* count,
                           size_t value)
    {
        if (value > 0x100 || value % sizeof(void*) != 0)
        {
            return;
        }
        for (unsigned int i = 0; i < *count; i++)
        {
            if (candidates[i] == value)
            {
                return;
            }
        }
        if (*count < 8)
        {
            candidates[(*count)++] = value;
        }
    };

    BYTE* instruction = static_cast<BYTE*>(function);
    size_t bytesRead = 0;
    for (int i = 0; i < 96 && bytesRead < 512; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) ||
            !Wh_Disasm(instruction, &result) || result.length == 0)
        {
            break;
        }
        std::string text = result.text;
        std::smatch match;
        if (std::regex_match(text, match, movePattern))
        {
            const std::string destination = match[1].str();
            if (findAlias(match[2].str()) >= 0)
            {
                addAlias(destination);
            }
            else
            {
                removeAlias(destination);
            }
        }
        else if (std::regex_match(text, match, countPattern) &&
                 findAlias(match[1].str()) >= 0)
        {
            addCandidate(countCandidates, &countCandidateCount,
                         std::stoull(match[2].str(), nullptr, 16));
        }
        else if (std::regex_match(text, match, arrayPattern))
        {
            const std::string destination = match[1].str();
            if (findAlias(match[2].str()) >= 0)
            {
                addCandidate(arrayCandidates, &arrayCandidateCount,
                             std::stoull(match[3].str(), nullptr, 16));
            }
            removeAlias(destination);
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }

    size_t matchedArray = SIZE_MAX;
    size_t matchedCount = SIZE_MAX;
    unsigned int matches = 0;
    for (unsigned int i = 0; i < arrayCandidateCount; i++)
    {
        for (unsigned int j = 0; j < countCandidateCount; j++)
        {
            if (countCandidates[j] == arrayCandidates[i] + 0x18)
            {
                matchedArray = arrayCandidates[i];
                matchedCount = countCandidates[j];
                matches++;
            }
        }
    }
    if (matches != 1)
    {
        return false;
    }
    *arrayOffset = matchedArray;
    *countOffset = matchedCount;
    return true;
}

static unsigned int FindEnsureRenderDataPointerOffsets(
    void* function, size_t* offsets, unsigned int capacity)
{
    if (!offsets || capacity == 0 || !IsDwmFunctionPointerValid(function))
    {
        return 0;
    }

    std::string thisAliases[16] = {"rcx"};
    unsigned int aliasCount = 1;
    auto findAlias = [&](const std::string& name) -> int
    {
        for (unsigned int i = 0; i < aliasCount; i++)
        {
            if (thisAliases[i] == name)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    auto removeAlias = [&](const std::string& name)
    {
        int index = findAlias(name);
        if (index >= 0)
        {
            thisAliases[index] = thisAliases[--aliasCount];
        }
    };
    auto addAlias = [&](const std::string& name)
    {
        if (findAlias(name) < 0 && aliasCount < ARRAYSIZE(thisAliases))
        {
            thisAliases[aliasCount++] = name;
        }
    };
    auto addOffset = [&](size_t offset, unsigned int count)
    {
        if (offset < 0x20 || offset > 0x800 ||
            offset % sizeof(void*) != 0)
        {
            return count;
        }
        for (unsigned int i = 0; i < count; i++)
        {
            if (offsets[i] == offset)
            {
                return count;
            }
        }
        if (count < capacity)
        {
            offsets[count++] = offset;
        }
        return count;
    };

    const std::regex movePattern(
        R"(^mov\s+(r[a-z0-9]+),\s*(r[a-z0-9]+)$)",
        std::regex_constants::icase);
    const std::regex loadPattern(
        R"(^mov\s+(r[a-z0-9]+),\s*(?:qword ptr\s*)?\[(r[a-z0-9]+)\s*\+\s*0x([0-9a-f]{1,8})\]$)",
        std::regex_constants::icase);
    BYTE* instruction = static_cast<BYTE*>(function);
    size_t bytesRead = 0;
    unsigned int count = 0;
    for (int i = 0; i < 256 && bytesRead < 1024; i++)
    {
        WH_DISASM_RESULT result = {};
        if (!IsDwmExecutableAddress(instruction) ||
            !Wh_Disasm(instruction, &result) || result.length == 0)
        {
            break;
        }
        std::string text = result.text;
        std::smatch match;
        if (std::regex_match(text, match, loadPattern))
        {
            const std::string destination = match[1].str();
            const std::string base = match[2].str();
            if (findAlias(base) >= 0)
            {
                count = addOffset(
                    std::stoull(match[3].str(), nullptr, 16), count);
            }
            removeAlias(destination);
        }
        else if (std::regex_match(text, match, movePattern))
        {
            const std::string destination = match[1].str();
            const std::string source = match[2].str();
            if (findAlias(source) >= 0)
            {
                addAlias(destination);
            }
            else
            {
                removeAlias(destination);
            }
        }
        if (text == "ret")
        {
            break;
        }
        instruction += result.length;
        bytesRead += result.length;
    }
    return count;
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
    RequestVisibleMeshCleanupForHwnd(GetHwndFromTrustedWindowData(pThis));
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
    RequestVisibleMeshCleanupForHwnd(GetHwndFromTrustedWindowData(windowData));
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

static ObservedVisualProxy* FindObservedNode(ObservedVisualProxy* table,
                                             void* object, bool create)
{
    if (!object)
    {
        return nullptr;
    }
    uintptr_t hash = reinterpret_cast<uintptr_t>(object) >> 4;
    hash ^= hash >> 17;
    for (unsigned int probe = 0; probe < OBSERVED_VISUAL_PROXY_PROBES; probe++)
    {
        ObservedVisualProxy& entry =
            table[(hash + probe) % OBSERVED_VISUAL_PROXY_COUNT];
        void* observed = entry.proxy.load(std::memory_order_acquire);
        if (observed == object)
        {
            return &entry;
        }
        if (!observed)
        {
            if (!create)
            {
                return nullptr;
            }
            void* expected = nullptr;
            if (entry.proxy.compare_exchange_strong(
                    expected, object, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                entry.parent.store(nullptr, std::memory_order_relaxed);
                entry.content.store(nullptr, std::memory_order_relaxed);
                entry.redirectTarget.store(nullptr, std::memory_order_relaxed);
                return &entry;
            }
            if (expected == object)
            {
                return &entry;
            }
        }
    }
    return nullptr;
}

static ObservedVisualProxy* FindObservedVisualProxy(void* proxy, bool create)
{
    return FindObservedNode(g_observedVisualProxies, proxy, create);
}

static ObservedVisualProxy* FindObservedVisual(void* visual, bool create)
{
    return FindObservedNode(g_observedVisuals, visual, create);
}

static bool FindObservedExactSourceProxy(std::atomic<void*>* table,
                                          void* proxy, bool create)
{
    if (!proxy)
    {
        return false;
    }
    uintptr_t hash = reinterpret_cast<uintptr_t>(proxy) >> 4;
    hash ^= hash >> 17;
    for (unsigned int probe = 0; probe < OBSERVED_VISUAL_PROXY_PROBES; probe++)
    {
        std::atomic<void*>& entry =
            table[(hash + probe) % OBSERVED_VISUAL_PROXY_COUNT];
        void* observed = entry.load(std::memory_order_acquire);
        if (observed == proxy)
        {
            return true;
        }
        if (!observed)
        {
            if (!create)
            {
                return false;
            }
            void* expected = nullptr;
            if (entry.compare_exchange_strong(
                    expected, proxy, std::memory_order_acq_rel,
                    std::memory_order_acquire) || expected == proxy)
            {
                return true;
            }
        }
    }
    return false;
}

static bool FindObservedBitmapSourceProxy(void* proxy, bool create)
{
    return FindObservedExactSourceProxy(g_observedBitmapSourceProxies, proxy,
                                         create);
}

static bool FindObservedVisualSurfaceProxy(void* proxy, bool create)
{
    return FindObservedExactSourceProxy(g_observedVisualSurfaceProxies, proxy,
                                         create);
}

static long __cdecl CreateBitmapSourceProxyHook(void* pThis,
                                                 void** bitmapProxy)
{
    long result = g_createBitmapSourceProxyOriginal(pThis, bitmapProxy);
    if (result >= 0 && bitmapProxy && *bitmapProxy &&
        !g_unloading.load(std::memory_order_acquire))
    {
        FindObservedBitmapSourceProxy(*bitmapProxy, true);
        g_observedBitmapSourceCreateCount.fetch_add(1,
                                                     std::memory_order_relaxed);
    }
    return result;
}

static long __cdecl CreateVisualSurfaceProxyHook(void* pThis,
                                                  void* sharedHandle,
                                                  void** surfaceProxy)
{
    long result = g_createVisualSurfaceProxyOriginal(
        pThis, sharedHandle, surfaceProxy);
    if (result >= 0 && surfaceProxy && *surfaceProxy &&
        !g_unloading.load(std::memory_order_acquire))
    {
        FindObservedVisualSurfaceProxy(*surfaceProxy, true);
        g_observedVisualSurfaceCreateCount.fetch_add(
            1, std::memory_order_relaxed);
    }
    return result;
}

template <typename Entry>
static Entry* FindObservedImageEntry(Entry* table, void* key, bool create)
{
    if (!key)
    {
        return nullptr;
    }
    uintptr_t hash = reinterpret_cast<uintptr_t>(key) >> 4;
    hash ^= hash >> 17;
    for (unsigned int probe = 0; probe < OBSERVED_VISUAL_PROXY_PROBES; probe++)
    {
        Entry& entry =
            table[(hash + probe) % OBSERVED_VISUAL_PROXY_COUNT];
        void* observed = nullptr;
        if constexpr (std::is_same_v<Entry, ObservedBitmapInstruction>)
        {
            observed = entry.instruction.load(std::memory_order_acquire);
        }
        else
        {
            observed = entry.visual.load(std::memory_order_acquire);
        }
        if (observed == key)
        {
            return &entry;
        }
        if (!observed)
        {
            if (!create)
            {
                return nullptr;
            }
            std::atomic<void*>& keySlot = [&]() -> std::atomic<void*>&
            {
                if constexpr (std::is_same_v<Entry, ObservedBitmapInstruction>)
                {
                    return entry.instruction;
                }
                else
                {
                    return entry.visual;
                }
            }();
            void* expected = nullptr;
            if (keySlot.compare_exchange_strong(
                    expected, key, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                entry.imageProxy.store(nullptr, std::memory_order_relaxed);
                if constexpr (std::is_same_v<Entry, ObservedRenderImage>)
                {
                    entry.instruction.store(nullptr,
                                            std::memory_order_relaxed);
                    entry.imageVtable.store(nullptr,
                                            std::memory_order_relaxed);
                    entry.ownerWindowData.store(nullptr,
                                                std::memory_order_relaxed);
                    entry.ownerHwnd.store(nullptr,
                                          std::memory_order_relaxed);
                    entry.sourceKind.store(
                        static_cast<int>(MeshSourceKind::None),
                        std::memory_order_relaxed);
                    entry.capturedInstructionCount.store(
                        0, std::memory_order_relaxed);
                    entry.capturedInstructionIndex.store(
                        -1, std::memory_order_relaxed);
                }
                return &entry;
            }
            if (expected == key)
            {
                return &entry;
            }
        }
    }
    return nullptr;
}

static long __cdecl DrawBitmapInstructionCreateHook(void* imageProxy,
                                                     void** instruction)
{
    long result =
        g_drawBitmapInstructionCreateOriginal(imageProxy, instruction);
    if (result >= 0 && instruction && *instruction && imageProxy &&
        !g_unloading.load(std::memory_order_acquire))
    {
        g_observedDrawBitmapCreateCount.fetch_add(1,
                                                   std::memory_order_relaxed);
        if (ObservedBitmapInstruction* entry = FindObservedImageEntry(
                g_observedBitmapInstructions, *instruction, true))
        {
            entry->imageProxy.store(imageProxy, std::memory_order_release);
        }
    }
    return result;
}

static long __cdecl DrawTileImageInstructionCreateHook(
    void* imageProxy, const RECT& sourceRect, const POINT& destinationOffset,
    float opacity, void** instruction)
{
    long result = g_drawTileImageInstructionCreateOriginal(
        imageProxy, sourceRect, destinationOffset, opacity, instruction);
    if (result >= 0 && instruction && *instruction && imageProxy &&
        !g_unloading.load(std::memory_order_acquire))
    {
        g_observedDrawTileCreateCount.fetch_add(1,
                                                 std::memory_order_relaxed);
        if (ObservedBitmapInstruction* entry = FindObservedImageEntry(
                g_observedBitmapInstructions, *instruction, true))
        {
            entry->imageProxy.store(imageProxy, std::memory_order_release);
        }
    }
    return result;
}

static long __cdecl RenderDataVisualAddInstructionHook(void* pThis,
                                                        void* instruction)
{
    ObservedBitmapInstruction* source =
        pThis && instruction
            ? FindObservedImageEntry(g_observedBitmapInstructions,
                                     instruction, false)
            : nullptr;
    void* imageProxy = source
                           ? source->imageProxy.load(std::memory_order_acquire)
                           : nullptr;
    HWND ownerHwnd = nullptr;
    void* ownerWindowData = nullptr;
    long result = E_NOINTERFACE;
    if (imageProxy && !g_unloading.load(std::memory_order_acquire))
    {
        ownerWindowData = RegisterAnimationTopLevelWindow3D(pThis, &ownerHwnd);
    }
    // Publish the original first. The guarded one-shot canary below only
    // replaces its proven list slot after the insertion layout is validated.
    result = g_renderDataVisualAddInstruction(pThis, instruction);
    if (result >= 0 && imageProxy &&
        !g_unloading.load(std::memory_order_acquire))
    {
        g_observedImageInstructionMatchedAddCount.fetch_add(
            1, std::memory_order_relaxed);
        if (ObservedRenderImage* entry = FindObservedImageEntry(
                g_observedRenderImages, pThis, true))
        {
            unsigned int capturedInstructionCount = 0;
            int capturedInstructionIndex = -1;
            FindRenderDataInstructionIndex(
                pThis, instruction, &capturedInstructionCount,
                &capturedInstructionIndex);
            void* imageVtable = nullptr;
            if (IsReadableMemory(imageProxy, sizeof(void*)))
            {
                void* candidate = *reinterpret_cast<void**>(imageProxy);
                if (IsDwmImageAddress(candidate, sizeof(void*)))
                {
                    imageVtable = candidate;
                }
            }
            entry->instruction.store(instruction, std::memory_order_relaxed);
            entry->imageVtable.store(imageVtable, std::memory_order_relaxed);
            entry->ownerWindowData.store(ownerWindowData,
                                         std::memory_order_relaxed);
            entry->ownerHwnd.store(ownerHwnd, std::memory_order_relaxed);
            entry->sourceKind.store(
                static_cast<int>(GetMeshSourceKind(imageProxy)),
                std::memory_order_relaxed);
            entry->capturedInstructionCount.store(
                capturedInstructionCount, std::memory_order_relaxed);
            entry->capturedInstructionIndex.store(
                capturedInstructionIndex, std::memory_order_relaxed);
            entry->imageProxy.store(imageProxy, std::memory_order_release);
        }
    }
    return result;
}

static long __cdecl RenderDataVisualUpdateRenderDataHook(void* pThis)
{
    HWND probeHwnd = nullptr;
    void* probeWindowData = nullptr;
    bool probeTarget = false;
    NativeRenderCallSnapshot before = {};
    if (pThis && !g_unloading.load(std::memory_order_acquire))
    {
        probeWindowData =
            RegisterAnimationTopLevelWindow3D(pThis, &probeHwnd);
        probeTarget = probeWindowData && probeHwnd &&
                      GetHwndFromWindowData(probeWindowData) == probeHwnd &&
                      g_liveBaseImageMeshTargetHwnd.load(
                          std::memory_order_acquire) == probeHwnd;
        if (probeTarget)
        {
            before = CaptureNativeRenderCallSnapshot(pThis);
        }
    }
    bool prepared = false;
    bool substituted = false;
    bool restored = false;
    void* meshInstruction = nullptr;
    if (NATIVE_MESH_TRANSACTION_PROBE_ENABLED && probeTarget &&
        before.listValid && before.unique && before.instruction &&
        before.imageProxy && before.instructionIndex >= 0 &&
        g_nativeMeshCanarySucceeded.load(std::memory_order_acquire))
    {
        if (!g_visibleMeshCanaryActive.load(std::memory_order_acquire))
        {
            prepared = TryInstallLiveBaseImageMeshCanary(
                pThis, before.instruction, before.imageProxy,
                probeWindowData, probeHwnd);
        }
        if (g_visibleMeshCanaryActive.load(std::memory_order_acquire) &&
            g_visibleMeshCanary.hwnd == probeHwnd &&
            g_visibleMeshCanary.pinnedImageProxy == before.imageProxy)
        {
            meshInstruction = g_visibleMeshCanary.instruction;
            if (meshInstruction &&
                IsWritableMemory(before.instructionArray +
                                     before.instructionIndex,
                                 sizeof(void*)) &&
                before.instructionArray[before.instructionIndex] ==
                    before.instruction)
            {
                before.instructionArray[before.instructionIndex] =
                    meshInstruction;
                substituted = true;
            }
        }
        else if (g_visibleMeshCanaryActive.load(std::memory_order_acquire) &&
                 g_visibleMeshCanary.hwnd == probeHwnd)
        {
            g_visibleMeshCanaryCleanupRequested.store(
                true, std::memory_order_release);
            RequestDwmScenePass();
        }
    }
    long result = g_renderDataVisualUpdateRenderData(pThis);
    if (substituted)
    {
        void** currentInstructions = nullptr;
        int* currentCountAddress = nullptr;
        int currentCount = 0;
        if (GetRenderDataInstructionList(
                pThis, &currentInstructions, &currentCountAddress,
                &currentCount) &&
            currentInstructions == before.instructionArray &&
            currentCountAddress == before.countAddress &&
            currentCount == before.instructionCount &&
            before.instructionIndex < currentCount &&
            currentInstructions[before.instructionIndex] == meshInstruction &&
            IsWritableMemory(currentInstructions + before.instructionIndex,
                             sizeof(void*)))
        {
            currentInstructions[before.instructionIndex] = before.instruction;
            restored = true;
        }
        if (!restored)
        {
            g_visibleMeshCanaryCleanupRequested.store(
                true, std::memory_order_release);
            RequestDwmScenePass();
        }
        Wh_Log(L"True 4x4 transactional publish: HWND=%p index=%d/%d "
               L"prepared=%d substituted=1 restored=%d result=0x%08X",
               probeHwnd, before.instructionIndex, before.instructionCount,
               prepared, restored, static_cast<unsigned int>(result));
    }
    if (probeTarget)
    {
        NativeRenderCallSnapshot after =
            CaptureNativeRenderCallSnapshot(pThis);
        bool changed =
            before.instructionArray != after.instructionArray ||
            before.countAddress != after.countAddress ||
            before.instruction != after.instruction ||
            before.imageProxy != after.imageProxy ||
            before.fingerprint != after.fingerprint ||
            before.instructionIndex != after.instructionIndex ||
            before.instructionCount != after.instructionCount ||
            before.listValid != after.listValid ||
            before.unique != after.unique;
        unsigned int sample = g_nativePublishProbeSamples.fetch_add(
                                  1, std::memory_order_relaxed) +
                              1;
        unsigned int changes = changed
                                   ? g_nativePublishProbeChanges.fetch_add(
                                         1, std::memory_order_relaxed) +
                                         1
                                   : g_nativePublishProbeChanges.load(
                                         std::memory_order_relaxed);
        bool missing = !before.listValid || !before.unique ||
                       !after.listValid || !after.unique;
        unsigned int missingCount = missing
                                        ? g_nativePublishProbeMissing.fetch_add(
                                              1, std::memory_order_relaxed) +
                                              1
                                        : g_nativePublishProbeMissing.load(
                                              std::memory_order_relaxed);
        if (sample <= 8 || changed || sample % 64 == 0)
        {
            Wh_Log(L"True 4x4 native publish-call stability: sample=%u "
                   L"HWND=%p result=0x%08X changed=%d changes=%u "
                   L"missing=%u before={list=%p countField=%p index=%d/%d "
                   L"instruction=%p image=%p fingerprint=0x%llX valid=%d "
                   L"unique=%d candidates=%u} after={list=%p countField=%p "
                   L"index=%d/%d instruction=%p image=%p fingerprint=0x%llX "
                   L"valid=%d unique=%d candidates=%u} (read-only)",
                   sample, probeHwnd, static_cast<unsigned int>(result),
                   changed, changes, missingCount, before.instructionArray,
                   before.countAddress, before.instructionIndex,
                   before.instructionCount, before.instruction,
                   before.imageProxy,
                   static_cast<unsigned long long>(before.fingerprint),
                   before.listValid, before.unique, before.candidateCount,
                   after.instructionArray, after.countAddress,
                   after.instructionIndex, after.instructionCount,
                   after.instruction, after.imageProxy,
                   static_cast<unsigned long long>(after.fingerprint),
                   after.listValid, after.unique, after.candidateCount);
        }
    }
    return result;
}

static void* FindTopLevelWindowForCompleteRoot(void* root, void** windowData,
                                                HWND* hwnd)
{
    if (windowData)
    {
        *windowData = nullptr;
    }
    if (hwnd)
    {
        *hwnd = nullptr;
    }
    if (!root || !g_topLevelWindowGetRootVisual)
    {
        return nullptr;
    }

    struct Candidate
    {
        void* windowData;
        void* topLevelWindow;
    };
    Candidate candidates[MAX_DWM_WINDOW_MAPPINGS] = {};
    unsigned int candidateCount = 0;
    AcquireSRWLockShared(&g_dwmWindowMappingsLock);
    for (const DwmWindowObjectMapping& mapping : g_dwmWindowMappings)
    {
        if (mapping.windowData && mapping.topLevelWindow &&
            candidateCount < ARRAYSIZE(candidates))
        {
            candidates[candidateCount++] =
                {mapping.windowData, mapping.topLevelWindow};
        }
    }
    ReleaseSRWLockShared(&g_dwmWindowMappingsLock);

    constexpr int completeWindowRoot = 0;
    for (unsigned int index = 0; index < candidateCount; index++)
    {
        const Candidate& candidate = candidates[index];
        if (!IsTopLevelWindowForWindowData(candidate.topLevelWindow,
                                            candidate.windowData) ||
            g_topLevelWindowGetRootVisual(candidate.topLevelWindow,
                                           completeWindowRoot) != root)
        {
            continue;
        }
        HWND candidateHwnd = GetHwndFromWindowData(candidate.windowData);
        if (!candidateHwnd)
        {
            return nullptr;
        }
        if (windowData)
        {
            *windowData = candidate.windowData;
        }
        if (hwnd)
        {
            *hwnd = candidateHwnd;
        }
        return candidate.topLevelWindow;
    }
    return nullptr;
}

static long __cdecl CWindowBorderCloneVisualTreeHook(void* pThis,
                                                      void** clonedVisual,
                                                      int cloneOptions)
{
    long result = g_windowBorderCloneVisualTreeOriginal(
        pThis, clonedVisual, cloneOptions);
    if (g_unloading.load(std::memory_order_acquire) || !pThis)
    {
        return result;
    }

    void* windowData = nullptr;
    HWND hwnd = nullptr;
    void* topLevelWindow =
        FindTopLevelWindowForCompleteRoot(pThis, &windowData, &hwnd);
    if (!topLevelWindow)
    {
        return result;
    }

    void* clone = result >= 0 && clonedVisual ? *clonedVisual : nullptr;
    void* sourceProxy = ReadPointerMember(pThis, g_visualProxyOffset);
    void* cloneProxy = ReadPointerMember(clone, g_visualProxyOffset);
    void* sourceParent = ReadPointerMember(pThis, g_visualParentOffset);
    void* cloneParent = ReadPointerMember(clone, g_visualParentOffset);
    void* sourceVtable =
        IsReadableMemory(pThis, sizeof(void*))
            ? *reinterpret_cast<void**>(pThis)
            : nullptr;
    void* cloneVtable =
        IsReadableMemory(clone, sizeof(void*))
            ? *reinterpret_cast<void**>(clone)
            : nullptr;
    unsigned int callNumber =
        g_windowBorderCloneCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
    DWORD sceneThreadId = g_dwmSceneThreadId.load(std::memory_order_acquire);
    HWND dragHwnd = g_realDraggedWindow.load(std::memory_order_acquire);
    HWND meshTarget =
        g_liveBaseImageMeshTargetHwnd.load(std::memory_order_acquire);
    Wh_Log(L"True 4x4 complete-root clone observed: call=%u result=0x%08X "
           L"HWND=%p WindowData=%p CTopLevelWindow=%p options=%d "
           L"source=%p sourceVtable=%p sourceParent=%p sourceProxy=%p "
           L"clone=%p cloneVtable=%p cloneParent=%p cloneProxy=%p "
           L"sceneThread=%d activeDrag=%d meshTarget=%d (read-only)",
           callNumber, static_cast<unsigned int>(result), hwnd, windowData,
           topLevelWindow, cloneOptions, pThis, sourceVtable, sourceParent,
           sourceProxy, clone, cloneVtable, cloneParent, cloneProxy,
           sceneThreadId != 0 && sceneThreadId == GetCurrentThreadId(),
           dragHwnd == hwnd, meshTarget == hwnd);
    return result;
}

static long __cdecl CTopLevelWindowCloneVisualTreeForLivePreviewHook(
    void* pThis, bool includeRenderData, void** clonedTopLevelWindow)
{
    long result = g_topLevelWindowCloneVisualTreeForLivePreviewOriginal(
        pThis, includeRenderData, clonedTopLevelWindow);
    if (g_unloading.load(std::memory_order_acquire) || !pThis)
    {
        return result;
    }

    void* clone = result >= 0 && clonedTopLevelWindow
                      ? *clonedTopLevelWindow
                      : nullptr;
    bool sourceValid =
        IsDwmObjectPointerValid(pThis, g_topLevelWindowVtable);
    bool cloneValid =
        IsDwmObjectPointerValid(clone, g_topLevelWindowVtable);
    void* sourceWindowData =
        sourceValid && g_topLevelWindowGetWindowData
            ? g_topLevelWindowGetWindowData(pThis)
            : nullptr;
    void* cloneWindowData =
        cloneValid && g_topLevelWindowGetWindowData
            ? g_topLevelWindowGetWindowData(clone)
            : nullptr;
    HWND sourceHwnd = GetHwndFromWindowData(sourceWindowData);
    HWND cloneHwnd = GetHwndFromWindowData(cloneWindowData);

    constexpr int completeWindowRoot = 0;
    void* sourceRoot =
        sourceValid && g_topLevelWindowGetRootVisual
            ? g_topLevelWindowGetRootVisual(pThis, completeWindowRoot)
            : nullptr;
    void* cloneRoot =
        cloneValid && g_topLevelWindowGetRootVisual
            ? g_topLevelWindowGetRootVisual(clone, completeWindowRoot)
            : nullptr;
    void* sourceProxy = ReadPointerMember(sourceRoot, g_visualProxyOffset);
    void* cloneProxy = ReadPointerMember(cloneRoot, g_visualProxyOffset);
    unsigned int callNumber =
        g_livePreviewCloneCallCount.fetch_add(1,
                                               std::memory_order_relaxed) +
        1;
    HWND dragHwnd = g_realDraggedWindow.load(std::memory_order_acquire);
    HWND meshTarget =
        g_liveBaseImageMeshTargetHwnd.load(std::memory_order_acquire);
    DWORD sceneThreadId = g_dwmSceneThreadId.load(std::memory_order_acquire);
    Wh_Log(L"True 4x4 live-preview clone observed: call=%u result=0x%08X "
           L"includeRenderData=%d source=%p sourceValid=%d "
           L"sourceWindowData=%p sourceHWND=%p sourceRoot=%p "
           L"sourceProxy=%p clone=%p cloneValid=%d cloneWindowData=%p "
           L"cloneHWND=%p cloneRoot=%p cloneProxy=%p sameWindowData=%d "
           L"sceneThread=%d activeDrag=%d meshTarget=%d (read-only)",
           callNumber, static_cast<unsigned int>(result), includeRenderData,
           pThis, sourceValid, sourceWindowData, sourceHwnd, sourceRoot,
           sourceProxy, clone, cloneValid, cloneWindowData, cloneHwnd,
           cloneRoot, cloneProxy,
           sourceWindowData && sourceWindowData == cloneWindowData,
           sceneThreadId != 0 && sceneThreadId == GetCurrentThreadId(),
           sourceHwnd && dragHwnd == sourceHwnd,
           sourceHwnd && meshTarget == sourceHwnd);
    return result;
}

static void LogTopLevelWindow3DRepresentationState(
    const wchar_t* eventName, unsigned int callNumber, void* pThis,
    long result, int argument1, int argument2, void* requestedParent,
    void* parentBefore)
{
    HWND hwnd = nullptr;
    void* windowData = RegisterAnimationTopLevelWindow3D(pThis, &hwnd);
    if (!windowData || !hwnd)
    {
        return;
    }
    int instructionCount = 0;
    GetRenderDataInstructionList(pThis, nullptr, nullptr,
                                 &instructionCount);
    void* parentAfter = ReadPointerMember(pThis, g_visualParentOffset);
    void* transitionProxy = GetTransitionVisualProxy(pThis);
    void* sourceImage =
        g_ensureRenderDataPointerOffsetCount > 0
            ? ReadPointerMember(pThis, g_ensureRenderDataPointerOffsets[0])
            : nullptr;
    HWND dragHwnd = g_realDraggedWindow.load(std::memory_order_acquire);
    ObserveNativeRenderSlotStability(pThis, windowData, hwnd, eventName);
    Wh_Log(L"True 4x4 secondary representation: event=%s call=%u "
           L"result=0x%08X object=%p WindowData=%p HWND=%p arg1=%d "
           L"arg2=%d requestedParent=%p parentBefore=%p parentAfter=%p "
           L"transitionProxy=%p sourceImage=%p instructions=%d "
           L"activeDrag=%d (read-only)",
           eventName, callNumber, static_cast<unsigned int>(result), pThis,
           windowData, hwnd, argument1, argument2, requestedParent,
           parentBefore, parentAfter, transitionProxy, sourceImage,
           instructionCount, dragHwnd == hwnd);
}

static long __cdecl TopLevelWindow3DEnsureSecondaryWindowRepresentationHook(
    void* pThis, bool forceRecreate)
{
    long result = g_topLevelWindow3DEnsureSecondaryWindowRepresentationOriginal(
        pThis, forceRecreate);
    if (!g_unloading.load(std::memory_order_acquire))
    {
        unsigned int callNumber = g_secondaryRepresentationCallCount.fetch_add(
                                      1, std::memory_order_relaxed) +
                                  1;
        LogTopLevelWindow3DRepresentationState(
            L"EnsureSecondary", callNumber, pThis, result, forceRecreate, 0,
            nullptr, nullptr);
    }
    return result;
}

static long __cdecl TopLevelWindow3DSetParentHook(void* pThis, void* parent)
{
    void* parentBefore = ReadPointerMember(pThis, g_visualParentOffset);
    long result = g_topLevelWindow3DSetParentOriginal(pThis, parent);
    if (!g_unloading.load(std::memory_order_acquire))
    {
        unsigned int callNumber =
            g_topLevelWindow3DSetParentCallCount.fetch_add(
                1, std::memory_order_relaxed) +
            1;
        LogTopLevelWindow3DRepresentationState(
            L"SetParent", callNumber, pThis, result, 0, 0, parent,
            parentBefore);
    }
    return result;
}

static long __cdecl TopLevelWindow3DShowWindowHook(void* pThis, bool show,
                                                   bool activate)
{
    void* parentBefore = ReadPointerMember(pThis, g_visualParentOffset);
    long result =
        g_topLevelWindow3DShowWindowOriginal(pThis, show, activate);
    if (!g_unloading.load(std::memory_order_acquire))
    {
        unsigned int callNumber =
            g_topLevelWindow3DShowWindowCallCount.fetch_add(
                1, std::memory_order_relaxed) +
            1;
        LogTopLevelWindow3DRepresentationState(
            L"ShowWindow", callNumber, pThis, result, show, activate, nullptr,
            parentBefore);
    }
    return result;
}

static bool FindTrackedTopLevelVisual(void* visual, void** windowData,
                                      HWND* hwnd, const wchar_t** kind)
{
    *windowData = nullptr;
    *hwnd = nullptr;
    *kind = L"None";
    if (!visual)
    {
        return false;
    }
    AcquireSRWLockShared(&g_dwmWindowMappingsLock);
    for (const DwmWindowObjectMapping& mapping : g_dwmWindowMappings)
    {
        if (mapping.topLevelWindow == visual)
        {
            *windowData = mapping.windowData;
            *kind = L"CTopLevelWindow";
            break;
        }
        if (mapping.topLevelWindow3D == visual)
        {
            *windowData = mapping.windowData;
            *kind = L"CTopLevelWindow3D";
            break;
        }
    }
    ReleaseSRWLockShared(&g_dwmWindowMappingsLock);
    *hwnd = GetHwndFromWindowData(*windowData);
    return *hwnd != nullptr;
}

static unsigned int ReadVisualFlags(void* visual)
{
    BYTE* flags = visual ? static_cast<BYTE*>(visual) + 0x5c : nullptr;
    return IsReadableMemory(flags, sizeof(*flags)) ? *flags : UINT_MAX;
}

static void LogTrackedVisualState(const wchar_t* eventName, void* visual,
                                  double opacity, unsigned int flagsBefore,
                                  unsigned int flagsAfter)
{
    void* windowData = nullptr;
    HWND hwnd = nullptr;
    const wchar_t* kind = nullptr;
    if (!FindTrackedTopLevelVisual(visual, &windowData, &hwnd, &kind))
    {
        return;
    }
    unsigned int callNumber =
        g_trackedVisualVisibilityCallCount.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    void* parent = ReadPointerMember(visual, g_visualParentOffset);
    void* proxy = ReadPointerMember(visual, g_visualProxyOffset);
    Wh_Log(L"True 4x4 tracked visual state: event=%s call=%u kind=%s "
           L"visual=%p WindowData=%p HWND=%p opacity=%.3f "
           L"flagsBefore=0x%02X flagsAfter=0x%02X parent=%p proxy=%p "
           L"(read-only observation)",
           eventName, callNumber, kind, visual, windowData, hwnd, opacity,
           flagsBefore, flagsAfter, parent, proxy);
}

static void __cdecl VisualHideHook(void* pThis)
{
    unsigned int flagsBefore = ReadVisualFlags(pThis);
    g_visualHideOriginal(pThis);
    LogTrackedVisualState(L"Hide", pThis, -1.0, flagsBefore,
                          ReadVisualFlags(pThis));
}

static void __cdecl VisualUnhideHook(void* pThis)
{
    unsigned int flagsBefore = ReadVisualFlags(pThis);
    g_visualUnhideOriginal(pThis);
    LogTrackedVisualState(L"Unhide", pThis, -1.0, flagsBefore,
                          ReadVisualFlags(pThis));
}

static void __cdecl VisualSetOpacityHook(void* pThis, double opacity)
{
    unsigned int flagsBefore = ReadVisualFlags(pThis);
    g_visualSetOpacityOriginal(pThis, opacity);
    LogTrackedVisualState(L"SetOpacity", pThis, opacity, flagsBefore,
                          ReadVisualFlags(pThis));
}

static long __cdecl TopLevelWindow3DEnsureRenderDataHook(void* pThis)
{
    long result = g_topLevelWindow3DEnsureRenderDataOriginal(pThis);
    unsigned int callNumber =
        g_ensureRenderDataCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (result < 0 || !pThis ||
        g_unloading.load(std::memory_order_acquire))
    {
        return result;
    }

    HWND hwnd = nullptr;
    void* windowData = RegisterAnimationTopLevelWindow3D(pThis, &hwnd);
    void** observedInstructions = nullptr;
    int observedInstructionCount = 0;
    GetRenderDataInstructionList(pThis, &observedInstructions, nullptr,
                                 &observedInstructionCount);
    if (observedInstructionCount > 0)
    {
        g_ensureRenderDataPopulatedCount.fetch_add(1,
                                                   std::memory_order_relaxed);
    }
    if (windowData && hwnd)
    {
        g_ensureRenderDataMappedCount.fetch_add(1,
                                                std::memory_order_relaxed);
    }
    if (callNumber <= 12)
    {
        Wh_Log(L"True 4x4 EnsureRenderData observed: call=%u result=0x%08X "
               L"object=%p WindowData=%p HWND=%p instructions=%d",
               callNumber, static_cast<unsigned int>(result), pThis,
               windowData, hwnd, observedInstructionCount);
    }
    if (!windowData || !hwnd)
    {
        return result;
    }
    if (ObservedRenderImage* entry = FindObservedImageEntry(
            g_observedRenderImages, pThis, true))
    {
        entry->ownerWindowData.store(windowData, std::memory_order_relaxed);
        entry->ownerHwnd.store(hwnd, std::memory_order_release);
    }
    ObserveNativeRenderSlotStability(pThis, windowData, hwnd,
                                     L"EnsureRenderData");

    if (!NATIVE_MESH_WRITE_PROBE_ENABLED ||
        g_liveBaseImageMeshTargetHwnd.load(std::memory_order_acquire) != hwnd ||
        g_visibleMeshCanaryActive.load(std::memory_order_acquire) ||
        !g_nativeMeshCanarySucceeded.load(std::memory_order_acquire))
    {
        return result;
    }

    void* instruction = nullptr;
    void* imageProxy = nullptr;
    int instructionIndex = -1;
    int instructionCount = 0;
    unsigned int candidateCount = 0;
    if (FindUniqueBaseImageInstruction(
            pThis, 0, &instruction, &imageProxy, &instructionIndex,
            &instructionCount, &candidateCount) &&
        TryInstallLiveBaseImageMeshCanary(
            pThis, instruction, imageProxy, windowData, hwnd))
    {
        Wh_Log(L"True 4x4 native EnsureRenderData boundary: installed "
               L"HWND=%p index=%d/%d candidates=%u",
               hwnd, instructionIndex, instructionCount, candidateCount);
    }
    return result;
}

static bool IsObservedMeshSource(void* content)
{
    if (!content || !IsReadableMemory(content, sizeof(void*)))
    {
        return false;
    }
    void* vtable = *reinterpret_cast<void**>(content);
    return vtable == g_bitmapSourceProxyVtable.load(std::memory_order_acquire) ||
           vtable == g_visualSurfaceProxyVtable.load(std::memory_order_acquire);
}

static long __cdecl VisualProxySetContentHook(void* pThis, const void* content)
{
    long result = g_visualProxySetContentOriginal(pThis, content);
    if (result >= 0 && !g_unloading.load(std::memory_order_acquire))
    {
        if (ObservedVisualProxy* entry = FindObservedVisualProxy(pThis, true))
        {
            entry->content.store(const_cast<void*>(content), std::memory_order_release);
        }
        if (IsObservedMeshSource(const_cast<void*>(content)))
        {
            g_meshSourceProbePending.store(true, std::memory_order_release);
        }
    }
    return result;
}

static long __cdecl VisualProxyInsertChildHook(void* pThis, void* child,
                                               void* reference, bool insertAbove)
{
    long result =
        g_visualProxyInsertChildOriginal(pThis, child, reference, insertAbove);
    if (result >= 0 && child && !g_unloading.load(std::memory_order_acquire))
    {
        if (ObservedVisualProxy* entry = FindObservedVisualProxy(child, true))
        {
            entry->parent.store(pThis, std::memory_order_release);
        }
        FindObservedVisualProxy(pThis, true);
    }
    return result;
}

static long __cdecl VisualProxyRemoveChildHook(void* pThis, void* child)
{
    long result = g_visualProxyRemoveChildOriginal(pThis, child);
    if (result >= 0 && child)
    {
        if (ObservedVisualProxy* entry = FindObservedVisualProxy(child, false))
        {
            void* expected = pThis;
            entry->parent.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel,
                std::memory_order_acquire);
        }
    }
    return result;
}

static long __cdecl VisualSetContentHook(void* pThis, void* content)
{
    long result = g_visualSetContentOriginal(pThis, content);
    if (result >= 0 && !g_unloading.load(std::memory_order_acquire))
    {
        if (ObservedVisualProxy* entry = FindObservedVisual(pThis, true))
        {
            entry->content.store(content, std::memory_order_release);
        }
        if (IsObservedMeshSource(content))
        {
            g_meshSourceProbePending.store(true, std::memory_order_release);
        }
    }
    return result;
}

static long __cdecl VisualSetParentHook(void* pThis, void* parent)
{
    long result = g_visualSetParentOriginal(pThis, parent);
    if (result >= 0 && !g_unloading.load(std::memory_order_acquire))
    {
        if (ObservedVisualProxy* entry = FindObservedVisual(pThis, true))
        {
            entry->parent.store(parent, std::memory_order_release);
        }
        FindObservedVisual(parent, true);
    }
    return result;
}

static long __cdecl VisualRemoveSelfFromParentHook(void* pThis)
{
    long result = g_visualRemoveSelfFromParentOriginal(pThis);
    if (result >= 0)
    {
        if (ObservedVisualProxy* entry = FindObservedVisual(pThis, false))
        {
            entry->parent.store(nullptr, std::memory_order_release);
        }
    }
    return result;
}

static long __cdecl RedirectVisualProxySetRedirectedVisualHook(void* pThis,
                                                               void* visual)
{
    long result =
        g_redirectVisualProxySetRedirectedVisualOriginal(pThis, visual);
    if (result >= 0 && !g_unloading.load(std::memory_order_acquire))
    {
        if (ObservedVisualProxy* entry = FindObservedVisualProxy(pThis, true))
        {
            entry->redirectTarget.store(visual, std::memory_order_release);
        }
        g_meshSourceProbePending.store(true, std::memory_order_release);
    }
    return result;
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
        {{L"public: virtual long __cdecl CWindowBorder::CloneVisualTree("
           L"class CVisual * *,enum CloneOptions)"},
         &g_windowBorderCloneVisualTreeOriginal,
         CWindowBorderCloneVisualTreeHook,
         true},
        {{L"public: long __cdecl "
           L"CTopLevelWindow::CloneVisualTreeForLivePreview("
           L"bool,class CTopLevelWindow * *)"},
         &g_topLevelWindowCloneVisualTreeForLivePreviewOriginal,
         CTopLevelWindowCloneVisualTreeForLivePreviewHook,
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
        {{L"public: long __cdecl CMeshGeometry2dProxy::Update("
           L"int,struct D2D_POINT_3F const *,struct D2D_POINT_2F const *,"
           L"unsigned int,unsigned int const *,unsigned int)",
          L"public: long __cdecl CMeshGeometry2dProxy::Update("
           L"int,struct MilPoint3F const *,struct MilPoint2D const *,"
           L"unsigned int,unsigned int const *,unsigned int)"},
         &g_meshGeometry2dProxyUpdate,
         nullptr,
         true},
        {{L"public: long __cdecl CCompositor::CreateMeshGeometry2dProxy("
           L"class CMeshGeometry2dProxy * *)",
          L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CMeshGeometry2dProxy>(class CMeshGeometry2dProxy * *)"},
         &g_createMeshGeometry2dProxy,
         nullptr,
         true},
        {{L"public: long __cdecl CCompositor::CreateGeometry2dGroupProxy("
           L"class CGeometry2dGroupProxy * *)",
          L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CGeometry2dGroupProxy>(class CGeometry2dGroupProxy * *)"},
         &g_createGeometry2dGroupProxy,
         nullptr,
         true},
        {{L"public: long __cdecl CCompositor::CreateBitmapSourceProxy("
           L"class CBitmapSourceProxy * *)",
          L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CBitmapSourceProxy>(class CBitmapSourceProxy * *)"},
         &g_createBitmapSourceProxyOriginal,
         CreateBitmapSourceProxyHook,
         true},
        {{L"public: long __cdecl "
           L"CCompositor::CreateVisualSurfaceProxyFromSharedHandle("
           L"void *,class CVisualSurfaceProxy * *)",
          L"protected: long __cdecl CCompositor::CreateProxyFromSharedHandle<"
           L"class CVisualSurfaceProxy>(void *,"
           L"class CVisualSurfaceProxy * *)"},
         &g_createVisualSurfaceProxyOriginal,
         CreateVisualSurfaceProxyHook,
         true},
        {{L"protected: long __cdecl CCompositor::CreateProxy<"
           L"class CCachedVisualImageProxy>("
           L"class CCachedVisualImageProxy * *)"},
         &g_createCachedVisualImageProxy,
         nullptr,
         true},
        {{L"public: long __cdecl CCachedVisualImageProxy::Update("
           L"struct MilRectF const &,struct MilSizeD const &,"
           L"class CRectResourceProxy const *,"
           L"class CSizeResourceProxy const *,class CVisualProxy *,"
           L"enum MilBrushMappingMode::Enum)"},
         &g_cachedVisualImageProxyUpdate,
         nullptr,
         true},
        {{L"public: long __cdecl CCachedVisualImageProxy::Snapshot("
           L"struct tagRECT const &)"},
         &g_cachedVisualImageProxySnapshot,
         nullptr,
         true},
        {{L"public: long __cdecl CCachedVisualImageProxy::Freeze(void)"},
         &g_cachedVisualImageProxyFreeze,
         nullptr,
         true},
        {{L"public: long __cdecl CGeometry2dGroupProxy::Update("
           L"class CMeshGeometry2dProxy const *)"},
         &g_geometry2dGroupProxyUpdate,
         nullptr,
         true},
        {{L"public: static long __cdecl CDrawMesh2DInstruction::Create("
           L"class CGeometry2dGroupProxy *,class CBitmapSourceProxy *,"
           L"class CDrawMesh2DInstruction * *)"},
         &g_drawMesh2DInstructionCreate,
         nullptr,
         true},
        {{L"long __cdecl CreateTouchVisual<class CTouchDragVisual>("
           L"unsigned __int64,class CTouchDragVisual * *)"},
         &g_createTouchDragVisualFunction,
         nullptr,
         true},
        {{L"public: long __cdecl CTouchDragVisual::NotifyTouchDrag("
           L"struct tagPOINT const *)"},
         &g_touchDragVisualNotifyFunction,
         nullptr,
         true},
        {{L"public: virtual void __cdecl CTouchDragVisual::Stop(void)"},
         &g_touchDragVisualStopFunction,
         nullptr,
         true},
        {{L"private: long __cdecl "
           L"CTouchDragVisual::CreateDrawMesh2DInstruction("
           L"struct Mesh2D const *,class CGeometry2dGroupProxy * *,"
           L"class CMeshGeometry2dProxy * *)"},
         &g_touchDragVisualCreateMeshInstructionFunction,
         nullptr,
         true},
        {{L"public: static long __cdecl CDrawBitmapInstruction::Create("
           L"class CBaseImageProxy *,class CDrawBitmapInstruction * *)"},
         &g_drawBitmapInstructionCreateOriginal,
         DrawBitmapInstructionCreateHook,
         true},
        {{L"public: static long __cdecl CDrawTileImageInstruction::Create("
           L"class CBaseImageProxy *,struct tagRECT const &,"
           L"struct tagPOINT const &,float,"
           L"class CDrawTileImageInstruction * *)"},
         &g_drawTileImageInstructionCreateOriginal,
         DrawTileImageInstructionCreateHook,
         true},
        {{L"public: long __cdecl CRenderDataVisual::AddInstruction("
           L"class CRenderDataInstruction *)"},
         &g_renderDataVisualAddInstruction,
         RenderDataVisualAddInstructionHook,
         true},
        {{L"public: long __cdecl CRenderDataVisual::ClearInstructions(void)"},
         &g_renderDataVisualClearInstructions,
         nullptr,
         true},
        {{L"public: virtual long __cdecl "
           L"CRenderDataVisual::UpdateRenderData(void)"},
         &g_renderDataVisualUpdateRenderData,
         RenderDataVisualUpdateRenderDataHook,
         true},
        {{L"private: long __cdecl CTopLevelWindow3D::EnsureRenderData(void)",
          L"protected: long __cdecl CTopLevelWindow3D::EnsureRenderData(void)",
          L"public: long __cdecl CTopLevelWindow3D::EnsureRenderData(void)"},
         &g_topLevelWindow3DEnsureRenderDataOriginal,
         TopLevelWindow3DEnsureRenderDataHook,
         true},
        {{L"public: long __cdecl "
           L"CTopLevelWindow3D::EnsureSecondaryWindowRepresentation(bool)"},
         &g_topLevelWindow3DEnsureSecondaryWindowRepresentationOriginal,
         TopLevelWindow3DEnsureSecondaryWindowRepresentationHook,
         true},
        {{L"public: virtual long __cdecl "
           L"CTopLevelWindow3D::SetParent(class CVisual *)"},
         &g_topLevelWindow3DSetParentOriginal,
         TopLevelWindow3DSetParentHook,
         true},
        {{L"public: long __cdecl CTopLevelWindow3D::ShowWindow(bool,bool)"},
         &g_topLevelWindow3DShowWindowOriginal,
         TopLevelWindow3DShowWindowHook,
         true},
        {{L"public: static long __cdecl CRenderDataVisual::Create("
           L"class CRenderDataVisual * *)"},
         &g_renderDataVisualCreate,
         nullptr,
         true},
        {{L"public: long __cdecl CVisualProxy::SetContent("
           L"class CResourceProxy const *)"},
         &g_visualProxySetContentOriginal,
         VisualProxySetContentHook,
         true},
        {{L"public: long __cdecl CVisualProxy::InsertChild("
           L"class CVisualProxy *,class CVisualProxy *,bool)"},
         &g_visualProxyInsertChildOriginal,
         VisualProxyInsertChildHook,
         true},
        {{L"public: long __cdecl CVisualProxy::RemoveChild("
           L"class CVisualProxy *)"},
         &g_visualProxyRemoveChildOriginal,
         VisualProxyRemoveChildHook,
         true},
        {{L"public: long __cdecl CRedirectVisualProxy::SetRedirectedVisual("
           L"class CVisualProxy *)"},
         &g_redirectVisualProxySetRedirectedVisualOriginal,
         RedirectVisualProxySetRedirectedVisualHook,
         true},
        {{L"public: virtual long __cdecl CVisual::SetContent("
           L"class CResourceProxy *)"},
         &g_visualSetContentOriginal,
         VisualSetContentHook,
         true},
        {{L"public: virtual long __cdecl CVisual::SetParent(class CVisual *)",
          L"public: virtual long __cdecl CVisual::SetParent("
           L"class CContainerVisual *)"},
         &g_visualSetParentOriginal,
         VisualSetParentHook,
         true},
        {{L"public: long __cdecl CVisual::RemoveSelfFromParent(void)"},
         &g_visualRemoveSelfFromParentOriginal,
         VisualRemoveSelfFromParentHook,
         true},
        {{L"public: void __cdecl CVisual::Hide(void)"},
         &g_visualHideOriginal,
         VisualHideHook,
         true},
        {{L"public: void __cdecl CVisual::Unhide(void)"},
         &g_visualUnhideOriginal,
         VisualUnhideHook,
         true},
        {{L"public: virtual void __cdecl CVisual::SetOpacity(double)"},
         &g_visualSetOpacityOriginal,
         VisualSetOpacityHook,
         true},
        {{L"public: virtual class CVisual * __cdecl "
           L"CVisual::GetTransformParent(void)const",
          L"public: virtual class CVisual * __cdecl "
           L"CVisual::GetTransformParent(void)const "},
         &g_visualGetTransformParent,
         nullptr,
         true},
        {{L"public: virtual class CVisualProxy * __cdecl "
           L"CVisual::GetVisualProxyForStructure(void)"},
         &g_visualGetVisualProxyForStructure,
         nullptr,
         true},
        {{L"public: long __cdecl VisualCollection::InsertRelative("
           L"class CVisual *,class CVisual *,bool,bool)"},
         &g_visualCollectionInsertRelativeFunction,
         nullptr,
         true},
        {{L"public: unsigned long __cdecl CBaseObject::Release(void)"},
         &g_cBaseObjectRelease,
         nullptr,
         true},
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
        {{L"const CBitmapSourceProxy::`vftable'"},
         &g_bitmapSourceProxyVtableSymbol,
         nullptr,
         true},
        {{L"const CVisualSurfaceProxy::`vftable'"},
         &g_visualSurfaceProxyVtableSymbol,
         nullptr,
         true},
        {{L"const CCachedVisualImageProxy::`vftable'"},
         &g_cachedVisualImageProxyVtableSymbol,
         nullptr,
         true},
        {{L"const CClientArea::`vftable'"},
         &g_clientAreaVtableSymbol,
         nullptr,
         true},
        {{L"const VisualCollection::`vftable'"},
         &g_visualCollectionVtableSymbol,
         nullptr,
         true}};
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
    keepValid(g_windowBorderCloneVisualTreeOriginal);
    keepValid(g_topLevelWindowCloneVisualTreeForLivePreviewOriginal);
    keepValid(g_topLevelWindowGetWindowData);
    keepValid(g_desktopManagerPostStartAnimations);
    keepValid(g_meshGeometry2dProxyUpdate);
    keepValid(g_createMeshGeometry2dProxy);
    keepValid(g_createGeometry2dGroupProxy);
    keepValid(g_createBitmapSourceProxyOriginal);
    keepValid(g_createVisualSurfaceProxyOriginal);
    keepValid(g_geometry2dGroupProxyUpdate);
    keepValid(g_drawMesh2DInstructionCreate);
    keepValid(g_createTouchDragVisualFunction);
    keepValid(g_touchDragVisualNotifyFunction);
    keepValid(g_touchDragVisualStopFunction);
    keepValid(g_touchDragVisualCreateMeshInstructionFunction);
    keepValid(g_drawBitmapInstructionCreateOriginal);
    keepValid(g_drawTileImageInstructionCreateOriginal);
    keepValid(g_renderDataVisualAddInstruction);
    keepValid(g_renderDataVisualClearInstructions);
    keepValid(g_renderDataVisualUpdateRenderData);
    keepValid(g_topLevelWindow3DEnsureRenderDataOriginal);
    keepValid(g_topLevelWindow3DEnsureSecondaryWindowRepresentationOriginal);
    keepValid(g_topLevelWindow3DSetParentOriginal);
    keepValid(g_topLevelWindow3DShowWindowOriginal);
    keepValid(g_renderDataVisualCreate);
    keepValid(g_createCachedVisualImageProxy);
    keepValid(g_cachedVisualImageProxyUpdate);
    keepValid(g_cachedVisualImageProxySnapshot);
    keepValid(g_cachedVisualImageProxyFreeze);
    keepValid(g_visualProxySetContentOriginal);
    keepValid(g_visualProxyInsertChildOriginal);
    keepValid(g_visualProxyRemoveChildOriginal);
    keepValid(g_redirectVisualProxySetRedirectedVisualOriginal);
    keepValid(g_visualSetContentOriginal);
    keepValid(g_visualSetParentOriginal);
    keepValid(g_visualRemoveSelfFromParentOriginal);
    keepValid(g_visualHideOriginal);
    keepValid(g_visualUnhideOriginal);
    keepValid(g_visualSetOpacityOriginal);
    keepValid(g_visualGetTransformParent);
    keepValid(g_visualGetVisualProxyForStructure);
    keepValid(g_visualCollectionInsertRelativeFunction);
    g_visualParentOffset = FindOffsetFromFunction(
        reinterpret_cast<void*>(g_visualGetTransformParent), SIZE_MAX);
    g_visualContentOffset = FindOffsetFromFunction(
        reinterpret_cast<void*>(g_visualSetContentOriginal), SIZE_MAX);
    g_visualCollectionArrayOffset = SIZE_MAX;
    g_visualCollectionCountOffset = SIZE_MAX;
    bool hasVisualCollectionLayout = FindVisualCollectionLayout(
        g_visualCollectionInsertRelativeFunction,
        &g_visualCollectionArrayOffset, &g_visualCollectionCountOffset);
    g_renderDataInstructionsOffset = SIZE_MAX;
    g_renderDataInstructionCountOffset = SIZE_MAX;
    bool hasRenderListLayout = FindRenderDataInstructionLayout(
        reinterpret_cast<void*>(g_renderDataVisualClearInstructions),
        &g_renderDataInstructionsOffset,
        &g_renderDataInstructionCountOffset);
    g_ensureRenderDataPointerOffsetCount =
        FindEnsureRenderDataPointerOffsets(
            reinterpret_cast<void*>(
                g_topLevelWindow3DEnsureRenderDataOriginal),
            g_ensureRenderDataPointerOffsets,
            ARRAYSIZE(g_ensureRenderDataPointerOffsets));
    Wh_Log(L"True 4x4 render-list probe: clear=%s ensureHook=%s layout=%s "
           L"instructions=0x%zx count=0x%zx sourceOffsets=%u (read-only)",
           g_renderDataVisualClearInstructions ? L"available" : L"unavailable",
           g_topLevelWindow3DEnsureRenderDataOriginal ? L"available"
                                                      : L"unavailable",
           hasRenderListLayout ? L"available" : L"unavailable",
           g_renderDataInstructionsOffset, g_renderDataInstructionCountOffset,
           g_ensureRenderDataPointerOffsetCount);
    Wh_Log(L"True 4x4 observation hooks: proxyContent=%p proxyInsert=%p "
           L"visualContent=%p visualParent=%p visualRemove=%p getParent=%p "
           L"getProxy=%p redirect=%p parentOffset=0x%zx contentOffset=0x%zx "
           L"childLayout=%s childArray=0x%zx childCount=0x%zx",
           g_visualProxySetContentOriginal, g_visualProxyInsertChildOriginal,
           g_visualSetContentOriginal, g_visualSetParentOriginal,
           g_visualRemoveSelfFromParentOriginal,
           g_visualGetTransformParent,
           g_visualGetVisualProxyForStructure,
           g_redirectVisualProxySetRedirectedVisualOriginal,
           g_visualParentOffset, g_visualContentOffset,
           hasVisualCollectionLayout ? L"available" : L"unavailable",
           g_visualCollectionArrayOffset,
           g_visualCollectionCountOffset);
    bool hasNativeMeshGeometry =
        g_meshGeometry2dProxyUpdate && g_createMeshGeometry2dProxy &&
        g_createGeometry2dGroupProxy && g_geometry2dGroupProxyUpdate;
    bool hasMeshBitmapRenderer =
        hasNativeMeshGeometry && g_drawMesh2DInstructionCreate;
    Wh_Log(L"True 4x4 mesh probe: geometry=%s bitmapRenderer=%s "
           L"bitmapObserver=%s tileObserver=%s "
           L"transactionalProbe=%s",
           hasNativeMeshGeometry ? L"available" : L"unavailable",
           hasMeshBitmapRenderer ? L"available" : L"unavailable",
           g_drawBitmapInstructionCreateOriginal ? L"available"
                                                  : L"unavailable",
            g_drawTileImageInstructionCreateOriginal ? L"available"
                                                     : L"unavailable",
            NATIVE_MESH_TRANSACTION_PROBE_ENABLED ? L"enabled" : L"disabled");
    Wh_Log(L"True 4x4 native touch path: create=%s notify=%s stop=%s "
           L"meshBuilder=%s (probe only)",
           g_createTouchDragVisualFunction ? L"available" : L"unavailable",
           g_touchDragVisualNotifyFunction ? L"available" : L"unavailable",
           g_touchDragVisualStopFunction ? L"available" : L"unavailable",
           g_touchDragVisualCreateMeshInstructionFunction ? L"available"
                                                          : L"unavailable");
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
    bool hasExactBitmapSourceProxyVtable =
        cacheVtableSymbol(g_bitmapSourceProxyVtableSymbol, g_bitmapSourceProxyVtable);
    bool hasExactVisualSurfaceProxyVtable =
        cacheVtableSymbol(g_visualSurfaceProxyVtableSymbol, g_visualSurfaceProxyVtable);
    bool hasExactCachedVisualImageProxyVtable = cacheVtableSymbol(
        g_cachedVisualImageProxyVtableSymbol, g_cachedVisualImageProxyVtable);
    bool hasExactClientAreaVtable =
        cacheVtableSymbol(g_clientAreaVtableSymbol, g_clientAreaVtable);
    bool hasExactVisualCollectionVtable = cacheVtableSymbol(
        g_visualCollectionVtableSymbol, g_visualCollectionVtable);
    Wh_Log(L"True 4x4 child-tree probe: collection=%s layout=%s "
           L"array=0x%zx count=0x%zx (read-only)",
           hasExactVisualCollectionVtable ? L"available" : L"unavailable",
           hasVisualCollectionLayout ? L"available" : L"unavailable",
           g_visualCollectionArrayOffset,
           g_visualCollectionCountOffset);
    Wh_Log(L"True 4x4 clone observers: border=%s livePreview=%s "
           L"(read-only)",
           g_windowBorderCloneVisualTreeOriginal ? L"available"
                                                  : L"unavailable",
           g_topLevelWindowCloneVisualTreeForLivePreviewOriginal
               ? L"available"
               : L"unavailable");
    Wh_Log(L"True 4x4 secondary representation observers: ensure=%s "
           L"parent=%s show=%s (read-only)",
           g_topLevelWindow3DEnsureSecondaryWindowRepresentationOriginal
               ? L"available"
               : L"unavailable",
           g_topLevelWindow3DSetParentOriginal ? L"available"
                                                : L"unavailable",
           g_topLevelWindow3DShowWindowOriginal ? L"available"
                                                 : L"unavailable");
    Wh_Log(L"True 4x4 visibility observers: hide=%s unhide=%s opacity=%s "
           L"(read-only)",
           g_visualHideOriginal ? L"available" : L"unavailable",
           g_visualUnhideOriginal ? L"available" : L"unavailable",
           g_visualSetOpacityOriginal ? L"available" : L"unavailable");
    Wh_Log(L"True 4x4 source probe: bitmap=%s visualSurface=%s "
           L"bitmapCreationObserver=%s surfaceCreationObserver=%s "
           L"vtableAlias=%d",
           hasExactBitmapSourceProxyVtable ? L"available" : L"unavailable",
           hasExactVisualSurfaceProxyVtable ? L"available" : L"unavailable",
           g_createBitmapSourceProxyOriginal ? L"available" : L"unavailable",
           g_createVisualSurfaceProxyOriginal ? L"available" : L"unavailable",
           g_bitmapSourceProxyVtable.load(std::memory_order_acquire) ==
               g_visualSurfaceProxyVtable.load(std::memory_order_acquire));
    Wh_Log(L"True 4x4 GPU source probe: cachedVisual=%s clientArea=%s "
           L"create=%s update=%s snapshot=%s freeze=%s "
           L"meshInstruction=%s renderVisual=%s publish=%s",
           hasExactCachedVisualImageProxyVtable ? L"available" : L"unavailable",
           hasExactClientAreaVtable ? L"available" : L"unavailable",
           g_createCachedVisualImageProxy ? L"available" : L"unavailable",
           g_cachedVisualImageProxyUpdate ? L"available" : L"unavailable",
           g_cachedVisualImageProxySnapshot ? L"available" : L"unavailable",
           g_cachedVisualImageProxyFreeze ? L"available" : L"unavailable",
           g_drawMesh2DInstructionCreate ? L"available" : L"unavailable",
           g_renderDataVisualCreate && g_renderDataVisualAddInstruction
               ? L"available"
               : L"unavailable",
           g_renderDataVisualUpdateRenderData ? L"available" : L"unavailable");
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

static MeshSourceKind GetMeshSourceKind(void* object)
{
    if (!object || !IsReadableMemory(object, sizeof(void*)))
    {
        return MeshSourceKind::None;
    }
    if (FindObservedBitmapSourceProxy(object, false))
    {
        return MeshSourceKind::Bitmap;
    }
    if (FindObservedVisualSurfaceProxy(object, false))
    {
        return MeshSourceKind::VisualSurface;
    }
    void* vtable = *reinterpret_cast<void**>(object);
    void* bitmapVtable =
        g_bitmapSourceProxyVtable.load(std::memory_order_acquire);
    void* visualSurfaceVtable =
        g_visualSurfaceProxyVtable.load(std::memory_order_acquire);
    if (visualSurfaceVtable && visualSurfaceVtable != bitmapVtable &&
        vtable == visualSurfaceVtable)
    {
        return MeshSourceKind::VisualSurface;
    }
    if ((bitmapVtable && vtable == bitmapVtable) ||
        (visualSurfaceVtable && vtable == visualSurfaceVtable))
    {
        return MeshSourceKind::AmbiguousProxy;
    }
    return MeshSourceKind::None;
}

static bool ReadBaseImageResourceId(void* imageProxy,
                                    unsigned int* resourceId)
{
    void* backing = ReadPointerMember(imageProxy, 0x10);
    if (!resourceId || !backing)
    {
        return false;
    }
    const BYTE* field = static_cast<const BYTE*>(backing) + 0x18;
    if (!IsReadableMemory(field, sizeof(*resourceId)))
    {
        return false;
    }
    *resourceId = *reinterpret_cast<const unsigned int*>(field);
    return *resourceId != 0;
}

static bool FindUniqueBaseImageInstruction(
    void* renderVisual, unsigned int requiredResourceId,
    void** instruction, void** imageProxy, int* instructionIndex,
    int* instructionCount, unsigned int* candidateCount)
{
    if (instruction)
    {
        *instruction = nullptr;
    }
    if (imageProxy)
    {
        *imageProxy = nullptr;
    }
    if (instructionIndex)
    {
        *instructionIndex = -1;
    }
    if (instructionCount)
    {
        *instructionCount = 0;
    }
    if (candidateCount)
    {
        *candidateCount = 0;
    }

    void** instructions = nullptr;
    int count = 0;
    if (!GetRenderDataInstructionList(renderVisual, &instructions, nullptr,
                                      &count))
    {
        return false;
    }
    if (instructionCount)
    {
        *instructionCount = count;
    }

    unsigned int matches = 0;
    void* matchedInstruction = nullptr;
    void* matchedImage = nullptr;
    int matchedIndex = -1;
    for (int index = 0; index < count; index++)
    {
        void* candidateInstruction = instructions[index];
        void* candidateImage = ReadPointerMember(candidateInstruction, 0x10);
        unsigned int resourceId = 0;
        if (!candidateInstruction || !candidateImage ||
            !IsReadableMemory(candidateImage, sizeof(void*)) ||
            !IsDwmImageAddress(*reinterpret_cast<void**>(candidateImage),
                               sizeof(void*)) ||
            GetMeshSourceKind(candidateImage) != MeshSourceKind::None ||
            !ReadBaseImageResourceId(candidateImage, &resourceId) ||
            (requiredResourceId && resourceId != requiredResourceId))
        {
            continue;
        }
        matches++;
        matchedInstruction = candidateInstruction;
        matchedImage = candidateImage;
        matchedIndex = index;
    }
    if (candidateCount)
    {
        *candidateCount = matches;
    }
    if (matches != 1)
    {
        return false;
    }
    if (instruction)
    {
        *instruction = matchedInstruction;
    }
    if (imageProxy)
    {
        *imageProxy = matchedImage;
    }
    if (instructionIndex)
    {
        *instructionIndex = matchedIndex;
    }
    return true;
}

static long UpdateNativeMeshGeometry(void* meshProxy, const WobbleMesh* mesh,
                                     double identityWidth,
                                     double identityHeight)
{
    if (!meshProxy || !g_meshGeometry2dProxyUpdate ||
        (mesh && (mesh->width <= 0.0 || mesh->height <= 0.0)) ||
        (!mesh && (identityWidth <= 0.0 || identityHeight <= 0.0)))
    {
        return E_INVALIDARG;
    }
    D2DPoint3F positions[GRID_POINT_COUNT] = {};
    MilPoint2DValue textureCoordinates[GRID_POINT_COUNT] = {};
    unsigned int indices[(GRID_WIDTH - 1) * (GRID_HEIGHT - 1) * 6] = {};
    Vec2 anchorDisplacement = {};
    if (mesh)
    {
        if (mesh->dragPointIndex >= 0 &&
            mesh->dragPointIndex < GRID_POINT_COUNT)
        {
            const WobblePoint& anchor = mesh->points[mesh->dragPointIndex];
            anchorDisplacement = {
                anchor.position.x - anchor.basePosition.x,
                anchor.position.y - anchor.basePosition.y};
        }
        else
        {
            for (const WobblePoint& point : mesh->points)
            {
                anchorDisplacement.x +=
                    point.position.x - point.basePosition.x;
                anchorDisplacement.y +=
                    point.position.y - point.basePosition.y;
            }
            anchorDisplacement.x /= GRID_POINT_COUNT;
            anchorDisplacement.y /= GRID_POINT_COUNT;
        }
    }
    // This diagnostic gain makes non-affine deformation unmistakable. The
    // production value will be tuned after the native path is visually proven.
    constexpr double nativeResidualGain = 4.0;
    for (int y = 0; y < GRID_HEIGHT; y++)
    {
        for (int x = 0; x < GRID_WIDTH; x++)
        {
            int index = GetPointIndex(x, y);
            double width = mesh ? mesh->width : identityWidth;
            double height = mesh ? mesh->height : identityHeight;
            double baseX = static_cast<double>(x) * width /
                           (GRID_WIDTH - 1);
            double baseY = static_cast<double>(y) * height /
                           (GRID_HEIGHT - 1);
            double renderedX = baseX;
            double renderedY = baseY;
            if (mesh)
            {
                const WobblePoint& point = mesh->points[index];
                baseX = point.basePosition.x;
                baseY = point.basePosition.y;
                renderedX = baseX +
                            ((point.position.x - baseX) -
                             anchorDisplacement.x) * nativeResidualGain;
                renderedY = baseY +
                            ((point.position.y - baseY) -
                             anchorDisplacement.y) * nativeResidualGain;
            }
            if (!std::isfinite(baseX) || !std::isfinite(baseY) ||
                !std::isfinite(renderedX) || !std::isfinite(renderedY))
            {
                return E_INVALIDARG;
            }
            renderedX = std::clamp(renderedX, -width * 8.0, width * 8.0);
            renderedY = std::clamp(renderedY, -height * 8.0, height * 8.0);
            positions[index] = {static_cast<float>(renderedX),
                                static_cast<float>(renderedY), 0.0f};
            textureCoordinates[index] = {baseX, baseY};
        }
    }
    unsigned int indexCount = 0;
    for (int y = 0; y < GRID_HEIGHT - 1; y++)
    {
        for (int x = 0; x < GRID_WIDTH - 1; x++)
        {
            unsigned int topLeft = GetPointIndex(x, y);
            unsigned int topRight = GetPointIndex(x + 1, y);
            unsigned int bottomLeft = GetPointIndex(x, y + 1);
            unsigned int bottomRight = GetPointIndex(x + 1, y + 1);
            indices[indexCount++] = topLeft;
            indices[indexCount++] = bottomLeft;
            indices[indexCount++] = topRight;
            indices[indexCount++] = topRight;
            indices[indexCount++] = bottomLeft;
            indices[indexCount++] = bottomRight;
        }
    }
    return g_meshGeometry2dProxyUpdate(
        meshProxy, 0, positions, textureCoordinates, GRID_POINT_COUNT,
        indices, indexCount);
}

static void* GetNativeMeshProxy(unsigned int index)
{
    if (index == 0)
    {
        return g_visibleMeshCanary.meshProxy;
    }
    return index <= ARRAYSIZE(g_visibleMeshCanary.additionalNativeBindings)
               ? g_visibleMeshCanary.additionalNativeBindings[index - 1]
                     .meshProxy
               : nullptr;
}

static void* GetNativeGeometryGroupProxy(unsigned int index)
{
    if (index == 0)
    {
        return g_visibleMeshCanary.groupProxy;
    }
    return index <= ARRAYSIZE(g_visibleMeshCanary.additionalNativeBindings)
               ? g_visibleMeshCanary.additionalNativeBindings[index - 1]
                     .groupProxy
               : nullptr;
}

static long UpdateAllNativeMeshGeometry(const WobbleMesh* mesh = nullptr)
{
    long result = S_OK;
    for (unsigned int index = 0;
         index < g_visibleMeshCanary.nativeBindingCount; index++)
    {
        void* meshProxy = GetNativeMeshProxy(index);
        long updateResult = UpdateNativeMeshGeometry(
            meshProxy, mesh, g_visibleMeshCanary.width,
            g_visibleMeshCanary.height);
        void* groupProxy = GetNativeGeometryGroupProxy(index);
        if (updateResult >= 0)
        {
            updateResult = groupProxy && g_geometry2dGroupProxyUpdate
                               ? g_geometry2dGroupProxyUpdate(groupProxy,
                                                              meshProxy)
                               : E_NOINTERFACE;
        }
        if (updateResult < 0 && result >= 0)
        {
            result = updateResult;
        }
    }
    return result;
}

static long RepublishOriginalRenderData(HWND hwnd)
{
    void* windowList = g_windowListForSceneWake.load(std::memory_order_acquire);
    if (!hwnd || !g_renderDataVisualUpdateRenderData ||
        !IsDwmObjectPointerValid(windowList, g_windowListVtable) ||
        !g_findWindowDataByHwnd)
    {
        return E_NOINTERFACE;
    }
    void* windowData = FindWindowDataByHwnd(windowList, hwnd);
    void* topLevelWindow = nullptr;
    void* renderVisual = nullptr;
    if (!windowData || GetHwndFromWindowData(windowData) != hwnd ||
        !ResolveDwmWindowObjects(windowData, &topLevelWindow, &renderVisual) ||
        !IsDwmObjectPointerValid(renderVisual, g_topLevelWindow3DVtable))
    {
        return E_NOINTERFACE;
    }
    return g_renderDataVisualUpdateRenderData(renderVisual);
}

static void RequestVisibleMeshCleanupForHwnd(HWND hwnd)
{
    if (!hwnd ||
        g_visibleMeshCanaryHwnd.load(std::memory_order_acquire) != hwnd ||
        !g_visibleMeshCanaryActive.load(std::memory_order_acquire))
    {
        return;
    }
    g_visibleMeshCanaryCleanupRequested.store(true,
                                               std::memory_order_release);
    RequestDwmScenePass();
}

static void MaintainVisibleMeshCanary()
{
    if (!IsOnDwmSceneThread() ||
        !g_visibleMeshCanaryActive.load(std::memory_order_acquire))
    {
        return;
    }
    bool forced = g_unloading.load(std::memory_order_acquire) ||
                  g_visibleMeshCanaryCleanupRequested.load(
                      std::memory_order_acquire);
    bool nativeBinding = !g_visibleMeshCanary.renderVisual &&
                         g_visibleMeshCanary.nativeBindingCount > 0;
    bool detach = forced ||
                  (!nativeBinding && g_visibleMeshCanary.detachAt &&
                   GetTickCount64() >= g_visibleMeshCanary.detachAt);
    if (!detach)
    {
        return;
    }
    VisibleMeshCanaryState state = g_visibleMeshCanary;
    long detachResult = E_NOINTERFACE;
    if (!state.renderVisual && state.meshProxy &&
        g_meshGeometry2dProxyUpdate)
    {
        detachResult = RepublishOriginalRenderData(state.hwnd);
        if (detachResult < 0)
        {
            detachResult = UpdateAllNativeMeshGeometry();
        }
    }
    if (state.renderVisual && g_visualRemoveSelfFromParentOriginal)
    {
        detachResult = g_visualRemoveSelfFromParentOriginal(state.renderVisual);
    }
    if (state.renderVisual && g_cBaseObjectRelease)
    {
        g_cBaseObjectRelease(state.renderVisual);
    }
    if (state.instruction && g_cBaseObjectRelease)
    {
        g_cBaseObjectRelease(state.instruction);
    }
    if (state.pinnedImageProxy && g_cBaseObjectRelease)
    {
        g_cBaseObjectRelease(state.pinnedImageProxy);
    }
    if (state.groupProxy && g_cBaseObjectRelease)
    {
        g_cBaseObjectRelease(state.groupProxy);
    }
    if (state.meshProxy && g_cBaseObjectRelease)
    {
        g_cBaseObjectRelease(state.meshProxy);
    }
    if (state.cachedVisual && g_cBaseObjectRelease)
    {
        g_cBaseObjectRelease(state.cachedVisual);
    }
    for (unsigned int index = 1; index < state.nativeBindingCount; index++)
    {
        auto& binding = state.additionalNativeBindings[index - 1];
        if (binding.instruction && g_cBaseObjectRelease)
        {
            g_cBaseObjectRelease(binding.instruction);
        }
        if (binding.groupProxy && g_cBaseObjectRelease)
        {
            g_cBaseObjectRelease(binding.groupProxy);
        }
        if (binding.meshProxy && g_cBaseObjectRelease)
        {
            g_cBaseObjectRelease(binding.meshProxy);
        }
    }
    g_visibleMeshCanary = {};
    g_visibleMeshCanaryHwnd.store(nullptr, std::memory_order_release);
    g_visibleMeshCanaryCleanupRequested.store(false,
                                               std::memory_order_release);
    g_visibleMeshCanaryActive.store(false, std::memory_order_release);
    if (!g_unloading.load(std::memory_order_acquire))
    {
        g_liveBaseImageMeshCanaryStarted.store(false,
                                                std::memory_order_release);
        g_liveBaseImageMeshCanarySucceeded.store(false,
                                                  std::memory_order_release);
        g_liveBaseImageMeshAnimationLogged.store(false,
                                                  std::memory_order_release);
    }
    RequestDwmScenePass();
    if (state.renderVisual)
    {
        Wh_Log(L"True 4x4 visible warped canary: detached result=0x%08X HWND=%p",
               static_cast<unsigned int>(detachResult), state.hwnd);
    }
    else
    {
        Wh_Log(L"True 4x4 native transaction canary: restored original result=0x%08X HWND=%p",
               static_cast<unsigned int>(detachResult), state.hwnd);
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

static void RunNativeMeshCanary()
{
    if (!g_nativeMeshCanaryPending.load(std::memory_order_acquire) ||
        !IsOnDwmSceneThread())
    {
        return;
    }
    void* compositor = g_dwmCompositor.load(std::memory_order_acquire);
    if (!IsDwmObjectPointerValid(compositor, g_compositorVtable))
    {
        return;
    }
    // Consume the one-shot canary before entering private DWM code. A failure
    // keeps the stable affine renderer active and is never retried in-process.
    g_nativeMeshCanaryPending.store(false, std::memory_order_release);
    long result = E_NOINTERFACE;
    const wchar_t* stage = L"Prerequisites";
    void* meshProxy = nullptr;
    void* groupProxy = nullptr;
    if (g_createMeshGeometry2dProxy && g_meshGeometry2dProxyUpdate &&
        g_createGeometry2dGroupProxy && g_geometry2dGroupProxyUpdate &&
        g_cBaseObjectRelease)
    {
        D2DPoint3F positions[GRID_POINT_COUNT] = {};
        MilPoint2DValue textureCoordinates[GRID_POINT_COUNT] = {};
        unsigned int indices[(GRID_WIDTH - 1) * (GRID_HEIGHT - 1) * 6] = {};
        for (int y = 0; y < GRID_HEIGHT; y++)
        {
            for (int x = 0; x < GRID_WIDTH; x++)
            {
                int index = GetPointIndex(x, y);
                float px = static_cast<float>(x) / (GRID_WIDTH - 1);
                float py = static_cast<float>(y) / (GRID_HEIGHT - 1);
                positions[index] = {px, py, 0.0f};
                textureCoordinates[index] = {px, py};
            }
        }
        unsigned int indexCount = 0;
        for (int y = 0; y < GRID_HEIGHT - 1; y++)
        {
            for (int x = 0; x < GRID_WIDTH - 1; x++)
            {
                unsigned int topLeft = GetPointIndex(x, y);
                unsigned int topRight = GetPointIndex(x + 1, y);
                unsigned int bottomLeft = GetPointIndex(x, y + 1);
                unsigned int bottomRight = GetPointIndex(x + 1, y + 1);
                indices[indexCount++] = topLeft;
                indices[indexCount++] = bottomLeft;
                indices[indexCount++] = topRight;
                indices[indexCount++] = topRight;
                indices[indexCount++] = bottomLeft;
                indices[indexCount++] = bottomRight;
            }
        }
        stage = L"CreateMesh";
        result = g_createMeshGeometry2dProxy(compositor, &meshProxy);
        if (result >= 0 && meshProxy)
        {
            stage = L"UpdateMesh";
            result = g_meshGeometry2dProxyUpdate(
                meshProxy, 0, positions, textureCoordinates, GRID_POINT_COUNT,
                indices, indexCount);
        }
        if (result >= 0)
        {
            stage = L"CreateGroup";
            result = g_createGeometry2dGroupProxy(compositor, &groupProxy);
        }
        if (result >= 0 && groupProxy)
        {
            stage = L"UpdateGroup";
            result = g_geometry2dGroupProxyUpdate(groupProxy, meshProxy);
        }
    }
    if (groupProxy)
    {
        g_cBaseObjectRelease(groupProxy);
    }
    if (meshProxy)
    {
        g_cBaseObjectRelease(meshProxy);
    }
    bool succeeded = result >= 0 && groupProxy && meshProxy;
    g_nativeMeshCanarySucceeded.store(succeeded, std::memory_order_release);
    Wh_Log(L"True 4x4 mesh canary: %s stage=%s result=0x%08X vertices=%u indices=%u",
           succeeded ? L"passed" : L"failed", stage,
           static_cast<unsigned int>(result), GRID_POINT_COUNT,
           (GRID_WIDTH - 1) * (GRID_HEIGHT - 1) * 6);
}

static bool TryInstallLiveBaseImageMeshCanary(void* renderVisual,
                                               void* originalInstruction,
                                               void* imageProxy,
                                               void* windowData, HWND hwnd)
{
    if (!IsOnDwmSceneThread() || !renderVisual || !originalInstruction ||
        !windowData || !hwnd || GetHwndFromWindowData(windowData) != hwnd ||
        g_liveBaseImageMeshTargetHwnd.load(std::memory_order_acquire) != hwnd ||
        !imageProxy ||
        g_visibleMeshCanaryActive.load(std::memory_order_acquire) ||
        !g_nativeMeshCanarySucceeded.load(std::memory_order_acquire) ||
        GetMeshSourceKind(imageProxy) != MeshSourceKind::None ||
        !IsReadableMemory(imageProxy, sizeof(void*)) ||
        !g_createMeshGeometry2dProxy || !g_meshGeometry2dProxyUpdate ||
        !g_createGeometry2dGroupProxy || !g_geometry2dGroupProxyUpdate ||
        !g_drawMesh2DInstructionCreate || !g_cBaseObjectRelease)
    {
        return false;
    }

    unsigned int originalCount = 0;
    int originalIndex = -1;
    if (!FindRenderDataInstructionIndex(renderVisual, originalInstruction,
                                        &originalCount, &originalIndex))
    {
        return false;
    }
    RECT bounds = {};
    if (!GetWindowRect(hwnd, &bounds) || bounds.right <= bounds.left ||
        bounds.bottom <= bounds.top)
    {
        return false;
    }
    double width = static_cast<double>(bounds.right - bounds.left);
    double height = static_cast<double>(bounds.bottom - bounds.top);
    void* imageVtable = *reinterpret_cast<void**>(imageProxy);
    if (!IsDwmImageAddress(imageVtable, sizeof(void*)))
    {
        return false;
    }
    unsigned int imageResourceId = 0;
    void* imageBacking = ReadPointerMember(imageProxy, 0x10);
    auto* imageReferenceCount = reinterpret_cast<volatile LONG*>(
        static_cast<BYTE*>(imageProxy) + sizeof(void*));
    if (!ReadBaseImageResourceId(imageProxy, &imageResourceId) ||
        !imageBacking ||
        !IsWritableMemory(const_cast<LONG*>(imageReferenceCount),
                          sizeof(*imageReferenceCount)))
    {
        Wh_Log(L"True 4x4 native slot source unresolved: HWND=%p "
               L"image=%p backing=%p resourceId=%u",
               hwnd, imageProxy, imageBacking, imageResourceId);
        return false;
    }

    // UpdateRenderData can run before a taskbar-restored window becomes the
    // foreground window. The live CWindowData/HWND match above is the stable
    // ownership proof; foreground state is only transient UI state.
    bool expected = false;
    if (!g_liveBaseImageMeshCanaryStarted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return false;
    }

    long result = E_NOINTERFACE;
    const wchar_t* stage = L"CreateMesh";
    void* meshProxy = nullptr;
    void* groupProxy = nullptr;
    bool imagePinned = false;
    void* meshInstruction = nullptr;
    D2DPoint3F positions[GRID_POINT_COUNT] = {};
    MilPoint2DValue textureCoordinates[GRID_POINT_COUNT] = {};
    unsigned int indices[(GRID_WIDTH - 1) * (GRID_HEIGHT - 1) * 6] = {};
    for (int y = 0; y < GRID_HEIGHT; y++)
    {
        for (int x = 0; x < GRID_WIDTH; x++)
        {
            int index = GetPointIndex(x, y);
            float tx = static_cast<float>(x) / (GRID_WIDTH - 1);
            float ty = static_cast<float>(y) / (GRID_HEIGHT - 1);
            positions[index] = {tx * static_cast<float>(width),
                                ty * static_cast<float>(height),
                                0.0f};
            textureCoordinates[index] = {tx * static_cast<float>(width),
                                         ty * static_cast<float>(height)};
        }
    }
    unsigned int indexCount = 0;
    for (int y = 0; y < GRID_HEIGHT - 1; y++)
    {
        for (int x = 0; x < GRID_WIDTH - 1; x++)
        {
            unsigned int topLeft = GetPointIndex(x, y);
            unsigned int topRight = GetPointIndex(x + 1, y);
            unsigned int bottomLeft = GetPointIndex(x, y + 1);
            unsigned int bottomRight = GetPointIndex(x + 1, y + 1);
            indices[indexCount++] = topLeft;
            indices[indexCount++] = bottomLeft;
            indices[indexCount++] = topRight;
            indices[indexCount++] = topRight;
            indices[indexCount++] = bottomLeft;
            indices[indexCount++] = bottomRight;
        }
    }

    void* compositor = g_dwmCompositor.load(std::memory_order_acquire);
    if (IsDwmObjectPointerValid(compositor, g_compositorVtable))
    {
        result = g_createMeshGeometry2dProxy(compositor, &meshProxy);
    }
    if (result >= 0 && meshProxy)
    {
        stage = L"UpdateMesh";
        result = g_meshGeometry2dProxyUpdate(
            meshProxy, 0, positions, textureCoordinates, GRID_POINT_COUNT,
            indices, indexCount);
    }
    if (result >= 0)
    {
        stage = L"CreateGroup";
        result = g_createGeometry2dGroupProxy(compositor, &groupProxy);
    }
    if (result >= 0 && groupProxy)
    {
        stage = L"UpdateGroup";
        result = g_geometry2dGroupProxyUpdate(groupProxy, meshProxy);
    }
    if (result >= 0)
    {
        stage = L"PinImageSource";
        InterlockedIncrement(imageReferenceCount);
        imagePinned = true;
    }
    if (result >= 0)
    {
        stage = L"CreateInstruction";
        result = g_drawMesh2DInstructionCreate(groupProxy, imageProxy,
                                                &meshInstruction);
    }
    if (result >= 0 && meshInstruction)
    {
        stage = L"Prepared";
    }

    bool succeeded = result >= 0 && meshProxy && groupProxy && meshInstruction;
    g_liveBaseImageMeshCanarySucceeded.store(succeeded,
                                               std::memory_order_release);
    if (!succeeded)
    {
        // A transient render-list rebuild must not permanently consume the
        // one-shot installation gate. A later valid publish can retry.
        g_liveBaseImageMeshCanaryStarted.store(false,
                                                std::memory_order_release);
    }
    if (succeeded)
    {
        g_visibleMeshCanary = {};
        g_visibleMeshCanary.meshProxy = meshProxy;
        g_visibleMeshCanary.groupProxy = groupProxy;
        g_visibleMeshCanary.instruction = meshInstruction;
        g_visibleMeshCanary.pinnedImageProxy = imageProxy;
        g_visibleMeshCanary.hwnd = hwnd;
        g_visibleMeshCanary.detachAt = 0;
        g_visibleMeshCanary.nativeBindingCount = 1;
        g_visibleMeshCanary.width = width;
        g_visibleMeshCanary.height = height;
        meshProxy = nullptr;
        groupProxy = nullptr;
        imagePinned = false;
        meshInstruction = nullptr;
        g_visibleMeshCanaryCleanupRequested.store(false,
                                                   std::memory_order_release);
        g_visibleMeshCanaryHwnd.store(hwnd, std::memory_order_release);
        g_visibleMeshCanaryActive.store(true, std::memory_order_release);
        RequestDwmScenePass();
    }
    if (meshInstruction)
    {
        g_cBaseObjectRelease(meshInstruction);
    }
    if (imagePinned)
    {
        g_cBaseObjectRelease(imageProxy);
    }
    if (groupProxy)
    {
        g_cBaseObjectRelease(groupProxy);
    }
    if (meshProxy)
    {
        g_cBaseObjectRelease(meshProxy);
    }

    Wh_Log(L"True 4x4 native transaction canary: %s stage=%s "
           L"result=0x%08X HWND=%p visual=%p original=%p index=%d/%u "
           L"image=%p imageVtable=%p imageBacking=%p directSource=1 "
           L"resourceId=%u target=active-drag "
           L"size=%.0fx%.0f",
           succeeded ? L"installed" : L"failed", stage,
           static_cast<unsigned int>(result), hwnd, renderVisual,
           originalInstruction, originalIndex, originalCount, imageProxy,
           imageVtable, imageBacking, imageResourceId,
           width, height);
    return succeeded;
}

static void* FindVisualChildCollection(void* visual, size_t* memberOffset,
                                       bool* indirect)
{
    if (memberOffset)
    {
        *memberOffset = SIZE_MAX;
    }
    if (indirect)
    {
        *indirect = false;
    }
    void* expectedVtable =
        g_visualCollectionVtable.load(std::memory_order_acquire);
    if (!visual || !expectedVtable ||
        !IsReadableMemory(visual, 0x108))
    {
        return nullptr;
    }

    void* match = nullptr;
    size_t matchOffset = SIZE_MAX;
    bool matchIndirect = false;
    unsigned int matchCount = 0;
    auto accept = [&](void* candidate, size_t offset, bool isIndirect)
    {
        if (!candidate || !IsReadableMemory(candidate, sizeof(void*)) ||
            *reinterpret_cast<void**>(candidate) != expectedVtable)
        {
            return;
        }
        if (candidate == match)
        {
            return;
        }
        match = candidate;
        matchOffset = offset;
        matchIndirect = isIndirect;
        matchCount++;
    };
    for (size_t offset = 0; offset <= 0x100; offset += sizeof(void*))
    {
        BYTE* embedded = static_cast<BYTE*>(visual) + offset;
        accept(embedded, offset, false);
        accept(ReadPointerMember(visual, offset), offset, true);
    }
    if (matchCount != 1)
    {
        return nullptr;
    }
    if (memberOffset)
    {
        *memberOffset = matchOffset;
    }
    if (indirect)
    {
        *indirect = matchIndirect;
    }
    return match;
}

static bool GetVisualChildren(void* visual, void*** children, int* count,
                              size_t* collectionOffset, bool* indirect)
{
    if (children)
    {
        *children = nullptr;
    }
    if (count)
    {
        *count = 0;
    }
    if (g_visualCollectionArrayOffset == SIZE_MAX ||
        g_visualCollectionCountOffset == SIZE_MAX)
    {
        return false;
    }
    void* collection =
        FindVisualChildCollection(visual, collectionOffset, indirect);
    if (!collection)
    {
        return false;
    }
    BYTE* countField = static_cast<BYTE*>(collection) +
                       g_visualCollectionCountOffset;
    if (!IsReadableMemory(countField, sizeof(int)))
    {
        return false;
    }
    int childCount = *reinterpret_cast<int*>(countField);
    if (childCount < 0 || childCount > 64)
    {
        return false;
    }
    void** childArray = static_cast<void**>(
        ReadPointerMember(collection, g_visualCollectionArrayOffset));
    if (childCount > 0 &&
        (!childArray ||
         !IsReadableMemory(childArray,
                           static_cast<size_t>(childCount) * sizeof(void*))))
    {
        return false;
    }
    if (children)
    {
        *children = childArray;
    }
    if (count)
    {
        *count = childCount;
    }
    return true;
}

static void ProbeNativeRepresentationAncestry(HWND hwnd, void* completeRoot,
                                               void* secondaryVisual)
{
    static HWND lastHwnd = nullptr;
    static void* lastCompleteRoot = nullptr;
    static void* lastSecondaryVisual = nullptr;
    if (!hwnd || !completeRoot || !secondaryVisual ||
        g_visualParentOffset == SIZE_MAX ||
        (lastHwnd == hwnd && lastCompleteRoot == completeRoot &&
         lastSecondaryVisual == secondaryVisual))
    {
        return;
    }
    lastHwnd = hwnd;
    lastCompleteRoot = completeRoot;
    lastSecondaryVisual = secondaryVisual;

    struct VisualAncestry
    {
        void* nodes[16];
        unsigned int count;
    };
    auto collect = [](void* start)
    {
        VisualAncestry ancestry = {};
        void* current = start;
        while (current && ancestry.count < ARRAYSIZE(ancestry.nodes) &&
               IsReadableMemory(current, sizeof(void*)))
        {
            bool duplicate = false;
            for (unsigned int index = 0; index < ancestry.count; index++)
            {
                duplicate |= ancestry.nodes[index] == current;
            }
            if (duplicate)
            {
                break;
            }
            void* vtable = *reinterpret_cast<void**>(current);
            if (!IsDwmImageAddress(vtable, sizeof(void*)))
            {
                break;
            }
            ancestry.nodes[ancestry.count++] = current;
            current = ReadPointerMember(current, g_visualParentOffset);
        }
        return ancestry;
    };

    VisualAncestry live = collect(completeRoot);
    VisualAncestry secondary = collect(secondaryVisual);
    void* common = nullptr;
    int liveCommonDepth = -1;
    int secondaryCommonDepth = -1;
    for (unsigned int liveIndex = 0; liveIndex < live.count && !common;
         liveIndex++)
    {
        for (unsigned int secondaryIndex = 0;
             secondaryIndex < secondary.count; secondaryIndex++)
        {
            if (live.nodes[liveIndex] == secondary.nodes[secondaryIndex])
            {
                common = live.nodes[liveIndex];
                liveCommonDepth = static_cast<int>(liveIndex);
                secondaryCommonDepth = static_cast<int>(secondaryIndex);
                break;
            }
        }
    }
    int liveChildIndex = -1;
    int secondaryChildIndex = -1;
    int commonChildCount = 0;
    if (common && liveCommonDepth > 0 && secondaryCommonDepth > 0)
    {
        void** commonChildren = nullptr;
        if (GetVisualChildren(common, &commonChildren, &commonChildCount,
                              nullptr, nullptr))
        {
            void* liveChild = live.nodes[liveCommonDepth - 1];
            void* secondaryChild =
                secondary.nodes[secondaryCommonDepth - 1];
            for (int index = 0; index < commonChildCount; index++)
            {
                if (commonChildren[index] == liveChild)
                {
                    liveChildIndex = index;
                }
                if (commonChildren[index] == secondaryChild)
                {
                    secondaryChildIndex = index;
                }
            }
        }
    }

    auto logChain = [hwnd](const wchar_t* name,
                           const VisualAncestry& ancestry)
    {
        for (unsigned int index = 0; index < ancestry.count; index++)
        {
            void* visual = ancestry.nodes[index];
            void* vtable = *reinterpret_cast<void**>(visual);
            void* parent = ReadPointerMember(visual, g_visualParentOffset);
            void* proxy = ReadPointerMember(visual, g_visualProxyOffset);
            int childCount = 0;
            size_t collectionOffset = SIZE_MAX;
            bool indirect = false;
            bool hasChildren = GetVisualChildren(
                visual, nullptr, &childCount, &collectionOffset, &indirect);
            Wh_Log(L"True 4x4 ancestry %s[%u]: HWND=%p visual=%p "
                   L"vtable=%p parent=%p proxy=%p childLayout=%d "
                   L"children=%d collectionOffset=0x%zx indirect=%d "
                   L"(read-only)",
                   name, index, hwnd, visual, vtable, parent, proxy,
                   hasChildren, childCount, collectionOffset, indirect);
        }
    };
    logChain(L"live", live);
    logChain(L"secondary", secondary);
    Wh_Log(L"True 4x4 ancestry summary: HWND=%p liveRoot=%p "
           L"secondary=%p liveDepth=%u secondaryDepth=%u common=%p "
           L"liveCommonDepth=%d secondaryCommonDepth=%d commonChildren=%d "
           L"liveChildIndex=%d secondaryChildIndex=%d (read-only)",
           hwnd, completeRoot, secondaryVisual, live.count,
           secondary.count, common, liveCommonDepth, secondaryCommonDepth,
           commonChildCount, liveChildIndex, secondaryChildIndex);
}

static void ProbeCompleteWindowRootTree(HWND hwnd, void* topLevelWindow)
{
    static HWND lastHwnd = nullptr;
    static void* lastRoot = nullptr;
    if (!hwnd || !topLevelWindow || !g_topLevelWindowGetRootVisual ||
        g_visualProxyOffset == SIZE_MAX ||
        g_visualParentOffset == SIZE_MAX ||
        g_visualContentOffset == SIZE_MAX)
    {
        return;
    }

    constexpr int completeWindowRoot = 0;
    void* root =
        g_topLevelWindowGetRootVisual(topLevelWindow, completeWindowRoot);
    if (!root || (lastHwnd == hwnd && lastRoot == root))
    {
        return;
    }
    lastHwnd = hwnd;
    lastRoot = root;

    struct PendingVisual
    {
        void* visual;
        void* expectedParent;
        unsigned int depth;
    };
    PendingVisual pending[64] = {{root, nullptr, 0}};
    void* visited[64] = {};
    unsigned int pendingBegin = 0;
    unsigned int pendingEnd = 1;
    unsigned int visitedCount = 0;
    unsigned int contentCount = 0;
    while (pendingBegin < pendingEnd && visitedCount < ARRAYSIZE(visited))
    {
        PendingVisual item = pending[pendingBegin++];
        if (!item.visual || !IsReadableMemory(item.visual, sizeof(void*)))
        {
            continue;
        }
        bool seen = false;
        for (unsigned int i = 0; i < visitedCount; i++)
        {
            if (visited[i] == item.visual)
            {
                seen = true;
                break;
            }
        }
        if (seen)
        {
            continue;
        }
        visited[visitedCount] = item.visual;
        unsigned int nodeIndex = visitedCount++;

        void* visualVtable = *reinterpret_cast<void**>(item.visual);
        if (!IsDwmImageAddress(visualVtable, sizeof(void*)))
        {
            Wh_Log(L"True 4x4 root tree[%u]: HWND=%p depth=%u visual=%p "
                   L"invalidVtable=%p",
                   nodeIndex, hwnd, item.depth, item.visual, visualVtable);
            continue;
        }
        void* parent = ReadPointerMember(item.visual, g_visualParentOffset);
        void* proxy = ReadPointerMember(item.visual, g_visualProxyOffset);
        void* content = ReadPointerMember(item.visual, g_visualContentOffset);
        void* proxyVtable =
            proxy && IsReadableMemory(proxy, sizeof(void*))
                ? *reinterpret_cast<void**>(proxy)
                : nullptr;
        void* contentVtable =
            content && IsReadableMemory(content, sizeof(void*))
                ? *reinterpret_cast<void**>(content)
                : nullptr;
        void* redirect = nullptr;
        if (ObservedVisualProxy* entry = FindObservedVisualProxy(proxy, false))
        {
            redirect = entry->redirectTarget.load(std::memory_order_acquire);
        }
        unsigned int resourceId = 0;
        if (content)
        {
            contentCount++;
            ReadBaseImageResourceId(content, &resourceId);
        }

        void** children = nullptr;
        int childCount = 0;
        size_t collectionOffset = SIZE_MAX;
        bool indirectCollection = false;
        bool hasChildren = GetVisualChildren(
            item.visual, &children, &childCount, &collectionOffset,
            &indirectCollection);
        void** renderInstructions = nullptr;
        int renderInstructionCount = 0;
        bool hasRenderList = content && GetRenderDataInstructionList(
            item.visual, &renderInstructions, nullptr,
            &renderInstructionCount);
        Wh_Log(L"True 4x4 root tree[%u]: HWND=%p depth=%u visual=%p "
               L"visualVtable=%p parent=%p parentMatch=%d proxy=%p "
               L"proxyVtable=%p content=%p contentVtable=%p contentKind=%d "
               L"resourceId=%u redirect=%p collection=%s offset=0x%zx "
               L"indirect=%d children=%d renderList=%s instructions=%d",
               nodeIndex, hwnd, item.depth, item.visual, visualVtable,
               parent, !item.expectedParent || parent == item.expectedParent,
               proxy, proxyVtable, content, contentVtable,
               static_cast<int>(GetMeshSourceKind(content)), resourceId,
               redirect, hasChildren ? L"valid" : L"unavailable",
               collectionOffset, indirectCollection, childCount,
               hasRenderList ? L"valid" : L"unavailable",
               renderInstructionCount);

        if (hasRenderList)
        {
            for (int i = 0; i < renderInstructionCount; i++)
            {
                void* instruction = renderInstructions[i];
                void* instructionVtable =
                    instruction && IsReadableMemory(instruction, sizeof(void*))
                        ? *reinterpret_cast<void**>(instruction)
                        : nullptr;
                void* source = ReadPointerMember(instruction, 0x10);
                void* sourceVtable =
                    source && IsReadableMemory(source, sizeof(void*))
                        ? *reinterpret_cast<void**>(source)
                        : nullptr;
                void* backing = ReadPointerMember(source, 0x10);
                unsigned int sourceResourceId = 0;
                bool hasResourceId =
                    ReadBaseImageResourceId(source, &sourceResourceId);
                Wh_Log(L"True 4x4 root tree[%u] instruction[%d]: "
                       L"instruction=%p instructionVtable=%p "
                       L"instructionVtableInDwm=%d source=%p "
                       L"sourceVtable=%p sourceVtableInDwm=%d "
                       L"sourceKind=%d backing=%p resourceId=%u "
                       L"resourceValid=%d",
                       nodeIndex, i, instruction, instructionVtable,
                       IsDwmImageAddress(instructionVtable, sizeof(void*)),
                       source, sourceVtable,
                       IsDwmImageAddress(sourceVtable, sizeof(void*)),
                       static_cast<int>(GetMeshSourceKind(source)), backing,
                       sourceResourceId, hasResourceId);
            }
        }

        if (!hasChildren || item.depth >= 8)
        {
            continue;
        }
        for (int i = 0; i < childCount && pendingEnd < ARRAYSIZE(pending);
             i++)
        {
            void* child = children[i];
            if (!child || !IsReadableMemory(child, sizeof(void*)))
            {
                Wh_Log(L"True 4x4 root tree[%u] child[%d]: invalid=%p",
                       nodeIndex, i, child);
                continue;
            }
            void* childVtable = *reinterpret_cast<void**>(child);
            if (!IsDwmImageAddress(childVtable, sizeof(void*)))
            {
                Wh_Log(L"True 4x4 root tree[%u] child[%d]: visual=%p "
                       L"invalidVtable=%p",
                       nodeIndex, i, child, childVtable);
                continue;
            }
            pending[pendingEnd++] =
                {child, item.visual, item.depth + 1};
        }
    }
    Wh_Log(L"True 4x4 root tree summary: HWND=%p root=%p nodes=%u "
           L"contents=%u queued=%u truncated=%d (read-only)",
           hwnd, root, visitedCount, contentCount, pendingEnd,
           pendingBegin < pendingEnd || pendingEnd == ARRAYSIZE(pending));
}

static void InstallRequestedLiveBaseImageMeshCanary()
{
    if (!IsOnDwmSceneThread() ||
        g_visibleMeshCanaryActive.load(std::memory_order_acquire))
    {
        return;
    }
    HWND target =
        g_liveBaseImageMeshTargetHwnd.load(std::memory_order_acquire);
    if (!target || !IsWindow(target))
    {
        return;
    }
    void* windowList =
        g_windowListForSceneWake.load(std::memory_order_acquire);
    void* currentWindowData =
        IsDwmObjectPointerValid(windowList, g_windowListVtable) &&
                g_findWindowDataByHwnd
            ? FindWindowDataByHwnd(windowList, target)
            : nullptr;
    if (!currentWindowData ||
        GetHwndFromWindowData(currentWindowData) != target)
    {
        return;
    }
    if (!NATIVE_MESH_WRITE_PROBE_ENABLED)
    {
        void* topLevelWindow = nullptr;
        void* topLevelWindow3D = nullptr;
        if (ResolveDwmWindowObjects(currentWindowData, &topLevelWindow,
                                    &topLevelWindow3D))
        {
            ProbeCompleteWindowRootTree(target, topLevelWindow);
            constexpr int completeWindowRoot = 0;
            void* completeRoot = g_topLevelWindowGetRootVisual
                                     ? g_topLevelWindowGetRootVisual(
                                           topLevelWindow, completeWindowRoot)
                                     : nullptr;
            ProbeNativeRepresentationAncestry(target, completeRoot,
                                               topLevelWindow3D);
        }
        return;
    }
    unsigned int ownerMatches = 0;
    unsigned int liveInstructionMatches = 0;
    unsigned int ownerRenderLists = 0;
    unsigned int ownerListCandidates = 0;
    for (ObservedRenderImage& entry : g_observedRenderImages)
    {
        if (entry.ownerHwnd.load(std::memory_order_acquire) != target)
        {
            continue;
        }
        ownerMatches++;
        void* renderVisual = entry.visual.load(std::memory_order_acquire);
        void* instruction = entry.instruction.load(std::memory_order_acquire);
        void* imageProxy = entry.imageProxy.load(std::memory_order_acquire);
        if (!renderVisual)
        {
            continue;
        }

        if (instruction && imageProxy &&
            FindRenderDataInstructionIndex(renderVisual, instruction,
                                           nullptr, nullptr))
        {
            liveInstructionMatches++;
            if (TryInstallLiveBaseImageMeshCanary(
                    renderVisual, instruction, imageProxy, currentWindowData,
                    target))
            {
                long result = g_renderDataVisualUpdateRenderData(renderVisual);
                Wh_Log(L"True 4x4 targeted publish: HWND=%p result=0x%08X",
                       target, static_cast<unsigned int>(result));
                return;
            }
        }

        // The observed instruction can be replaced when DWM rebuilds the
        // render list, while the owning render visual remains stable. Search
        // only that proven visual for the same live image resource instead of
        // walking arbitrary DWM objects from the animation loop.
        unsigned int observedResourceId = 0;
        if (imageProxy)
        {
            ReadBaseImageResourceId(imageProxy, &observedResourceId);
        }
        void* replacementInstruction = nullptr;
        void* replacementImageProxy = nullptr;
        int replacementIndex = -1;
        int ownerInstructionCount = 0;
        unsigned int replacementCount = 0;
        bool uniqueOwnerCandidate = FindUniqueBaseImageInstruction(
            renderVisual, observedResourceId, &replacementInstruction,
            &replacementImageProxy, &replacementIndex,
            &ownerInstructionCount, &replacementCount);
        ownerRenderLists += ownerInstructionCount > 0;
        ownerListCandidates += replacementCount;
        if (uniqueOwnerCandidate &&
            TryInstallLiveBaseImageMeshCanary(
                renderVisual, replacementInstruction, replacementImageProxy,
                currentWindowData, target))
        {
            long result = g_renderDataVisualUpdateRenderData(renderVisual);
            Wh_Log(L"True 4x4 owner-list targeted publish: HWND=%p "
                   L"index=%d/%d resourceId=%u result=0x%08X",
                   target, replacementIndex, ownerInstructionCount,
                   observedResourceId, static_cast<unsigned int>(result));
            return;
        }
    }

    // Existing windows can keep a render list that was built before our
    // observation hooks were installed. CTopLevelWindow3D is also the
    // CRenderDataVisual base on this verified ABI, so inspect its current list
    // directly. CDrawBitmapInstruction stores its CBaseImageProxy at +0x10;
    // accept the field only when the image vtable, backing and resource ID all
    // validate as live uDWM objects.
    void* topLevelWindow = nullptr;
    void* renderVisual = nullptr;
    void** instructions = nullptr;
    int instructionCount = 0;
    unsigned int directCandidates = 0;
    void* directInstruction = nullptr;
    void* directImageProxy = nullptr;
    int directInstructionIndex = -1;
    unsigned int directSourceCandidates = 0;
    void* directSourceImage = nullptr;
    size_t directSourceOffset = SIZE_MAX;
    void* probedSourceImage = nullptr;
    void* probedSourceVtable = nullptr;
    void* probedSourceBacking = nullptr;
    unsigned int probedSourceResourceId = 0;
    MeshSourceKind probedSourceKind = MeshSourceKind::None;
    if (ResolveDwmWindowObjects(currentWindowData, &topLevelWindow,
                                &renderVisual) &&
        IsDwmObjectPointerValid(renderVisual, g_topLevelWindow3DVtable))
    {
        ProbeCompleteWindowRootTree(target, topLevelWindow);
        for (unsigned int index = 0;
             index < g_ensureRenderDataPointerOffsetCount; index++)
        {
            size_t offset = g_ensureRenderDataPointerOffsets[index];
            void* imageProxy = ReadPointerMember(renderVisual, offset);
            if (index == 0)
            {
                directSourceOffset = offset;
                probedSourceImage = imageProxy;
                if (imageProxy &&
                    IsReadableMemory(imageProxy, sizeof(void*)))
                {
                    probedSourceVtable =
                        *reinterpret_cast<void**>(imageProxy);
                    probedSourceBacking =
                        ReadPointerMember(imageProxy, 0x10);
                    probedSourceKind = GetMeshSourceKind(imageProxy);
                    ReadBaseImageResourceId(
                        imageProxy, &probedSourceResourceId);
                }
            }
            if (!imageProxy || !IsReadableMemory(imageProxy, sizeof(void*)) ||
                !IsDwmImageAddress(*reinterpret_cast<void**>(imageProxy),
                                   sizeof(void*)) ||
                GetMeshSourceKind(imageProxy) != MeshSourceKind::None)
            {
                continue;
            }
            unsigned int resourceId = 0;
            if (!ReadBaseImageResourceId(imageProxy, &resourceId))
            {
                continue;
            }
            directSourceCandidates++;
            directSourceImage = imageProxy;
            directSourceOffset = offset;
        }
        GetRenderDataInstructionList(renderVisual, &instructions, nullptr,
                                     &instructionCount);
        if (instructions)
        {
            for (int index = 0; index < instructionCount; index++)
            {
                void* instruction = instructions[index];
                void* imageProxy = ReadPointerMember(instruction, 0x10);
                if (!instruction || !imageProxy ||
                    !IsReadableMemory(imageProxy, sizeof(void*)) ||
                    !IsDwmImageAddress(*reinterpret_cast<void**>(imageProxy),
                                       sizeof(void*)) ||
                    GetMeshSourceKind(imageProxy) != MeshSourceKind::None)
                {
                    continue;
                }
                unsigned int resourceId = 0;
                if (!ReadBaseImageResourceId(imageProxy, &resourceId))
                {
                    continue;
                }
                directCandidates++;
                directInstruction = instruction;
                directImageProxy = imageProxy;
                directInstructionIndex = index;
            }
        }
    }
    if (directCandidates == 1 &&
        TryInstallLiveBaseImageMeshCanary(
            renderVisual, directInstruction, directImageProxy,
            currentWindowData, target))
    {
        long result = g_renderDataVisualUpdateRenderData(renderVisual);
        Wh_Log(L"True 4x4 direct targeted publish: HWND=%p index=%d/%d "
               L"result=0x%08X",
               target, directInstructionIndex, instructionCount,
               static_cast<unsigned int>(result));
        return;
    }

    static HWND lastMissTarget = nullptr;
    static ULONGLONG lastMissTime = 0;
    ULONGLONG now = GetTickCount64();
    if (lastMissTarget != target || now - lastMissTime >= 1000)
    {
        lastMissTarget = target;
        lastMissTime = now;
        Wh_Log(L"True 4x4 target pending: HWND=%p WindowData=%p "
               L"ownerMatches=%u liveInstructions=%u renderVisual=%p "
               L"ownerLists=%u ownerCandidates=%u "
               L"directInstructions=%d directCandidates=%u "
               L"sourceOffsets=%u sourceCandidates=%u "
               L"sourceOffset=0x%zx sourceImage=%p sourceVtable=%p "
               L"sourceBacking=%p sourceResourceId=%u sourceKind=%d "
               L"topLevelWindow=%p "
               L"ensureCalls=%u ensureMapped=%u ensurePopulated=%u "
               L"drawBitmap=%u matchedAdds=%u",
               target, currentWindowData, ownerMatches,
               liveInstructionMatches, renderVisual, ownerRenderLists,
               ownerListCandidates, instructionCount, directCandidates,
               g_ensureRenderDataPointerOffsetCount,
               directSourceCandidates, directSourceOffset,
               probedSourceImage ? probedSourceImage : directSourceImage,
               probedSourceVtable, probedSourceBacking,
               probedSourceResourceId,
               static_cast<int>(probedSourceKind), topLevelWindow,
               g_ensureRenderDataCallCount.load(std::memory_order_relaxed),
               g_ensureRenderDataMappedCount.load(std::memory_order_relaxed),
               g_ensureRenderDataPopulatedCount.load(
                   std::memory_order_relaxed),
               g_observedDrawBitmapCreateCount.load(
                   std::memory_order_relaxed),
               g_observedImageInstructionMatchedAddCount.load(
                   std::memory_order_relaxed));
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
    MaintainVisibleMeshCanary();
    InstallRequestedLiveBaseImageMeshCanary();
    // Restore retiring/quiet windows before discovery, creation or normal rendering.
    RestorePendingAnimationIdentities();
    if (!g_unloading.load(std::memory_order_acquire))
    {
        if (NATIVE_MESH_WRITE_PROBE_ENABLED ||
            NATIVE_MESH_TRANSACTION_PROBE_ENABLED)
        {
            RunNativeMeshCanary();
        }
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
    return g_nativeMeshCanaryPending.load(std::memory_order_acquire) ||
           g_visibleMeshCanaryCleanupRequested.load(
               std::memory_order_acquire) ||
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
    if (g_unloading.load(std::memory_order_acquire) && !HasAnyAnimationSlots())
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
    HWND previousMeshTarget =
        g_liveBaseImageMeshTargetHwnd.exchange(hwnd,
                                                std::memory_order_acq_rel);
    if (previousMeshTarget != hwnd)
    {
        g_liveBaseImageMeshCanaryStarted.store(false,
                                                std::memory_order_release);
        g_liveBaseImageMeshCanarySucceeded.store(false,
                                                  std::memory_order_release);
        g_liveBaseImageMeshAnimationLogged.store(false,
                                                  std::memory_order_release);
    }
    HWND activeMeshWindow =
        g_visibleMeshCanaryHwnd.load(std::memory_order_acquire);
    if (activeMeshWindow && activeMeshWindow != hwnd)
    {
        RequestVisibleMeshCleanupForHwnd(activeMeshWindow);
    }
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
    g_realResizing = operationTypeKnown && operationResizing;
    g_moveTypeKnown = operationTypeKnown;
    g_dragResizeWobbleEnabled = activeSettings.resizeWobbleEnabled;
    if (!EnsureAnimationClockRunning(hwnd))
    {
        Wh_Log(L"Failed to start drag animation timer");
        RetireAnimationSlot(slotIndex);
        return;
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
        bool retiring;
        bool dragging;
        ULONGLONG generation;
        ULONGLONG meshRevision;
        HWND hwnd;
        void* matrixTransformProxy;
        WobbleMesh mesh;
    } snapshot = {};
    bool identityUpdate = false;
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
        snapshot.retiring = slot.retiring;
        snapshot.dragging = slot.dragging;
        snapshot.generation = slot.generation;
        snapshot.meshRevision = slot.meshRevision;
        snapshot.hwnd = slot.hwnd;
        snapshot.matrixTransformProxy = slot.matrixTransformProxy;
        identityUpdate = slot.retiring || slot.meshIdentityPending || !slot.mesh.active ||
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
    bool nativeMeshTarget =
        g_visibleMeshCanaryActive.load(std::memory_order_acquire) &&
        !g_visibleMeshCanary.renderVisual &&
        g_visibleMeshCanary.hwnd == snapshot.hwnd &&
        g_visibleMeshCanary.nativeBindingCount > 0 &&
        GetTickCount64() >= g_visibleMeshCanary.detachAt;
    bool nativeMeshApplied = false;
    if (nativeMeshTarget)
    {
        double maximumDisplacement = 0.0;
        double maximumResidual = 0.0;
        if (!identityUpdate)
        {
            Vec2 anchorDisplacement = {};
            if (snapshot.mesh.dragPointIndex >= 0 &&
                snapshot.mesh.dragPointIndex < GRID_POINT_COUNT)
            {
                const WobblePoint& anchor =
                    snapshot.mesh.points[snapshot.mesh.dragPointIndex];
                anchorDisplacement = {
                    anchor.position.x - anchor.basePosition.x,
                    anchor.position.y - anchor.basePosition.y};
            }
            else
            {
                for (const WobblePoint& point : snapshot.mesh.points)
                {
                    anchorDisplacement.x +=
                        point.position.x - point.basePosition.x;
                    anchorDisplacement.y +=
                        point.position.y - point.basePosition.y;
                }
                anchorDisplacement.x /= GRID_POINT_COUNT;
                anchorDisplacement.y /= GRID_POINT_COUNT;
            }
            for (int i = 0; i < GRID_POINT_COUNT; i++)
            {
                double dx = snapshot.mesh.points[i].position.x -
                            snapshot.mesh.points[i].basePosition.x;
                double dy = snapshot.mesh.points[i].position.y -
                            snapshot.mesh.points[i].basePosition.y;
                maximumDisplacement = std::max(
                    maximumDisplacement, std::sqrt(dx * dx + dy * dy));
                dx -= anchorDisplacement.x;
                dy -= anchorDisplacement.y;
                maximumResidual = std::max(
                    maximumResidual, std::sqrt(dx * dx + dy * dy));
            }
        }
        long nativeResult = UpdateAllNativeMeshGeometry(
            identityUpdate ? nullptr : &snapshot.mesh);
        if (nativeResult >= 0)
        {
            if (!identityUpdate && maximumDisplacement > 1.0 &&
                !g_liveBaseImageMeshAnimationLogged.exchange(
                    true, std::memory_order_acq_rel))
            {
                const WobblePoint& corner = snapshot.mesh.points[0];
                const WobblePoint& inner = snapshot.mesh.points[5];
                Wh_Log(L"True 4x4 native animation active: HWND=%p "
                       L"bindings=%u MaxDisplacement=%.2f MaxResidual=%.2f "
                       L"residualGain=4 size=%.0fx%.0f "
                       L"cornerDelta=(%.2f,%.2f) innerDelta=(%.2f,%.2f)",
                       snapshot.hwnd,
                       g_visibleMeshCanary.nativeBindingCount,
                       maximumDisplacement, maximumResidual,
                       snapshot.mesh.width, snapshot.mesh.height,
                       corner.position.x - corner.basePosition.x,
                       corner.position.y - corner.basePosition.y,
                       inner.position.x - inner.basePosition.x,
                       inner.position.y - inner.basePosition.y);
            }
            if (identityUpdate)
            {
                MilMatrix3x2D identityMatrix = {1.0, 0.0, 0.0, 1.0,
                                                0.0, 0.0};
                long matrixResult = UpdateMatrixTransformProxy(
                    snapshot.matrixTransformProxy, identityMatrix);
                finishUpdate(matrixResult, matrixResult >= 0);
                return;
            }
            // Keep the established whole-window affine motion and add only the
            // non-rigid residual through the native mesh.
            nativeMeshApplied = true;
        }
        else
        {
            g_visibleMeshCanaryCleanupRequested.store(
                true, std::memory_order_release);
            RequestDwmScenePass();
            Wh_Log(L"True 4x4 native animation update failed for HWND=%p: "
                   L"0x%08X; using affine fallback",
                   snapshot.hwnd, static_cast<unsigned int>(nativeResult));
        }
    }
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
    if (updateResult < 0 && nativeMeshApplied)
    {
        UpdateAllNativeMeshGeometry();
        g_visibleMeshCanaryCleanupRequested.store(true,
                                                   std::memory_order_release);
        RequestDwmScenePass();
    }
    finishUpdate(updateResult, false);
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
    if (g_visibleMeshCanaryActive.load(std::memory_order_acquire))
    {
        g_visibleMeshCanaryCleanupRequested.store(true,
                                                   std::memory_order_release);
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
        bool visibleCanaryActive =
            g_visibleMeshCanaryActive.load(std::memory_order_acquire);
        if (!hookUsers && !retainedProxies && !visibleCanaryActive)
        {
            break;
        }
        ULONGLONG now = GetTickCount64();
        if (now >= hookWaitDeadline)
        {
            Wh_Log(L"DWM cleanup timeout: pendingIdentities=%u retainedProxies=%u "
                   L"hookUsers=%u wakePending=%d",
                   pendingIdentities, retainedProxies, hookUsers, wakePending);
            cleanupTimedOut = true;
            break;
        }
        ULONGLONG lastWakePost = g_sceneWakePostTimestamp.load(std::memory_order_acquire);
        if ((retainedProxies || visibleCanaryActive) &&
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
            RequestVisibleMeshCleanupForHwnd(hwnd);
            HWND expected = hwnd;
            g_liveBaseImageMeshTargetHwnd.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel,
                std::memory_order_acquire);
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
    while (running)
    {
        bool frameDue = false;
        DWORD waitResult = MsgWaitForMultipleObjectsEx(
            ARRAYSIZE(waitHandles), waitHandles, INFINITE, QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_OBJECT_0)
        {
            frameDue = true;
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
    Wh_Log(L"Settings: "
           L"WobblinessPreset=%d, "
           L"Advanced=%d, "
           L"ResizeWobble=%d, "
           L"WindowStateWobble=%d, "
           L"Stiffness=%.2f, "
           L"Drag=%.2f, "
           L"MoveFactor=%.2f",
           wobbliness, advancedMode, settings.resizeWobbleEnabled,
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
    if (HasRecentDwmCrashLoop())
    {
        Wh_Log(L"Refusing to load: repeated recent Dwminit warnings; "
               L"disable the mod and investigate DWM stability");
        return FALSE;
    }
    g_unloading.store(false, std::memory_order_release);
    g_dwmSceneWakeMessage =
        RegisterWindowMessageW(L"Windhawk.WobblyWindows.DwmSceneWake");
    if (g_dwmSceneWakeMessage == 0)
    {
        Wh_Log(L"Failed to register DWM scene wake message");
        return FALSE;
    }
    g_desktopManagerThreadIdOffset = SIZE_MAX;
    g_topLevelWindow3DWindowDataOffset = SIZE_MAX;
    g_renderDataInstructionsOffset = SIZE_MAX;
    g_renderDataInstructionCountOffset = SIZE_MAX;
    g_visualParentOffset = SIZE_MAX;
    g_visualContentOffset = SIZE_MAX;
    g_visualCollectionArrayOffset = SIZE_MAX;
    g_visualCollectionCountOffset = SIZE_MAX;
    g_visualCollectionVtable.store(nullptr, std::memory_order_release);
    std::fill_n(g_ensureRenderDataPointerOffsets,
                ARRAYSIZE(g_ensureRenderDataPointerOffsets), SIZE_MAX);
    g_ensureRenderDataPointerOffsetCount = 0;
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
    g_nativeMeshCanaryPending.store(false, std::memory_order_release);
    g_nativeMeshCanarySucceeded.store(false, std::memory_order_release);
    g_liveBaseImageMeshCanaryStarted.store(false, std::memory_order_release);
    g_liveBaseImageMeshCanarySucceeded.store(false,
                                              std::memory_order_release);
    g_liveBaseImageMeshAnimationLogged.store(false,
                                              std::memory_order_release);
    g_liveBaseImageMeshTargetHwnd.store(nullptr,
                                         std::memory_order_release);
    g_meshSourceProbePending.store(false, std::memory_order_release);
    g_meshSourceProbeCompleted.store(false, std::memory_order_release);
    g_cachedVisualImageCanaryCompleted.store(false, std::memory_order_release);
    g_visibleMeshCanary = {};
    g_visibleMeshCanaryActive.store(false, std::memory_order_release);
    g_visibleMeshCanaryCleanupRequested.store(false,
                                               std::memory_order_release);
    g_visibleMeshCanaryHwnd.store(nullptr, std::memory_order_release);
    g_nativeRenderSlotProbe = {};
    g_nativePublishProbeSamples.store(0, std::memory_order_release);
    g_nativePublishProbeChanges.store(0, std::memory_order_release);
    g_nativePublishProbeMissing.store(0, std::memory_order_release);
    auto resetObservedNodes = [](ObservedVisualProxy* table)
    {
        for (unsigned int index = 0; index < OBSERVED_VISUAL_PROXY_COUNT; index++)
        {
            table[index].content.store(nullptr, std::memory_order_relaxed);
            table[index].parent.store(nullptr, std::memory_order_relaxed);
            table[index].redirectTarget.store(nullptr, std::memory_order_relaxed);
            table[index].proxy.store(nullptr, std::memory_order_relaxed);
        }
    };
    resetObservedNodes(g_observedVisualProxies);
    resetObservedNodes(g_observedVisuals);
    for (std::atomic<void*>& proxy : g_observedBitmapSourceProxies)
    {
        proxy.store(nullptr, std::memory_order_relaxed);
    }
    for (std::atomic<void*>& proxy : g_observedVisualSurfaceProxies)
    {
        proxy.store(nullptr, std::memory_order_relaxed);
    }
    g_observedBitmapSourceCreateCount.store(0, std::memory_order_relaxed);
    g_observedVisualSurfaceCreateCount.store(0, std::memory_order_relaxed);
    g_observedDrawBitmapCreateCount.store(0, std::memory_order_relaxed);
    g_observedDrawTileCreateCount.store(0, std::memory_order_relaxed);
    g_observedImageInstructionMatchedAddCount.store(
        0, std::memory_order_relaxed);
    g_ensureRenderDataCallCount.store(0, std::memory_order_relaxed);
    g_ensureRenderDataMappedCount.store(0, std::memory_order_relaxed);
    g_ensureRenderDataPopulatedCount.store(0, std::memory_order_relaxed);
    g_windowBorderCloneCallCount.store(0, std::memory_order_relaxed);
    g_livePreviewCloneCallCount.store(0, std::memory_order_relaxed);
    g_secondaryRepresentationCallCount.store(0, std::memory_order_relaxed);
    g_topLevelWindow3DSetParentCallCount.store(0,
                                               std::memory_order_relaxed);
    g_topLevelWindow3DShowWindowCallCount.store(0,
                                                std::memory_order_relaxed);
    g_trackedVisualVisibilityCallCount.store(0,
                                              std::memory_order_relaxed);
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
    if (g_meshGeometry2dProxyUpdate && g_createMeshGeometry2dProxy &&
        g_createGeometry2dGroupProxy && g_geometry2dGroupProxyUpdate)
    {
        g_nativeMeshCanaryPending.store(true, std::memory_order_release);
        RequestDwmScenePass();
    }
}

void Wh_ModSettingsChanged()
{
    Wh_Log(L"Settings changed");
    LoadSettings();
}

void Wh_ModBeforeUninit()
{
    g_unloading.store(true, std::memory_order_release);
    g_nativeMeshCanaryPending.store(false, std::memory_order_release);
    g_meshSourceProbePending.store(false, std::memory_order_release);
    g_visibleMeshCanaryCleanupRequested.store(true,
                                               std::memory_order_release);
    Wh_Log(L"Preparing to unload");
    // Restore scene resources before Windhawk removes the hooks.
    StopWindowEventThread();
    UninitializeDpiSupport();
}

