#include "hvh_api.h"
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct Module { std::uintptr_t base = 0; std::wstring path; };

std::vector<DWORD> find_processes(const wchar_t* name) {
    std::vector<DWORD> result;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (snapshot && Process32FirstW(snapshot.value, &entry)) do {
        if (_wcsicmp(entry.szExeFile, name) == 0) result.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot.value, &entry));
    return result;
}

Module find_module(DWORD pid, const wchar_t* name) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (!snapshot && GetLastError() == ERROR_BAD_LENGTH) { Sleep(50); continue; }
        MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (snapshot && Module32FirstW(snapshot.value, &entry)) do {
            if (_wcsicmp(entry.szModule, name) == 0)
                return {reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.szExePath};
        } while (Module32NextW(snapshot.value, &entry));
        break;
    }
    return {};
}

std::uintptr_t remote_system_function(DWORD pid, const char* name) {
    auto function = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    HMODULE owner = nullptr;
    if (!function || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(function), &owner)) return 0;
    wchar_t path[32768]{}; GetModuleFileNameW(owner, path, 32768);
    const auto remote = find_module(pid, std::filesystem::path(path).filename().c_str());
    if (!remote.base) return 0;
    return remote.base + reinterpret_cast<std::uintptr_t>(function) - reinterpret_cast<std::uintptr_t>(owner);
}

bool invoke(HANDLE process, std::uintptr_t address, void* argument, DWORD& result) {
    if (!address) { fwprintf(stderr, L"Remote function could not be resolved.\n"); return false; }
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(address), argument, 0, nullptr));
    if (!thread) { fwprintf(stderr, L"CreateRemoteThread failed: %lu\n", GetLastError()); return false; }
    const auto wait = WaitForSingleObject(thread.value, 15000);
    if (wait != WAIT_OBJECT_0) {
        fwprintf(stderr, L"Remote call did not finish (wait=%lu). Target left running; no remote memory was freed.\n", wait);
        return false;
    }
    return GetExitCodeThread(thread.value, &result) != FALSE;
}

std::uintptr_t export_rva(HMODULE local, const char* name) {
    const auto address = GetProcAddress(local, name);
    return address ? reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(local) : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    bool unload = false;
    DWORD pid = 0;
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    std::filesystem::path dll = std::filesystem::path(executable).parent_path() / L"hvh_menu.dll";
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--unload") == 0) unload = true;
        else if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            wchar_t* end = nullptr; const auto value = wcstoul(argv[++i], &end, 10);
            if (!value || !end || *end) { fwprintf(stderr, L"Invalid PID.\n"); return 2; }
            pid = value;
        } else if (wcscmp(argv[i], L"--dll") == 0 && i + 1 < argc) dll = std::filesystem::absolute(argv[++i]);
        else { wprintf(L"Usage: hvh_loader.exe [--pid PID] [--dll PATH] [--unload]\n"); return 2; }
    }
    if (!pid) {
        const auto matches = find_processes(L"cs2.exe");
        if (matches.size() != 1) { fwprintf(stderr, L"Expected one running cs2.exe (found %zu). Use --pid PID to select a process.\n", matches.size()); return 2; }
        pid = matches[0];
    }
    Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid));
    if (!process) { fwprintf(stderr, L"OpenProcess failed: %lu. Loader and target need matching privileges.\n", GetLastError()); return 3; }
    USHORT process_machine = 0, native_machine = 0;
    if (!IsWow64Process2(process.value, &process_machine, &native_machine) || process_machine != IMAGE_FILE_MACHINE_UNKNOWN || native_machine != IMAGE_FILE_MACHINE_AMD64) {
        fwprintf(stderr, L"The target must be a native Windows x64 process.\n"); return 3;
    }
    if (!std::filesystem::is_regular_file(dll)) { fwprintf(stderr, L"DLL not found: %ls\n", dll.c_str()); return 2; }
    // Map exports locally without running DllMain or loading DLL dependencies.
    HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) { fwprintf(stderr, L"Cannot read DLL exports: %lu\n", GetLastError()); return 3; }
    const auto start = export_rva(local, "HvH_Start"), stop = export_rva(local, "HvH_Stop"), state = export_rva(local, "HvH_GetStatus");
    FreeLibrary(local);
    if (!start || !stop || !state) { fwprintf(stderr, L"DLL is missing HvH API exports.\n"); return 3; }
    auto remote = find_module(pid, dll.filename().c_str());
    if (remote.base && _wcsicmp(remote.path.c_str(), dll.c_str()) != 0) {
        fwprintf(stderr, L"A DLL with this filename is already loaded from a different path: %ls\n", remote.path.c_str()); return 3;
    }
    DWORD result = 0;
    if (unload) {
        if (!remote.base) { wprintf(L"DLL is not loaded.\n"); return 0; }
        if (!invoke(process.value, remote.base + stop, nullptr, result) || !result) {
            fwprintf(stderr, L"Stop failed. DLL remains loaded to avoid an unsafe unload. See hvh-menu.log.\n"); return 4;
        }
        if (!invoke(process.value, remote_system_function(pid, "FreeLibrary"), reinterpret_cast<void*>(remote.base), result) || !result) return 4;
        if (find_module(pid, dll.filename().c_str()).base) { fwprintf(stderr, L"Hooks stopped; another module reference still keeps the DLL loaded.\n"); return 4; }
        wprintf(L"DLL stopped and unloaded from PID %lu.\n", pid); return 0;
    }
    if (!remote.base) {
        const std::wstring text = dll.wstring(); const auto bytes = (text.size() + 1) * sizeof(wchar_t);
        void* memory = VirtualAllocEx(process.value, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!memory) { fwprintf(stderr, L"VirtualAllocEx failed: %lu\n", GetLastError()); return 3; }
        SIZE_T written = 0;
        if (!WriteProcessMemory(process.value, memory, text.c_str(), bytes, &written) || written != bytes) {
            VirtualFreeEx(process.value, memory, 0, MEM_RELEASE); return 3;
        }
        if (!invoke(process.value, remote_system_function(pid, "LoadLibraryW"), memory, result)) return 4;
        VirtualFreeEx(process.value, memory, 0, MEM_RELEASE);
        // GetExitCodeThread truncates HMODULE on x64; enumerate the real module base.
        remote = find_module(pid, dll.filename().c_str());
        if (!remote.base) { fwprintf(stderr, L"LoadLibrary did not load the DLL. Check dependencies and hvh-menu.log.\n"); return 4; }
    }
    if (!invoke(process.value, remote.base + start, nullptr, result) || !result) return 4;
    for (int i = 0; i < 80; ++i) {
        if (!invoke(process.value, remote.base + state, nullptr, result)) return 4;
        if (result == static_cast<DWORD>(HvhStatus::hooked) || result == static_cast<DWORD>(HvhStatus::rendering)) {
            wprintf(L"DX11 hooks active in PID %lu. INSERT: menu. Use --unload to stop.\n", pid); return 0;
        }
        if (result == static_cast<DWORD>(HvhStatus::failed) || result == static_cast<DWORD>(HvhStatus::stopped)) break;
        Sleep(100);
    }
    fwprintf(stderr, L"Hook initialization failed or timed out. See hvh-menu.log next to the DLL.\n"); return 4;
}
