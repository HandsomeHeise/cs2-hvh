#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace game {
enum class State { disabled, unavailable, profile_mismatch, menu, connecting, loading, in_game, read_error };
struct Snapshot {
    State state = State::unavailable;
    bool profile_valid = false, connected = false, background = false, in_game = false;
    bool local_valid = false, alive = false, weapon_valid = false;
    int build = -1, signon = -1, health = -1, team = -1, ammo = -1, item_id = -1;
    std::uint64_t sampled_at = 0;
    std::string map, player, weapon, detail = "Waiting for CS2 modules";
};
using ReadFunction = bool (*)(void*, std::uintptr_t, void*, std::size_t);
struct Memory {
    ReadFunction function = nullptr;
    void* user = nullptr;
    bool read(std::uintptr_t address, void* output, std::size_t count) const;
    template<class T> bool get(std::uintptr_t address, T& output) const { return read(address, &output, sizeof(T)); }
    bool string(std::uintptr_t address, std::string& output, std::size_t limit = 128) const;
};
struct Module { std::uintptr_t base = 0; std::uint32_t image_size = 0, timestamp = 0; };
struct Sources { Memory memory; Module client, engine; };
Snapshot collect(const Sources&);
Sources current_sources();
const char* state_name(State);
void configure(bool enabled, int interval_ms);
void tick(); // Called only by the existing worker; no game functions are executed.
Snapshot latest();
}
