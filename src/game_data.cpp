#include "game_data.hpp"
#include "generated/dump_offsets.hpp"
#include "verified_layout.hpp"
#include "generated/weapon_names.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>

namespace game {
namespace {
namespace o = dump_offsets;
namespace v = verified_layout;
std::mutex snapshot_mutex;
Snapshot published;
std::atomic<bool> requested_enabled{true};
std::atomic<int> requested_interval{250};
bool pointer(std::uintptr_t address) { return address >= 0x10000 && address <= 0x00007fffffffffffULL; }

std::uintptr_t identity_for(const Memory& memory, std::uintptr_t system, std::uint32_t handle) {
    if (!pointer(system) || handle == 0xffffffff) return 0;
    const auto index = handle & v::index_mask;
    std::uintptr_t page = 0, instance = 0;
    if (!memory.get(system + v::chunk_table + (index / v::entries_per_chunk) * sizeof(std::uintptr_t), page) || !pointer(page)) return 0;
    const auto identity = page + (index % v::entries_per_chunk) * v::identity_stride;
    std::uint32_t stored = 0;
    if (!memory.get(identity + v::identity_handle, stored) || stored != handle || !memory.get(identity, instance) || !pointer(instance)) return 0;
    std::uintptr_t backlink = 0;
    return memory.get(instance + o::identity, backlink) && backlink == identity ? identity : 0;
}

bool stable_entity(const Memory& memory, std::uintptr_t system, std::uintptr_t instance) {
    std::uintptr_t identity = 0; std::uint32_t handle = 0xffffffff;
    return pointer(instance) && memory.get(instance + o::identity, identity) && pointer(identity) &&
        memory.get(identity + v::identity_handle, handle) && identity_for(memory, system, handle) == identity;
}

void weapon_data(const Memory& memory, std::uintptr_t system, std::uintptr_t pawn, Snapshot& s) {
    std::uintptr_t service = 0;
    std::uint32_t handle = 0xffffffff;
    if (!memory.get(pawn + o::weapon_services, service) || !pointer(service) || !memory.get(service + o::active_weapon, handle)) return;
    const auto identity = identity_for(memory, system, handle);
    std::uintptr_t instance = 0, name = 0;
    if (!identity || !memory.get(identity, instance) || !memory.get(identity + o::designer_name, name)) return;
    std::string designer;
    if (!memory.string(name, designer) || designer.rfind("weapon_", 0) != 0) return;
    std::uint16_t item = 0;
    int ammo = -1;
    if (!memory.get(instance + o::clip, ammo)) ammo = -1;
    if (!memory.get(instance + o::attribute_manager + o::item + o::item_definition, item)) item = 0;
    // Recheck the serial-bearing handle after the field reads to reject a recycled slot.
    if (identity_for(memory, system, handle) != identity) return;
    s.weapon_valid = true; s.weapon = designer.substr(7);
    for (auto& character : s.weapon) {
        if (character == '_') character = ' ';
        else if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
    }
    if (const auto* label = weapon_names::lookup(item)) s.weapon = label;
    s.item_id = item ? item : -1;
    s.ammo = ammo >= 0 && ammo <= 10000 ? ammo : -1;
}

bool self_read(void*, std::uintptr_t address, void* output, std::size_t count) {
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, count, &copied) && copied == count;
}
Module module_info(const Memory& memory, const wchar_t* name) {
    Module result;
    result.base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(name));
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 headers{};
    if (!result.base || !memory.get(result.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x100000 ||
        !memory.get(result.base + dos.e_lfanew, headers) || headers.Signature != IMAGE_NT_SIGNATURE || headers.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return {};
    result.timestamp = headers.FileHeader.TimeDateStamp; result.image_size = headers.OptionalHeader.SizeOfImage;
    return result;
}
}

bool Memory::read(std::uintptr_t address, void* output, std::size_t count) const {
    if (!function || !output || !count || !pointer(address) || count > 4096 || address > std::numeric_limits<std::uintptr_t>::max() - count) return false;
    return function(user, address, output, count);
}
bool Memory::string(std::uintptr_t address, std::string& output, std::size_t limit) const {
    output.clear(); limit = std::min<std::size_t>(limit, 128);
    for (std::size_t i = 0; i < limit; ++i) {
        unsigned char character = 0;
        if (!get(address + i, character)) { output.clear(); return false; }
        if (!character) return true;
        output += character < 32 || character == 127 ? ' ' : static_cast<char>(character);
    }
    output.clear(); return false; // Never display an unterminated/truncated foreign string.
}

Snapshot collect(const Sources& source) {
    Snapshot s;
    const auto& m = source.memory;
    if (!source.client.base || !source.engine.base) return s;
    if (source.client.timestamp != o::client_timestamp || source.client.image_size != o::client_image_size ||
        source.engine.timestamp != o::engine_timestamp || source.engine.image_size != o::engine_image_size) {
        s.state = State::profile_mismatch; s.detail = "Module version differs from the imported dump profile"; return s;
    }
    if (!m.get(source.engine.base + o::build_number, s.build)) { s.state = State::read_error; s.detail = "Build number unreadable"; return s; }
    if (s.build != static_cast<int>(o::expected_build)) {
        s.state = State::profile_mismatch; s.detail = "Build differs from the imported dump profile"; return s;
    }
    s.profile_valid = true;
    std::uintptr_t network = 0;
    if (!m.get(source.engine.base + o::network_client, network)) { s.state = State::read_error; s.detail = "Network state unreadable"; return s; }
    if (!network) { s.state = State::menu; s.detail = "No active network client"; return s; }
    std::uint8_t background = 0;
    if (!m.get(network + o::sign_on, s.signon) || !m.get(network + o::background_map, background) || s.signon < 0 || s.signon > 8 || background > 1) {
        s.state = State::read_error; s.detail = "Network state changed or became unreadable"; return s;
    }
    s.background = background != 0; s.connected = s.signon >= 2;
    s.in_game = !s.background && s.signon >= 6;
    s.state = s.background || s.signon == 0 ? State::menu : s.signon < 2 ? State::connecting : s.signon < 6 ? State::loading : State::in_game;
    s.detail = state_name(s.state);
    if (s.connected && !s.background) {
        std::uintptr_t level = 0;
        if (m.get(network + v::map_short, level)) m.string(level, s.map);
        if (s.map.empty() && m.get(network + v::map_full, level)) m.string(level, s.map);
        if (s.map == "<empty>" || s.map == "maps/<empty>.vpk") s.map.clear();
    }
    if (!s.in_game) return s; // Never expose stale player data from a menu/background world.
    std::uintptr_t controller = 0, pawn = 0, system = 0;
    m.get(source.client.base + o::local_controller, controller);
    m.get(source.client.base + o::local_pawn, pawn);
    m.get(source.client.base + o::entity_system, system);
    if (!pointer(controller) || !pointer(pawn) || !stable_entity(m, system, controller) || !stable_entity(m, system, pawn)) {
        s.detail = "Waiting for a valid local player"; return s;
    }
    std::uint32_t pawn_handle = 0xffffffff;
    std::uintptr_t expected_pawn_identity = 0;
    m.get(controller + o::pawn_handle, pawn_handle); m.get(pawn + o::identity, expected_pawn_identity);
    if (identity_for(m, system, pawn_handle) != expected_pawn_identity) { s.detail = "Local player changed; waiting for the next sample"; return s; }
    std::uint8_t team = 0, life = 255;
    if (!m.get(pawn + o::health, s.health) || !m.get(pawn + o::team, team) || !m.get(pawn + o::life_state, life) || s.health < 0 || s.health > 100000 || life > 3) {
        s.health = -1; s.detail = "Local player fields unreadable"; return s;
    }
    s.local_valid = true; s.team = team; s.alive = life == 0 && s.health > 0;
    m.string(controller + o::player_name, s.player, 128);
    weapon_data(m, system, pawn, s);
    std::uintptr_t final_pawn = 0, final_network = 0;
    int final_signon = -1;
    std::uint8_t final_background = 255;
    if (!m.get(source.client.base + o::local_pawn, final_pawn) || final_pawn != pawn ||
        !m.get(source.engine.base + o::network_client, final_network) || final_network != network ||
        !m.get(network + o::sign_on, final_signon) || final_signon != s.signon ||
        !m.get(network + o::background_map, final_background) || final_background != background) {
        Snapshot changed; changed.state = State::read_error; changed.build = s.build; changed.profile_valid = true;
        changed.detail = "World changed during the sample; retrying"; return changed;
    }
    return s;
}

const char* state_name(State state) {
    switch (state) {
    case State::disabled: return "Disabled";
    case State::profile_mismatch: return "Dump profile mismatch";
    case State::menu: return "Main menu / background";
    case State::connecting: return "Connecting";
    case State::loading: return "Loading map";
    case State::in_game: return "In game";
    case State::read_error: return "Data unavailable";
    default: return "CS2 modules unavailable";
    }
}
Sources current_sources() {
    const Memory memory{self_read, nullptr};
    return {memory, module_info(memory, L"client.dll"), module_info(memory, L"engine2.dll")};
}
void configure(bool enabled, int interval_ms) { requested_enabled = enabled; requested_interval = std::clamp(interval_ms, 100, 1000); }
void tick() {
    static ULONGLONG next = 0;
    const auto now = GetTickCount64();
    if (now < next) return;
    next = now + requested_interval.load();
    Snapshot sample;
    if (!requested_enabled.load()) { sample.state = State::disabled; sample.detail = "Live data sampling disabled"; }
    else {
        sample = collect(current_sources());
    }
    sample.sampled_at = now;
    std::lock_guard guard(snapshot_mutex); published = std::move(sample);
}
Snapshot latest() { std::lock_guard guard(snapshot_mutex); return published; }
}
