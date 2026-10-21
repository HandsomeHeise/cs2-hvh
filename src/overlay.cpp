#define HVH_EXPORTS
#include "hvh_api.h"
#include "com_ptr.hpp"
#include "menu.hpp"
#include "skin_runtime.hpp"
#include "skins.hpp"
#include "aim.hpp"
#include "server_rules.hpp"
#include "esp.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "MinHook.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using Resize = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using SdlSet = bool (*)(void*, bool);
using SdlGet = bool (*)(void*);
using SdlFocus = void* (*)();

HMODULE module = nullptr;
HANDLE worker_handle = nullptr;
HMODULE resident_reference = nullptr;
std::mutex lifecycle_mutex;
std::mutex log_mutex;
std::recursive_mutex ui_mutex;
std::wstring directory;
std::atomic<HvhStatus> status{HvhStatus::idle};
std::atomic<bool> stop_requested{false}, stopping{false}, dirty{false}, menu_open{false};
std::atomic<int> requested_visibility{-1};
std::atomic<unsigned> callbacks{0};
std::atomic<std::uint64_t> present_count{0}, frame_count{0}, resize_count{0}, input_count{0};
std::atomic<HWND> window{nullptr};
std::atomic<WNDPROC> previous_proc{nullptr};
UINT detach_message = 0, input_message = 0;
ImGuiContext* imgui = nullptr;
IDXGISwapChain* selected_chain = nullptr; // Identity only; never dereferenced outside an active callback.
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<ID3D11RenderTargetView> target;
bool resizing = false;
bool renderer_ready = false;
std::atomic<bool> subclass_attached{false};
Present original_present = nullptr;
Present1 original_present1 = nullptr;
Resize original_resize = nullptr;
void* hooks[5]{};
unsigned hook_count = 0;
bool minhook_initialized = false;
SdlSet original_relative = nullptr, original_grab = nullptr;
SdlGet get_relative = nullptr, get_grab = nullptr;
SdlFocus get_keyboard_focus = nullptr, get_mouse_focus = nullptr;
std::atomic<void*> sdl_window{nullptr};
std::atomic<bool> restore_relative{false}, restore_grab{false};

struct Callback {
    Callback() { callbacks.fetch_add(1, std::memory_order_seq_cst); }
    ~Callback() { callbacks.fetch_sub(1, std::memory_order_seq_cst); }
};

void log(const char* format, ...) {
    char message[1400];
    va_list args; va_start(args, format);
    vsnprintf(message, sizeof(message), format, args); va_end(args);
    std::lock_guard guard(log_mutex);
    OutputDebugStringA(message); OutputDebugStringA("\n");
    const auto path = directory + L"\\hvh-menu.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME time; GetLocalTime(&time);
    char line[1500];
    const int count = snprintf(line, sizeof(line), "%02u:%02u:%02u %s\r\n", time.wHour, time.wMinute, time.wSecond, message);
    DWORD written = 0;
    if (count > 0) WriteFile(file, line, static_cast<DWORD>(std::min<int>(count, sizeof(line) - 1)), &written, nullptr);
    CloseHandle(file);
}

HvhStats snapshot() {
    HvhStats value;
    value.status = status.load(); value.presents = present_count.load();
    value.frames = frame_count.load(); value.resizes = resize_count.load();
    value.inputs = input_count.load(); value.menu_visible = menu_open.load();
    return value;
}

// SDL window operations are issued only from the host's window thread.
void sync_input_on_window_thread(bool visible) {
    if (visible && GetForegroundWindow() == window.load()) {
        if (!sdl_window.load() && get_relative && original_relative) {
            void* focused = get_keyboard_focus ? get_keyboard_focus() : nullptr;
            if (!focused && get_mouse_focus) focused = get_mouse_focus();
            if (focused) {
                restore_relative = get_relative(focused);
                restore_grab = get_grab ? get_grab(focused) : false;
                sdl_window = focused;
                original_relative(focused, false);
                if (original_grab) original_grab(focused, false);
            }
        }
        ClipCursor(nullptr);
    } else if (!visible) {
        if (auto* saved = sdl_window.exchange(nullptr)) {
            if (original_relative) original_relative(saved, restore_relative.load());
            if (original_grab) original_grab(saved, restore_grab.load());
        }
    }
}

void set_visible(bool visible) {
    menu_open = visible;
    aim::set_menu_open(visible);
    if (imgui) ImGui::GetIO().MouseDrawCursor = visible;
    if (const auto hwnd = window.load()) PostMessageW(hwnd, input_message, visible, 0);
}

bool keyboard_message(UINT message) {
    return message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN ||
        message == WM_SYSKEYUP || message == WM_CHAR || message == WM_SYSCHAR;
}

bool mouse_message(UINT message) { return message >= WM_MOUSEFIRST && message <= WM_MOUSELAST; }

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Callback callback;
    const auto original = previous_proc.load();
    {
        std::lock_guard guard(ui_mutex);
        if (message == detach_message && stopping.load()) {
            sync_input_on_window_thread(false);
            if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) != window_proc) return 0;
            SetLastError(0);
            const auto old = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
            if (old || GetLastError() == 0) { subclass_attached = false; return 1; }
            return 0;
        }
        if (!stopping.load() && imgui) {
            ImGui::SetCurrentContext(imgui);
            if (message == input_message) { sync_input_on_window_thread(menu_open.load()); return 0; }
            if ((message == WM_KEYDOWN || message == WM_KEYUP) && wparam == VK_INSERT) {
                if (message == WM_KEYUP) set_visible(!menu_open.load());
                return 0;
            }
            if (message == WM_ACTIVATEAPP && wparam && menu_open.load()) sync_input_on_window_thread(true);
            if (mouse_message(message) || keyboard_message(message) || message == WM_INPUT) ++input_count;
            ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam);
            if (menu_open.load()) {
                if (message == WM_INPUT) return DefWindowProcW(hwnd, message, wparam, lparam);
                if (mouse_message(message) || keyboard_message(message)) return 0;
                if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
            }
        }
    }
    return original ? CallWindowProcW(original, hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}

bool create_target(IDXGISwapChain* chain) {
    ComPtr<ID3D11Texture2D> buffer;
    HRESULT hr = chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(buffer.put()));
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(buffer.get(), nullptr, target.put());
    if (FAILED(hr)) { log("Render target failed: 0x%08lX", static_cast<unsigned long>(hr)); return false; }
    return true;
}

bool initialize_ui(IDXGISwapChain* chain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow || !IsWindowVisible(desc.OutputWindow)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(desc.OutputWindow, &pid);
    RECT area{}; GetClientRect(desc.OutputWindow, &area);
    if (pid != GetCurrentProcessId() || area.right < 200 || area.bottom < 150) return false;
    if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(device.put())))) return false;
    device->GetImmediateContext(context.put());
    if (!context || !create_target(chain)) { device.reset(); context.reset(); return false; }
    IMGUI_CHECKVERSION();
    imgui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
        ImGui::DestroyContext(imgui); imgui = nullptr; target.reset(); context.reset(); device.reset(); return false;
    }
    if (!ImGui_ImplDX11_Init(device.get(), context.get())) {
        ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(imgui); imgui = nullptr;
        target.reset(); context.reset(); device.reset(); return false;
    }
    menu::initialize(directory);
    renderer_ready = true;
    window = desc.OutputWindow;
    // Publish the forwarding address before publishing the subclass itself.
    previous_proc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(desc.OutputWindow, GWLP_WNDPROC));
    SetLastError(0);
    const auto old = SetWindowLongPtrW(desc.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(window_proc));
    if (!old && GetLastError() != 0) {
        log("Window subclass failed: %lu", GetLastError());
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(imgui); imgui = nullptr;
        renderer_ready = false;
        window = nullptr; previous_proc = nullptr;
        target.reset(); context.reset(); device.reset(); return false;
    }
    previous_proc = reinterpret_cast<WNDPROC>(old);
    subclass_attached = true;
    selected_chain = chain;
    set_visible(true);
    status = HvhStatus::rendering;
    log("ImGui initialized, HWND=%p, swapchain=%p", desc.OutputWindow, chain);
    return true;
}

bool rebind_renderer(IDXGISwapChain* chain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc)) || desc.OutputWindow != window.load()) return false;
    if (renderer_ready) { ImGui_ImplDX11_Shutdown(); renderer_ready = false; }
    target.reset(); context.reset(); device.reset();
    if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(device.put())))) return false;
    device->GetImmediateContext(context.put());
    if (!context || !create_target(chain) || !ImGui_ImplDX11_Init(device.get(), context.get())) return false;
    renderer_ready = true; selected_chain = chain;
    log("Renderer rebound to a replacement swapchain/device: %p", chain);
    return true;
}

void render(IDXGISwapChain* chain, UINT flags) {
    if (stopping.load() || (flags & DXGI_PRESENT_TEST)) return;
    std::lock_guard guard(ui_mutex);
    if (stopping.load() || resizing) return;
    if (!imgui && !initialize_ui(chain)) return;
    ImGui::SetCurrentContext(imgui);
    ComPtr<ID3D11Device> current_device;
    if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(current_device.put())))) return;
    if ((chain != selected_chain || !renderer_ready || current_device.get() != device.get()) && !rebind_renderer(chain)) return;
    if (!target && !create_target(chain)) return;
    const int command = requested_visibility.exchange(-1);
    if (command != -1) set_visible(command != 0);
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    bool visible = menu_open.load();
    const auto data = game::latest();
    menu::draw_watermark(); menu::draw_overlay(data, visible); menu::draw(visible, snapshot(), data);
    if (visible != menu_open.load()) set_visible(visible);
    ImGui::Render();
    ID3D11RenderTargetView* saved[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved, &depth);
    auto* view = target.get(); context->OMSetRenderTargets(1, &view, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved, depth);
    for (auto* value : saved) if (value) value->Release();
    if (depth) depth->Release();
    ++frame_count;
}

thread_local bool inside_present = false;
struct PresentScope { PresentScope() { inside_present = true; } ~PresentScope() { inside_present = false; } };

HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* chain, UINT sync, UINT flags) {
    Callback callback;
    if (inside_present) return original_present(chain, sync, flags);
    PresentScope scope;
    ++present_count; render(chain, flags);
    return original_present(chain, sync, flags);
}

HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain1* chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    Callback callback;
    if (inside_present) return original_present1(chain, sync, flags, parameters);
    PresentScope scope;
    ++present_count; render(chain, flags);
    return original_present1(chain, sync, flags, parameters);
}

HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    Callback callback;
    bool ours = false;
    {
        std::lock_guard guard(ui_mutex);
        ours = chain == selected_chain && !stopping.load();
        if (ours) { resizing = true; target.reset(); ++resize_count; }
    }
    const auto result = original_resize(chain, count, width, height, format, flags);
    if (ours) {
        std::lock_guard guard(ui_mutex); resizing = false;
        if (FAILED(result)) log("ResizeBuffers failed: 0x%08lX", static_cast<unsigned long>(result));
        // Recreate the view lazily in the next Present, including failed-resize recovery.
    }
    return result;
}

bool relative_hook(void* hwnd, bool enabled) {
    Callback callback;
    if (!stopping.load() && menu_open.load() && hwnd == sdl_window.load()) {
        restore_relative = enabled; return original_relative(hwnd, false);
    }
    return original_relative(hwnd, enabled);
}

bool grab_hook(void* hwnd, bool enabled) {
    Callback callback;
    if (!stopping.load() && menu_open.load() && hwnd == sdl_window.load()) {
        restore_grab = enabled; return original_grab(hwnd, false);
    }
    return original_grab(hwnd, enabled);
}

bool add_hook(void* address, void* replacement, void** original) {
    const auto result = MH_CreateHook(address, replacement, original);
    if (result != MH_OK) { log("MH_CreateHook: %s", MH_StatusToString(result)); return false; }
    hooks[hook_count++] = address;
    return true;
}

bool install_hooks() {
    const wchar_t* name = L"HvhDx11Bootstrap";
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = module; wc.lpszClassName = name;
    if (!RegisterClassExW(&wc)) { log("Bootstrap window class failed: %lu", GetLastError()); return false; }
    HWND dummy = CreateWindowExW(0, name, L"", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr, nullptr, module, nullptr);
    if (!dummy) { UnregisterClassW(name, module); return false; }
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow = dummy;
    desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Device> dummy_device;
    ComPtr<ID3D11DeviceContext> dummy_context;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &desc, chain.put(), dummy_device.put(), nullptr, dummy_context.put());
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &desc, chain.put(), dummy_device.put(), nullptr, dummy_context.put());
    if (FAILED(hr)) { log("Bootstrap D3D11 failed: 0x%08lX", static_cast<unsigned long>(hr)); DestroyWindow(dummy); UnregisterClassW(name, module); return false; }
    void** table = *reinterpret_cast<void***>(chain.get());
    void* present_address = table[8]; void* resize_address = table[13];
    ComPtr<IDXGISwapChain1> chain1;
    void* present1_address = nullptr;
    if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(chain1.put()))))
        present1_address = (*reinterpret_cast<void***>(chain1.get()))[22];
    const auto init = MH_Initialize();
    bool ok = init == MH_OK;
    minhook_initialized = ok;
    if (!ok) log("MH_Initialize: %s", MH_StatusToString(init));
    if (ok) ok = add_hook(present_address, reinterpret_cast<void*>(present_hook), reinterpret_cast<void**>(&original_present));
    if (ok) ok = add_hook(resize_address, reinterpret_cast<void*>(resize_hook), reinterpret_cast<void**>(&original_resize));
    if (ok && present1_address && present1_address != present_address)
        ok = add_hook(present1_address, reinterpret_cast<void*>(present1_hook), reinterpret_cast<void**>(&original_present1));
    if (ok) {
        if (auto sdl = GetModuleHandleW(L"SDL3.dll")) {
            get_relative = reinterpret_cast<SdlGet>(GetProcAddress(sdl, "SDL_GetWindowRelativeMouseMode"));
            get_grab = reinterpret_cast<SdlGet>(GetProcAddress(sdl, "SDL_GetWindowMouseGrab"));
            get_keyboard_focus = reinterpret_cast<SdlFocus>(GetProcAddress(sdl, "SDL_GetKeyboardFocus"));
            get_mouse_focus = reinterpret_cast<SdlFocus>(GetProcAddress(sdl, "SDL_GetMouseFocus"));
            if (auto address = GetProcAddress(sdl, "SDL_SetWindowRelativeMouseMode")) {
                if (get_relative) ok = add_hook(reinterpret_cast<void*>(address), reinterpret_cast<void*>(relative_hook), reinterpret_cast<void**>(&original_relative));
            }
            if (ok && original_relative) if (auto address = GetProcAddress(sdl, "SDL_SetWindowMouseGrab")) {
                if (get_grab) ok = add_hook(reinterpret_cast<void*>(address), reinterpret_cast<void*>(grab_hook), reinterpret_cast<void**>(&original_grab));
            }
        }
    }
    chain1.reset(); chain.reset(); dummy_context.reset(); dummy_device.reset();
    DestroyWindow(dummy); UnregisterClassW(name, module);
    if (ok) {
        for (unsigned i = 0; i < hook_count; ++i) if (MH_QueueEnableHook(hooks[i]) != MH_OK) ok = false;
        if (ok) ok = MH_ApplyQueued() == MH_OK;
    }
    dirty = hook_count != 0;
    if (ok) log("DXGI hooks installed (%u hooks); Present=%p Resize=%p Present1=%p SDL=%s", hook_count,
        present_address, resize_address, present1_address, original_relative ? "yes" : "no");
    if (ok) { skins::install_runtime(); aim::install_runtime(); server_rules::install_runtime(); }
    return ok;
}

bool cleanup() {
    stopping = true; status = HvhStatus::stopping;
    if (!aim::stop_runtime()) { log("Input callback could not be stopped yet; keeping DLL resident."); return false; }
    if (!server_rules::stop_runtime()) { log("Weapon filter callbacks could not be stopped yet; keeping DLL resident."); return false; }
    if (!skins::stop_runtime()) { log("Cosmetic items or frame callbacks could not be restored yet; keeping DLL resident."); return false; }
    // Restore the subclass on its owning thread before any code can be unloaded.
    const auto hwnd = window.load();
    if (subclass_attached.load() && hwnd && IsWindow(hwnd)) {
        DWORD_PTR detached = 0;
        if (!SendMessageTimeoutW(hwnd, detach_message, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2500, &detached) || !detached) {
            log("Cannot restore WndProc safely. Keeping DLL resident; retry Stop while the window responds."); return false;
        }
    }
    for (unsigned i = 0; i < hook_count; ++i) {
        const auto result = MH_QueueDisableHook(hooks[i]);
        if (result != MH_OK && result != MH_ERROR_DISABLED && result != MH_ERROR_NOT_CREATED) { log("Cannot queue hook disable."); return false; }
    }
    if (hook_count && MH_ApplyQueued() != MH_OK) { log("Cannot disable hooks."); return false; }
    const auto deadline = GetTickCount64() + 5000;
    while (callbacks.load() != 0 && GetTickCount64() < deadline) Sleep(5);
    if (callbacks.load() != 0) { log("Callbacks are still active; keeping DLL resident."); return false; }
    {
        std::lock_guard guard(ui_mutex);
        if (imgui) {
            ImGui::SetCurrentContext(imgui);
            if (renderer_ready) ImGui_ImplDX11_Shutdown();
            renderer_ready = false;
            ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(imgui); imgui = nullptr;
        }
        target.reset(); context.reset(); device.reset(); selected_chain = nullptr;
        window = nullptr; previous_proc = nullptr; menu_open = false; resizing = false;
        subclass_attached = false;
    }
    for (unsigned i = 0; i < hook_count; ++i) {
        const auto removed = MH_RemoveHook(hooks[i]);
        if (removed != MH_OK && removed != MH_ERROR_NOT_CREATED) { log("Cannot remove hook."); return false; }
    }
    hook_count = 0;
    if (minhook_initialized && MH_Uninitialize() != MH_OK) { log("Cannot uninitialize MinHook."); return false; }
    minhook_initialized = false; dirty = false;
    log("Hooks removed; ImGui, input and renderer state released.");
    return true;
}

DWORD WINAPI worker(void* self_reference) {
    wchar_t path[32768]{}; GetModuleFileNameW(module, path, 32768);
    directory = path;
    directory.resize(directory.find_last_of(L"\\/"));
    detach_message = RegisterWindowMessageW(L"HvhMenu.Detach.16BEA198");
    input_message = RegisterWindowMessageW(L"HvhMenu.Input.16BEA198");
    log("Starting x64 DX11 overlay (ImGui %s).", IMGUI_VERSION);
    const bool installed = detach_message && input_message && install_hooks();
    if (installed) {
        // A Present can already have promoted the status to 'rendering'.
        HvhStatus expected = HvhStatus::starting;
        status.compare_exchange_strong(expected, HvhStatus::hooked);
        skins::Status previous_skin_status;
        aim::Status previous_aim_status;
        server_rules::Status previous_rule_status;
        esp::Frame previous_esp;
        std::uint64_t next_skin_log = 0;
        std::uint64_t next_aim_timing_log = 0;
        std::uint64_t next_esp_log = 0;
        while (!stop_requested.load()) {
            game::tick();
            server_rules::poll_runtime();
            if (GetTickCount64() >= next_skin_log) {
                next_skin_log = GetTickCount64() + 1000;
                const auto cosmetic = skins::latest();
                if (cosmetic.state != previous_skin_status.state || cosmetic.detail != previous_skin_status.detail ||
                    cosmetic.applied_weapons != previous_skin_status.applied_weapons || cosmetic.knife_applied != previous_skin_status.knife_applied ||
                    cosmetic.gloves_applied != previous_skin_status.gloves_applied || (cosmetic.frame_callbacks && !previous_skin_status.frame_callbacks)) {
                    log("Cosmetics: %s; %s; weapons=%u knife=%u gloves=%u callbacks=%llu", skins::state_name(cosmetic.state),
                        cosmetic.detail.c_str(), cosmetic.applied_weapons, cosmetic.knife_applied, cosmetic.gloves_applied,
                        static_cast<unsigned long long>(cosmetic.frame_callbacks));
                }
                previous_skin_status = cosmetic;
                const auto aim_status = aim::latest();
                if (aim_status.state != previous_aim_status.state || aim_status.detail != previous_aim_status.detail ||
                    (aim_status.callbacks && !previous_aim_status.callbacks) ||
                    (aim_status.active && GetTickCount64() >= next_aim_timing_log)) {
                    next_aim_timing_log=GetTickCount64()+5000;
                    log("Aim: %s; %s; candidates=%d target=%d callbacks=%llu updates=%llu cpu_ms=%.3f peak_ms=%.3f rays=%d chance_n=%d fire_gate=%s alignment=%.3f chance_pct=%d silent=%u silent_samples=%llu penetrations=%d damage=%.1f", aim::state_name(aim_status.state),
                        aim_status.detail.c_str(), aim_status.candidates, aim_status.target_index,
                        static_cast<unsigned long long>(aim_status.callbacks), static_cast<unsigned long long>(aim_status.updates),
                        aim_status.work_ms,aim_status.peak_ms,aim_status.chance_samples,aim_status.chance_sample_count,
                        aim::fire_block_name(aim_status.fire_block),aim_status.alignment_error,aim_status.hit_chance,
                        aim_status.capabilities.silent ? 1u : 0u,static_cast<unsigned long long>(aim_status.silent_samples),aim_status.penetrations,aim_status.damage);
                }
                previous_aim_status = aim_status;
                const auto rules = server_rules::latest();
                if (rules.state != previous_rule_status.state || rules.detail != previous_rule_status.detail ||
                    rules.spread_value != previous_rule_status.spread_value || rules.recoil_value != previous_rule_status.recoil_value ||
                    (rules.server_calls && !previous_rule_status.server_calls)) {
                    log("Server Rules: %s; %s; spread=%.3f recoil=%.3f observed=%.6f/%.6f/%.6f client=%llu server=%llu",
                        server_rules::state_name(rules.state),rules.detail.c_str(),rules.spread_value,rules.recoil_value,
                        rules.observed_spread,rules.observed_inaccuracy,rules.observed_recoil,
                        static_cast<unsigned long long>(rules.client_calls),static_cast<unsigned long long>(rules.server_calls));
                }
                previous_rule_status=rules;
                const auto esp_frame=esp::latest();
                if (esp_frame.active!=previous_esp.active || esp_frame.detail!=previous_esp.detail ||
                    esp_frame.players.size()!=previous_esp.players.size() || (esp_frame.active && GetTickCount64()>=next_esp_log)) {
                    next_esp_log=GetTickCount64()+5000;
                    log("ESP: %s; active=%u players=%u cpu_ms=%.3f skeletons=%u bones=%u bone_budget_limited=%u",esp_frame.detail.c_str(),esp_frame.active ? 1u : 0u,
                        static_cast<unsigned>(esp_frame.players.size()),esp_frame.work_ms,esp_frame.skeleton_players,esp_frame.bone_queries,esp_frame.skeleton_limited ? 1u : 0u);
                }
                previous_esp=esp_frame;
            }
            Sleep(10);
        }
    }
    const bool clean = cleanup();
    status = clean ? (installed ? HvhStatus::stopped : HvhStatus::failed) : HvhStatus::failed;
    if (!installed) log("Initialization failed; see preceding diagnostics.");
    if (clean) { resident_reference = nullptr; FreeLibraryAndExitThread(static_cast<HMODULE>(self_reference), installed ? 0 : 1); }
    // Retain the self-reference on unsafe cleanup. The loader must not FreeLibrary.
    return 1;
}
}

DWORD WINAPI HvH_Start(void*) {
    std::lock_guard guard(lifecycle_mutex);
    const auto current = status.load();
    if (current == HvhStatus::starting || current == HvhStatus::hooked || current == HvhStatus::rendering) return 1;
    if (dirty.load() || (worker_handle && WaitForSingleObject(worker_handle, 0) != WAIT_OBJECT_0)) return 0;
    if (worker_handle) { CloseHandle(worker_handle); worker_handle = nullptr; }
    HMODULE self_reference = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&HvH_Start), &self_reference)) return 0;
    resident_reference = self_reference;
    stop_requested = false; stopping = false; requested_visibility = -1;
    present_count = 0; frame_count = 0; resize_count = 0; input_count = 0;
    original_relative = nullptr; original_grab = nullptr; get_relative = nullptr; get_grab = nullptr;
    get_keyboard_focus = nullptr; get_mouse_focus = nullptr;
    status = HvhStatus::starting;
    worker_handle = CreateThread(nullptr, 0, worker, self_reference, 0, nullptr);
    if (!worker_handle) { status = HvhStatus::failed; resident_reference = nullptr; FreeLibrary(self_reference); return 0; }
    return 1;
}

DWORD WINAPI HvH_Stop(void*) {
    std::lock_guard guard(lifecycle_mutex);
    stop_requested = true;
    if (worker_handle && WaitForSingleObject(worker_handle, 10000) != WAIT_OBJECT_0) return 0;
    if (dirty.load() && cleanup()) {
        status = HvhStatus::stopped;
        if (resident_reference) { auto reference = resident_reference; resident_reference = nullptr; FreeLibrary(reference); }
    }
    if (worker_handle && !dirty.load()) { CloseHandle(worker_handle); worker_handle = nullptr; }
    return dirty.load() ? 0 : 1;
}

DWORD WINAPI HvH_GetStatus(void*) { return static_cast<DWORD>(status.load()); }
BOOL WINAPI HvH_GetStats(HvhStats* output) {
    if (!output || output->size != sizeof(HvhStats)) return FALSE;
    *output = snapshot(); return TRUE;
}
void WINAPI HvH_SetMenuVisible(BOOL visible) { requested_visibility = visible ? 1 : 0; }
void WINAPI HvH_RequestStop() { stop_requested = true; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) { module = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}
