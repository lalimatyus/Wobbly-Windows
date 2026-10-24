#pragma once

#include <windows.h>
#include <cstddef>

namespace mesh_bridge {
constexpr wchar_t name[] = L"Local\\Windhawk.WobblyWindows.MeshBridge.v1";
constexpr DWORD magic = 0x48535757;
constexpr DWORD version = 1;
constexpr unsigned slot_count = 6;

struct Point {
    float baseX, baseY, x, y;
};
struct Payload {
    DWORD active, reserved;
    ULONGLONG hwnd, generation, timestamp;
    float width, height;
    Point points[16];
};
struct Slot {
    volatile LONG sequence;
    LONG reserved;
    Payload payload;
};
struct Data {
    DWORD magic, version, size, ownerPid;
    Slot slots[slot_count];
};
static_assert(sizeof(Point) == 16);
static_assert(sizeof(Payload) == 296);
static_assert(sizeof(Slot) == 304);
static_assert(offsetof(Data, slots) == 16);
} // namespace mesh_bridge
