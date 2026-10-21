#pragma once
#include "game_data.hpp"
#include "generated/dump_offsets.hpp"
#include "generated/aim_native.hpp"
#include <array>
#include <cstring>

// Read-only, replicated match rule. Native registration and relation consumers:
// analysis/ffa-enemy-rule-recovery.json. Never infer FFA from the map name.
namespace enemy_rules {
struct Rule {
    bool known=false, free_for_all=false;
    bool operator==(const Rule&) const = default;
};
inline Rule read(const game::Sources& source) {
    namespace o=dump_offsets;
    if (!source.client.base || source.client.timestamp!=o::client_timestamp ||
        source.client.image_size!=o::client_image_size) return {};
    const auto& m=source.memory;
    std::uintptr_t data=0,name=0,current=0;
    std::uint16_t type=0xffff; std::uint8_t value=255;
    constexpr char expected[]="mp_teammates_are_enemies";
    std::array<char,sizeof(expected)> text{};
    const auto pointer=[](std::uintptr_t p) { return p>=0x10000 && p<=0x7fffffffffffULL; };
    if (!m.get(source.client.base+aim_native::teammates_are_enemies,data) || !pointer(data) ||
        !m.get(data,name) || !pointer(name) || !m.get(name,text) || std::memcmp(text.data(),expected,sizeof(expected))!=0 ||
        !m.get(data+0x28,type) || type!=0 || !m.get(data+0x58,value) || value>1 ||
        !m.get(source.client.base+aim_native::teammates_are_enemies,current) || current!=data) return {};
    return {true,value!=0};
}
inline bool enemy(int local_team,int target_team,Rule rule) {
    return (local_team==2 || local_team==3) && (target_team==2 || target_team==3) &&
        (target_team!=local_team || (rule.known && rule.free_for_all));
}
}
