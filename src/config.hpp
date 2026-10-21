#pragma once
#include <array>
#include <string>
#include <vector>

namespace config {
struct Menu {
    bool watermark = false, frame_graph = true;
    float scale = 1.0f, opacity = 0.52f;
};
struct Crosshair {
    bool enabled = true, only_in_game = true, only_alive = true, hide_in_menu = true;
    bool outline = true, center_dot = false;
    int style = 0; // Cross, dot, circle, T.
    float length = 7, gap = 4, thickness = 1.5f, outline_width = 1, dot_radius = 2;
    float color[4] = {0.20f, 0.90f, 0.72f, 1};
};
struct Diagnostics {
    bool enabled = true, hud = false;
    int interval_ms = 250;
};
struct Finish {
    int item_id = 0, paint_id = 0, seed = 0, stattrak = -1;
    float wear = .1f;
    bool enabled = false;
    bool operator==(const Finish&) const = default;
};
struct Skins {
    bool enabled = false;
    int knives[2]{}, gloves[2]{}; // T, CT; zero retains the equipped model.
    std::vector<Finish> finishes;
    bool operator==(const Skins&) const = default;
};
struct AimProfile {
    bool override_global = false, recoil = true, auto_fire = false, auto_scope = false, auto_stop = false, silent = false;
    bool penetration = false, visible_only = true, ignore_smoke = true, ignore_flashed = true;
    bool ignore_airborne = false, multipoint = false;
    float fov = 30, smoothing = 1, multipoint_scale = .7f;
    int priority = 0, hitboxes = 3, min_damage = 20, hitchance = 60, switch_delay_ms = 100;
    bool operator==(const AimProfile&) const = default;
};
// Autowall evaluates direct and penetrable shots through native ballistics.
// Keep the saved visibility preference for when Autowall is switched off.
inline bool requires_visibility(const AimProfile& p) { return p.visible_only && !p.penetration; }
struct Aim {
    bool enabled = false, per_weapon = true, fov_ring = true, target_marker = true, info = false;
    int activation = 0, key = 5; // Hold, toggle, always; VK_XBUTTON1 (Mouse 4).
    std::array<AimProfile, 9> profiles{}; // Global, pistols, heavy pistols, rifles/SMG, AWP, Scout, auto snipers, shotguns, machineguns.
    bool operator==(const Aim&) const = default;
};
struct ServerRules {
    bool no_spread = false, no_recoil = false;
    bool operator==(const ServerRules&) const = default;
};
struct Esp {
    bool enabled=false, names=true, health_bar=true, health_value=true, weapons=true, distance=true, snaplines=false;
    bool skeleton=false, ammo=false, ammo_bar=false, status=false;
    bool armor=true, equipment=true, scoped=true, reloading=true, flashed=true, defusing=true, immunity=true;
    bool enemies_only=true, visible_only=false, only_alive=true, hide_in_menu=true;
    int box_style=1, interval_ms=33; // None, full, corners.
    float max_distance=150, thickness=1.5f, font_size=14, fill_alpha=.08f;
    std::array<float,4> visible_color{.2f,.9f,.5f,1}, hidden_color{1,.35f,.3f,1}, team_color{.3f,.65f,1,1};
    bool operator==(const Esp&) const = default;
};
struct Settings { Menu menu; Crosshair crosshair; Diagnostics diagnostics; Skins skins; Aim aim; ServerRules server_rules; Esp esp; };
void sanitize(Esp&);
const AimProfile& aim_profile(const Aim&, int group);
void sanitize(Aim&);
const Finish* find_finish(const Skins&, int item_id);
Finish& finish(Skins&, int item_id);
// Explicit item-page Apply enables only the selected valid finish and the master switch.
bool activate_finish(Skins&, int item_id);
void sanitize(Skins&);
void sanitize(Settings&);
bool load(const std::wstring& path, Settings&);
bool save(const std::wstring& path, const Settings&);
}
