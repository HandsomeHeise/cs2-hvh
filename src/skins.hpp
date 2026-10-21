#pragma once
#include "config.hpp"
#include <string>
#include <cstdint>

namespace skins {
enum class State { disabled, unavailable, profile_mismatch, waiting_for_player, ready, applied, error };
struct Status {
    State state = State::unavailable;
    bool supported = false, weapons_supported = false, knives_supported = false, gloves_supported = false;
    unsigned applied_weapons = 0;
    bool knife_applied = false, gloves_applied = false;
    std::uint64_t frame_callbacks = 0;
    std::vector<int> carried_items, applied_items;
    std::string detail = "Waiting for runtime verification";
};
void configure(const config::Skins&);
void request_apply();
Status latest();
const char* state_name(State);
}
