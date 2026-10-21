#include "hvh_api.h"
#include "com_ptr.hpp"
#include "generated/cosmetics.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
HWND window = nullptr;
bool quit = false;
UINT resize_width = 0, resize_height = 0;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IDXGISwapChain> chain;
ComPtr<ID3D11RenderTargetView> target;
ComPtr<ID3D11VertexShader> vertex_shader;
ComPtr<ID3D11PixelShader> pixel_shader;
bool warp = false, use_present1 = false;
std::filesystem::path directory;
const char* failure = nullptr;

LRESULT CALLBACK host_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SIZE && wparam != SIZE_MINIMIZED) {
        resize_width = LOWORD(lparam); resize_height = HIWORD(lparam); return 0;
    }
    if (message == WM_CLOSE) { quit = true; return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool create_target() {
    ComPtr<ID3D11Texture2D> buffer;
    return SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(buffer.put()))) &&
        SUCCEEDED(device->CreateRenderTargetView(buffer.get(), nullptr, target.put()));
}

bool create_renderer() {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow = window;
    desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, chain.put(), device.put(), nullptr, context.put()))) return false;
    const char* shader = R"(
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output vertex(uint id : SV_VertexID) {
    Output o; o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1); return o;
}
float4 pixel(Output i) : SV_TARGET {
    float2 uv = i.uv;
    float3 sky = lerp(float3(0.43,0.59,0.68), float3(0.78,0.82,0.81), saturate(uv.y * 2));
    float ridge = 0.46 + sin(uv.x*8)*0.035 + sin(uv.x*19)*0.02;
    float terrain = smoothstep(ridge-0.008, ridge+0.008, uv.y);
    float3 ground = lerp(float3(0.31,0.39,0.34), float3(0.16,0.24,0.22), saturate((uv.y-ridge)*2));
    float grid = step(0.975, frac(uv.x*24 + uv.y*5)) * 0.03;
    return float4(lerp(sky, ground+grid, terrain), 1);
})";
    ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vertex", "vs_5_0", 0, 0, vs.put(), errors.put())) ||
        FAILED(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "pixel", "ps_5_0", 0, 0, ps.put(), errors.put()))) return false;
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertex_shader.put())) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, pixel_shader.put()))) return false;
    return create_target();
}

bool draw_frame() {
    if (resize_width && resize_height) {
        context->OMSetRenderTargets(0, nullptr, nullptr); target.reset();
        if (FAILED(chain->ResizeBuffers(0, resize_width, resize_height, DXGI_FORMAT_UNKNOWN, 0)) || !create_target()) return false;
        resize_width = resize_height = 0;
    }
    DXGI_SWAP_CHAIN_DESC desc{}; chain->GetDesc(&desc);
    RECT rect{}; GetClientRect(window, &rect);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(rect.right), static_cast<float>(rect.bottom), 0, 1};
    context->RSSetViewports(1, &viewport);
    auto* rtv = target.get(); context->OMSetRenderTargets(1, &rtv, nullptr);
    context->VSSetShader(vertex_shader.get(), nullptr, 0); context->PSSetShader(pixel_shader.get(), nullptr, 0);
    context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(3, 0);
    HRESULT hr;
    if (use_present1) {
        ComPtr<IDXGISwapChain1> extended;
        if (FAILED(chain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(extended.put())))) return false;
        DXGI_PRESENT_PARAMETERS parameters{}; hr = extended->Present1(0, 0, &parameters);
    } else hr = chain->Present(0, 0);
    if (FAILED(hr)) return false;
    ID3D11RenderTargetView* restored = nullptr;
    context->OMGetRenderTargets(1, &restored, nullptr);
    const bool correct = restored == target.get(); if (restored) restored->Release();
    D3D11_PRIMITIVE_TOPOLOGY topology; context->IAGetPrimitiveTopology(&topology);
    return correct && topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

struct Pixels { UINT width = 0, height = 0; std::vector<unsigned char> rgb; };
Pixels read_pixels() {
    ComPtr<ID3D11Texture2D> buffer, staging;
    if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(buffer.put())))) return {};
    D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc);
    desc.BindFlags = 0; desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put()))) return {};
    context->CopyResource(staging.get(), buffer.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    Pixels pixels{desc.Width, desc.Height, std::vector<unsigned char>(desc.Width * desc.Height * 3)};
    for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x)
        for (UINT c = 0; c < 3; ++c) pixels.rgb[(y * desc.Width + x) * 3 + c] = static_cast<unsigned char*>(mapped.pData)[y * mapped.RowPitch + x * 4 + c];
    context->Unmap(staging.get(), 0); return pixels;
}
std::size_t differences(const Pixels& a, const Pixels& b) {
    if (a.rgb.size() != b.rgb.size()) return 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.rgb.size(); i += 3)
        if (std::abs(a.rgb[i] - b.rgb[i]) + std::abs(a.rgb[i+1] - b.rgb[i+1]) + std::abs(a.rgb[i+2] - b.rgb[i+2]) > 12) ++count;
    return count;
}
void save_pixels(const Pixels& pixels) {
    std::ofstream out(directory / "menu-preview.ppm", std::ios::binary);
    out << "P6\n" << pixels.width << " " << pixels.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(pixels.rgb.data()), static_cast<std::streamsize>(pixels.rgb.size()));
}
void key(UINT value) {
    SendMessageW(window, WM_KEYDOWN, value, 1);
    SendMessageW(window, WM_KEYUP, value, 0xC0000001);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int) {
    const bool smoke = wcsstr(command, L"--smoke-test") != nullptr;
    const bool external = wcsstr(command, L"--external") != nullptr;
    const bool crosshair_test = wcsstr(command, L"--crosshair-test") != nullptr;
    const bool skin_apply_test = wcsstr(command, L"--skin-apply-test") != nullptr;
    const bool skins_test = skin_apply_test || wcsstr(command, L"--skins-test") != nullptr;
    const bool aim_test = wcsstr(command, L"--aim-test") != nullptr;
    const bool esp_details_test=wcsstr(command,L"--esp-details")!=nullptr;
    const bool esp_test = wcsstr(command,L"--esp-test") != nullptr || esp_details_test;
    const bool esp_style_test = wcsstr(command,L"--esp-style") != nullptr;
    int esp_phase=0;
    const bool navigation_test = crosshair_test || skins_test || aim_test || esp_test;
    const bool background = smoke || wcsstr(command, L"--background") != nullptr;
    warp = wcsstr(command, L"--warp") != nullptr;
    use_present1 = wcsstr(command, L"--present1") != nullptr;
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
    directory = std::filesystem::path(path).parent_path();
    const auto profile_path = directory / "hvh-menu.ini";
    float navigation_scale = std::clamp(GetPrivateProfileIntW(L"Menu", L"Scale", 100, profile_path.c_str()) / 100.f, .75f, 1.25f);
    navigation_scale = std::min(navigation_scale, std::min(1500.f / 1100.f, 845.f / 740.f));
    const float menu_left = (1500 - 1016 * navigation_scale) * .5f;
    const float tab_width = (1016.f - 6 * 4) / 7;
    const int navigation_index = aim_test ? 0 : skins_test ? 1 : 2; // AIM, SKINS, VISUALS, MISC, SETTINGS, CONFIG, ABOUT.
    const int navigation_x = static_cast<int>(std::lround(menu_left + (navigation_index * (tab_width + 4) + tab_width * .5f) * navigation_scale));
    const int navigation_y = static_cast<int>(std::lround(845 * .08f + 88.5f * navigation_scale));
    const int selected_tab_x = static_cast<int>(std::lround(menu_left + (navigation_index * (tab_width + 4) + 12) * navigation_scale));
    const int selected_tab_y = static_cast<int>(std::lround(845 * .08f + 78 * navigation_scale));
    const int initial_paint = GetPrivateProfileIntW(L"Skin.7", L"Paint", 0, profile_path.c_str());
    const int initial_stattrak = GetPrivateProfileIntW(L"Skin.7", L"StatTrak", -1, profile_path.c_str());
    struct AimValue { std::wstring section, key; int value; };
    std::vector<AimValue> initial_aim_values;
    const bool aim_host_has_no_game_modules = !GetModuleHandleW(L"client.dll") && !GetModuleHandleW(L"engine2.dll");
    if (aim_test) {
        // The caller supplies an isolated folder. Starting OFF ensures the real
        // master toggle and Save Profile clicks must work for this test to pass.
        if (!aim_host_has_no_game_modules || GetPrivateProfileIntW(L"Aim", L"Enabled", 0, profile_path.c_str())) {
            std::ofstream(directory / "smoke-test.json") << "{\"passed\":false,\"failure\":\"Aim UI test requires initially disabled aim and an ordinary host without CS2 modules\"}\n";
            return 13;
        }
        const auto remember = [&](const wchar_t* section, const wchar_t* key, int fallback) {
            initial_aim_values.push_back({section,key,static_cast<int>(GetPrivateProfileIntW(section,key,fallback,profile_path.c_str()))});
        };
        remember(L"Aim",L"Activation",0); remember(L"Aim",L"Key",5); remember(L"Aim",L"PerWeapon",1);
        remember(L"Aim",L"FOVRing",1); remember(L"Aim",L"TargetMarker",1); remember(L"Aim",L"Info",0);
        for (int group=0;group<9;++group) {
            const auto section=L"Aim.Group."+std::to_wstring(group);
            remember(section.c_str(),L"FOV",30000); remember(section.c_str(),L"OverrideGlobal",0);
        }
    }
    std::vector<std::pair<int, int>> initial_item_states;
    if (skin_apply_test) {
        // Callers use an isolated test-runtime folder. Requiring both switches OFF
        // ensures an old no-op Apply cannot pass by reading an already enabled file.
        if (GetPrivateProfileIntW(L"Skins", L"Enabled", 0, profile_path.c_str()) ||
            GetPrivateProfileIntW(L"Skin.7", L"Enabled", 0, profile_path.c_str())) {
            std::ofstream(directory / "smoke-test.json") << "{\"passed\":false,\"failure\":\"Apply test requires initially disabled global and AK-47 settings\"}\n";
            return 13;
        }
        for (const auto& item : cosmetics::items) if (item.id != 7) {
            const auto section = L"Skin." + std::to_wstring(item.id);
            initial_item_states.emplace_back(item.id, GetPrivateProfileIntW(section.c_str(), L"Enabled", 0, profile_path.c_str()));
        }
    }
    SetProcessDPIAware();
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = host_proc; wc.hInstance = instance;
    wc.lpszClassName = L"HvhDx11TestHost"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassExW(&wc);
    RECT size{0, 0, 1500, 845}; AdjustWindowRect(&size, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowExW(0, wc.lpszClassName, L"HVH / DX11 Test Host", WS_OVERLAPPEDWINDOW,
        background ? -20000 : CW_USEDEFAULT, background ? -20000 : CW_USEDEFAULT,
        size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    if (!window || !create_renderer()) return 10;
    ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
    resize_width = resize_height = 0;
    if (!draw_frame()) return 11;
    const Pixels baseline = read_pixels();
    HMODULE library = nullptr;
    HvhThreadFunction start = nullptr, stop = nullptr;
    HvhStatsFunction stats_function = nullptr;
    HvhVisibilityFunction visibility = nullptr;
    using RequestStop = void (WINAPI*)(); RequestStop request_stop = nullptr;
    auto bind = [&] {
        start = reinterpret_cast<HvhThreadFunction>(GetProcAddress(library, "HvH_Start"));
        stop = reinterpret_cast<HvhThreadFunction>(GetProcAddress(library, "HvH_Stop"));
        stats_function = reinterpret_cast<HvhStatsFunction>(GetProcAddress(library, "HvH_GetStats"));
        visibility = reinterpret_cast<HvhVisibilityFunction>(GetProcAddress(library, "HvH_SetMenuVisible"));
        request_stop = reinterpret_cast<RequestStop>(GetProcAddress(library, "HvH_RequestStop"));
        return start && stop && stats_function && visibility && request_stop;
    };
    if (!external) {
        library = LoadLibraryW((directory / "hvh_menu.dll").c_str());
        if (!library || !bind() || !start(nullptr)) return 12;
    }
    const auto beginning = GetTickCount64();
    int phase = 0;
    int navigation_phase = 0;
    int apply_phase = 0, pointer_x = navigation_x, pointer_y = navigation_y;
    bool skin_apply_saved = false, aim_profile_saved = false;
    int aim_phase = 0;
    std::uint64_t phase_frame = 0;
    std::size_t open_pixels = 0, closed_pixels = 0;
    bool failed_resize_recovered = false, recreated = false, shutdown_requested = false;
    HvhStats stats;
    bool completed = false;
    while (GetTickCount64() - beginning < (smoke ? 30000 : 3600000)) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        // The test window is off-screen: refresh its synthetic pointer position after
        // TrackMouseEvent has delivered the real desktop pointer's WM_MOUSELEAVE.
        if (navigation_test && phase == 0 && navigation_phase > 0)
            SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(pointer_x, pointer_y));
        // External mode deliberately owns no DLL reference and never calls its API:
        // this lets the loader exercise load/start/stop/unload against an ordinary host.
        if (!draw_frame()) { failure = "renderer or state restoration"; break; }
        stats = HvhStats{};
        if (library && stats_function) stats_function(&stats);
        if (quit && library && !shutdown_requested) { request_stop(); shutdown_requested = true; }
        if (quit && (!library || stats.status == HvhStatus::stopped || stats.status == HvhStatus::failed)) break;
        if (stats.status == HvhStatus::failed) { failure = "DLL failed"; break; }
        if (smoke && library && stats.status == HvhStatus::rendering && stats.frames > 4) {
            if (phase == 0) {
                if (navigation_test && navigation_phase == 0) {
                    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(navigation_x, navigation_y));
                    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(navigation_x, navigation_y));
                    navigation_phase = 1; phase_frame = stats.frames; continue;
                }
                if (navigation_test && navigation_phase == 1) {
                    if (stats.frames <= phase_frame + 4) continue;
                    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(navigation_x, navigation_y));
                    navigation_phase = 2; phase_frame = stats.frames; continue;
                }
                if (navigation_test && stats.frames <= phase_frame + 8) continue;
                if (esp_test && esp_phase<(esp_details_test ? 20 : 12)) {
                    if (!(esp_phase%2)) {
                        const int step=esp_phase/2;
                        const int last=esp_details_test ? 9 : 5,config_step=esp_details_test ? 6 : 2;
                        const bool category_click=step==0 || step==last;
                        int target_row=0;
                        if (step==0) target_row=1;
                        else if (step==last) target_row=esp_details_test ? 6 : esp_style_test ? 2 : 1;
                        else if (step==config_step+1) target_row=1;
                        else if (esp_details_test && step>=2 && step<=5) target_row=8+step-2;
                        pointer_x=static_cast<int>(std::lround(menu_left+(category_click ? 168.f : 679.f)*navigation_scale));
                        pointer_y=static_cast<int>(std::lround(845*.08f+(121+32*target_row+15)*navigation_scale));
                        if (step==config_step || step==config_step+2) {
                            const int target_tab=step==config_step ? 5 : 2;
                            pointer_x=static_cast<int>(std::lround(menu_left+(target_tab*(tab_width+4)+tab_width*.5f)*navigation_scale)); pointer_y=navigation_y;
                        }
                        SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(pointer_x,pointer_y));
                        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(pointer_x,pointer_y));
                    } else SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(pointer_x,pointer_y));
                    ++esp_phase; phase_frame=stats.frames; continue;
                }
                if (esp_test && GetPrivateProfileIntW(L"ESP",L"Enabled",0,profile_path.c_str())!=1) { failure="ESP toggle / CONFIG Save did not persist"; break; }
                if (esp_details_test) {
                    for (const auto* key:{L"Skeleton",L"Ammo",L"AmmoBar",L"Status"})
                        if (GetPrivateProfileIntW(L"ESP",key,0,profile_path.c_str())!=1) failure="ESP detail controls / CONFIG Save did not persist";
                    if (failure) break;
                }
                if (aim_test && aim_phase < 8) {
                    if (aim_phase == 0 || aim_phase == 2 || aim_phase == 4 || aim_phase == 6) {
                        if (aim_phase == 6) {
                            aim_profile_saved = GetPrivateProfileIntW(L"Aim",L"Enabled",0,profile_path.c_str()) == 1;
                            for (const auto& value:initial_aim_values)
                                if (static_cast<int>(GetPrivateProfileIntW(value.section.c_str(),value.key.c_str(),-1,profile_path.c_str())) != value.value)
                                    aim_profile_saved=false;
                            if (!aim_profile_saved) { failure="AIM master toggle / CONFIG Save did not persist activation independently"; break; }
                        }
                        if (aim_phase == 0 || aim_phase == 4) {
                            pointer_x=static_cast<int>(std::lround(menu_left+679*navigation_scale));
                            pointer_y=static_cast<int>(std::lround(845*.08f+(121+32*(aim_phase==4 ? 1 : 0)+15)*navigation_scale));
                        } else {
                            const int target_tab=aim_phase==2 ? 5 : 0; // Save through CONFIG, then return to AIM for its preview and selected-tab check.
                            pointer_x=static_cast<int>(std::lround(menu_left+(target_tab*(tab_width+4)+tab_width*.5f)*navigation_scale));
                            pointer_y=navigation_y;
                        }
                        SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(pointer_x,pointer_y));
                        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(pointer_x,pointer_y));
                    } else {
                        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(pointer_x,pointer_y));
                    }
                    ++aim_phase; phase_frame=stats.frames; continue;
                }
                if (skin_apply_test && apply_phase < 3) {
                    if (apply_phase == 0) {
                        float scale = std::clamp(GetPrivateProfileIntW(L"Menu", L"Scale", 100, profile_path.c_str()) / 100.f, .75f, 1.25f);
                        scale = std::min(scale, std::min(1500.f / 1100.f, 845.f / 740.f));
                        const bool has_finish = initial_paint && cosmetics::compatible(7, initial_paint);
                        const int rows_before_compatible = 5 + (has_finish ? 3 + (initial_stattrak >= 0 ? 1 : 0) : 0);
                        pointer_x = static_cast<int>(std::lround((1500 - 1016 * scale) * .5f + 679 * scale));
                        pointer_y = static_cast<int>(std::lround(845 * .08f + (121 + 32 * (rows_before_compatible + 1) + 7 + 15) * scale));
                        SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(pointer_x, pointer_y));
                        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pointer_x, pointer_y));
                        apply_phase = 1; phase_frame = stats.frames; continue;
                    }
                    if (apply_phase == 1) {
                        if (stats.frames <= phase_frame + 4) continue;
                        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(pointer_x, pointer_y));
                        apply_phase = 2; phase_frame = stats.frames; continue;
                    }
                    skin_apply_saved = GetPrivateProfileIntW(L"Skins", L"Enabled", 0, profile_path.c_str()) == 1 &&
                        GetPrivateProfileIntW(L"Skin.7", L"Enabled", 0, profile_path.c_str()) == 1 &&
                        static_cast<int>(GetPrivateProfileIntW(L"Skin.7", L"Paint", 0, profile_path.c_str())) == initial_paint;
                    for (const auto& [id, enabled] : initial_item_states) {
                        const auto section = L"Skin." + std::to_wstring(id);
                        if (static_cast<int>(GetPrivateProfileIntW(section.c_str(), L"Enabled", 0, profile_path.c_str())) != enabled) skin_apply_saved = false;
                    }
                    if (!skin_apply_saved) { failure = "Apply did not enable/persist selected item and global switch independently"; break; }
                    apply_phase = 3;
                }
                const auto pixels = read_pixels(); open_pixels = differences(baseline, pixels); save_pixels(pixels);
                if (navigation_test) {
                    const auto selected = (selected_tab_y * pixels.width + selected_tab_x) * 3; // Blank part of the selected tab, including profile scale.
                    if (pixels.rgb[selected] < 200 || pixels.rgb[selected+1] < 200 || pixels.rgb[selected+2] < 200) {
                        failure = aim_test ? "AIM tab did not respond to mouse input" : skins_test ? "Skins tab did not respond to mouse input" : "Visuals tab did not respond to mouse input"; break;
                    }
                }
                if (!stats.menu_visible || open_pixels < 10000) { failure = "overlay pixels missing"; break; }
                key(VK_INSERT); phase_frame = stats.frames; phase = 1;
            } else if (phase == 1 && stats.frames > phase_frame + 3) {
                closed_pixels = differences(baseline, read_pixels());
                if (stats.menu_visible || (!crosshair_test && closed_pixels > 500) || (crosshair_test && (closed_pixels < 15 || closed_pixels > 2000))) {
                    failure = crosshair_test ? "Crosshair did not render with the menu hidden" : "Insert did not hide menu cleanly"; break;
                }
                visibility(TRUE); phase_frame = stats.frames; phase = 2;
            } else if (phase == 2 && stats.frames > phase_frame + 3) {
                if (!stats.menu_visible) { failure = "show menu API"; break; }
                SetWindowPos(window, nullptr, -20000, -20000, 1280, 760, SWP_NOACTIVATE | SWP_NOZORDER);
                phase_frame = stats.frames; phase = 3;
            } else if (phase == 3 && stats.frames > phase_frame + 3) {
                if (stats.resizes == 0) { failure = "Resize hook missing"; break; }
                ComPtr<ID3D11Texture2D> held;
                chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(held.put()));
                context->OMSetRenderTargets(0, nullptr, nullptr); target.reset();
                const auto invalid = chain->ResizeBuffers(0, 900, 600, DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(invalid) || !create_target()) { failure = "expected resize failure missing"; break; }
                failed_resize_recovered = true; phase_frame = stats.frames; phase = 4;
            } else if (phase == 4 && stats.frames > phase_frame + 3) {
                context->ClearState(); target.reset(); vertex_shader.reset(); pixel_shader.reset();
                chain.reset(); context.reset(); device.reset();
                if (!create_renderer()) { failure = "device recreation"; break; }
                recreated = true; phase_frame = stats.frames; phase = 5;
            } else if (phase == 5 && stats.frames > phase_frame + 3) {
                request_stop(); shutdown_requested = true; phase = 6;
            }
        }
        if (smoke && phase == 6 && stats.status == HvhStatus::stopped) { completed = true; break; }
        Sleep(2);
    }
    bool unloaded = false;
    if (library && stop) {
        // Normal window close was requested asynchronously above so its thread can still service detach.
        if (!shutdown_requested && request_stop) {
            request_stop();
            const auto deadline = GetTickCount64() + 5000;
            do {
                MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                stats_function(&stats); Sleep(5);
            } while (stats.status != HvhStatus::stopped && stats.status != HvhStatus::failed && GetTickCount64() < deadline);
        }
        if (stop(nullptr)) { FreeLibrary(library); unloaded = GetModuleHandleW(L"hvh_menu.dll") == nullptr; }
    }
    const bool passed = completed && unloaded && failed_resize_recovered && recreated && !failure;
    if (smoke) {
        std::ofstream report(directory / "smoke-test.json");
        report << "{\n  \"passed\": " << (passed ? "true" : "false")
            << ",\n  \"driver\": \"" << (warp ? "warp" : "hardware") << "\",\n  \"present1\": " << (use_present1 ? "true" : "false")
            << ",\n  \"overlay_pixels\": " << open_pixels << ",\n  \"hidden_pixels\": " << closed_pixels
            << ",\n  \"crosshair_test\": " << (crosshair_test ? "true" : "false")
            << ",\n  \"skins_test\": " << (skins_test ? "true" : "false")
            << ",\n  \"skin_apply_test\": " << (skin_apply_test ? "true" : "false")
            << ",\n  \"skin_apply_saved\": " << (skin_apply_saved ? "true" : "false")
            << ",\n  \"aim_test\": " << (aim_test ? "true" : "false")
            << ",\n  \"aim_profile_saved\": " << (aim_profile_saved ? "true" : "false")
            << ",\n  \"aim_host_has_no_game_modules\": " << (aim_host_has_no_game_modules ? "true" : "false")
            << ",\n  \"frames\": " << stats.frames << ",\n  \"resizes\": " << stats.resizes
            << ",\n  \"resize_failure_recovered\": " << (failed_resize_recovered ? "true" : "false")
            << ",\n  \"device_recreated\": " << (recreated ? "true" : "false")
            << ",\n  \"unloaded\": " << (unloaded ? "true" : "false")
            << ",\n  \"failure\": \"" << (failure ? failure : passed ? "" : "timeout") << "\"\n}\n";
    }
    context->ClearState(); target.reset(); vertex_shader.reset(); pixel_shader.reset();
    chain.reset(); context.reset(); device.reset();
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName, instance);
    return smoke ? (passed ? 0 : 20) : 0;
}
