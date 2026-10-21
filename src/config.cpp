#include "config.hpp"
#include "generated/cosmetics.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <limits>

namespace config {
namespace {
float bounded(float value, float low, float high, float fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
int read(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback) {
    wchar_t buffer[64]{};
    if (!GetPrivateProfileStringW(section, key, L"", buffer, 64, path.c_str())) return fallback;
    wchar_t* end = nullptr;
    const auto value = wcstoll(buffer, &end, 10);
    if (end == buffer || *end || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) return fallback;
    return static_cast<int>(value);
}
}
const AimProfile& aim_profile(const Aim& a, int group) {
    if (a.per_weapon && group > 0 && static_cast<std::size_t>(group) < a.profiles.size() && a.profiles[group].override_global)
        return a.profiles[group];
    return a.profiles[0];
}
void sanitize(Aim& a) {
    a.activation = std::clamp(a.activation, 0, 2);
    a.key = std::clamp(a.key, 1, 254);
    a.profiles[0].override_global = false;
    for (auto& p : a.profiles) {
        p.fov = bounded(p.fov, .1f, 180, 30);
        p.smoothing = bounded(p.smoothing, 1, 30, 1);
        p.multipoint_scale = bounded(p.multipoint_scale, .1f, 1, .7f);
        p.priority = std::clamp(p.priority, 0, 2);
        p.hitboxes &= 31;
        if (!p.hitboxes) p.hitboxes = 3;
        p.min_damage = std::clamp(p.min_damage, 0, 200);
        p.hitchance = std::clamp(p.hitchance, 0, 100);
        p.switch_delay_ms = std::clamp(p.switch_delay_ms, 0, 2000);
    }
}
const Finish* find_finish(const Skins& s, int item_id) {
    const auto it = std::find_if(s.finishes.begin(), s.finishes.end(), [=](const Finish& f) { return f.item_id == item_id; });
    return it == s.finishes.end() ? nullptr : &*it;
}
Finish& finish(Skins& s, int item_id) {
    for (auto& f : s.finishes) if (f.item_id == item_id) return f;
    Finish f; f.item_id = item_id; s.finishes.push_back(f); return s.finishes.back();
}
bool activate_finish(Skins& s, int item_id) {
    if (!cosmetics::find_item(item_id)) return false;
    const auto* existing = find_finish(s, item_id);
    if (existing && !cosmetics::compatible(item_id, existing->paint_id)) return false;
    finish(s, item_id).enabled = true;
    s.enabled = true;
    sanitize(s);
    const auto* active = find_finish(s, item_id);
    return active && active->enabled;
}
void sanitize(Skins& s) {
    for (int team = 0; team < 2; ++team) {
        const auto* knife = cosmetics::find_item(s.knives[team]);
        if (!knife || knife->category != cosmetics::Category::knife) s.knives[team] = 0;
        const auto* glove = cosmetics::find_item(s.gloves[team]);
        if (!glove || glove->category != cosmetics::Category::gloves) s.gloves[team] = 0;
    }
    std::erase_if(s.finishes, [](const Finish& f) { return !cosmetics::find_item(f.item_id); });
    std::stable_sort(s.finishes.begin(), s.finishes.end(), [](const Finish& a, const Finish& b) { return a.item_id < b.item_id; });
    s.finishes.erase(std::unique(s.finishes.begin(), s.finishes.end(), [](const Finish& a, const Finish& b) { return a.item_id == b.item_id; }), s.finishes.end());
    for (auto& f : s.finishes) {
        if (!cosmetics::compatible(f.item_id, f.paint_id)) { f.paint_id = 0; f.enabled = false; }
        f.seed = std::clamp(f.seed, 0, 1000);
        f.stattrak = std::clamp(f.stattrak, -1, 1000000);
        if (cosmetics::find_item(f.item_id)->category == cosmetics::Category::gloves) f.stattrak = -1;
        const auto* p = cosmetics::find_paint(f.paint_id);
        const float low = p ? p->wear_min : 0, high = p ? p->wear_max : 1;
        f.wear = bounded(f.wear, low, high, std::clamp(.1f, low, high));
    }
}
void sanitize(Esp& e) {
    e.box_style=std::clamp(e.box_style,0,2); e.interval_ms=std::clamp(e.interval_ms,16,100);
    e.max_distance=bounded(e.max_distance,1,1000,150); e.thickness=bounded(e.thickness,.5f,4,1.5f);
    e.font_size=bounded(e.font_size,10,22,14); e.fill_alpha=bounded(e.fill_alpha,0,.5f,.08f);
    const Esp defaults;
    for (int i=0;i<4;++i) {
        e.visible_color[i]=bounded(e.visible_color[i],0,1,defaults.visible_color[i]);
        e.hidden_color[i]=bounded(e.hidden_color[i],0,1,defaults.hidden_color[i]);
        e.team_color[i]=bounded(e.team_color[i],0,1,defaults.team_color[i]);
    }
}
void sanitize(Settings& s) {
    s.menu.scale = bounded(s.menu.scale, .75f, 1.25f, 1);
    s.menu.opacity = bounded(s.menu.opacity, .25f, .85f, .52f);
    auto& c = s.crosshair;
    c.style = std::clamp(c.style, 0, 3);
    c.length = bounded(c.length, 1, 40, 7); c.gap = bounded(c.gap, 0, 30, 4);
    c.thickness = bounded(c.thickness, 1, 8, 1.5f);
    c.outline_width = bounded(c.outline_width, .5f, 4, 1);
    c.dot_radius = bounded(c.dot_radius, .5f, 8, 2);
    const Crosshair defaults;
    for (int i = 0; i < 4; ++i) c.color[i] = bounded(c.color[i], 0, 1, defaults.color[i]);
    s.diagnostics.interval_ms = std::clamp(s.diagnostics.interval_ms, 100, 1000);
    sanitize(s.skins);
    sanitize(s.aim);
    sanitize(s.esp);
}
bool load(const std::wstring& path, Settings& output) {
    Settings s;
    const auto integer = [&](const wchar_t* section, const wchar_t* key, int fallback) { return read(path, section, key, fallback); };
    s.menu.watermark = integer(L"Menu", L"Watermark", 0) != 0;
    s.menu.frame_graph = integer(L"Menu", L"FrameGraph", 1) != 0;
    s.menu.scale = integer(L"Menu", L"Scale", 100) / 100.f;
    s.menu.opacity = integer(L"Menu", L"Opacity", 52) / 100.f;
    auto& c = s.crosshair;
    c.enabled = integer(L"Crosshair", L"Enabled", 1) != 0;
    c.only_in_game = integer(L"Crosshair", L"OnlyInGame", 1) != 0;
    c.only_alive = integer(L"Crosshair", L"OnlyAlive", 1) != 0;
    c.hide_in_menu = integer(L"Crosshair", L"HideInMenu", 1) != 0;
    c.outline = integer(L"Crosshair", L"Outline", 1) != 0;
    c.center_dot = integer(L"Crosshair", L"CenterDot", 0) != 0;
    c.style = integer(L"Crosshair", L"Style", 0);
    c.length = integer(L"Crosshair", L"Length", 7000) / 1000.f;
    c.gap = integer(L"Crosshair", L"Gap", 4000) / 1000.f;
    c.thickness = integer(L"Crosshair", L"Thickness", 1500) / 1000.f;
    c.outline_width = integer(L"Crosshair", L"OutlineWidth", 1000) / 1000.f;
    c.dot_radius = integer(L"Crosshair", L"DotRadius", 2000) / 1000.f;
    const wchar_t* keys[] = {L"Red", L"Green", L"Blue", L"Alpha"};
    const int defaults[] = {200, 900, 720, 1000};
    for (int i = 0; i < 4; ++i) c.color[i] = integer(L"Crosshair", keys[i], defaults[i]) / 1000.f;
    s.diagnostics.enabled = integer(L"Diagnostics", L"Enabled", 1) != 0;
    s.diagnostics.hud = integer(L"Diagnostics", L"HUD", 0) != 0;
    s.diagnostics.interval_ms = integer(L"Diagnostics", L"IntervalMs", 250);
    s.skins.enabled = integer(L"Skins", L"Enabled", 0) != 0;
    s.skins.knives[0] = integer(L"Skins", L"KnifeT", 0); s.skins.knives[1] = integer(L"Skins", L"KnifeCT", 0);
    s.skins.gloves[0] = integer(L"Skins", L"GlovesT", 0); s.skins.gloves[1] = integer(L"Skins", L"GlovesCT", 0);
    for (const auto& item : cosmetics::items) {
        const auto section = L"Skin." + std::to_wstring(item.id);
        wchar_t values[2]{};
        // A missing item section stays sparse, so legacy profiles retain the original loadout.
        if (!GetPrivateProfileStringW(section.c_str(), L"Enabled", L"", values, 2, path.c_str())) continue;
        Finish f; f.item_id = item.id;
        f.enabled = integer(section.c_str(), L"Enabled", 0) != 0;
        f.paint_id = integer(section.c_str(), L"Paint", 0);
        f.seed = integer(section.c_str(), L"Seed", 0);
        f.stattrak = integer(section.c_str(), L"StatTrak", -1);
        f.wear = integer(section.c_str(), L"Wear", 100000) / 1000000.f;
        s.skins.finishes.push_back(f);
    }
    auto& a = s.aim;
    a.enabled = integer(L"Aim", L"Enabled", 0) != 0;
    a.per_weapon = integer(L"Aim", L"PerWeapon", 1) != 0;
    a.fov_ring = integer(L"Aim", L"FOVRing", 1) != 0;
    a.target_marker = integer(L"Aim", L"TargetMarker", 1) != 0;
    a.info = integer(L"Aim", L"Info", 0) != 0;
    a.activation = integer(L"Aim", L"Activation", 0);
    a.key = integer(L"Aim", L"Key", 5);
    for (std::size_t i = 0; i < a.profiles.size(); ++i) {
        const auto section = L"Aim.Group." + std::to_wstring(i);
        auto& p = a.profiles[i];
        p.override_global = integer(section.c_str(), L"OverrideGlobal", 0) != 0;
        p.recoil = integer(section.c_str(), L"Recoil", 1) != 0;
        p.auto_fire = integer(section.c_str(), L"AutoFire", 0) != 0;
        p.auto_scope = integer(section.c_str(), L"AutoScope", 0) != 0;
        p.auto_stop = integer(section.c_str(), L"AutoStop", 0) != 0;
        p.penetration = integer(section.c_str(), L"Penetration", 0) != 0;
        p.silent = integer(section.c_str(), L"Silent", 0) != 0;
        p.visible_only = integer(section.c_str(), L"VisibleOnly", 1) != 0;
        p.ignore_smoke = integer(section.c_str(), L"IgnoreSmoke", 1) != 0;
        p.ignore_flashed = integer(section.c_str(), L"IgnoreFlashed", 1) != 0;
        p.ignore_airborne = integer(section.c_str(), L"IgnoreAirborne", 0) != 0;
        p.multipoint = integer(section.c_str(), L"Multipoint", 0) != 0;
        p.fov = integer(section.c_str(), L"FOV", 30000) / 1000.f;
        p.smoothing = integer(section.c_str(), L"Smoothing", 1000) / 1000.f;
        p.multipoint_scale = integer(section.c_str(), L"MultipointScale", 700) / 1000.f;
        p.priority = integer(section.c_str(), L"Priority", 0);
        p.hitboxes = integer(section.c_str(), L"Hitboxes", 3);
        p.min_damage = integer(section.c_str(), L"MinDamage", 20);
        p.hitchance = integer(section.c_str(), L"Hitchance", 60);
        p.switch_delay_ms = integer(section.c_str(), L"SwitchDelayMs", 100);
    }
    s.server_rules.no_spread = integer(L"ServerRules", L"NoSpread", 0) != 0;
    s.server_rules.no_recoil = integer(L"ServerRules", L"NoRecoil", 0) != 0;
    auto& e=s.esp;
    e.enabled=integer(L"ESP",L"Enabled",0)!=0;
    const wchar_t* flags[]={L"Names",L"HealthBar",L"HealthValue",L"Weapons",L"Distance",L"Snaplines",L"EnemiesOnly",L"VisibleOnly",L"OnlyAlive",L"HideInMenu"};
    bool* values[]={&e.names,&e.health_bar,&e.health_value,&e.weapons,&e.distance,&e.snaplines,&e.enemies_only,&e.visible_only,&e.only_alive,&e.hide_in_menu};
    for (int i=0;i<10;++i) *values[i]=integer(L"ESP",flags[i],*values[i])!=0;
    const wchar_t* detail_flags[]={L"Skeleton",L"Ammo",L"AmmoBar",L"Status",L"Armor",L"Equipment",L"Scoped",L"Reloading",L"Flashed",L"Defusing",L"Immunity"};
    bool* detail_values[]={&e.skeleton,&e.ammo,&e.ammo_bar,&e.status,&e.armor,&e.equipment,&e.scoped,&e.reloading,&e.flashed,&e.defusing,&e.immunity};
    for (int i=0;i<11;++i) *detail_values[i]=integer(L"ESP",detail_flags[i],*detail_values[i])!=0;
    e.box_style=integer(L"ESP",L"BoxStyle",1); e.interval_ms=integer(L"ESP",L"IntervalMs",33);
    e.max_distance=integer(L"ESP",L"MaxDistance",150000)/1000.f;
    e.thickness=integer(L"ESP",L"Thickness",1500)/1000.f;
    e.font_size=integer(L"ESP",L"FontSize",14000)/1000.f;
    e.fill_alpha=integer(L"ESP",L"FillAlpha",80)/1000.f;
    const wchar_t* color_sections[]={L"ESP.Visible",L"ESP.Hidden",L"ESP.Team"};
    std::array<float,4>* colors[]={&e.visible_color,&e.hidden_color,&e.team_color};
    const wchar_t* components[]={L"Red",L"Green",L"Blue",L"Alpha"};
    for (int i=0;i<3;++i) for (int j=0;j<4;++j)
        (*colors[i])[j]=integer(color_sections[i],components[j],static_cast<int>(std::lround((*colors[i])[j]*1000)))/1000.f;
    sanitize(s); output = s;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
bool save(const std::wstring& path, const Settings& input) {
    Settings s = input; sanitize(s);
    std::string text;
    const auto section = [&](const char* name) { text += std::string("\r\n[") + name + "]\r\n"; };
    const auto put = [&](const char* key, int value) { text += std::string(key) + "=" + std::to_string(value) + "\r\n"; };
    const auto real = [&](const char* key, float value) { put(key, static_cast<int>(std::lround(value * 1000))); };
    section("Menu"); put("Watermark", s.menu.watermark); put("FrameGraph", s.menu.frame_graph);
    put("Scale", static_cast<int>(std::lround(s.menu.scale * 100)));
    put("Opacity", static_cast<int>(std::lround(s.menu.opacity * 100)));
    const auto& c = s.crosshair;
    section("Crosshair"); put("Enabled", c.enabled); put("OnlyInGame", c.only_in_game); put("OnlyAlive", c.only_alive);
    put("HideInMenu", c.hide_in_menu); put("Outline", c.outline); put("CenterDot", c.center_dot); put("Style", c.style);
    real("Length", c.length); real("Gap", c.gap); real("Thickness", c.thickness); real("OutlineWidth", c.outline_width); real("DotRadius", c.dot_radius);
    const char* keys[] = {"Red", "Green", "Blue", "Alpha"};
    for (int i = 0; i < 4; ++i) real(keys[i], c.color[i]);
    section("Diagnostics"); put("Enabled", s.diagnostics.enabled); put("HUD", s.diagnostics.hud); put("IntervalMs", s.diagnostics.interval_ms);
    section("Skins"); put("Enabled", s.skins.enabled); put("KnifeT", s.skins.knives[0]); put("KnifeCT", s.skins.knives[1]);
    put("GlovesT", s.skins.gloves[0]); put("GlovesCT", s.skins.gloves[1]);
    for (const auto& f : s.skins.finishes) {
        section(("Skin." + std::to_string(f.item_id)).c_str());
        put("Enabled", f.enabled); put("Paint", f.paint_id); put("Seed", f.seed); put("StatTrak", f.stattrak);
        put("Wear", static_cast<int>(std::lround(f.wear * 1000000)));
    }
    const auto& a = s.aim;
    section("Aim"); put("Enabled", a.enabled); put("PerWeapon", a.per_weapon); put("FOVRing", a.fov_ring);
    put("TargetMarker", a.target_marker); put("Info", a.info); put("Activation", a.activation); put("Key", a.key);
    for (std::size_t i = 0; i < a.profiles.size(); ++i) {
        section(("Aim.Group." + std::to_string(i)).c_str());
        const auto& p = a.profiles[i];
        put("OverrideGlobal", p.override_global); put("Recoil", p.recoil); put("AutoFire", p.auto_fire);
        put("AutoScope", p.auto_scope); put("AutoStop", p.auto_stop); put("Penetration", p.penetration);
        put("Silent",p.silent);
        put("VisibleOnly", p.visible_only); put("IgnoreSmoke", p.ignore_smoke); put("IgnoreFlashed", p.ignore_flashed);
        put("IgnoreAirborne", p.ignore_airborne); put("Multipoint", p.multipoint);
        real("FOV", p.fov); real("Smoothing", p.smoothing); real("MultipointScale", p.multipoint_scale);
        put("Priority", p.priority); put("Hitboxes", p.hitboxes); put("MinDamage", p.min_damage);
        put("Hitchance", p.hitchance); put("SwitchDelayMs", p.switch_delay_ms);
    }
    section("ServerRules"); put("NoSpread", s.server_rules.no_spread); put("NoRecoil", s.server_rules.no_recoil);
    const auto& e=s.esp; section("ESP"); put("Enabled",e.enabled);
    put("Names",e.names); put("HealthBar",e.health_bar); put("HealthValue",e.health_value); put("Weapons",e.weapons);
    put("Distance",e.distance); put("Snaplines",e.snaplines); put("EnemiesOnly",e.enemies_only);
    put("VisibleOnly",e.visible_only); put("OnlyAlive",e.only_alive); put("HideInMenu",e.hide_in_menu);
    put("Skeleton",e.skeleton); put("Ammo",e.ammo); put("AmmoBar",e.ammo_bar); put("Status",e.status);
    put("Armor",e.armor); put("Equipment",e.equipment); put("Scoped",e.scoped); put("Reloading",e.reloading);
    put("Flashed",e.flashed); put("Defusing",e.defusing); put("Immunity",e.immunity);
    put("BoxStyle",e.box_style); put("IntervalMs",e.interval_ms); real("MaxDistance",e.max_distance);
    real("Thickness",e.thickness); real("FontSize",e.font_size); real("FillAlpha",e.fill_alpha);
    const char* color_sections[]={"ESP.Visible","ESP.Hidden","ESP.Team"};
    const std::array<float,4>* colors[]={&e.visible_color,&e.hidden_color,&e.team_color};
    const char* components[]={"Red","Green","Blue","Alpha"};
    for (int i=0;i<3;++i) { section(color_sections[i]); for (int j=0;j<4;++j) real(components[j],(*colors[i])[j]); }
    const auto temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str()); return false;
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}
}
