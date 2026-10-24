#include <windows.h>
#include <cwchar>

namespace {

constexpr UINT_PTR kTimerId = 1;
unsigned int g_frame = 0;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_CREATE:
        SetTimer(hwnd, kTimerId, 250, nullptr);
        return 0;
    case WM_TIMER:
        if (wparam == kTimerId)
        {
            ++g_frame;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT area = {};
        GetClientRect(hwnd, &area);
        HBRUSH background = CreateSolidBrush(g_frame % 2 ? RGB(25, 110, 180)
                                                 : RGB(170, 65, 35));
        FillRect(dc, &area, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HFONT font = CreateFontW(84, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        wchar_t label[64];
        std::swprintf(label, sizeof(label) / sizeof(label[0]), L"Frame %u", g_frame);
        TextOutW(dc, 35, 70, label, lstrlenW(label));
        SelectObject(dc, oldFont);
        DeleteObject(font);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    WNDCLASSW windowClass = {};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = L"WobblyLiveSourceFrameTest";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&windowClass))
        return 1;
    HWND window = CreateWindowExW(0, windowClass.lpszClassName,
                                 L"Wobbly live-image source test", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 900, 500,
                                 nullptr, nullptr, instance, nullptr);
    if (!window)
        return 2;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
