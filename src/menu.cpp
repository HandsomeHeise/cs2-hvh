#include "menu.hpp"
#include "config.hpp"
#include "crosshair.hpp"
#include "aim.hpp"
#include "skins.hpp"
#include "server_rules.hpp"
#include "esp.hpp"
#include "generated/cosmetics.hpp"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cwchar>
#include <string>

namespace menu {
namespace {
config::Settings preferences;
auto& settings = preferences.menu;
auto& opacity = preferences.menu.opacity;
std::wstring ini_path;
enum Tab { aim_tab, skins_tab, visuals_tab, misc_tab, settings_tab, config_tab, about_tab };
int tab = aim_tab, category = 0;
const char* feedback = "";
std::array<float, 120> timings{};
int timing_index = 0;
float unit = 1;
ImVec2 origin;
float right_x = 0, right_width = 0, row_y = 0;
int row_number = 0;

ImU32 color(float alpha) { return IM_COL32(12, 15, 18, static_cast<int>(alpha * 255)); }
void text(ImVec2 position, const char* value, float size, ImU32 tint = IM_COL32_WHITE) {
    ImGui::GetWindowDrawList()->AddText(nullptr, size * unit, position, tint, value);
}
bool button(const char* id, ImVec2 position, ImVec2 size) {
    ImGui::SetCursorScreenPos(position);
    return ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
}
void save() {
    feedback = config::save(ini_path, preferences) ? "Profile saved" : "Could not save profile";
}
void load() {
    feedback = config::load(ini_path, preferences) ? "Profile loaded" : "Defaults loaded";
}
bool row(const char* label, const char* value, bool interactive = true) {
    const ImVec2 p{right_x, row_y};
    const ImVec2 size{right_width, 30 * unit};
    ImGui::PushID(row_number++);
    const bool pressed = button("row", p, size) && interactive;
    const bool hover = interactive && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, {p.x + size.x, p.y + size.y}, hover ? IM_COL32(255, 255, 255, 250) : color(opacity));
    const auto ink = hover ? IM_COL32(15, 18, 22, 255) : IM_COL32_WHITE;
    text({p.x + 15 * unit, p.y + 6 * unit}, label, 17, ink);
    ImGui::PushFont(nullptr, 17 * unit);
    const float width = ImGui::CalcTextSize(value).x;
    ImGui::PopFont();
    text({p.x + size.x - 15 * unit - width, p.y + 6 * unit}, value, 17, ink);
    ImGui::PopID();
    row_y += 32 * unit;
    return pressed;
}
void gap() { row_y += 7 * unit; }
void slider_row(const char* label, float& value, float low, float high, const char* format = "%.1f px") {
    char current[64]; snprintf(current, sizeof(current), format, value);
    ImGui::PushID(label);
    if (row(label, current)) ImGui::OpenPopup("Adjust");
    ImGui::SetNextWindowBgAlpha(.97f);
    if (ImGui::BeginPopup("Adjust")) {
        ImGui::TextUnformatted(label); ImGui::SetNextItemWidth(280 * unit);
        ImGui::SliderFloat("##value", &value, low, high, format, ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}
void toggle_row(const char* label, bool& value) { if (row(label, value ? "ON" : "OFF")) value = !value; }
void apply_skins(int selected_item = 0) {
    auto& s = preferences.skins;
    if (selected_item) {
        if (!config::activate_finish(s, selected_item)) { feedback = "Choose a compatible finish first"; return; }
    } else {
        const bool any = std::any_of(s.finishes.begin(), s.finishes.end(), [](const config::Finish& f) {
            return f.enabled && cosmetics::compatible(f.item_id, f.paint_id);
        });
        if (!any) { feedback = "Choose item + enable Apply This Item"; return; }
    }
    // Send these exact settings now, before requesting the refresh; the render sync
    // may otherwise still hold the previous disabled configuration until the next frame.
    skins::configure(s);
    skins::request_apply();
    const bool saved = config::save(ini_path, preferences);
    const auto* item = cosmetics::find_item(selected_item);
    feedback = s.enabled && saved && item && item->category == cosmetics::Category::weapon ? "Saved; applies when this weapon is carried" :
        s.enabled ? saved ? "Apply requested; profile saved" : "Apply requested; profile save failed" :
        saved ? "Disabled; profile saved" : "Disabled; profile save failed";
}
void paint_crosshair(const config::Crosshair& c, ImDrawList* draw, ImVec2 center) {
    const auto geometry = crosshair::geometry(c, center.x, center.y);
    const auto ink = ImGui::ColorConvertFloat4ToU32({c.color[0], c.color[1], c.color[2], c.color[3]});
    const auto outline = IM_COL32(0, 0, 0, static_cast<int>(c.color[3] * 255));
    for (unsigned i = 0; i < geometry.count; ++i) {
        const auto& p = geometry.items[i];
        if (p.kind == crosshair::Kind::line) {
            if (c.outline) draw->AddLine({p.x1, p.y1}, {p.x2, p.y2}, outline, c.thickness + 2 * c.outline_width);
            draw->AddLine({p.x1, p.y1}, {p.x2, p.y2}, ink, c.thickness);
        } else if (p.kind == crosshair::Kind::ring) {
            if (c.outline) draw->AddCircle({p.x1, p.y1}, p.x2, outline, 48, c.thickness + 2 * c.outline_width);
            draw->AddCircle({p.x1, p.y1}, p.x2, ink, 48, c.thickness);
        } else {
            if (c.outline) draw->AddCircleFilled({p.x1, p.y1}, p.x2 + c.outline_width, outline, 32);
            draw->AddCircleFilled({p.x1, p.y1}, p.x2, ink, 32);
        }
    }
}
void crosshair_rows() {
    auto& c = preferences.crosshair;
    toggle_row("Crosshair", c.enabled);
    const char* styles[] = {"Cross", "Dot", "Circle", "T Shape"};
    if (row("Shape", styles[c.style])) c.style = (c.style + 1) % 4;
    slider_row("Length / Radius", c.length, 1, 40);
    slider_row("Gap", c.gap, 0, 30);
    slider_row("Thickness", c.thickness, 1, 8);
    char tint[32]; snprintf(tint, sizeof(tint), "#%02X%02X%02X / %.0f%%", static_cast<unsigned>(c.color[0] * 255),
        static_cast<unsigned>(c.color[1] * 255), static_cast<unsigned>(c.color[2] * 255), c.color[3] * 100);
    if (row("Color / Alpha", tint)) ImGui::OpenPopup("Crosshair color");
    if (ImGui::BeginPopup("Crosshair color")) {
        ImGui::ColorPicker4("##color", c.color, ImGuiColorEditFlags_AlphaBar); ImGui::EndPopup();
    }
    toggle_row("Outline", c.outline);
    slider_row("Outline Width", c.outline_width, .5f, 4);
    toggle_row("Center Dot", c.center_dot);
    slider_row("Dot Radius", c.dot_radius, .5f, 8);
    gap();
    const ImVec2 p{right_x, row_y}, end{right_x + right_width, row_y + 95 * unit};
    auto* draw = ImGui::GetWindowDrawList(); draw->AddRectFilled(p, end, color(opacity));
    text({p.x + 15 * unit, p.y + 7 * unit}, "PREVIEW", 14);
    paint_crosshair(c, draw, {p.x + right_width * .5f, p.y + 50 * unit});
}
std::string brief(const std::string& value, std::size_t length = 42) {
    if (value.empty()) return "--";
    return value.size() > length ? value.substr(0, length - 3) + "..." : value;
}
const char* team_name(int team) {
    switch (team) { case 1: return "Spectator"; case 2: return "T"; case 3: return "CT"; case 0: return "Unassigned"; default: return "--"; }
}
void integer_row(const char* label, int& value, int low, int high) {
    ImGui::PushID(label);
    if (row(label, std::to_string(value).c_str())) ImGui::OpenPopup("Edit");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 12});
    if (ImGui::BeginPopup("Edit")) {
        ImGui::TextUnformatted(label); ImGui::SetNextItemWidth(260 * unit);
        ImGui::InputInt("##value", &value);
        value = std::clamp(value, low, high);
        ImGui::Text("%d - %d", low, high);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(); ImGui::PopID();
}
void capability_toggle(const char* label, bool& value, bool available) {
    if (row(label, available ? value ? "ON" : "OFF" : "UNAVAILABLE", available)) value = !value;
    if (!available && ImGui::IsItemHovered()) ImGui::SetTooltip("Runtime support for this feature is unavailable.");
}
void capability_integer(const char* label, int& value, int low, int high, bool available) {
    if (available) integer_row(label, value, low, high);
    else row(label, "UNAVAILABLE", false);
}
const char* aim_key_name(int key) {
    switch (key) {
    case VK_XBUTTON1: return "Mouse 4"; case VK_XBUTTON2: return "Mouse 5";
    case VK_RBUTTON: return "Right Mouse"; case VK_LBUTTON: return "Left Mouse";
    case VK_MBUTTON: return "Middle Mouse"; case VK_MENU: return "Alt";
    case VK_SHIFT: return "Shift"; case VK_CONTROL: return "Ctrl"; case VK_CAPITAL: return "Caps Lock";
    case 'X': return "X"; case 'C': return "C"; case 'V': return "V"; case 'F': return "F";
    default: return nullptr;
    }
}
void aim_key_row() {
    auto& key = preferences.aim.key;
    const auto* name = aim_key_name(key);
    const auto custom = "Key " + std::to_string(key);
    if (row("Activation Key", name ? name : custom.c_str())) ImGui::OpenPopup("Aim activation key");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 12});
    if (ImGui::BeginPopup("Aim activation key")) {
        const int keys[] = {VK_XBUTTON1, VK_XBUTTON2, VK_RBUTTON, VK_LBUTTON, VK_MBUTTON, VK_MENU,
            VK_SHIFT, VK_CONTROL, VK_CAPITAL, 'X', 'C', 'V', 'F'};
        for (const int code : keys) if (ImGui::Selectable(aim_key_name(code), key == code)) { key = code; ImGui::CloseCurrentPopup(); }
        ImGui::Separator(); ImGui::SetNextItemWidth(180 * unit);
        ImGui::InputInt("Custom Key Code", &key);
        key = std::clamp(key, 1, 254);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}
int aim_profile_row() {
    static int selected = 0;
    auto& a = preferences.aim;
    if (row("Edit Profile", aim::group_name(selected))) ImGui::OpenPopup("Aim profile");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 12});
    if (ImGui::BeginPopup("Aim profile")) {
        for (int i = 0; i < static_cast<int>(a.profiles.size()); ++i)
            if (ImGui::Selectable(aim::group_name(i), selected == i)) { selected = i; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    if (selected) {
        toggle_row("Override Global", a.profiles[selected].override_global);
        if (!a.per_weapon) row("Profile Mode", "Global settings active", false);
        else if (!a.profiles[selected].override_global) row("Profile Mode", "Inheriting Global", false);
    }
    return selected;
}
void aim_rows(int section, const game::Snapshot& data) {
    auto& a = preferences.aim;
    const auto status = aim::latest();
    const auto& caps = status.capabilities;
    if (section == 0) {
        toggle_row("Aimbot", a.enabled);
        const char* activation[] = {"Hold", "Toggle", "Always"};
        if (row("Activation", activation[a.activation])) a.activation = (a.activation + 1) % 3;
        if (a.activation != 2) aim_key_row();
        toggle_row("Per-Weapon Profiles", a.per_weapon);
        gap();
        row("Runtime", aim::state_name(status.state), false);
        row("Active Group", data.weapon_valid ? aim::group_name(aim::weapon_group(data.item_id)) : "--", false);
        row("View Angle Control", caps.view_angles ? "AVAILABLE" : "UNAVAILABLE", false);
        row("Target Selection", caps.target_selection ? "AVAILABLE" : "UNAVAILABLE", false);
        row("Silent Aim Support",caps.silent ? "AVAILABLE" : "UNAVAILABLE",false);
        row("Enemy Rules", "AUTOMATIC (SERVER)", false);
        row("Hitbox Data", caps.bones ? "AVAILABLE" : "UNAVAILABLE", false);
        row("Candidates", std::to_string(status.candidates).c_str(), false);
        row("Input Callbacks", std::to_string(status.callbacks).c_str(), false);
        char timing[64];
        snprintf(timing, sizeof(timing), "%.3f ms", status.work_ms);
        row("Aim CPU Time", timing, false);
        snprintf(timing, sizeof(timing), "%.3f ms", status.peak_ms);
        row("Peak Aim CPU Time", timing, false);
        row("Chance Rays This Update", std::to_string(status.chance_samples).c_str(), false);
        row("Chance Sample Size", std::to_string(status.chance_sample_count).c_str(), false);
        row("Status", brief(status.detail, 48).c_str(), false);
        return;
    }
    if (section == 4) {
        toggle_row("FOV Ring", a.fov_ring);
        toggle_row("Target Marker", a.target_marker);
        toggle_row("Target Information", a.info);
        gap();
        row("Projection", status.projection_valid ? "VERIFIED" : "UNAVAILABLE", false);
        row("Runtime", aim::state_name(status.state), false);
        row("Overlay", "Active while aiming; hidden in menu", false);
        return;
    }
    const int group = aim_profile_row();
    auto& p = a.profiles[group];
    const bool inherited = group && !p.override_global;
    ImGui::BeginDisabled(inherited);
    if (section == 1) {
        capability_toggle("Silent Aim",p.silent,caps.silent);
        slider_row("Aim FOV", p.fov, .1f, 180, "%.1f deg");
        if (p.silent) row("Smoothing","DIRECT SHOT ANGLES (SILENT)",false);
        else slider_row("Smoothing", p.smoothing, 1, 30, "%.1f");
        row("Silent History Samples",std::to_string(status.silent_samples).c_str(),false);
        const char* priorities[] = {"Crosshair Distance", "World Distance", "Lowest Health"};
        if (row("Target Priority", priorities[p.priority])) p.priority = (p.priority + 1) % 3;
        const char* hitbox_labels[] = {"Head", "Chest", "Stomach", "Arms", "Legs"};
        std::string hitbox_names;
        for (int i = 0; i < 5; ++i) if (p.hitboxes & (1 << i)) {
            if (!hitbox_names.empty()) hitbox_names += ", ";
            hitbox_names += hitbox_labels[i];
        }
        if (row("Hitboxes", brief(hitbox_names, 36).c_str())) ImGui::OpenPopup("Aim hitboxes");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 12});
        if (ImGui::BeginPopup("Aim hitboxes")) {
            for (int i = 0; i < 5; ++i) {
                bool selected = (p.hitboxes & (1 << i)) != 0;
                if (ImGui::Checkbox(hitbox_labels[i], &selected)) {
                    const int updated = selected ? p.hitboxes | (1 << i) : p.hitboxes & ~(1 << i);
                    if (updated) p.hitboxes = updated;
                }
            }
            ImGui::TextUnformatted("Select at least one hitbox group."); ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        integer_row("Target Switch Delay / ms", p.switch_delay_ms, 0, 2000);
        capability_toggle("Multipoint", p.multipoint, caps.multipoint);
        if (p.multipoint && caps.multipoint) slider_row("Multipoint Scale", p.multipoint_scale, .1f, 1, "%.2f");
    } else if (section == 2) {
        capability_toggle("Recoil Compensation", p.recoil, caps.recoil);
        capability_toggle("Auto Fire", p.auto_fire, caps.auto_fire);
        capability_toggle("Auto Scope", p.auto_scope, caps.auto_scope);
        capability_toggle("Auto Stop", p.auto_stop, caps.auto_stop);
        gap();
        capability_integer("Minimum Damage", p.min_damage, 0, 200, caps.damage);
        capability_integer("Sampled Hit Chance / %", p.hitchance, 0, 100, caps.hitchance);
        if (group == 7 || aim::weapon_group(data.item_id) == 7) row("Pellet Pattern Sampling", "UNAVAILABLE", false);
        capability_toggle("Autowall", p.penetration, caps.penetration);
        gap();
        row("Fire Gate", aim::fire_block_name(status.fire_block), false);
        char alignment[48];
        snprintf(alignment, sizeof(alignment), "%.3f deg", status.alignment_error);
        row("Alignment Error", alignment, false);
        row("Current Sample N", std::to_string(status.chance_sample_count).c_str(), false);
        row("Chance Estimate", status.chance_complete ? "COMPLETE" : "INCOMPLETE", false);
        char damage[48];
        snprintf(damage,sizeof(damage),"%.1f HP",status.damage);
        row("Expected Damage",status.has_target ? damage : "--",false);
        row("Penetrations",status.has_target ? std::to_string(status.penetrations).c_str() : "--",false);
    } else if (section == 3) {
        if (p.penetration) row("Target Visibility","DIRECT + PENETRABLE (AUTOWALL)",false);
        else capability_toggle("Visible Targets Only", p.visible_only, caps.visibility);
        capability_toggle("Skip Targets In Smoke", p.ignore_smoke, caps.smoke);
        capability_toggle("Pause While Flashed", p.ignore_flashed, caps.target_selection);
        capability_toggle("Skip Airborne Targets", p.ignore_airborne, caps.target_selection);
    }
    ImGui::EndDisabled();
    if (inherited) { gap(); row("Settings", "Enable Override Global to edit", false); }
}
void server_rule_rows() {
    auto& rules = preferences.server_rules;
    const auto status = server_rules::latest();
    const bool authority = status.supported && status.local_server;
    row("Local Listen Server", status.supported ? status.local_server ? "YES" : "NO" : "UNAVAILABLE", false);
    row("Authority", !status.supported ? "UNAVAILABLE" : status.local_server ? "LOCAL HOST" : "REMOTE SERVER", false);
    row("sv_cheats Required", "NO", false);
    row("Scope", "All players on this server", false);
    gap();
    const auto toggle = [&](const char* label, bool& enabled, bool capability) {
        const bool available = authority && capability;
        if (row(label, available ? enabled ? "ON" : "OFF" : enabled ? "ON / UNAVAILABLE" : "UNAVAILABLE", available || enabled)) enabled = !enabled;
    };
    toggle("No Spread", rules.no_spread, status.no_spread_supported);
    toggle("No Recoil", rules.no_recoil, status.no_recoil_supported);
    gap();
    const auto readback = [&](const char* label, float value) {
        char buffer[40]{};
        if (std::isfinite(value) && value >= 0) snprintf(buffer, sizeof(buffer), "%.3f", value);
        else snprintf(buffer, sizeof(buffer), "--");
        row(label, buffer, false);
    };
    readback("Spread Multiplier", status.spread_value);
    readback("Recoil Multiplier", status.recoil_value);
    readback("Observed Spread", status.observed_spread);
    readback("Observed Inaccuracy", status.observed_inaccuracy);
    readback("Observed Shot Kick", status.observed_recoil);
    row("Runtime", server_rules::state_name(status.state), false);
    row("Status", brief(status.detail, 48).c_str(), false);
}
ImU32 esp_color(const std::array<float,4>& c,float alpha=1) {
    return ImGui::ColorConvertFloat4ToU32({c[0],c[1],c[2],c[3]*alpha});
}
void esp_label(ImDrawList* draw,ImVec2 p,const char* label,float size,ImU32 tint,bool centered=true) {
    const auto bounds=ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,label);
    if (centered) p.x-=bounds.x*.5f;
    p.y=std::max(0.f,p.y);
    draw->AddText(nullptr,size,{p.x+1,p.y+1},IM_COL32(0,0,0,230),label);
    draw->AddText(nullptr,size,p,tint,label);
}
void esp_box(ImDrawList* draw,const esp::Box& b,const esp::Player& p,const config::Esp& e,ImU32 ink) {
    if (e.box_style) {
        if (e.fill_alpha>0) draw->AddRectFilled({b.left,b.top},{b.right,b.bottom},(ink&0x00ffffff)|
            (static_cast<ImU32>(static_cast<float>((ink>>24)&255)*e.fill_alpha)<<24));
        if (e.box_style==1) {
            draw->AddRect({b.left,b.top},{b.right,b.bottom},IM_COL32(0,0,0,210),0,0,e.thickness+2);
            draw->AddRect({b.left,b.top},{b.right,b.bottom},ink,0,0,e.thickness);
        } else {
            const float x=(b.right-b.left)*.25f,y=(b.bottom-b.top)*.2f;
            for (int i=0;i<4;++i) {
                const ImVec2 corner{i&1 ? b.right : b.left,i&2 ? b.bottom : b.top};
                const ImVec2 horizontal{corner.x+(i&1 ? -x : x),corner.y}, vertical{corner.x,corner.y+(i&2 ? -y : y)};
                draw->AddLine(corner,horizontal,IM_COL32(0,0,0,210),e.thickness+2); draw->AddLine(corner,vertical,IM_COL32(0,0,0,210),e.thickness+2);
                draw->AddLine(corner,horizontal,ink,e.thickness); draw->AddLine(corner,vertical,ink,e.thickness);
            }
        }
    }
    const float health=std::clamp(p.health/100.f,0.f,1.f);
    if (e.health_bar) {
        draw->AddRectFilled({b.left-7,b.top-1},{b.left-2,b.bottom+1},IM_COL32(0,0,0,220));
        draw->AddRectFilled({b.left-6,b.bottom-(b.bottom-b.top)*health},{b.left-3,b.bottom},
            IM_COL32(static_cast<int>(255*(1-health)),static_cast<int>(220*health),55,255));
    }
    if (e.health_value) esp_label(draw,{b.right+5,b.top},std::to_string(p.health).c_str(),e.font_size,IM_COL32_WHITE,false);
    float status_y=b.top+(e.health_value ? e.font_size+2 : 0);
    for (const auto& label:esp::status_labels(p,e)) {
        esp_label(draw,{b.right+5,status_y},label.c_str(),e.font_size,ink,false); status_y+=e.font_size+2;
    }
    const float center=(b.left+b.right)*.5f;
    if (e.names) esp_label(draw,{center,b.top-e.font_size-4},p.name.c_str(),e.font_size,ink);
    float bottom=b.bottom+3;
    if (e.ammo_bar && p.ammo>=0 && p.max_clip>0) {
        const auto ratio=std::clamp(static_cast<float>(p.ammo)/p.max_clip,0.f,1.f);
        draw->AddRectFilled({b.left-1,bottom-1},{b.right+1,bottom+4},IM_COL32(0,0,0,220));
        draw->AddRectFilled({b.left,bottom},{b.left+(b.right-b.left)*ratio,bottom+3},ink);
        bottom+=7;
    }
    if (e.weapons && !p.weapon.empty()) { esp_label(draw,{center,bottom},p.weapon.c_str(),e.font_size,IM_COL32_WHITE); bottom+=e.font_size+2; }
    if (e.ammo) { const auto ammo=esp::ammo_text(p); if (!ammo.empty()) { esp_label(draw,{center,bottom},ammo.c_str(),e.font_size,ink); bottom+=e.font_size+2; } }
    if (e.distance) { char distance[32]; snprintf(distance,sizeof(distance),"%.0f m",p.distance); esp_label(draw,{center,bottom},distance,e.font_size,ink); }
}
void paint_esp(bool menu_visible) {
    const auto& e=preferences.esp;
    if (!e.enabled || (menu_visible && e.hide_in_menu)) return;
    const auto f=esp::latest(); if (!f.active) return;
    const auto& io=ImGui::GetIO(); auto* draw=ImGui::GetForegroundDrawList();
    draw->PushClipRect({0,0},io.DisplaySize,true);
    for (const auto& p:f.players) {
        if ((e.enemies_only && p.teammate) || (e.visible_only && !p.visible) || p.distance>e.max_distance) continue;
        const auto b=esp::project_box(f.matrix,p,io.DisplaySize.x,io.DisplaySize.y); if (!b.valid) continue;
        const auto ink=esp_color(p.teammate ? e.team_color : p.visible ? e.visible_color : e.hidden_color);
        if (e.snaplines) draw->AddLine({io.DisplaySize.x*.5f,io.DisplaySize.y},{(b.left+b.right)*.5f,b.bottom},ink,e.thickness);
        esp_box(draw,b,p,e,ink);
        if (e.skeleton) for (const auto& link:esp::bone_links) {
            const auto segment=esp::project_segment(f.matrix,p,link[0],link[1],io.DisplaySize.x,io.DisplaySize.y);
            if (!segment.valid) continue;
            draw->AddLine({segment.x1,segment.y1},{segment.x2,segment.y2},IM_COL32(0,0,0,210),e.thickness+2);
            draw->AddLine({segment.x1,segment.y1},{segment.x2,segment.y2},ink,e.thickness);
        }
    }
    draw->PopClipRect();
}
void esp_color_row(const char* label,std::array<float,4>& c) {
    ImGui::PushID(label); char tint[48]; snprintf(tint,sizeof(tint),"#%02X%02X%02X / %.0f%%",static_cast<int>(255*c[0]),static_cast<int>(255*c[1]),static_cast<int>(255*c[2]),100*c[3]);
    if (row(label,tint)) ImGui::OpenPopup("Color");
    if (ImGui::BeginPopup("Color")) { ImGui::ColorPicker4("##value",c.data(),ImGuiColorEditFlags_AlphaBar); ImGui::EndPopup(); }
    ImGui::PopID();
}
void esp_rows(bool style) {
    auto& e=preferences.esp;
    if (!style) {
        toggle_row("Player ESP",e.enabled);
        const char* boxes[]={"OFF","Full Box","Corner Box"};
        if (row("Box Style",boxes[e.box_style])) e.box_style=(e.box_style+1)%3;
        toggle_row("Player Names",e.names); toggle_row("Health Bar",e.health_bar); toggle_row("Health Value",e.health_value);
        toggle_row("Weapon Names",e.weapons); toggle_row("Distance",e.distance); toggle_row("Snaplines",e.snaplines);
        toggle_row("Skeleton",e.skeleton); toggle_row("Ammo Count",e.ammo); toggle_row("Ammo Bar",e.ammo_bar); toggle_row("Status Flags",e.status);
        gap(); const auto f=esp::latest();
        row("Runtime",f.active ? "READY" : f.supported ? "WAITING" : "UNAVAILABLE",false);
        row("Players Sampled",std::to_string(f.players.size()).c_str(),false);
        row("Skeletons Sampled",std::to_string(f.skeleton_players).c_str(),false);
        char duration[40]; snprintf(duration,sizeof(duration),"%.3f ms",f.work_ms); row("ESP Sample CPU Time",duration,false);
        row("Status",brief(f.detail,48).c_str(),false);
    } else {
        toggle_row("Enemies Only",e.enemies_only); toggle_row("Visible Only",e.visible_only);
        toggle_row("Only When Alive",e.only_alive); toggle_row("Hide While Menu Is Open",e.hide_in_menu);
        slider_row("Maximum Distance",e.max_distance,1,1000,"%.0f m"); integer_row("Update Interval / ms",e.interval_ms,16,100);
        gap(); esp_color_row("Visible Enemy Color",e.visible_color); esp_color_row("Hidden Enemy Color",e.hidden_color); esp_color_row("Teammate Color",e.team_color);
        slider_row("Line Thickness",e.thickness,.5f,4,"%.1f px"); slider_row("Text Size",e.font_size,10,22,"%.0f px");
        slider_row("Box Fill Opacity",e.fill_alpha,0,.5f,"%.2f");
        gap(); auto* draw=ImGui::GetWindowDrawList(); const float cx=right_x+right_width*.5f;
        esp::Player p; p.name="PLAYER PREVIEW"; p.weapon="AK-47"; p.health=75; p.distance=24;
        p.ammo=18; p.reserve=90; p.max_clip=30; p.armor=75; p.helmet=true; p.scoped=true;
        auto preview=e; preview.font_size=std::min(14.f,e.font_size)*unit;
        esp_box(draw,{true,cx-24*unit,row_y+25*unit,cx+24*unit,row_y+115*unit},p,preview,esp_color(e.visible_color));
        if (e.skeleton) {
            const ImVec2 points[]={{0,0},{0,12},{0,23},{0,34},{0,46},{-12,17},{-20,31},{-23,43},
                {12,17},{20,31},{23,43},{-8,49},{-12,65},{-14,82},{8,49},{12,65},{14,82}};
            for (const auto& link:esp::bone_links) {
                const ImVec2 a{cx+points[link[0]].x*unit,row_y+(31+points[link[0]].y)*unit};
                const ImVec2 b{cx+points[link[1]].x*unit,row_y+(31+points[link[1]].y)*unit};
                draw->AddLine(a,b,esp_color(e.visible_color),e.thickness);
            }
        }
    }
}
void esp_detail_rows() {
    auto& e=preferences.esp;
    toggle_row("Status Flags",e.status); toggle_row("Armor Value",e.armor); toggle_row("Helmet / Defuse Kit",e.equipment);
    toggle_row("Scoped",e.scoped); toggle_row("Reloading",e.reloading); toggle_row("Flashed",e.flashed);
    toggle_row("Defusing",e.defusing); toggle_row("Spawn Protection",e.immunity);
    gap(); const auto frame=esp::latest();
    row("Bone Queries",std::to_string(frame.bone_queries).c_str(),false);
    row("Extra Skeleton Budget","2 ms / sample",false);
    row("Skeleton Sampling",frame.skeleton_limited ? "BUDGET LIMITED" : "WITHIN BUDGET",false);
    row("Missing Data","Hidden until valid data is available",false);
}
void paint_aim(const game::Snapshot& data, bool menu_visible, bool fresh) {
    const auto& a = preferences.aim;
    if (!a.enabled || menu_visible || !fresh || !data.in_game || !data.local_valid || !data.alive) return;
    const auto status = aim::latest();
    const auto now = GetTickCount64();
    if (!status.supported || !status.active || !status.projection_valid || !status.sampled_at || now < status.sampled_at || now - status.sampled_at > 500) return;
    const auto& io = ImGui::GetIO();
    auto* draw = ImGui::GetForegroundDrawList();
    const auto ink = IM_COL32(51, 205, 233, 210);
    constexpr float radians = 3.14159265358979323846f / 180;
    if (a.fov_ring && std::isfinite(status.fov_degrees) && std::isfinite(status.camera_fov) &&
        status.fov_degrees > 0 && status.fov_degrees < 89 && status.camera_fov > 1 && status.camera_fov < 179) {
        const float radius = io.DisplaySize.x * .5f * std::tan(status.fov_degrees * radians) / std::tan(status.camera_fov * .5f * radians);
        if (std::isfinite(radius) && radius > 0 && radius < std::hypot(io.DisplaySize.x, io.DisplaySize.y))
            draw->AddCircle({io.DisplaySize.x * .5f, io.DisplaySize.y * .5f}, radius, ink, 128, 1.2f);
    }
    const bool projected = status.has_target && status.target_on_screen && std::isfinite(status.target_screen_x) && std::isfinite(status.target_screen_y) &&
        status.target_screen_x >= 0 && status.target_screen_x <= 1 && status.target_screen_y >= 0 && status.target_screen_y <= 1;
    if (!projected) return;
    const ImVec2 target{status.target_screen_x * io.DisplaySize.x, status.target_screen_y * io.DisplaySize.y};
    if (a.target_marker) {
        draw->AddCircle(target, 6, IM_COL32(0, 0, 0, 220), 24, 3);
        draw->AddCircle(target, 6, ink, 24, 1.5f);
        draw->AddLine({target.x - 10, target.y}, {target.x + 10, target.y}, ink);
        draw->AddLine({target.x, target.y - 10}, {target.x, target.y + 10}, ink);
    }
    if (a.info) {
        std::string label = "Target " + std::to_string(status.target_index);
        if (status.target_health >= 0) label += "  |  " + std::to_string(status.target_health) + " HP";
        if (status.capabilities.damage && std::isfinite(status.damage)) label += "  |  " + std::to_string(static_cast<int>(status.damage)) + " damage";
        if (status.capabilities.hitchance && status.hit_chance >= 0) label += "  |  " + std::to_string(status.hit_chance) + "% sampled";
        const auto size = ImGui::CalcTextSize(label.c_str());
        const ImVec2 p{std::clamp(target.x + 16, 8.f, std::max(8.f, io.DisplaySize.x - size.x - 16)),
            std::clamp(target.y + 12, 8.f, std::max(8.f, io.DisplaySize.y - size.y - 16))};
        draw->AddRectFilled({p.x - 6, p.y - 4}, {p.x + size.x + 6, p.y + size.y + 4}, color(opacity));
        draw->AddText(p, IM_COL32_WHITE, label.c_str());
    }
}
bool skin_item_row(const char* label, cosmetics::Category kind, int& selected, bool original) {
    const auto* item = cosmetics::find_item(selected);
    bool changed = false;
    ImGui::PushID(label);
    static ImGuiTextFilter filter;
    if (row(label, item ? brief(item->name, 35).c_str() : "Original loadout")) { filter.Clear(); ImGui::OpenPopup("Choose item"); }
    ImGui::SetNextWindowSize({390 * unit, 360 * unit}, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(.98f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 12});
    if (ImGui::BeginPopup("Choose item")) {
        filter.Draw("Search", 260 * unit);
        ImGui::Separator();
        if (ImGui::BeginChild("Items", {0, 0})) {
            if (original && filter.PassFilter("Original loadout") && ImGui::Selectable("Original loadout", selected == 0)) {
                selected = 0; changed = true; ImGui::CloseCurrentPopup();
            }
            for (const auto& entry : cosmetics::items) {
                if (entry.category != kind || !filter.PassFilter(entry.name)) continue;
                ImGui::PushID(entry.id);
                if (ImGui::Selectable(entry.name, selected == entry.id)) { selected = entry.id; changed = true; ImGui::CloseCurrentPopup(); }
                ImGui::PopID();
            }
        }
        ImGui::EndChild(); ImGui::EndPopup();
    }
    ImGui::PopStyleVar(); ImGui::PopID(); return changed;
}
void skin_finish_rows(const cosmetics::Item& item, const game::Snapshot& data) {
    auto& f = config::finish(preferences.skins, item.id);
    row("Held Weapon", data.weapon_valid ? brief(data.weapon, 35).c_str() : "--", false);
    toggle_row("Apply This Item", f.enabled);
    const auto* paint = cosmetics::find_paint(f.paint_id);
    static ImGuiTextFilter filter;
    if (row("Finish", paint && paint->id ? brief(paint->name, 38).c_str() : "Default")) { filter.Clear(); ImGui::OpenPopup("Choose finish"); }
    ImGui::SetNextWindowSize({430 * unit, 390 * unit}, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(.98f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 12});
    if (ImGui::BeginPopup("Choose finish")) {
        ImGui::TextUnformatted(item.name); filter.Draw("Search", 285 * unit); ImGui::Separator();
        if (ImGui::BeginChild("Finishes", {0, 0})) {
            for (const auto id : cosmetics::paints_for(item)) {
                const auto* p = cosmetics::find_paint(id);
                if (!p || !filter.PassFilter(id ? p->name : "Default")) continue;
                ImGui::PushID(id);
                const auto label = std::string(id ? p->name : "Default") + "  [" + std::to_string(id) + "]";
                if (ImGui::Selectable(label.c_str(), f.paint_id == id)) {
                    f.paint_id = id; f.enabled = true;
                    f.wear = std::clamp(f.wear, p->wear_min, p->wear_max);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild(); ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    paint = cosmetics::find_paint(f.paint_id);
    if (paint && f.paint_id) {
        slider_row("Wear", f.wear, paint->wear_min, paint->wear_max, "%.6f");
        integer_row("Pattern Seed", f.seed, 0, 1000);
        if (item.category != cosmetics::Category::gloves) {
            bool stattrak = f.stattrak >= 0;
            if (row("StatTrak", stattrak ? "ON" : "OFF")) f.stattrak = stattrak ? -1 : 0;
            if (f.stattrak >= 0) integer_row("StatTrak Count", f.stattrak, 0, 1000000);
        }
    }
    gap();
    row("Compatible Finishes", std::to_string(item.paint_count - 1).c_str(), false);
    if (row("Apply / Save", "ENTER")) apply_skins(item.id);
    const auto status = skins::latest();
    const auto* selected_finish = config::find_finish(preferences.skins, item.id);
    row("Runtime", skins::state_name(status.state), false);
    if (item.category == cosmetics::Category::weapon && preferences.skins.enabled && selected_finish && selected_finish->enabled && status.supported &&
        data.in_game && data.local_valid && data.alive) {
        const bool carried = std::find(status.carried_items.begin(), status.carried_items.end(), item.id) != status.carried_items.end();
        const bool applied = std::find(status.applied_items.begin(), status.applied_items.end(), item.id) != status.applied_items.end();
        row("Selected Weapon", !carried ? "Weapon not in your loadout" : applied ? "Local values applied" : "Waiting for local values", false);
    }
    if (*feedback) row("Action", brief(feedback, 46).c_str(), false);
}
void skin_rows(int section, const game::Snapshot& data) {
    auto& s = preferences.skins;
    if (section == 5) {
        unsigned weapons = 0, knives = 0, gloves = 0;
        for (const auto& item : cosmetics::items) {
            weapons += item.category == cosmetics::Category::weapon;
            knives += item.category == cosmetics::Category::knife;
            gloves += item.category == cosmetics::Category::gloves;
        }
        row("Source", "Installed CS2 item data", false);
        row("Weapon Definitions", std::to_string(weapons).c_str(), false);
        row("Knife Models", std::to_string(knives).c_str(), false);
        row("Glove Models", std::to_string(gloves).c_str(), false);
        static const auto available_paints = [] {
            unsigned count = 0;
            for (const auto& paint : cosmetics::paints)
                if (paint.id && std::find(std::begin(cosmetics::item_paints), std::end(cosmetics::item_paints), paint.id) != std::end(cosmetics::item_paints)) ++count;
            return count;
        }();
        row("Available Finishes", std::to_string(available_paints).c_str(), false);
        gap(); row("Compatibility", "Verified item / finish pairs", false);
        row("Profile", "Per item, T / CT models", false);
        return;
    }
    toggle_row("Skin Changer", s.enabled);
    if (section == 3) {
        const auto status = skins::latest();
        row("Runtime", skins::state_name(status.state), false);
        row("Game Callbacks", std::to_string(status.frame_callbacks).c_str(), false);
        row("Weapons", status.weapons_supported ? "SUPPORTED" : "UNAVAILABLE", false);
        row("Knives", status.knives_supported ? "SUPPORTED" : "UNAVAILABLE", false);
        row("Gloves", status.gloves_supported ? "SUPPORTED" : "UNAVAILABLE", false);
        gap();
        row("Applied Weapons", std::to_string(status.applied_weapons).c_str(), false);
        row("Knife Applied", status.knife_applied ? "YES" : "NO", false);
        row("Gloves Applied", status.gloves_applied ? "YES" : "NO", false);
        if (row("Apply / Save", "ENTER")) apply_skins();
        gap(); row("Status", brief(status.detail, 46).c_str(), false);
        if (*feedback) row("Action", brief(feedback, 46).c_str(), false);
        return;
    }
    static int weapon = 7, team = 0;
    static bool weapon_initialized = false;
    if (!weapon_initialized && data.weapon_valid) {
        const auto* active = cosmetics::find_item(data.item_id);
        if (active && active->category == cosmetics::Category::weapon) { weapon = active->id; weapon_initialized = true; }
    }
    int selected = weapon;
    if (section == 0) {
        if (skin_item_row("Weapon", cosmetics::Category::weapon, weapon, false)) weapon_initialized = true;
        selected = weapon;
    } else {
        if (row("Team", team ? "CT" : "T")) team = 1 - team;
        auto& model = section == 1 ? s.knives[team] : s.gloves[team];
        if (skin_item_row("Model", section == 1 ? cosmetics::Category::knife : cosmetics::Category::gloves, model, true) && model)
            config::finish(s, model).enabled = true;
        selected = model;
    }
    if (const auto* item = cosmetics::find_item(selected)) skin_finish_rows(*item, data);
    else { gap(); row("Model", "Using original loadout", false); }
}
void diagnostics_rows(const game::Snapshot& data, int section) {
    auto& d = preferences.diagnostics;
    if (section == 0 || section == 4) toggle_row("Live Diagnostics", d.enabled);
    if (!d.enabled) { row("Status", "DISABLED", false); return; }
    const auto integer = [&](const char* label, int value) {
        row(label, value >= 0 ? std::to_string(value).c_str() : "--", false);
    };
    if (section == 0 || section == 3) {
        row("Connection", game::state_name(data.state), false);
        row("Map", brief(data.map).c_str(), false);
    }
    if (section == 0 || section == 1) {
        row("Local Player", data.local_valid ? brief(data.player, 32).c_str() : "--", false);
        integer("Health", data.local_valid ? data.health : -1);
        row("Team", data.local_valid ? team_name(data.team) : "--", false);
        row("Life State", !data.local_valid ? "--" : data.alive ? "ALIVE" : "DEAD", false);
    }
    if (section == 0 || section == 2) {
        row("Active Weapon", data.weapon_valid ? brief(data.weapon).c_str() : "--", false);
        integer("Clip Ammo", data.weapon_valid ? data.ammo : -1);
        if (section == 2) integer("Item Definition", data.weapon_valid ? data.item_id : -1);
    }
    if (section == 0 || section == 3) {
        integer("Game Build", data.build);
        row("Dump Profile", data.profile_valid ? "MATCHED" : "UNAVAILABLE / MISMATCH", false);
        if (section == 3) {
            integer("Signon State", data.signon);
            row("Background World", data.background ? "YES" : "NO", false);
        }
    }
    if (section == 0 || section == 4) {
        toggle_row("Diagnostics HUD", d.hud);
        if (row("Refresh Interval", (std::to_string(d.interval_ms) + " ms").c_str())) {
            d.interval_ms += 50; if (d.interval_ms > 1000) d.interval_ms = 100;
        }
    }
    if (section == 4) {
        const auto age = data.sampled_at ? GetTickCount64() - data.sampled_at : 0;
        row("Sample Age", data.sampled_at ? (std::to_string(age) + " ms").c_str() : "--", false);
    }
    if (!data.detail.empty() && (!data.local_valid || data.state != game::State::in_game)) {
        gap(); row("Status", brief(data.detail, 49).c_str(), false);
    }
}
void display(const HvhStats& stats) {
    const auto& io = ImGui::GetIO();
    char resolution[64], fps[64], frames[64], scale[32], alpha[32];
    snprintf(resolution, sizeof(resolution), "%.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
    snprintf(fps, sizeof(fps), "%.0f", io.Framerate);
    snprintf(frames, sizeof(frames), "%llu", static_cast<unsigned long long>(stats.frames));
    snprintf(scale, sizeof(scale), "%.0f%%", settings.scale * 100);
    snprintf(alpha, sizeof(alpha), "%.0f%%", opacity * 100);
    row("DirectX Version", "DX 11", false);
    row("Resolution", resolution, false);
    row("Frame Rate", fps, false);
    row("Menu Style", "Classic", false);
    gap();
    if (row("Watermark", settings.watermark ? "ON" : "OFF")) settings.watermark = !settings.watermark;
    if (row("Frame Graph", settings.frame_graph ? "ON" : "OFF")) settings.frame_graph = !settings.frame_graph;
    if (row("Menu Scale", scale)) { settings.scale += 0.05f; if (settings.scale > 1.251f) settings.scale = 0.75f; }
    if (row("Panel Opacity", alpha)) { opacity += 0.05f; if (opacity > 0.851f) opacity = 0.25f; }
    gap();
    row("Rendered Frames", frames, false);
    row("Menu Key", "INSERT", false);
    if (settings.frame_graph) {
        row_y += 6 * unit;
        ImGui::SetCursorScreenPos({right_x + 10 * unit, row_y});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, color(opacity));
        ImGui::PushStyleColor(ImGuiCol_PlotLines, IM_COL32(51, 205, 233, 255));
        ImGui::PlotLines("##timing", timings.data(), static_cast<int>(timings.size()),
            timing_index % timings.size(), "FRAME TIME / ms", 0, 40, {right_width - 20 * unit, 55 * unit});
        ImGui::PopStyleColor(2);
    }
}
}

void initialize(const std::wstring& directory) {
    ini_path = directory + L"\\hvh-menu.ini";
    load(); feedback = "";
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {0, 0}; style.WindowRounding = 0;
    style.FrameRounding = 0; style.WindowBorderSize = 0;
    wchar_t windows[32768]{};
    if (GetWindowsDirectoryW(windows, 32768)) {
        const auto path = std::wstring(windows) + L"\\Fonts\\arial.ttf";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const int length = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string utf8(length, '\0');
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), length, nullptr, nullptr);
            if (auto* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(utf8.c_str(), 18))
                ImGui::GetIO().FontDefault = font;
        }
    }
}

void draw_watermark() {
    if (!settings.watermark) return;
    const auto& io = ImGui::GetIO();
    char label[100];
    snprintf(label, sizeof(label), "HVH  /  DX11  /  %.0f FPS", io.Framerate);
    auto* draw = ImGui::GetForegroundDrawList();
    const auto size = ImGui::CalcTextSize(label);
    draw->AddRectFilled({18, 18}, {size.x + 42, 49}, color(opacity));
    draw->AddRectFilled({18, 18}, {size.x + 42, 20}, IM_COL32(51, 205, 233, 255));
    draw->AddText({30, 26}, IM_COL32_WHITE, label);
}

void draw_overlay(const game::Snapshot& data, bool menu_visible) {
    config::sanitize(preferences);
    static config::Skins sent;
    static bool configured = false;
    if (!configured || sent != preferences.skins) { skins::configure(preferences.skins); sent = preferences.skins; configured = true; }
    static config::Aim sent_aim;
    static bool aim_configured = false;
    if (!aim_configured || sent_aim != preferences.aim) { aim::configure(preferences.aim); sent_aim = preferences.aim; aim_configured = true; }
    static config::ServerRules sent_rules;
    static bool rules_configured = false;
    if (!rules_configured || sent_rules != preferences.server_rules) {
        server_rules::configure(preferences.server_rules); sent_rules = preferences.server_rules; rules_configured = true;
    }
    static config::Esp sent_esp; static bool esp_configured=false;
    if (!esp_configured || sent_esp!=preferences.esp) { esp::configure(preferences.esp); sent_esp=preferences.esp; esp_configured=true; }
    const auto& c = preferences.crosshair;
    game::configure(preferences.diagnostics.enabled || preferences.aim.enabled || (c.enabled && (c.only_in_game || c.only_alive)), preferences.diagnostics.interval_ms);
    const auto& io = ImGui::GetIO();
    const bool fresh = data.sampled_at && GetTickCount64() - data.sampled_at <= static_cast<ULONGLONG>(preferences.diagnostics.interval_ms * 3 + 500);
    if (crosshair::visible(c, menu_visible, fresh && data.in_game, fresh && data.local_valid && data.alive))
        paint_crosshair(c, ImGui::GetForegroundDrawList(), {io.DisplaySize.x * .5f, io.DisplaySize.y * .5f});
    paint_aim(data, menu_visible, fresh);
    paint_esp(menu_visible);
    if (!preferences.diagnostics.enabled || !preferences.diagnostics.hud || menu_visible) return;
    const std::string health = data.local_valid ? std::to_string(data.health) + " HP" : "-- HP";
    const auto label = std::string(game::state_name(data.state)) + "  |  " + brief(data.map, 32) + "\n" +
        health + "  |  " + team_name(data.team) + "  |  " + brief(data.weapon, 26);
    auto* draw = ImGui::GetForegroundDrawList();
    const auto size = ImGui::CalcTextSize(label.c_str());
    const ImVec2 p{18, preferences.menu.watermark ? 64.f : 18.f};
    draw->AddRectFilled(p, {p.x + size.x + 24, p.y + size.y + 18}, color(opacity));
    draw->AddText({p.x + 12, p.y + 9}, IM_COL32_WHITE, label.c_str());
}

void draw(bool& visible, const HvhStats& stats, const game::Snapshot& data) {
    const auto& io = ImGui::GetIO();
    timings[timing_index++ % timings.size()] = io.DeltaTime * 1000;
    if (!visible) return;
    unit = std::min(io.DisplaySize.x / 1500.0f, io.DisplaySize.y / 845.0f);
    unit = std::max(0.4f, unit * settings.scale);
    unit = std::min(unit, std::min(io.DisplaySize.x / 1100.0f, io.DisplaySize.y / 740.0f));
    origin = {(io.DisplaySize.x - 1016 * unit) * 0.5f, io.DisplaySize.y * 0.08f};
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0, 0, 0, 0});
    ImGui::Begin("##hvh-classic", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    auto* draw = ImGui::GetWindowDrawList();
    const float width = 1016 * unit;
    draw->AddRectFilled(origin, {origin.x + width, origin.y + 70 * unit}, color(opacity * 0.60f));
    text({origin.x + 15 * unit, origin.y + 20 * unit}, "Counter-Strike 2", 29);
    const char* tabs[] = {"AIM", "SKINS", "VISUALS", "MISC", "SETTINGS", "CONFIG", "ABOUT"};
    const float tab_width = (width - 6 * 4 * unit) / 7;
    for (int i = 0; i < 7; ++i) {
        ImGui::PushID(i);
        const ImVec2 p{origin.x + i * (tab_width + 4 * unit), origin.y + 73 * unit};
        if (button("tab", p, {tab_width, 31 * unit})) { tab = i; category = 0; }
        const bool active = tab == i;
        draw->AddRectFilled(p, {p.x + tab_width, p.y + 31 * unit}, active ? IM_COL32(255, 255, 255, 246) : color(opacity));
        if (active) draw->AddRectFilled({p.x, p.y - 3 * unit}, {p.x + tab_width, p.y}, IM_COL32(51, 205, 233, 255));
        ImGui::PushFont(nullptr, 16 * unit);
        const float label_width = ImGui::CalcTextSize(tabs[i]).x;
        ImGui::PopFont();
        text({p.x + (tab_width - label_width) * 0.5f, p.y + 6 * unit}, tabs[i], 16,
            active ? IM_COL32(13, 15, 18, 255) : IM_COL32_WHITE);
        ImGui::PopID();
    }
    const char* categories[] = {"Controls", "Display", "Interface", "Notifications", "Profiles", "Session", ""};
    if (tab == aim_tab) { categories[0] = "General"; categories[1] = "Targeting"; categories[2] = "Firing"; categories[3] = "Conditions"; categories[4] = "Overlay"; categories[5] = "Profiles"; categories[6] = "Server Rules"; }
    if (tab == skins_tab) { categories[0] = "Weapons"; categories[1] = "Knives"; categories[2] = "Gloves"; categories[3] = "Application"; categories[4] = "Profiles"; categories[5] = "Catalog"; }
    if (tab == visuals_tab) { categories[0] = "Crosshair"; categories[1] = "Player ESP"; categories[2] = "ESP Style / Filters"; categories[3] = "Conditions"; categories[4] = "Diagnostics HUD"; categories[5] = "Profiles"; categories[6]="ESP Status / Details"; }
    if (tab == misc_tab) { categories[0] = "Live Diagnostics"; categories[1] = "Player"; categories[2] = "Weapon"; categories[3] = "Connection"; categories[4] = "Sampling"; }
    if (tab == config_tab) { categories[0] = "Local Profile"; categories[1] = "Appearance"; }
    if (tab == about_tab) { categories[0] = "Information"; categories[1] = "Renderer"; categories[2] = "Statistics"; }
    const float left_width = 337 * unit;
    const int category_count = tab == aim_tab || tab == visuals_tab ? 7 : 6;
    for (int i = 0; i < category_count; ++i) {
        ImGui::PushID(100 + i);
        const ImVec2 p{origin.x, origin.y + (121 + 32 * i) * unit};
        if (button("category", p, {left_width, 30 * unit})) category = i;
        draw->AddRectFilled(p, {p.x + left_width, p.y + 30 * unit},
            category == i ? IM_COL32(255, 255, 255, 248) : color(opacity));
        text({p.x + 15 * unit, p.y + 6 * unit}, categories[i], 17,
            category == i ? IM_COL32(13, 15, 18, 255) : IM_COL32_WHITE);
        ImGui::PopID();
    }
    right_x = origin.x + 342 * unit; right_width = width - 342 * unit;
    row_y = origin.y + 121 * unit; row_number = 0;
    if (tab == aim_tab && category == 6) {
        server_rule_rows();
    } else if (tab == aim_tab && category != 5) {
        aim_rows(category, data);
    } else if (tab == skins_tab && category != 4) {
        skin_rows(category, data);
    } else if (tab == config_tab || (tab == skins_tab && category == 4) || (tab == settings_tab && category == 4) || (tab == visuals_tab && category == 5) || (tab == aim_tab && category == 5)) {
        row("Active Profile", "Local", false);
        if (row("Save Profile", "ENTER")) save();
        if (row("Load Profile", "ENTER")) load();
        if (row("Restore Defaults", "ENTER")) { preferences = config::Settings{}; feedback = "Defaults restored"; }
        gap(); row("File", "hvh-menu.ini", false);
        if (*feedback) row("Status", feedback, false);
    } else if ((tab == misc_tab && category < 5) || (tab == visuals_tab && category == 4)) {
        diagnostics_rows(data, tab == misc_tab ? category : 4);
    } else if (tab==visuals_tab && category==6) {
        esp_detail_rows();
    } else if (tab == visuals_tab && (category==1 || category==2)) {
        esp_rows(category==2);
    } else if (tab == visuals_tab && category == 0) {
        crosshair_rows();
    } else if (tab == visuals_tab && category == 3) {
        toggle_row("Only In Game", preferences.crosshair.only_in_game);
        toggle_row("Only When Alive", preferences.crosshair.only_alive);
        toggle_row("Hide While Menu Is Open", preferences.crosshair.hide_in_menu);
        gap(); row("Current State", game::state_name(data.state), false);
        row("Local Player", data.local_valid ? data.alive ? "ALIVE" : "DEAD" : "UNAVAILABLE", false);
    } else if (tab == about_tab || (tab == settings_tab && category == 5) || (tab == misc_tab && category == 5)) {
        row("Renderer", "DirectX 11", false);
        char count[64];
        snprintf(count, sizeof(count), "%llu", static_cast<unsigned long long>(stats.presents)); row("Present Calls", count, false);
        snprintf(count, sizeof(count), "%llu", static_cast<unsigned long long>(stats.frames)); row("Rendered Frames", count, false);
        snprintf(count, sizeof(count), "%llu", static_cast<unsigned long long>(stats.resizes)); row("Resize Calls", count, false);
        snprintf(count, sizeof(count), "%llu", static_cast<unsigned long long>(stats.inputs)); row("Input Events", count, false);
    } else if (tab == settings_tab && category == 0) {
        row("Open / Close Menu", "INSERT", false);
        row("Mouse Input", "CAPTURED WHILE OPEN", false);
        row("Keyboard Input", "CAPTURED WHILE OPEN", false);
    } else if (tab == settings_tab && category == 3) {
        row("Initialization Errors", "hvh-menu.log", false);
        row("Status Messages", "LOG FILE", false);
    } else {
        display(stats);
    }
    const char* footer = "Back  [ INSERT ]     Select  [ ENTER ]";
    ImGui::PushFont(nullptr, 16 * unit);
    const auto footer_size = ImGui::CalcTextSize(footer);
    ImGui::PopFont();
    const ImVec2 footer_pos{io.DisplaySize.x - footer_size.x - 32 * unit, io.DisplaySize.y - 48 * unit};
    draw->AddRectFilled({footer_pos.x - 10 * unit, footer_pos.y - 6 * unit},
        {io.DisplaySize.x - 22 * unit, footer_pos.y + 25 * unit}, color(opacity));
    text(footer_pos, footer, 16);
    ImGui::End();
    ImGui::PopStyleColor();
}
}
