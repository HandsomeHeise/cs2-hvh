#pragma once
#include "config.hpp"
#include <cstdint>
#include <string>

namespace server_rules {
enum class State { unavailable, disabled, waiting_for_server, ready, applied, permission_denied, error, profile_mismatch };
struct Status {
    State state = State::unavailable;
    bool supported = false, local_server = false;
    bool cheats = false; // Observed sv_cheats value only; never an authority requirement.
    bool no_spread_supported = false, no_recoil_supported = false;
    float spread_value = -1, recoil_value = -1;
    float observed_spread = -1, observed_inaccuracy = -1, observed_recoil = -1;
    std::uint64_t client_calls = 0, server_calls = 0;
    std::uint64_t updates = 0;
    std::string detail = "Waiting for your active local listen server";
};
void configure(const config::ServerRules&);
Status latest();
const char* state_name(State);
void install_runtime();
void poll_runtime(); // Existing overlay worker; retries only if modules are not loaded yet.
void update_on_input();
bool stop_runtime();
}
