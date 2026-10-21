#pragma once
#include "config.hpp"
#include <cstdint>
#include <string>

namespace aim {
enum class State { disabled, unavailable, profile_mismatch, waiting_for_player, idle, tracking, firing, error };
enum class FireBlock {
    none, not_ready, auto_fire_off, switch_delay, input_unavailable, context_changed, weapon_state,
    reloading, empty_clip, alignment, damage, scope, stopping, chance_unavailable,
    weapon_cooldown, readiness_unavailable,
    sample_shortage, chance_below_threshold, visibility, semi_auto_cooldown
};
struct Capabilities {
    bool view_angles = false, target_selection = false, bones = false, visibility = false;
    bool recoil = false, auto_fire = false, auto_scope = false, auto_stop = false;
    bool damage = false, penetration = false, hitchance = false, multipoint = false, smoke = false;
    bool silent = false;
};
struct Status {
    State state = State::unavailable;
    Capabilities capabilities;
    bool supported = false, active = false, has_target = false;
    bool target_on_screen = false, projection_valid = false;
    int group = 0, candidates = 0, target_index = -1, target_health = -1, hitbox = 0, hit_chance = -1;
    int penetrations = 0;
    float angular_error = 0, damage = 0, inaccuracy = 0, speed = 0;
    float camera_fov = 90, fov_degrees = 30, target_screen_x = 0, target_screen_y = 0;
    float work_ms = 0, peak_ms = 0;
    int chance_samples = 0; // Native chance rays executed in this input callback.
    int chance_sample_count = 0; // N of the completed current empirical estimate; otherwise zero.
    FireBlock fire_block = FireBlock::not_ready;
    float alignment_error = 0;
    bool chance_complete = false;
    std::uint64_t callbacks = 0, updates = 0, shots = 0, sampled_at = 0;
    std::uint64_t silent_samples = 0;
    std::string detail = "Waiting for verified input and trace interfaces";
};
void configure(const config::Aim&);
void set_menu_open(bool);
Status latest();
const char* state_name(State);
const char* fire_block_name(FireBlock);
int weapon_group(int item_id);
const char* group_name(int group);
// MinHook lifecycle belongs to the existing overlay. Native calls execute only
// in the inspected game input callback; the menu can be used in the test host.
void install_runtime();
bool stop_runtime();
}
