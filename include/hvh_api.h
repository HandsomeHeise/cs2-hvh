#pragma once
#include <windows.h>
#include <cstdint>

enum class HvhStatus : DWORD { idle, starting, hooked, rendering, stopping, stopped, failed };
struct HvhStats {
    std::uint32_t size = sizeof(HvhStats);
    HvhStatus status = HvhStatus::idle;
    std::uint64_t presents = 0;
    std::uint64_t frames = 0;
    std::uint64_t resizes = 0;
    std::uint64_t inputs = 0;
    BOOL menu_visible = FALSE;
};

// Start/Stop/GetStatus also match LPTHREAD_START_ROUTINE for the x64 loader.
using HvhThreadFunction = DWORD (WINAPI*)(void*);
using HvhStatsFunction = BOOL (WINAPI*)(HvhStats*);
using HvhVisibilityFunction = void (WINAPI*)(BOOL);

#ifdef HVH_EXPORTS
#define HVH_API extern "C" __declspec(dllexport)
HVH_API DWORD WINAPI HvH_Start(void*);
HVH_API DWORD WINAPI HvH_Stop(void*);
HVH_API DWORD WINAPI HvH_GetStatus(void*);
HVH_API BOOL WINAPI HvH_GetStats(HvhStats*);
HVH_API void WINAPI HvH_SetMenuVisible(BOOL);
HVH_API void WINAPI HvH_RequestStop();
#endif
