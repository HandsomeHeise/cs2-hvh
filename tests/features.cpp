#include "config.hpp"
#include "crosshair.hpp"
#include "game_data.hpp"
#include "generated/dump_offsets.hpp"
#include "generated/cosmetics.hpp"
#include "verified_layout.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
namespace o = dump_offsets;
namespace v = verified_layout;
unsigned checks = 0;
void require(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
bool close(float a, float b) { return std::abs(a-b) < .0011f; }
struct Block { std::uintptr_t address; std::vector<unsigned char> data; };
struct Fixture {
    std::vector<Block> blocks;
    static constexpr std::uintptr_t client = 0x10000000, engine = 0x20000000, network = 0x30000000,
        system = 0x40000000, page0 = 0x50000000, page1 = 0x50010000,
        controller = 0x60000000, pawn = 0x60010000, weapon = 0x60020000, service = 0x60030000, names = 0x60040000;
    static constexpr std::uint32_t controller_handle = (7 << 15) | 1, pawn_handle = (11 << 15) | 3, weapon_handle = (13 << 15) | 513;
    bool change_world = false;
    int pawn_reads = 0;
    void add(std::uintptr_t address, std::size_t bytes) { blocks.push_back({address, std::vector<unsigned char>(bytes)}); }
    template<class T> void set(std::uintptr_t address, T value) {
        for (auto& block : blocks) if (address >= block.address && address - block.address + sizeof(T) <= block.data.size()) {
            std::memcpy(block.data.data() + address - block.address, &value, sizeof(T)); return;
        }
        throw std::runtime_error("fixture write out of range");
    }
    void text(std::uintptr_t address, const char* value) { for (std::size_t i=0; i<=strlen(value); ++i) set(address+i, value[i]); }
    static bool read(void* user, std::uintptr_t address, void* output, std::size_t count) {
        auto& self = *static_cast<Fixture*>(user);
        if (self.change_world && address == client + o::local_pawn && ++self.pawn_reads > 1) {
            std::memset(output, 0, count); return true;
        }
        for (const auto& block : self.blocks) if (address >= block.address && address - block.address <= block.data.size() && count <= block.data.size() - (address - block.address)) {
            std::memcpy(output, block.data.data() + address - block.address, count); return true;
        }
        return false;
    }
    void entity(std::uint32_t handle, std::uintptr_t instance) {
        const auto index = handle & v::index_mask;
        const auto identity = (index < 512 ? page0 : page1) + (index & 511) * v::identity_stride;
        set(identity, instance); set(identity + v::identity_handle, handle); set(instance + o::identity, identity);
    }
    Fixture() {
        add(client + o::local_controller, 8); add(client + o::local_pawn, 8); add(client + o::entity_system, 8);
        add(engine + o::build_number, 4); add(engine + o::network_client, 8);
        add(network, o::background_map + 64); add(system, 1024);
        add(page0, 512 * v::identity_stride); add(page1, 512 * v::identity_stride);
        add(controller, 0x1000); add(pawn, 0x2000); add(weapon, 0x2000); add(service, 0x100); add(names, 512);
        set(client + o::local_controller, controller); set(client + o::local_pawn, pawn); set(client + o::entity_system, system);
        set(engine + o::network_client, network); set(engine + o::build_number, o::expected_build);
        set(network + o::sign_on, 6); set<std::uint8_t>(network + o::background_map, 0);
        set(network + v::map_short, names); text(names, "de_dust2"); text(names + 128, "weapon_ak47");
        set(system + v::chunk_table, page0); set(system + v::chunk_table + 8, page1);
        entity(controller_handle, controller); entity(pawn_handle, pawn); entity(weapon_handle, weapon);
        set(controller + o::pawn_handle, pawn_handle); text(controller + o::player_name, "Fixture Player");
        set(pawn + o::health, 87); set<std::uint8_t>(pawn + o::team, 3); set<std::uint8_t>(pawn + o::life_state, 0);
        set(pawn + o::weapon_services, service); set(service + o::active_weapon, weapon_handle);
        const auto identity = page1 + v::identity_stride;
        set(identity + o::designer_name, names + 128); set(weapon + o::clip, 23);
        set<std::uint16_t>(weapon + o::attribute_manager + o::item + o::item_definition, 7);
    }
    game::Sources sources() {
        return {{read, this}, {client, o::client_image_size, o::client_timestamp}, {engine, o::engine_image_size, o::engine_timestamp}};
    }
};

void diagnostics_tests() {
    Fixture f;
    const auto s = game::collect(f.sources());
    require(s.profile_valid && s.in_game && s.connected && s.local_valid && s.alive, "valid local player");
    require(s.health == 87 && s.team == 3 && s.player == "Fixture Player" && s.map == "de_dust2", "dump-backed local fields");
    require(s.weapon_valid && s.weapon == "AK-47" && s.ammo == 23 && s.item_id == 7, "serial-checked weapon across entity chunks");
    f.text(Fixture::names + 128, "weapon_hkp2000");
    f.set<std::uint16_t>(Fixture::weapon + o::attribute_manager + o::item + o::item_definition, 61);
    const auto variant = game::collect(f.sources());
    require(variant.weapon == "USP-S", "item definition distinguishes weapon variants sharing a designer class");
    f.text(Fixture::names + 128, "weapon_ak47");
    f.set<std::uint16_t>(Fixture::weapon + o::attribute_manager + o::item + o::item_definition, 7);
    f.set<std::uint8_t>(Fixture::network + o::background_map, 1);
    auto value = game::collect(f.sources());
    require(value.state == game::State::menu && !value.in_game && !value.local_valid && value.player.empty() && !value.weapon_valid, "background map clears stale player data");
    f.set<std::uint8_t>(Fixture::network + o::background_map, 0); f.set(Fixture::network + o::sign_on, 4);
    value = game::collect(f.sources()); require(value.state == game::State::loading && !value.local_valid, "map loading state");
    f.set(Fixture::network + o::sign_on, 6);
    f.set(Fixture::service + o::active_weapon, Fixture::weapon_handle + (1 << 15));
    value = game::collect(f.sources()); require(value.local_valid && !value.weapon_valid, "recycled weapon handle rejected");
    f.set(Fixture::service + o::active_weapon, Fixture::weapon_handle);
    f.set(Fixture::controller + o::pawn_handle, Fixture::pawn_handle + (1 << 15));
    value = game::collect(f.sources()); require(!value.local_valid && !value.weapon_valid, "recycled pawn handle rejected");
    f.set(Fixture::controller + o::pawn_handle, Fixture::pawn_handle);
    f.set(Fixture::pawn + o::health, 0); f.set<std::uint8_t>(Fixture::pawn + o::life_state, 1);
    value = game::collect(f.sources()); require(value.local_valid && !value.alive && value.health == 0, "dead player state");
    f.set(Fixture::engine + o::build_number, o::expected_build + 1);
    value = game::collect(f.sources()); require(value.state == game::State::profile_mismatch && !value.local_valid, "build mismatch blocks field reads");
    f.set(Fixture::engine + o::build_number, o::expected_build);
    auto source = f.sources(); ++source.client.timestamp;
    value = game::collect(source); require(value.state == game::State::profile_mismatch, "PE version mismatch");
    f.set<std::uintptr_t>(Fixture::client + o::local_pawn, 0xdeadbeef);
    value = game::collect(f.sources()); require(!value.local_valid && value.health == -1, "unreadable pawn is safe");
    f.set(Fixture::client + o::local_pawn, Fixture::pawn);
    f.change_world = true; f.pawn_reads = 0;
    value = game::collect(f.sources()); require(value.state == game::State::read_error && value.player.empty() && value.map.empty(), "map transition rejects mixed samples");
    std::uint64_t empty = 0; require(!source.memory.get(0, empty), "null reads rejected");
}
void profile_tests() {
    const auto path = (std::filesystem::current_path() / "feature-test-profile.ini").wstring();
    config::Settings settings;
    settings.crosshair.style = 3; settings.crosshair.length = 23.5f; settings.crosshair.gap = 11;
    settings.crosshair.color[0] = .381f; settings.crosshair.color[3] = .723f;
    settings.crosshair.only_alive = false; settings.crosshair.center_dot = true;
    settings.diagnostics.enabled = false; settings.diagnostics.hud = true; settings.diagnostics.interval_ms = 850;
    settings.menu.watermark = true; settings.menu.opacity = .65f;
    require(config::save(path, settings), "atomic profile save");
    config::Settings loaded;
    require(config::load(path, loaded), "profile load");
    require(loaded.crosshair.style == 3 && close(loaded.crosshair.length, 23.5f) && close(loaded.crosshair.color[0], .381f) && close(loaded.crosshair.color[3], .723f), "crosshair shape, geometry and RGBA persisted");
    require(!loaded.crosshair.only_alive && loaded.crosshair.center_dot && !loaded.diagnostics.enabled && loaded.diagnostics.hud && loaded.diagnostics.interval_ms == 850 && loaded.menu.watermark && close(loaded.menu.opacity, .65f), "conditions and diagnostics persisted");
    const auto legacy = std::filesystem::current_path() / "feature-test-legacy.ini";
    { std::ofstream out(legacy); out << "[Menu]\nWatermark=1\nScale=115\n"; }
    require(config::load(legacy.wstring(), loaded) && loaded.menu.watermark && close(loaded.menu.scale, 1.15f) && loaded.crosshair.enabled && loaded.crosshair.only_in_game && !loaded.skins.enabled && loaded.skins.finishes.empty() && !loaded.aim.enabled && loaded.aim == config::Aim{} && loaded.server_rules == config::ServerRules{}, "legacy menu profiles keep defaults for new fields");
    const auto invalid = std::filesystem::current_path() / "feature-test-invalid.ini";
    { std::ofstream out(invalid); out << "[Crosshair]\nLength=-100\nStyle=999\nThickness=garbage\nRed=4000\n[Diagnostics]\nIntervalMs=-1\n"; }
    require(config::load(invalid.wstring(), loaded) && loaded.crosshair.length == 1 && loaded.crosshair.style == 3 && loaded.crosshair.thickness == 1.5f && loaded.crosshair.color[0] == 1 && loaded.diagnostics.interval_ms == 100, "invalid profile values bounded");
    require(!config::save((std::filesystem::current_path()/"missing-directory"/"profile.ini").wstring(), settings), "save failure reported");
    std::filesystem::remove(path); std::filesystem::remove(legacy); std::filesystem::remove(invalid);
}
void skin_profile_tests() {
    const auto path = (std::filesystem::current_path() / "feature-test-skins.ini").wstring();
    config::Settings settings;
    auto& s = settings.skins;
    s.enabled = true;
    int knife_count = 0, glove_count = 0;
    for (const auto& item : cosmetics::items) {
        if (item.category == cosmetics::Category::knife && knife_count < 2) s.knives[knife_count++] = item.id;
        if (item.category == cosmetics::Category::gloves && glove_count < 2) s.gloves[glove_count++] = item.id;
        auto& f = config::finish(s, item.id); f.enabled = true;
        const auto paints = cosmetics::paints_for(item);
        f.paint_id = paints.size() > 1 ? paints[1] : 0;
        f.seed = item.id % 1001; f.stattrak = item.category == cosmetics::Category::gloves ? -1 : 12345;
        f.wear = .137891f;
    }
    require(knife_count == 2 && glove_count == 2, "catalog provides independent T/CT knife and glove selections");
    config::sanitize(settings);
    require(config::save(path, settings), "complete skin profile save");
    config::Settings loaded;
    require(config::load(path, loaded) && loaded.skins.enabled && loaded.skins.finishes.size() == s.finishes.size(), "all catalog item profiles loaded");
    require(loaded.skins.knives[0] == s.knives[0] && loaded.skins.knives[1] == s.knives[1] && loaded.skins.gloves[0] == s.gloves[0] && loaded.skins.gloves[1] == s.gloves[1], "T/CT model choices persist independently");
    bool same = true;
    for (const auto& f : s.finishes) {
        const auto* actual = config::find_finish(loaded.skins, f.item_id);
        same &= actual && actual->enabled == f.enabled && actual->paint_id == f.paint_id && actual->seed == f.seed && actual->stattrak == f.stattrak && std::abs(actual->wear - f.wear) < .0000011f;
    }
    require(same, "compatible paint IDs, seed, wear precision and StatTrak roundtrip for every item");
    auto& invalid = loaded.skins;
    invalid.knives[0] = 7; invalid.gloves[1] = 99999;
    auto& ak = config::finish(invalid, 7);
    ak.seed = -123; ak.stattrak = 2000000; ak.wear = std::numeric_limits<float>::quiet_NaN();
    for (const auto& p : cosmetics::paints) if (!cosmetics::compatible(7, p.id)) { ak.paint_id = p.id; break; }
    require(!cosmetics::compatible(7, ak.paint_id), "fixture contains a finish incompatible with AK-47");
    config::Finish unknown; unknown.item_id = 99999; unknown.enabled = true; invalid.finishes.push_back(unknown);
    auto duplicate = *config::find_finish(invalid, 7); duplicate.seed = 999; invalid.finishes.push_back(duplicate);
    config::sanitize(invalid);
    const auto* clean = config::find_finish(invalid, 7);
    require(clean && clean->paint_id == 0 && !clean->enabled && clean->seed == 0 && clean->stattrak == 1000000 && std::isfinite(clean->wear), "incompatible paint disabled; bounds and NaN wear sanitized");
    require(invalid.knives[0] == 0 && invalid.gloves[1] == 0 && !config::find_finish(invalid, 99999), "unknown item IDs and models of wrong category rejected");
    unsigned duplicates = 0; for (const auto& f : invalid.finishes) duplicates += f.item_id == 7;
    require(duplicates == 1, "duplicate per-item settings normalized deterministically");
    config::finish(invalid, s.gloves[0]).stattrak = 42; config::sanitize(invalid);
    require(config::find_finish(invalid, s.gloves[0])->stattrak == -1, "gloves never retain unsupported StatTrak");
    std::filesystem::remove(path);
}
void skin_activation_tests() {
    config::Settings settings;
    config::finish(settings.skins, 9); // An unrelated, disabled AWP selection must stay disabled.
    require(!settings.skins.enabled && config::activate_finish(settings.skins, 7) && settings.skins.enabled &&
        config::find_finish(settings.skins, 7)->enabled && !config::find_finish(settings.skins, 9)->enabled &&
        config::find_finish(settings.skins, 7)->paint_id == 0, "explicit Apply activates selected default finish and master switch only");
    config::Skins invalid;
    auto& f = config::finish(invalid, 7);
    for (const auto& paint : cosmetics::paints) if (!cosmetics::compatible(7, paint.id)) { f.paint_id = paint.id; break; }
    const auto before = invalid;
    require(!config::activate_finish(invalid, 7) && invalid == before && !config::activate_finish(invalid, 99999) && invalid == before,
        "invalid Apply does not activate or modify an incompatible or unknown item");
    const auto path = (std::filesystem::current_path() / "feature-test-apply.ini").wstring();
    require(config::save(path, settings), "explicit Apply settings save atomically");
    config::Settings loaded;
    require(config::load(path, loaded) && loaded.skins.enabled && config::find_finish(loaded.skins, 7)->enabled &&
        !config::find_finish(loaded.skins, 9)->enabled, "applied activation survives clean profile reload without enabling other items");
    std::filesystem::remove(path);
}
void crosshair_tests() {
    config::Crosshair c;
    const auto shape = crosshair::geometry(c, 960, 600);
    require(shape.count == 4 && shape.items[0].x1 == 949 && shape.items[0].x2 == 956 && shape.items[1].x1 == 964, "crosshair gap and length");
    c.center_dot = true; require(crosshair::geometry(c, 960, 600).count == 5, "center dot geometry");
    c.style = 3; require(crosshair::geometry(c, 960, 600).count == 4, "T shape geometry");
    c.style = 2; require(crosshair::geometry(c, 640, 360).items[0].x1 == 640 && crosshair::geometry(c, 640, 360).items[0].kind == crosshair::Kind::ring, "circle follows resized viewport center");
    c.style = 1; require(crosshair::geometry(c, 640, 360).count == 1, "dot mode avoids duplicate center dot");
    require(crosshair::visible(c, false, true, true) && !crosshair::visible(c, true, true, true) && !crosshair::visible(c, false, false, false) && !crosshair::visible(c, false, true, false), "menu, match and alive conditions");
    c.only_alive = false; c.only_in_game = false; require(crosshair::visible(c, false, false, false), "explicit always-visible mode");
}
void aim_profile_tests() {
    config::Settings settings;
    auto& a = settings.aim;
    require(!a.enabled && a.per_weapon && a.profiles[0].hitboxes == 3 && a.key == VK_XBUTTON1, "aim defaults OFF with head/chest and Mouse 4 hold activation");
    { config::AimProfile p; require(config::requires_visibility(p),"visibility remains required without Autowall");
      p.penetration=true; require(!config::requires_visibility(p) && p.visible_only,"Autowall includes covered targets without erasing saved visibility preference");
      p.penetration=false; require(config::requires_visibility(p),"turning Autowall off restores saved direct-visibility filter"); }
    a.profiles[0].fov = 17;
    a.profiles[4].fov = 8;
    require(&config::aim_profile(a, 4) == &a.profiles[0] && &config::aim_profile(a, 99) == &a.profiles[0], "weapon and unknown groups inherit global until explicitly overridden");
    a.profiles[4].override_global = true;
    require(&config::aim_profile(a, 4) == &a.profiles[4] && config::aim_profile(a, 4).fov == 8, "selected weapon override is independent from global");
    a.per_weapon = false;
    require(&config::aim_profile(a, 4) == &a.profiles[0] && a.profiles[4].override_global, "disabling per-weapon mode preserves overrides while using global");
    a.enabled = true; a.per_weapon = true; a.activation = 2; a.key = VK_MENU;
    a.fov_ring = false; a.target_marker = false; a.info = true;
    for (std::size_t i = 0; i < a.profiles.size(); ++i) {
        auto& p = a.profiles[i];
        p.override_global = i != 0; p.recoil = i % 2; p.auto_fire = true; p.auto_scope = i % 2;
        p.auto_stop = true; p.penetration = i % 2; p.visible_only = false; p.silent=i%2;
        p.ignore_smoke = i % 2; p.ignore_flashed = false; p.ignore_airborne = true; p.multipoint = true;
        p.fov = 12.125f + i; p.smoothing = 2.375f + i; p.multipoint_scale = (125 + static_cast<int>(i) * 100) / 1000.f;
        p.priority = i % 3; p.hitboxes = 1 + i; p.min_damage = 7 + i * 13;
        p.hitchance = 11 + i * 10; p.switch_delay_ms = 50 + i * 81;
    }
    const auto path = (std::filesystem::current_path() / "feature-test-aim.ini").wstring();
    require(config::save(path, settings), "aim profile saved atomically");
    config::Settings loaded;
    require(config::load(path, loaded) && loaded.aim == a, "all nine aim profiles, feature toggles and fractional values roundtrip");
    a.activation = 99; a.key = -9; a.profiles[0].override_global = true;
    auto& invalid = a.profiles[4];
    invalid.fov = std::numeric_limits<float>::quiet_NaN(); invalid.smoothing = -1;
    invalid.multipoint_scale = std::numeric_limits<float>::infinity(); invalid.priority = -20;
    invalid.hitboxes = 32; invalid.min_damage = 999; invalid.hitchance = -1; invalid.switch_delay_ms = 99999;
    config::sanitize(a);
    require(a.activation == 2 && a.key == 1 && !a.profiles[0].override_global && invalid.fov == 30 && invalid.smoothing == 1 && close(invalid.multipoint_scale, .7f) &&
        invalid.priority == 0 && invalid.hitboxes == 3 && invalid.min_damage == 200 && invalid.hitchance == 0 && invalid.switch_delay_ms == 2000,
        "aim invalid modes, NaN, infinity, unknown hitboxes and numerical ranges sanitized");
    { std::ofstream out(path.c_str()); out << "[Aim]\nEnabled=0\nActivation=-1\nKey=999\n[Aim.Group.4]\nOverrideGlobal=1\nFOV=garbage\nSmoothing=999999\nMultipointScale=-9\nHitboxes=0\nMinDamage=-5\nHitchance=999\nSwitchDelayMs=-5\n"; }
    require(config::load(path, loaded) && !loaded.aim.enabled && loaded.aim.activation == 0 && loaded.aim.key == 254 &&
        loaded.aim.profiles[4].fov == 30 && loaded.aim.profiles[4].smoothing == 30 && close(loaded.aim.profiles[4].multipoint_scale, .1f) &&
        loaded.aim.profiles[4].hitboxes == 3 && loaded.aim.profiles[4].min_damage == 0 && loaded.aim.profiles[4].hitchance == 100 && loaded.aim.profiles[4].switch_delay_ms == 0,
        "malformed and out-of-range persisted aim values recover to safe defaults and bounds");
    std::filesystem::remove(path);
}
void server_rule_profile_tests() {
    config::Settings settings;
    require(!settings.server_rules.no_spread && !settings.server_rules.no_recoil, "server-wide spread and recoil overrides default OFF");
    const auto path = (std::filesystem::current_path() / "feature-test-server-rules.ini").wstring();
    settings.server_rules.no_spread = true;
    config::Settings loaded;
    require(config::save(path, settings) && config::load(path, loaded) && loaded.server_rules == settings.server_rules &&
        !loaded.aim.enabled && loaded.aim.profiles[0].recoil,
        "No Spread persists independently from No Recoil and client aim/recoil compensation settings");
    settings.server_rules = {false,true};
    require(config::save(path, settings) && config::load(path, loaded) && loaded.server_rules == settings.server_rules,
        "server No Recoil persists independently while No Spread is disabled");
    { std::ofstream out(path.c_str()); out << "[ServerRules]\nNoSpread=garbage\n[Aim]\nEnabled=1\n"; }
    require(config::load(path, loaded) && loaded.server_rules == config::ServerRules{} && loaded.aim.enabled,
        "missing or malformed server rule keys remain OFF without changing independent aim settings");
    std::filesystem::remove(path);
}
}
int main() {
    try {
        diagnostics_tests(); profile_tests(); skin_profile_tests(); skin_activation_tests(); crosshair_tests(); aim_profile_tests(); server_rule_profile_tests();
        std::ofstream("feature-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"memory fixtures, cosmetic/aim/server rule profiles, inheritance, crosshair geometry and visibility\"}\n";
        std::cout << "features PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "features FAIL: " << error.what() << '\n'; return 1;
    }
}
