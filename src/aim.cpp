#include "aim.hpp"
#include "aim_core.hpp"
#include "aim_spread.hpp"
#include "server_rules.hpp"
#include "esp.hpp"
#include "enemy_rules.hpp"
#include "game_data.hpp"
#include "generated/dump_offsets.hpp"
#include "generated/aim_native.hpp"
#include "verified_layout.hpp"
#include "MinHook.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <limits>

namespace aim {
namespace {
namespace o = dump_offsets;
namespace n = aim_native;
namespace v = verified_layout;
using aim_core::Vec3;
using InputFrame = void (*)(void*, int, bool);
using Angles = void (*)(void*, int, Vec3*);
using BoneIndex = int (*)(void*, const char*);
using BonePosition = void (*)(void*, int, Vec3*, Vec3*);
using TraceLine = void (*)(void*, const Vec3*, const Vec3*, void*, std::uint64_t, void*);
using Spread = float (*)(void*);
using Inaccuracy = float (*)(void*, float*, float*);
using Punch = Vec3* (*)(void*, Vec3*, bool);
using Construct = void* (*)(void*);
using Destruct = void (*)(void*);
using FilterConstruct = void* (*)(void*, void*, std::uint64_t, std::uint8_t, std::uint8_t);
using BulletCreate = void (*)(void*, const Vec3*, const Vec3*, void*, int, bool);
using BulletPenetrate = void (*)(void*, float, float, float, int, int, void*);
using BulletInterval = void (*)(void*, void*, void*);
using HitGroup = int (*)(void*);
using IsArmored = bool (*)(void*, int);
using ActionDelta = void (*)(void*, int, float, float);
using ThroughSmoke = bool (*)(const Vec3*, const Vec3*);
using GetHitboxes = int (*)(void*, void*, void*, std::uint32_t);
using GameFree = void (*)(void*);
using PrimaryReady = bool (*)(void*);
using HistoryParse = void (*)(const void*,void*,bool,float,float,void*);
std::mutex settings_mutex, status_mutex, execution_mutex;
config::Aim requested;
std::uint64_t settings_revision = 0, sampled_settings_revision = ~std::uint64_t{0};
std::atomic<std::uint64_t> context_revision{0};
std::uint64_t sampled_context_revision=~std::uint64_t{0};
Status published;
std::atomic<bool> closing{false}, menu_open{true}, native_fault{false};
std::atomic<unsigned> callbacks{0};
std::atomic<std::uint64_t> callback_count{0};
std::atomic<float> last_work_ms{0}, peak_work_ms{0};
void* hook_address = nullptr;
void* history_hook_address = nullptr;
InputFrame original_frame = nullptr;
HistoryParse original_history = nullptr;
std::atomic<unsigned> history_callbacks{0};
std::atomic<std::uint64_t> silent_sample_count{0};
Angles get_angles = nullptr, set_angles = nullptr;
BoneIndex bone_index = nullptr;
BonePosition bone_position = nullptr;
TraceLine trace_line = nullptr;
Spread get_spread = nullptr;
Inaccuracy get_inaccuracy = nullptr;
Punch get_punch = nullptr;
Construct trace_construct = nullptr;
Destruct trace_destruct = nullptr;
FilterConstruct filter_construct = nullptr;
BulletCreate bullet_create = nullptr;
BulletPenetrate bullet_penetrate = nullptr;
BulletInterval bullet_interval = nullptr;
HitGroup hit_group = nullptr;
IsArmored is_armored = nullptr;
bool ballistics_verified = false;
ActionDelta action_delta = nullptr;
ThroughSmoke through_smoke = nullptr;
bool smoke_verified = false;
GetHitboxes get_hitboxes = nullptr;
GameFree free_hitboxes = nullptr;
bool hitboxes_verified = false;
bool actions_verified = false;
bool primary_ready_verified = false;
PrimaryReady primary_ready_query = nullptr;
std::array<bool,6> owned_actions{};
std::atomic<bool> actions_released{true};
std::atomic<bool> action_fault{false};
struct FireState {
    std::uintptr_t system=0, pawn=0, weapon=0;
    std::uint32_t local_handle=0xffffffff, weapon_handle=0xffffffff;
    std::uint64_t pulse_until=0, next_pulse=0;
    bool bound=false;
} fire_state;
void reset_fire_state() { fire_state={}; }
game::Sources sources;
std::uint64_t updates = 0, selected_at = 0;
std::uint32_t pending = 0xffffffff;
bool toggle = false, key_previous = false;
int previous_activation = -1, previous_key = -1;
bool previous_enabled = false;
bool focused();
struct SilentRequest {
    bool valid=false;
    Vec3 angles{},target_origin{};
    std::uintptr_t input=0,system=0,pawn=0,weapon=0,target=0;
    std::uint32_t local_handle=0xffffffff,weapon_handle=0xffffffff,target_handle=0xffffffff;
    std::uint64_t settings=0,context=0,created_at=0;
    int team=0,health=0;
    bool ignore_airborne=false;
    enemy_rules::Rule enemy_rule;
};
thread_local const SilentRequest* active_silent=nullptr;
struct SilentScope {
    const SilentRequest* previous=active_silent;
    explicit SilentScope(const SilentRequest& request) { active_silent=&request; }
    ~SilentScope() { active_silent=previous; }
};
void clear_target_caches();

bool pointer(std::uintptr_t p) { return p >= 0x10000 && p <= 0x7fffffffffffULL; }
void publish(Status s) {
    if (native_fault) { s.supported = false; s.capabilities = {}; s.active = false; }
    if (!s.supported || s.state == State::error || s.state == State::profile_mismatch || s.state == State::waiting_for_player) {
        s.has_target = s.target_on_screen = s.active = false; pending = 0xffffffff;
    }
    if (!s.active || !s.has_target) clear_target_caches();
    s.sampled_at = GetTickCount64(); std::lock_guard guard(status_mutex); published = std::move(s);
}
template<class T> bool read(std::uintptr_t p, T& value) { return sources.memory.get(p, value); }
std::uintptr_t indexed(std::uintptr_t system, unsigned index) {
    std::uintptr_t chunk = 0, entity = 0, back = 0;
    if (index > v::index_mask || !read(system + v::chunk_table + (index / 512) * 8, chunk) || !pointer(chunk)) return 0;
    const auto identity = chunk + (index % 512) * v::identity_stride;
    std::uint32_t handle = 0xffffffff;
    return read(identity, entity) && pointer(entity) && read(identity + v::identity_handle, handle) &&
        (handle & v::index_mask) == index && read(entity + o::identity, back) && back == identity ? entity : 0;
}
std::uintptr_t resolve(std::uintptr_t system, std::uint32_t handle) {
    if (handle == 0xffffffff) return 0;
    const auto entity = indexed(system, handle & v::index_mask);
    std::uintptr_t identity = 0; std::uint32_t actual = 0;
    return entity && read(entity + o::identity, identity) && read(identity + v::identity_handle, actual) && actual == handle ? entity : 0;
}
bool stable(std::uintptr_t system, std::uintptr_t entity, std::uint32_t* output = nullptr) {
    std::uintptr_t identity = 0; std::uint32_t handle = 0xffffffff;
    if (!read(entity + o::identity, identity) || !read(identity + v::identity_handle, handle) || resolve(system, handle) != entity) return false;
    if (output) *output = handle;
    return true;
}
bool scene(std::uintptr_t entity, std::uintptr_t& node, Vec3& origin) {
    std::uintptr_t owner = 0; std::uint8_t dormant = 1;
    return read(entity + o::scene_node, node) && pointer(node) && read(node + o::scene_owner, owner) && owner == entity &&
        read(node + o::dormant, dormant) && !dormant && read(node + o::abs_origin, origin) && aim_core::finite(origin);
}
bool input_context(std::uintptr_t self, std::uintptr_t& angle_storage) {
    std::uintptr_t actual = 0, entries = 0; int count = -1; Vec3 value{};
    if (self != sources.client.base+o::csgo_input || !read(sources.client.base+0x223bd00,actual) || actual != self ||
        !read(self+0xb50,count) || count < 0 || count > 128) return false;
    angle_storage = self+0x688;
    if (count) {
        if (!read(self+0xb58,entries) || !pointer(entries)) return false;
        angle_storage = entries + static_cast<std::uintptr_t>(count)*0x440 - 0x10;
    }
    return read(angle_storage,value) && aim_core::finite(value);
}
// Flat wrappers have no C++ unwinding objects. A native fault stops further aim
// calls for this load; it never changes the game's original input callback.
bool angles_native(Angles fn, void* self, Vec3* angles) {
    __try { fn(self, 0, angles); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { native_fault = true; return false; }
}
bool bone_native(std::uintptr_t entity, const char* name, Vec3& position) {
    if (native_fault) return false;
    __try {
        const int index = bone_index(reinterpret_cast<void*>(entity), name);
        if (index < 0 || index > 512) return false;
        Vec3 angles{};
        bone_position(reinterpret_cast<void*>(entity), index, &position, &angles);
        return aim_core::finite(position);
    } __except(EXCEPTION_EXECUTE_HANDLER) { native_fault = true; return false; }
}
struct alignas(16) TraceResult { std::array<unsigned char, 0xc0> bytes{}; };
bool visible_native(std::uintptr_t query, std::uintptr_t pawn, std::uintptr_t target, const Vec3& eye, const Vec3& point) {
    if (native_fault) return false;
    __try {
        TraceResult result;
        trace_line(reinterpret_cast<void*>(query), &eye, &point, reinterpret_cast<void*>(pawn), 1ULL, &result);
        float fraction = 0; std::uintptr_t hit = 0;
        std::memcpy(&fraction, result.bytes.data() + 0xac, 4);
        std::memcpy(&hit, result.bytes.data() + 0x08, 8);
        return std::isfinite(fraction) && fraction >= 0 && fraction <= 1 && !result.bytes[0xbb] &&
            (hit == target || fraction > .9999f);
    } __except(EXCEPTION_EXECUTE_HANDLER) { native_fault = true; return false; }
}
bool accuracy_native(std::uintptr_t weapon, float& spread, float& inaccuracy) {
    __try {
        spread = get_spread(reinterpret_cast<void*>(weapon));
        inaccuracy = get_inaccuracy(reinterpret_cast<void*>(weapon), nullptr, nullptr);
        return std::isfinite(spread) && spread >= 0 && spread < 10 && std::isfinite(inaccuracy) && inaccuracy >= 0 && inaccuracy < 10;
    } __except(EXCEPTION_EXECUTE_HANDLER) { native_fault = true; return false; }
}
bool punch_native(std::uintptr_t pawn, Vec3& value) {
    __try {
        get_punch(reinterpret_cast<void*>(pawn), &value, true);
        return aim_core::finite(value) && std::abs(value.x) < 180 && std::abs(value.y) < 180 && std::abs(value.z) < 180;
    } __except(EXCEPTION_EXECUTE_HANDLER) { native_fault = true; return false; }
}
bool primary_ready_native(std::uintptr_t weapon,bool& ready) {
    ready=false;
    std::uintptr_t globals=0,rules=0,table=0,tick=0; unsigned char probe=0;
    if (!primary_ready_verified || !primary_ready_query ||
        !read(sources.client.base+n::global_vars,globals) || !pointer(globals) || !read(globals,probe) ||
        !read(sources.client.base+n::game_rules,rules) || !pointer(rules) || !read(rules,table) ||
        table!=sources.client.base+n::game_rules_vtable || !read(table+66*8,tick) || tick!=sources.client.base+n::game_rules_tick) return false;
    __try { ready=primary_ready_query(reinterpret_cast<void*>(weapon)); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { native_fault=true; return false; }
}
bool smoke_native(const Vec3& start, const Vec3& end) {
    __try { return through_smoke(&start,&end); }
    __except(EXCEPTION_EXECUTE_HANDLER) { native_fault = true; return true; }
}
bool action_native(unsigned index, bool pressed) {
    __try {
        action_delta(reinterpret_cast<void*>(sources.client.base + n::actions[index]),0,pressed ? 1.f : -1.f,1.f);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { native_fault = true; action_fault = true; return false; }
}
void sync_actions(const std::array<bool,6>& desired) {
    if (!actions_verified || action_fault) return;
    // Native action accumulation clamps at zero, not one: one balanced owned
    // contribution preserves simultaneous physical input contributions.
    for (unsigned i = 0; i < owned_actions.size(); ++i) {
        const bool press = desired[i] && !native_fault;
        if (owned_actions[i] == press) continue;
        if (press) owned_actions[i] = true; // Preserve uncertainty after a native exception.
        if (!action_native(i,press)) break;
        owned_actions[i] = press;
    }
    actions_released = std::none_of(owned_actions.begin(),owned_actions.end(),[](bool value){return value;});
}
struct ActionFrame {
    std::array<bool,6> desired{};
    ~ActionFrame() {
        if (!desired[0]) fire_state.pulse_until=0;
        sync_actions(desired);
    }
};
bool bind_fire_context(std::uintptr_t system,std::uintptr_t pawn,std::uint32_t local_handle,
                       std::uintptr_t weapon,std::uint32_t weapon_handle) {
    if (fire_state.bound && fire_state.system==system && fire_state.pawn==pawn && fire_state.weapon==weapon &&
        fire_state.local_handle==local_handle && fire_state.weapon_handle==weapon_handle) return false;
    // A new weapon must not inherit our old attack contribution. Its original
    // input callback observes a release before a later callback can press again.
    const bool held=owned_actions[0];
    if (held) { auto desired=owned_actions; desired[0]=false; sync_actions(desired); }
    reset_fire_state();
    if (pointer(system) && pointer(pawn) && pointer(weapon) && local_handle!=0xffffffff && weapon_handle!=0xffffffff)
        fire_state={system,pawn,weapon,local_handle,weapon_handle,0,0,true};
    return held;
}
bool request_fire(bool full_auto,std::uint64_t now) {
    if (!fire_state.bound) return false;
    if (full_auto) { fire_state.pulse_until=0; return true; }
    if (fire_state.pulse_until) {
        if (now<fire_state.pulse_until && owned_actions[0]) return true;
        fire_state.pulse_until=0;
        if (owned_actions[0]) return false; // At least one callback serializes release.
    }
    if (owned_actions[0] || now<fire_state.next_pulse) return false;
    // Keep a semi-auto pulse across the usual command interval instead of a
    // single high-FPS render/input callback. Every intervening aim gate still
    // has to pass; this timer never grants permission to bypass one.
    fire_state.pulse_until=now+20;
    fire_state.next_pulse=now+90;
    return true;
}
struct BallisticParameters { int damage = 0; float head = 0, armor = 0, penetration = 0, range = 0, modifier = 0; };
struct BallisticResult { bool hit = false; float damage = 0; int penetrations = 0, group = 0; };
struct BlockerRoster {
    struct Player { std::uintptr_t entity=0; std::uint32_t handle=0xffffffff; };
    struct Entry { std::uintptr_t entity=0; std::uint32_t handle=0xffffffff; bool player=true; };
    std::array<Player,64> players{};
    std::array<Entry,32> entries{};
    std::uintptr_t system=0;
    unsigned player_count=0, entry_count=0, builds=0, cache_hits=0;
    bool built=false;
    void build() {
        if (built) return;
        built=true; ++builds;
        if (!read(sources.client.base+o::entity_system,system) || !pointer(system)) { system=0; return; }
        for (unsigned index=1;index<=64;++index) {
            const auto controller=indexed(system,index);
            std::uintptr_t identity=0,name=0; std::uint32_t handle=0xffffffff; std::string designer;
            // Sparse slots and controllers without an attached pawn are normal.
            // Unknown encountered impacts are checked separately, below.
            if (!controller || !read(controller+o::identity,identity) || !read(identity+o::designer_name,name) ||
                !sources.memory.string(name,designer) || designer!="cs_player_controller" ||
                !read(controller+o::pawn_handle,handle)) continue;
            const auto pawn=resolve(system,handle);
            if (pawn) players[player_count++]={pawn,handle};
        }
    }
    bool player(std::uintptr_t entity) {
        build();
        if (!system) return true;
        for (unsigned i=0;i<player_count;++i) {
            if (players[i].entity==entity) return true; // Recycled/unknown former players also fail closed.
        }
        std::uint32_t handle=0xffffffff;
        if (!stable(system,entity,&handle)) return true;
        for (unsigned i=0;i<entry_count;++i) {
            if (entries[i].entity==entity && entries[i].handle==handle) { ++cache_hits; return entries[i].player; }
        }
        std::uintptr_t identity=0,name=0; std::string designer;
        if (!read(entity+o::identity,identity) || !read(identity+o::designer_name,name) ||
            !sources.memory.string(name,designer) || designer.empty() || resolve(system,handle)!=entity) return true;
        // The roster establishes controller-owned pawns. The name also catches
        // a newly created pawn that was not yet present when the roster was built.
        const bool is_player=designer=="player" || designer=="cs_player_controller" || designer.find("player_pawn")!=std::string::npos;
        if (entry_count<entries.size()) entries[entry_count++]={entity,handle,is_player};
        return is_player;
    }
};
BlockerRoster* active_blockers=nullptr;
struct BlockerScope {
    BlockerRoster own;
    BlockerRoster* previous=active_blockers;
    BlockerScope() { if (!active_blockers) active_blockers=&own; }
    ~BlockerScope() { active_blockers=previous; }
};
bool player_entity(std::uintptr_t entity) {
    if (active_blockers) return active_blockers->player(entity);
    std::uintptr_t system = 0;
    if (!read(sources.client.base + o::entity_system,system)) return true; // Unknown blockers fail closed.
    for (unsigned index = 1; index <= 64; ++index) {
        const auto controller = indexed(system,index);
        std::uintptr_t identity = 0, name = 0; std::string designer; std::uint32_t handle = 0xffffffff;
        if (controller && read(controller+o::identity,identity) && read(identity+o::designer_name,name) &&
            sources.memory.string(name,designer) && designer == "cs_player_controller" && read(controller+o::pawn_handle,handle) &&
            resolve(system,handle) == entity) return true;
    }
    return false;
}
bool parameters(std::uintptr_t weapon, BallisticParameters& p) {
    std::uintptr_t data = 0;
    return read(weapon + 0x388, data) && pointer(data) && read(data + 0x820,p.damage) && read(data + 0x824,p.head) &&
        read(data + 0x828,p.armor) && read(data + 0x82c,p.penetration) && read(data + 0x830,p.range) && read(data + 0x834,p.modifier) &&
        p.damage > 0 && p.damage < 1000 && std::isfinite(p.head) && p.head > 0 && p.head <= 10 &&
        std::isfinite(p.armor) && p.armor >= 0 && p.armor <= 10 && std::isfinite(p.penetration) && p.penetration >= 0 && p.penetration <= 10 &&
        std::isfinite(p.range) && p.range > 0 && p.range <= 100000 && std::isfinite(p.modifier) && p.modifier > 0 && p.modifier <= 1;
}
// Opaque native buffers own interior pointers. They must never be copied; the
// native destructor runs exactly once after a completed constructor.
bool ballistic_native(std::uintptr_t pawn, int shooter_team, std::uintptr_t target, const Vec3& eye, const Vec3& point,
                      const BallisticParameters& p, bool allow_penetration, BallisticResult& output) {
    alignas(16) unsigned char data[0x1d20]{};
    alignas(16) unsigned char filter[0x48]{};
    bool constructed = false, completed = false;
    output = {};
    __try {
        __try {
            trace_construct(data); constructed = true;
            filter_construct(filter, reinterpret_cast<void*>(pawn), 0x1c300bULL, 3, 0x0f);
            filter[0x39] |= 2;
            std::uint64_t flags = 0; std::memcpy(&flags, filter + 0x10, 8); flags |= 0x4000000000ULL; std::memcpy(filter + 0x10,&flags,8);
            const auto difference = aim_core::sub(point, eye); const auto distance = aim_core::length(difference);
            if (!std::isfinite(distance) || distance < 1 || distance > p.range) return false;
            const auto delta = aim_core::mul(difference, p.range / distance);
            bullet_create(data, &eye, &delta, filter, 4, true);
            bullet_penetrate(data, static_cast<float>(p.damage), p.penetration, p.modifier, 4, shooter_team, nullptr);
            int count = 0; std::uintptr_t intervals = 0;
            std::memcpy(&count,data+0x1c20,4); std::memcpy(&intervals,data+0x1c28,8);
            if (count < 0 || count > 256 || (count && !pointer(intervals))) return false;
            int record_count = 0; std::uintptr_t records = 0;
            std::memcpy(&record_count,data,4); std::memcpy(&records,data+8,8);
            if (record_count < 0 || record_count > 4096 || (record_count && !pointer(records))) return false;
            for (int i = 0; i < count; ++i) {
                std::array<unsigned char,0x18> interval{};
                if (!sources.memory.read(intervals + i*0x18, interval.data(), interval.size())) return false;
                if (interval[0x14] & 1) continue;
                std::uint16_t record_index = 0; std::memcpy(&record_index,interval.data()+0x12,2);
                std::array<unsigned char,0x38> record{};
                if (record_index >= record_count || !sources.memory.read(records + record_index*0x38,record.data(),record.size())) return false;
                TraceResult trace;
                bullet_interval(data, &trace, reinterpret_cast<void*>(intervals + i*0x18));
                std::uintptr_t entity = 0; std::memcpy(&entity,trace.bytes.data()+8,8);
                if (!entity) continue;
                // Only the first player impact can authorize a shot. A body in
                // front of the selected target must never be silently ignored.
                if (entity != target) { if (player_entity(entity)) break; else continue; }
                float damage = 0; int remaining = 0;
                std::memcpy(&damage,interval.data()+8,4); std::memcpy(&remaining,interval.data()+0xc,4);
                if (!std::isfinite(damage) || damage < 0 || damage > 10000 || remaining < 0 || remaining > 4) return false;
                if (!allow_penetration && remaining != 4) break;
                const int group = hit_group(&trace);
                std::uint8_t team = 0; int armor = 0;
                if (group < 0 || group > 8 || !read(target + o::team, team) || (team != 2 && team != 3) || !read(target + 0x1ec8,armor) || armor < 0 || armor > 1000) return false;
                const std::uint32_t cvar = team == 3 ? (group == 1 ? 0x24bca90 : 0x24bca80) : (group == 1 ? 0x24bcab0 : 0x24bcaa0);
                std::uintptr_t variable = 0; float scale = 1;
                if (group && (!read(sources.client.base + cvar, variable) || !read(variable + 0x58,scale) || !std::isfinite(scale) || scale < 0 || scale > 100)) return false;
                damage *= scale;
                if (group == 1) damage *= p.head;
                else if (group == 3) damage *= 1.25f;
                else if (group == 6 || group == 7) damage *= .75f;
                if (is_armored(reinterpret_cast<void*>(target), group)) {
                    const float reduced = damage * p.armor * .5f;
                    damage = (damage-reduced)*.5f > armor ? damage - armor*2.f : reduced;
                }
                output = {true, std::max(0.f,damage), 4-remaining, group}; break;
            }
            completed = true;
        } __finally { if (constructed) trace_destruct(data); }
    } __except(EXCEPTION_EXECUTE_HANDLER) { native_fault = true; return false; }
    return completed;
}
struct ChanceResult {
    bool supported = false, complete = false;
    int percent = -1, samples = 0;
    bool threshold_met = false, decision = false;
    int rays = 0;
};
[[maybe_unused]] ChanceResult sampled_chance(std::uintptr_t weapon, int item, std::uintptr_t pawn, int team, std::uintptr_t target,
                            const Vec3& eye, Vec3 firing_angles, const BallisticParameters& parameters,
                            float inaccuracy, float spread, bool penetration, float minimum_damage) {
    ChanceResult result;
    if (!aim_core::finite(eye) || !aim_core::finite(firing_angles) || !std::isfinite(inaccuracy) || !std::isfinite(spread) ||
        inaccuracy < 0 || spread < 0 || inaccuracy >= 10 || spread >= 10 || !std::isfinite(minimum_damage) || minimum_damage < 0) return result;
    std::uintptr_t data = 0, max_variable = 0, up_variable = 0;
    int mode = -1, bullets = 0; float recoil = 0; std::uint8_t max_inaccuracy = 0, only_up = 0;
    if (!read(weapon+0x388,data) || !read(data+0x730,bullets) || bullets != 1 ||
        !read(weapon+0x1a00,mode) || mode < 0 || mode > 1 || !read(weapon+0x1a28,recoil) ||
        !std::isfinite(recoil) || recoil < 0 || recoil > 1000 ||
        !read(sources.client.base+0x25becb0,max_variable) || !read(max_variable+0x58,max_inaccuracy) ||
        !read(sources.client.base+0x25becc0,up_variable) || !read(up_variable+0x58,only_up) || max_inaccuracy > 1 || only_up > 1) return result;
    result.supported = true;
    // This samples the recovered distribution against the current native world;
    // it does not predict the server seed and never touches the shared RNG.
    aim_spread::Rng random(0x6a09e667u);
    const auto started = GetTickCount64(); int hits = 0;
    constexpr int sample_count = 64;
    for (int i = 0; i < sample_count && !native_fault; ++i) {
        if (i && (i % 4 == 0) && GetTickCount64()-started >= 4) return result;
        const auto offset = aim_spread::sample(random,item,mode,recoil,inaccuracy,spread,max_inaccuracy != 0,only_up != 0);
        const auto direction = aim_spread::direction(firing_angles,offset);
        if (!aim_core::finite(direction)) return result;
        BallisticResult hit;
        const auto endpoint = aim_core::add(eye,aim_core::mul(direction,parameters.range*.999f));
        if (!ballistic_native(pawn,team,target,eye,endpoint,parameters,penetration,hit)) return result;
        ++result.samples;
        if (hit.hit && hit.damage >= minimum_damage) ++hits;
    }
    if (native_fault || result.samples != sample_count) return result;
    result.complete = true; result.percent = hits*100/sample_count; return result;
}
struct ChanceContext {
    std::uintptr_t pawn = 0, weapon = 0, target = 0;
    std::uint32_t local_handle = 0, weapon_handle = 0, target_handle = 0;
    int item = 0, team = 0, mode = 0, health = 0, hitbox = 0, threshold = 0;
    float recoil = 0, inaccuracy = 0, spread = 0, minimum_damage = 0;
    bool maximum = false, only_up = false, penetration = false;
    Vec3 eye{}, angles{}, point{}, origin{};
    BallisticParameters parameters{};
};
std::uint32_t sample_sequence=0;
struct SelectionCache {
    bool valid = false;
    std::uint64_t scanned_at = 0;
    std::uint32_t local_handle = 0, weapon_handle = 0, target_handle = 0;
    int index = 0;
    std::size_t point_index = 0;
    bool multipoint = false;
    config::AimProfile profile;
    enemy_rules::Rule enemy_rule;
} selection_cache;
void clear_target_caches() { selection_cache.valid = false; reset_fire_state(); }
bool near_position(Vec3 a, Vec3 b, float tolerance) {
    return aim_core::finite(a) && aim_core::finite(b) && aim_core::length(aim_core::sub(a,b)) <= tolerance;
}
bool silent_request_current(const SilentRequest& r) {
    const auto now=GetTickCount64();
    if (!r.valid || !history_hook_address || !original_history || closing || native_fault || menu_open ||
        now<r.created_at || now-r.created_at>32 || !focused() || !aim_core::finite(r.angles)) return false;
    { std::lock_guard guard(settings_mutex);
      if (r.settings!=settings_revision || r.context!=context_revision.load()) return false; }
    std::uintptr_t input=0,pawn=0,system=0,service=0,node=0; std::uint32_t handle=0xffffffff,flags=0;
    std::uint8_t local_team=0,team=0,life=255,immune=1; int health=0; Vec3 origin{};
    return r.input==sources.client.base+o::csgo_input && read(sources.client.base+0x223bd00,input) && input==r.input &&
        read(sources.client.base+o::local_pawn,pawn) && pawn==r.pawn &&
        read(sources.client.base+o::entity_system,system) && system==r.system &&
        resolve(system,r.local_handle)==pawn && read(pawn+o::team,local_team) && local_team==r.team &&
        read(pawn+o::health,health) && health>0 && read(pawn+o::life_state,life) && !life &&
        read(pawn+o::weapon_services,service) && read(service+o::active_weapon,handle) && handle==r.weapon_handle &&
        resolve(system,handle)==r.weapon && r.target!=pawn && resolve(system,r.target_handle)==r.target &&
        enemy_rules::read(sources)==r.enemy_rule && read(r.target+o::team,team) && enemy_rules::enemy(r.team,team,r.enemy_rule) &&
        read(r.target+o::health,health) && health==r.health && read(r.target+o::life_state,life) && !life &&
        read(r.target+o::immunity,immune) && !immune && read(r.target+o::flags,flags) && (!r.ignore_airborne || (flags&1)) &&
        scene(r.target,node,origin) && near_position(origin,r.target_origin,.5f);
}
bool silent_message(const void* source,std::uintptr_t caller,std::array<unsigned char,0x60>& copy) {
    // Only real command-history appends are eligible. The debug-only temporary
    // at d02cfd and all unrelated callers always receive the original source.
    if (caller!=sources.client.base+0xcde18c && caller!=sources.client.base+0xd026d9) return false;
    if (!active_silent || !silent_request_current(*active_silent) ||
        !sources.memory.read(reinterpret_cast<std::uintptr_t>(source),copy.data(),copy.size())) return false;
    std::memcpy(copy.data()+0x10,&active_silent->angles,sizeof(Vec3));
    return true;
}
void history_dispatch(const void* source,void* destination,bool extra,float fraction,float time,void* context,std::uintptr_t caller) {
    struct CallbackGuard {
        CallbackGuard() { ++history_callbacks; }
        ~CallbackGuard() { --history_callbacks; }
    } guard;
    alignas(16) std::array<unsigned char,0x60> copy{};
    const bool replace=silent_message(source,caller,copy);
    // Native code retains ownership of allocation, presence bits, timestamps,
    // interpolation, and command history. No source/camera/base command writes.
    if (original_history) original_history(replace ? copy.data() : source,destination,extra,fraction,time,context);
    if (replace) ++silent_sample_count;
}
void history_hook(const void* source,void* destination,bool extra,float fraction,float time,void* context) {
    history_dispatch(source,destination,extra,fraction,time,context,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
bool chance_inputs(ChanceContext& context) {
    if (!aim_core::finite(context.eye) || !aim_core::finite(context.angles) || !aim_core::finite(context.point) ||
        !aim_core::finite(context.origin) || !std::isfinite(context.inaccuracy) || !std::isfinite(context.spread) ||
        context.inaccuracy < 0 || context.spread < 0 || context.inaccuracy >= 10 || context.spread >= 10 ||
        !std::isfinite(context.minimum_damage) || context.minimum_damage < 0 || context.threshold < 0 || context.threshold > 100) return false;
    std::uintptr_t data=0, maximum=0, up=0; int bullets=0; std::uint8_t maximum_value=0, up_value=0;
    if (!read(context.weapon+0x388,data) || !read(data+0x730,bullets) || bullets!=1 ||
        !read(context.weapon+0x1a00,context.mode) || context.mode<0 || context.mode>1 ||
        !read(context.weapon+0x1a28,context.recoil) || !std::isfinite(context.recoil) || context.recoil<0 || context.recoil>1000 ||
        !read(sources.client.base+0x25becb0,maximum) || !read(maximum+0x58,maximum_value) || maximum_value>1 ||
        !read(sources.client.base+0x25becc0,up) || !read(up+0x58,up_value) || up_value>1) return false;
    context.maximum=maximum_value!=0; context.only_up=up_value!=0;
    return true;
}
ChanceResult fresh_chance(ChanceContext context) {
    BlockerScope blocker_scope;
    ChanceResult result;
    if (native_fault || !chance_inputs(context)) return result;
    result.supported=true;
    LARGE_INTEGER frequency{}, started{}, current{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0 || !QueryPerformanceCounter(&started)) return result;
    // Every estimate uses only this input callback's current world. The private
    // stream never reads or seeds the engine RNG and is not a server-shot seed.
    aim_spread::Rng random(0x6a09e667u ^ (++sample_sequence*0x9e3779b9u));
    constexpr int maximum_per_callback=16, minimum_samples=8;
    const bool deterministic=context.inaccuracy==0 && context.spread==0;
    int hits=0;
    const auto tick_started=GetTickCount64();
    for (int step=0;step<(deterministic ? 1 : maximum_per_callback) && !native_fault;++step) {
        // An opaque native ray cannot be interrupted, but no additional ray
        // starts after this callback's 0.75 ms slice has been consumed.
        if (!QueryPerformanceCounter(&current) || current.QuadPart<started.QuadPart) return result;
        if (static_cast<double>(current.QuadPart-started.QuadPart)/frequency.QuadPart>=.00075) break;
        const auto offset=aim_spread::sample(random,context.item,context.mode,context.recoil,
            context.inaccuracy,context.spread,context.maximum,context.only_up);
        const auto direction=aim_spread::direction(context.angles,offset);
        if (!aim_core::finite(direction)) return result;
        BallisticResult hit;
        ++result.rays;
        if (!ballistic_native(context.pawn,context.team,context.target,context.eye,
            aim_core::add(context.eye,aim_core::mul(direction,context.parameters.range*.999f)),context.parameters,context.penetration,hit)) return result;
        ++result.samples;
        if (hit.hit && hit.damage>=context.minimum_damage) ++hits;
        // Do not publish a result after a native stall or latched fault.
        if (native_fault || GetTickCount64()-tick_started>=50) return result;
    }
    if (native_fault || result.samples<(deterministic ? 1 : minimum_samples)) return result;
    result.complete=result.decision=true;
    result.percent=hits*100/result.samples;
    result.threshold_met=result.percent>=context.threshold;
    // This is an empirical estimate with its actual N, not a probability proof
    // or a confidence bound. No incomplete or prior-callback estimate is used.
    return result;
}
struct TargetPoint { Vec3 point{}; int bit = 0, source_index = -1; };
struct HitboxVector { int count = 0; std::uint32_t padding = 0; std::uintptr_t data = 0; int capacity = 0; std::uint32_t flags = 0; };
struct alignas(16) TransformVector { int count = 0; unsigned char padding[12]{}; std::uintptr_t data = 0; int capacity = 0; std::uint32_t flags = 0; };
struct BoneTransform { Vec3 position; float scale; float x,y,z,w; };
static_assert(sizeof(HitboxVector)==24 && offsetof(HitboxVector,data)==8 && sizeof(TransformVector)==32 && offsetof(TransformVector,data)==0x10 && sizeof(BoneTransform)==32);
int hitbox_bit(int group) { return group == 1 ? 1 : (group == 2 || group == 8) ? 2 : group == 3 ? 4 : (group == 4 || group == 5) ? 8 : (group == 6 || group == 7) ? 16 : 0; }
Vec3 transform_point(const BoneTransform& t, Vec3 local) {
    const double norm = static_cast<double>(t.x)*t.x + static_cast<double>(t.y)*t.y + static_cast<double>(t.z)*t.z + static_cast<double>(t.w)*t.w;
    if (!aim_core::finite(t.position) || !aim_core::finite(local) || !std::isfinite(t.scale) || t.scale <= 0 || t.scale > 16 || !std::isfinite(norm) || std::abs(norm-1) > .01) return aim_core::invalid();
    local = aim_core::mul(local,t.scale);
    const Vec3 q{t.x,t.y,t.z};
    const float dot = q.x*local.x + q.y*local.y + q.z*local.z;
    const float qq = q.x*q.x + q.y*q.y + q.z*q.z;
    const Vec3 cross{q.y*local.z-q.z*local.y,q.z*local.x-q.x*local.z,q.x*local.y-q.y*local.x};
    return aim_core::add(t.position,aim_core::add(aim_core::add(aim_core::mul(q,2*dot),aim_core::mul(local,t.w*t.w-qq)),aim_core::mul(cross,2*t.w)));
}
bool hitbox_points_native(std::uintptr_t node, int mask, float scale, std::array<TargetPoint,256>& output, std::size_t& count) {
    HitboxVector descriptors; TransformVector transforms; bool completed = false;
    count = 0;
    if (!std::isfinite(scale) || scale < .1f || scale > 1 || !(mask & 31) || (mask & ~31)) return false;
    __try {
        __try {
            const int native_count = get_hitboxes(reinterpret_cast<void*>(node),&descriptors,&transforms,0);
            if (native_count <= 0 || native_count > 128 || native_count != descriptors.count || transforms.count != descriptors.count ||
                descriptors.capacity < descriptors.count || transforms.capacity < transforms.count ||
                (descriptors.count && (!pointer(descriptors.data) || !pointer(transforms.data)))) return false;
            for (int i = 0; i < descriptors.count; ++i) {
                std::uintptr_t hitbox = 0; BoneTransform transform{};
                Vec3 low{}, high{}; float radius = 0; int group = 0; std::uint8_t shape = 0;
                if (!read(descriptors.data+i*16,hitbox) || !pointer(hitbox) || !read(transforms.data+i*32,transform) ||
                    !read(hitbox+0x18,low) || !read(hitbox+0x24,high) || !read(hitbox+0x30,radius) || !read(hitbox+0x38,group) || !read(hitbox+0x3c,shape)) return false;
                const int bit = hitbox_bit(group);
                if (!(mask & bit)) continue;
                if (!aim_core::finite(low) || !aim_core::finite(high) || !std::isfinite(radius) || radius < 0 || radius > 256 ||
                    aim_core::length(low) > 512 || aim_core::length(high) > 512) return false;
                const Vec3 center = shape == 1 ? low : aim_core::mul(aim_core::add(low,high),.5f);
                Vec3 extents = shape == 1 || shape == 2 ? Vec3{radius,radius,radius} : aim_core::mul(aim_core::sub(high,low),.5f);
                if (extents.x < 0 || extents.y < 0 || extents.z < 0) return false;
                Vec3 locals[9]{center}; unsigned local_count = 1;
                for (unsigned axis = 0; axis < 3; ++axis) {
                    Vec3 offset{};
                    if (axis==0) offset.x = extents.x*scale;
                    else if (axis==1) offset.y = extents.y*scale;
                    else offset.z = extents.z*scale;
                    if (aim_core::length(offset) > .001f) { locals[local_count++] = aim_core::add(center,offset); locals[local_count++] = aim_core::sub(center,offset); }
                }
                if (shape == 2) { const auto half = aim_core::mul(aim_core::sub(high,low),scale*.5f); locals[local_count++] = aim_core::add(center,half); locals[local_count++] = aim_core::sub(center,half); }
                for (unsigned j = 0; j < local_count && count < output.size(); ++j) {
                    const auto world = transform_point(transform,locals[j]);
                    if (!aim_core::finite(world)) return false;
                    output[count++] = {world,bit};
                }
            }
            completed = true;
        } __finally {
            if (!completed) count = 0;
            if (descriptors.data && !(descriptors.flags & 0xc0000000u)) free_hitboxes(reinterpret_cast<void*>(descriptors.data));
            if (transforms.data && !(transforms.flags & 0xc0000000u)) free_hitboxes(reinterpret_cast<void*>(transforms.data));
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { native_fault = true; count = 0; return false; }
    if (!completed) count = 0;
    return completed;
}
bool focused() {
    DWORD pid = 0;
    const auto window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &pid) && pid == GetCurrentProcessId();
}
Capabilities capabilities() {
    Capabilities c;
    c.view_angles = c.target_selection = c.bones = c.visibility = hook_address && !native_fault;
    c.recoil = c.view_angles;
    c.damage = c.penetration = c.view_angles && ballistics_verified;
    c.hitchance = c.damage;
    c.auto_scope = c.auto_stop = c.view_angles && actions_verified;
    c.auto_fire = c.auto_scope && primary_ready_verified;
    c.smoke = c.view_angles && smoke_verified;
    c.multipoint = c.view_angles && hitboxes_verified;
    c.silent = c.view_angles && history_hook_address && original_history;
    return c;
}
void update(void* self, bool active,SilentRequest* silent=nullptr) {
    if (silent) *silent={};
    ActionFrame action_frame;
    BlockerScope blocker_scope;
    config::Aim settings;
    { std::lock_guard guard(settings_mutex); settings = requested;
      if (sampled_settings_revision != settings_revision) { clear_target_caches(); sampled_settings_revision=settings_revision; } }
    const auto revision=context_revision.load();
    if (sampled_context_revision!=revision) { clear_target_caches(); sampled_context_revision=revision; }
    Status s; s.supported = hook_address && !native_fault; s.capabilities = capabilities(); s.updates = ++updates;
    if (closing || native_fault || !s.supported) { s.state = State::error; s.detail = "Native aim interface unavailable; reload required"; publish(s); return; }
    if (previous_key != settings.key || previous_activation != settings.activation || previous_enabled != settings.enabled) {
        toggle = false; key_previous = (GetAsyncKeyState(settings.key) & 0x8000) != 0;
        previous_key = settings.key; previous_activation = settings.activation; previous_enabled = settings.enabled;
    }
    if (!settings.enabled) { pending = 0xffffffff; toggle = false; s.state = State::disabled; s.detail = "Enable Aimbot to begin"; publish(s); return; }
    if (!active || menu_open || !focused()) { pending = 0xffffffff; toggle = false; key_previous = true; s.state = State::idle; s.detail = "Input paused while menu is open or game is unfocused"; publish(s); return; }
    const bool down = (GetAsyncKeyState(settings.key) & 0x8000) != 0;
    if (down && !key_previous) toggle = !toggle;
    key_previous = down;
    s.active = settings.activation == 2 || (settings.activation == 1 ? toggle : down);
    const auto local = game::collect(sources);
    if (!local.profile_valid) { pending = 0xffffffff; s.state = State::profile_mismatch; s.detail = "Native build profile mismatch"; publish(s); return; }
    if (!local.in_game || !local.local_valid || !local.alive || !local.weapon_valid) {
        pending = 0xffffffff; s.state = State::waiting_for_player; s.detail = "Waiting for an alive local player and firearm"; publish(s); return;
    }
    s.group = weapon_group(local.item_id);
    const auto& p = config::aim_profile(settings, s.group); s.fov_degrees = p.fov;
    s.fire_block=p.auto_fire ? FireBlock::not_ready : FireBlock::auto_fire_off;
    if (p.silent && (!s.capabilities.silent || !silent)) {
        pending=0xffffffff; s.state=State::idle; s.active=false;
        s.detail="Silent Aim history interface is unavailable; input released"; publish(s); return;
    }
    if (p.penetration && !s.capabilities.penetration) {
        pending=0xffffffff; s.state=State::idle; s.active=false;
        s.detail="Autowall ballistics are unavailable; input released"; publish(s); return;
    }
    if (!s.group || (local.team != 2 && local.team != 3)) { pending = 0xffffffff; s.state = State::idle; s.detail = "Current team or item is not supported"; publish(s); return; }
    if (!s.active) { pending = 0xffffffff; s.state = State::idle; s.detail = "Waiting for activation key"; publish(s); return; }
    std::uintptr_t pawn = 0, system = 0, node = 0, query = 0;
    std::uintptr_t input_storage = 0;
    std::uint32_t local_handle = 0xffffffff;
    Vec3 origin{}, offset{}, view{};
    if (!read(sources.client.base + o::local_pawn, pawn) || !read(sources.client.base + o::entity_system, system) ||
        !stable(system, pawn, &local_handle) || !scene(pawn, node, origin) || !read(pawn + o::view_offset, offset) || !aim_core::finite(offset) ||
        std::abs(offset.x) > 32 || std::abs(offset.y) > 32 || offset.z < 0 || offset.z > 100 ||
        !read(sources.client.base + n::trace_query, query) || !pointer(query) ||
        !input_context(reinterpret_cast<std::uintptr_t>(self),input_storage) ||
        !angles_native(get_angles, self, &view) || !aim_core::finite(view)) {
        pending = 0xffffffff; s.state = State::error; s.detail = "Input, eye position or trace query is unreadable"; publish(s); return;
    }
    Vec3 velocity{};
    const bool velocity_valid = read(pawn + o::velocity, velocity) && aim_core::finite(velocity);
    if (velocity_valid) s.speed = std::hypot(velocity.x, velocity.y);
    else if (p.auto_stop) { s.state = State::error; s.detail = "Movement velocity unavailable; input released"; publish(s); return; }
    const auto eye = aim_core::add(origin, offset);
    std::uintptr_t services = 0, weapon = 0; std::uint32_t weapon_handle = 0xffffffff;
    if (read(pawn + o::weapon_services, services) && read(services + o::active_weapon, weapon_handle)) weapon = resolve(system, weapon_handle);
    const bool fire_transition=bind_fire_context(system,pawn,local_handle,weapon,weapon_handle);
    if (native_fault) { s.state=State::error; s.detail="Native weapon transition release failed; reload required"; publish(s); return; }
    BallisticParameters ballistic_parameters;
    const bool ballistic_ready = s.capabilities.damage && weapon && parameters(weapon, ballistic_parameters);
    std::array<float,16> matrix{};
    if (read(sources.client.base + o::view_matrix, matrix)) {
        const auto projection = aim_core::project(matrix, aim_core::add(eye, aim_core::mul(aim_core::forward(view), 100)));
        s.projection_valid = projection.valid; s.camera_fov = projection.horizontal_fov;
    }
    float flash = 0;
    if (p.ignore_flashed && (!read(pawn + o::flash_duration, flash) || !std::isfinite(flash) || flash > .05f)) {
        pending = 0xffffffff; s.state = State::idle; s.detail = "Aiming paused while flashed"; publish(s); return;
    }
    struct Bone { const char* name; int bit; };
    static constexpr Bone bones[] = {{"head_0",1},{"spine_2",2},{"spine_3",2},{"spine_1",4},{"pelvis",4},
        {"arm_upper_L",8},{"arm_lower_L",8},{"arm_upper_R",8},{"arm_lower_R",8},
        {"leg_upper_L",16},{"leg_lower_L",16},{"leg_upper_R",16},{"leg_lower_R",16}};
    aim_core::Candidate best; std::uintptr_t best_entity = 0; std::uint32_t best_handle = 0xffffffff;
    float best_damage = 0; int best_penetrations=0; Vec3 best_origin{}; std::size_t best_point_index = 0; bool best_multipoint = false;
    std::uintptr_t network = 0; int max_clients = 0;
    if (!read(sources.engine.base + o::network_client, network) || !read(network + o::max_clients, max_clients) || max_clients < 1 || max_clients > 64) {
        pending = 0xffffffff; s.state = State::error; s.detail = "Player bounds unavailable"; publish(s); return;
    }
    const auto enemy_rule=enemy_rules::read(sources);
    const auto selection_now=GetTickCount64();
    const bool reuse_selection=selection_cache.valid && selection_now>=selection_cache.scanned_at &&
        selection_now-selection_cache.scanned_at<16 && selection_cache.local_handle==local_handle &&
        selection_cache.weapon_handle==weapon_handle && selection_cache.profile==p && selection_cache.enemy_rule==enemy_rule &&
        selection_cache.index>=1 && selection_cache.index<=max_clients;
    const auto scan_target = [&](int index,bool cached) {
        const auto controller = indexed(system, index);
        std::uint32_t handle = 0xffffffff;
        std::uintptr_t identity = 0, name = 0; std::string designer;
        if (!controller || !read(controller + o::identity, identity) || !read(identity + o::designer_name, name) ||
            !sources.memory.string(name, designer) || designer != "cs_player_controller" || !read(controller + o::pawn_handle, handle)) return;
        if (cached && handle!=selection_cache.target_handle) return;
        const auto target = resolve(system, handle);
        if (!target || target == pawn) return;
        std::uint8_t team = 0, life = 255, immune = 1; int health = 0; std::uint32_t flags = 0;
        if (!read(target + o::health, health) || health <= 0 || health > 100000 || !read(target + o::life_state, life) || life ||
            !read(target + o::team, team) || !enemy_rules::enemy(local.team,team,enemy_rule) ||
            !read(target + o::immunity, immune) || immune || !read(target + o::flags, flags) || (p.ignore_airborne && !(flags & 1))) return;
        std::uintptr_t target_node = 0; Vec3 target_origin{};
        if (!scene(target, target_node, target_origin)) return;
        // A broad prefilter only avoids unnecessary skeleton work. Actual FOV
        // and visibility are evaluated on named native world-space bones.
        if (aim_core::angular_error(view, eye, aim_core::add(target_origin, Vec3{0,0,48})) > std::min(180.f, p.fov + 35)) return;
        ++s.candidates;
        std::array<TargetPoint,256> points{}; std::size_t point_count = 0;
        std::uintptr_t table = 0, skeleton_getter = 0;
        const bool use_multipoint=p.multipoint && s.capabilities.multipoint && read(target_node,table) && read(table+13*8,skeleton_getter) && skeleton_getter == sources.client.base+n::get_skeleton;
        if (cached && use_multipoint!=selection_cache.multipoint) return;
        if (use_multipoint && read(target_node,table) && read(table+13*8,skeleton_getter) && skeleton_getter == sources.client.base+n::get_skeleton) {
            hitbox_points_native(target_node,p.hitboxes,p.multipoint_scale,points,point_count);
        } else {
            for (std::size_t source_index=0;source_index<std::size(bones);++source_index) {
                const auto& bone=bones[source_index];
                if (native_fault) break;
                if (!(p.hitboxes & bone.bit)) continue;
                if (cached && source_index!=selection_cache.point_index) continue;
                Vec3 point{};
                if (bone_native(target,bone.name,point)) points[point_count++] = {point,bone.bit,static_cast<int>(source_index)};
            }
        }
        for (std::size_t point_index = 0; point_index < point_count; ++point_index) {
            if (native_fault) break;
            if (cached && use_multipoint && point_index!=selection_cache.point_index) continue;
            const auto& target_point = points[point_index];
            const auto point = target_point.point;
            const auto displacement = aim_core::sub(point, target_origin);
            if (aim_core::length(displacement) > 160) continue;
            aim_core::Candidate candidate;
            candidate.point = point; candidate.health = health; candidate.index = index; candidate.hitbox = target_point.bit;
            candidate.distance = aim_core::length(aim_core::sub(point, eye)); candidate.error = aim_core::angular_error(view, eye, point);
            if (candidate.distance < 1 || candidate.error > p.fov || !aim_core::better(candidate, best, p.priority)) continue;
            if (p.ignore_smoke && s.capabilities.smoke && smoke_native(eye,point)) continue;
            if (config::requires_visibility(p) && !visible_native(query, pawn, target, eye, point)) continue;
            BallisticResult ballistic;
            if (s.capabilities.damage && (!ballistic_ready || !ballistic_native(pawn,local.team,target,eye,point,ballistic_parameters,p.penetration,ballistic) ||
                !ballistic.hit || ballistic.damage < std::min(p.min_damage,health))) continue;
            if (resolve(system, handle) != target) continue;
            best = candidate; best_entity = target; best_handle = handle; best_damage = ballistic.damage; best_penetrations=ballistic.penetrations;
            best_origin=target_origin; best_point_index=use_multipoint ? point_index : static_cast<std::size_t>(target_point.source_index); best_multipoint=use_multipoint;
        }
    };
    if (reuse_selection) scan_target(selection_cache.index,true);
    if (!best_entity && !native_fault) {
        selection_cache.valid=false;
        for (int index=1;index<=max_clients && !native_fault;++index) scan_target(index,false);
        if (best_entity) {
            selection_cache={true,selection_now,local_handle,weapon_handle,best_handle,best.index,best_point_index,best_multipoint,p,enemy_rule};
        }
    }
    if (native_fault) { s.state = State::error; s.detail = "Native aim call failed; reload required"; publish(s); return; }
    if (!best_entity) { pending = 0xffffffff; s.state = State::idle; s.detail = "No valid target inside FOV"; publish(s); return; }
    s.has_target = true; s.target_index = best.index; s.target_health = best.health; s.hitbox = best.hitbox; s.angular_error = best.error;
    s.damage = best_damage; s.penetrations=best_penetrations;
    if (read(sources.client.base + o::view_matrix, matrix)) {
        const auto projection = aim_core::project(matrix, best.point);
        s.projection_valid = projection.valid; s.target_on_screen = projection.on_screen;
        s.camera_fov = projection.horizontal_fov; s.target_screen_x = projection.x; s.target_screen_y = projection.y;
    }
    float spread = 0;
    const bool accuracy_ready=weapon && accuracy_native(weapon, spread, s.inaccuracy);
    s.capabilities.hitchance=s.capabilities.hitchance && accuracy_ready;
    if (native_fault) { s.state = State::error; s.detail = "Native accuracy call failed; reload required"; publish(s); return; }
    const auto now = GetTickCount64();
    if (pending != best_handle) { pending = best_handle; selected_at = now; }
    if (now - selected_at < static_cast<unsigned>(p.switch_delay_ms)) {
        if (p.auto_fire) s.fire_block=FireBlock::switch_delay;
        s.state = State::tracking; s.detail = "Target acquired; waiting for switch delay"; publish(s); return;
    }
    std::uintptr_t current_pawn = 0;
    const auto target_current = [&] {
        int health=0; std::uint8_t team=0,life=255,immune=1; std::uint32_t flags=0;
        std::uintptr_t current_node=0; Vec3 current_origin{};
        std::uint8_t current_team=0;
        return enemy_rules::read(sources)==enemy_rule && read(pawn+o::team,current_team) && current_team==local.team &&
            resolve(system,best_handle)==best_entity && read(best_entity+o::health,health) && health==best.health &&
            read(best_entity+o::team,team) && enemy_rules::enemy(local.team,team,enemy_rule) &&
            read(best_entity+o::life_state,life) && !life && read(best_entity+o::immunity,immune) && !immune &&
            read(best_entity+o::flags,flags) && (!p.ignore_airborne || (flags&1)) &&
            scene(best_entity,current_node,current_origin) && near_position(current_origin,best_origin,.5f);
    };
    const auto settings_current = [&] {
        std::lock_guard guard(settings_mutex);
        return sampled_settings_revision==settings_revision && context_revision.load()==revision;
    };
    if (resolve(system, best_handle) != best_entity || resolve(system, local_handle) != pawn ||
        !read(sources.client.base + o::local_pawn, current_pawn) || current_pawn != pawn || !target_current() ||
        !settings_current() || closing || native_fault || menu_open || !focused()) {
        pending = 0xffffffff; s.has_target = s.target_on_screen = s.active = false;
        s.fire_block=FireBlock::context_changed;
        s.state = State::idle; s.detail = "Target or input context changed"; publish(s); return;
    }
    auto desired = aim_core::angle_to(eye, best.point);
    Vec3 current_punch{};
    if (p.recoil || p.auto_fire || settings.info) {
        Vec3 punch{}; std::uintptr_t service = 0, owner = 0;
        if (!read(pawn + o::aim_punch_services, service) || !pointer(service) || !read(service + 0x38, owner) || owner != pawn || !punch_native(pawn, punch)) {
            pending = 0xffffffff; s.state = State::error; s.detail = "Recoil service unavailable; view left unchanged"; publish(s); return;
        }
        if (p.recoil) desired = aim_core::sub(desired, punch);
        current_punch = punch;
    }
    auto wanted = p.silent ? aim_core::smooth(view,desired,1) : aim_core::smooth(view, desired, p.smoothing);
    std::uintptr_t current_storage = 0;
    if (!input_context(reinterpret_cast<std::uintptr_t>(self),current_storage) || current_storage != input_storage ||
        resolve(system,local_handle) != pawn || !read(sources.client.base+o::local_pawn,current_pawn) || current_pawn != pawn ||
        !target_current() || !settings_current() || closing || menu_open || !focused()) {
        pending = 0xffffffff; s.active = s.has_target = s.target_on_screen = false; s.state = State::idle;
        s.fire_block=FireBlock::context_changed;
        s.detail = "Input or player context changed"; publish(s); return;
    }
    if (!aim_core::finite(wanted) || (!p.silent && !angles_native(set_angles, self, &wanted))) { s.state = State::error; s.detail = "View angle update rejected"; publish(s); return; }
    bool scoped = false; std::uint8_t scope_byte = 0;
    if (read(pawn + o::scoped,scope_byte)) scoped = scope_byte != 0;
    const bool needs_scope = p.auto_scope && s.capabilities.auto_scope && s.group >= 4 && s.group <= 6 && !scoped;
    if (needs_scope) action_frame.desired[1] = true;
    const bool needs_stop = p.auto_stop && s.capabilities.auto_stop && s.speed > 15;
    if (needs_stop) {
        const float radians = view.y * static_cast<float>(aim_core::radians);
        const float forward = -velocity.x*std::cos(radians)-velocity.y*std::sin(radians);
        const float left = velocity.x*std::sin(radians)-velocity.y*std::cos(radians);
        action_frame.desired[2] = forward > 10; action_frame.desired[3] = forward < -10;
        action_frame.desired[4] = left > 10; action_frame.desired[5] = left < -10;
    }
    std::uint8_t reloading = 1; int clip = 0;
    const bool weapon_current=weapon && resolve(system,weapon_handle)==weapon;
    const bool reload_known=weapon_current && read(weapon+o::reloading,reloading) && reloading<=1;
    const bool clip_known=weapon_current && read(weapon+o::clip,clip);
    const bool damage_ready=!s.capabilities.damage || best_damage>=std::min(p.min_damage,best.health);
    s.alignment_error=aim_core::angular_error(aim_core::add(wanted,current_punch),eye,best.point);
    const bool aligned=std::isfinite(s.alignment_error) && s.alignment_error<.15f;
    bool primary_ready=false;
    const bool readiness_known=p.auto_fire && s.capabilities.auto_fire && weapon_current && primary_ready_native(weapon,primary_ready);
    if (native_fault) { s.state=State::error; s.detail="Native weapon readiness query failed; input released"; publish(s); return; }
    if (p.auto_fire && (!readiness_known || !primary_ready)) { fire_state.pulse_until=0; fire_state.next_pulse=0; }
    const bool can_fire=weapon_current && reload_known && !reloading && clip_known && clip>0 && damage_ready && aligned && (!p.auto_fire || (readiness_known && primary_ready));
    bool chance_ready = p.hitchance == 0;
    s.hit_chance = -1;
    if (s.capabilities.hitchance && ballistic_ready && (settings.info || (p.auto_fire && p.hitchance>0 && can_fire && !needs_scope && !needs_stop))) {
        ChanceContext context;
        context.pawn=pawn; context.weapon=weapon; context.target=best_entity;
        context.local_handle=local_handle; context.weapon_handle=weapon_handle; context.target_handle=best_handle;
        context.item=local.item_id; context.team=local.team; context.health=best.health; context.hitbox=best.hitbox;
        context.threshold=p.hitchance; context.eye=eye; context.angles=aim_core::add(wanted,current_punch);
        context.point=best.point; context.origin=best_origin; context.parameters=ballistic_parameters;
        context.inaccuracy=s.inaccuracy; context.spread=spread; context.penetration=p.penetration;
        context.minimum_damage=static_cast<float>(std::min(p.min_damage,best.health));
        const auto chance=fresh_chance(context);
        s.capabilities.hitchance = chance.supported; s.hit_chance = chance.percent; s.chance_samples=chance.rays; s.chance_sample_count=chance.complete ? chance.samples : 0;
        s.chance_complete=chance.complete;
        chance_ready = p.hitchance == 0 || chance.threshold_met;
        if (native_fault) { s.state = State::error; s.detail = "Native shot sampling failed; input released"; publish(s); return; }
    }
    // A fresh native estimate only authorizes input while the current entity
    // serials, health and scene origin are still valid.
    const bool shot_context_current=target_current() && settings_current() && resolve(system,local_handle)==pawn &&
        resolve(system,weapon_handle)==weapon && read(sources.client.base+o::local_pawn,current_pawn) && current_pawn==pawn &&
        input_context(reinterpret_cast<std::uintptr_t>(self),current_storage) && current_storage==input_storage &&
        !closing && !native_fault && !menu_open && focused();
    if (!shot_context_current) { clear_target_caches(); chance_ready=false; }
    if (p.auto_fire) {
        if (!shot_context_current || fire_transition) s.fire_block=FireBlock::context_changed;
        else if (!s.capabilities.auto_fire) s.fire_block=FireBlock::input_unavailable;
        else if (needs_scope) s.fire_block=FireBlock::scope;
        else if (needs_stop) s.fire_block=FireBlock::stopping;
        else if (!weapon_current) s.fire_block=FireBlock::context_changed;
        else if (!reload_known || !clip_known) s.fire_block=FireBlock::weapon_state;
        else if (reloading) s.fire_block=FireBlock::reloading;
        else if (clip<=0) s.fire_block=FireBlock::empty_clip;
        else if (!readiness_known) s.fire_block=FireBlock::readiness_unavailable;
        else if (!primary_ready) s.fire_block=FireBlock::weapon_cooldown;
        else if (!damage_ready) s.fire_block=FireBlock::damage;
        else if (!aligned) s.fire_block=FireBlock::alignment;
        else if (!chance_ready && !s.capabilities.hitchance) s.fire_block=FireBlock::chance_unavailable;
        else if (!chance_ready && !s.chance_complete) s.fire_block=FireBlock::sample_shortage;
        else if (!chance_ready) s.fire_block=FireBlock::chance_below_threshold;
        else if (!(p.penetration ? s.capabilities.damage && best_damage>0 : visible_native(query,pawn,best_entity,eye,best.point))) s.fire_block=FireBlock::visibility;
        else {
            std::uintptr_t data=0; std::uint8_t full_auto=0;
            if (!read(weapon+0x388,data) || !read(data+0x72d,full_auto) || full_auto>1) s.fire_block=FireBlock::weapon_state;
            else if (request_fire(full_auto!=0,now)) {
                action_frame.desired[0]=true; s.fire_block=FireBlock::none;
            } else s.fire_block=FireBlock::semi_auto_cooldown;
        }
    }
    if (native_fault) { s.state=State::error; s.detail="Native fire validation failed; input released"; publish(s); return; }
    // Native action messages can be captured in one update and serialized in a
    // later command while the weapon is already cooling down. Preparing valid
    // history angles is independent of authorizing a new attack this update.
    if (p.silent && silent && shot_context_current) {
        *silent={true,wanted,best_origin,reinterpret_cast<std::uintptr_t>(self),system,pawn,weapon,best_entity,
            local_handle,weapon_handle,best_handle,sampled_settings_revision,revision,GetTickCount64(),local.team,best.health,p.ignore_airborne,enemy_rule};
    }
    s.state = action_frame.desired[0] ? State::firing : State::tracking;
    s.detail=p.auto_fire ? (action_frame.desired[0] ? "Automatic fire input requested" : fire_block_name(s.fire_block)) : "Tracking selected target";
    publish(s);
}
void input_hook(void* self, int slot, bool active) {
    callbacks.fetch_add(1); ++callback_count;
    SilentRequest silent;
    if (slot == 0 && reinterpret_cast<std::uintptr_t>(self) == sources.client.base + o::csgo_input) {
        std::lock_guard guard(execution_mutex);
        server_rules::update_on_input();
        if (!closing) esp::on_input(sources,{bone_native,visible_native,[] { return !native_fault.load(); }},active && focused(),menu_open,hook_address && !native_fault);
        LARGE_INTEGER start{}, end{}, frequency{};
        const bool measured=QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0 && QueryPerformanceCounter(&start);
        if (closing) { reset_fire_state(); sync_actions({}); } else update(self, active,&silent);
        if (measured && QueryPerformanceCounter(&end) && end.QuadPart>=start.QuadPart) {
            const float milliseconds=static_cast<float>(static_cast<double>(end.QuadPart-start.QuadPart)*1000/frequency.QuadPart);
            if (std::isfinite(milliseconds)) {
                last_work_ms=milliseconds;
                float peak=peak_work_ms.load();
                while (milliseconds>peak && !peak_work_ms.compare_exchange_weak(peak,milliseconds)) {}
            }
        }
    }
    { SilentScope scope(silent); original_frame(self, slot, active); }
    callbacks.fetch_sub(1);
}
template<std::size_t N> bool code(std::uint32_t rva, const std::array<unsigned char,N>& expected) {
    std::array<unsigned char,N> actual{};
    return sources.memory.read(sources.client.base + rva, actual.data(), N) && actual == expected;
}
bool history_call(std::uint32_t return_rva) {
    std::array<unsigned char,5> instruction{}; std::int32_t relative=0;
    if (!sources.memory.get(sources.client.base+return_rva-5,instruction) || instruction[0]!=0xe8) return false;
    std::memcpy(&relative,instruction.data()+1,4);
    return static_cast<std::int64_t>(return_rva)+relative==n::history_parse;
}
}
void configure(const config::Aim& value) { auto clean = value; config::sanitize(clean); std::lock_guard guard(settings_mutex); if (requested!=clean) { requested=clean; ++settings_revision; } }
void set_menu_open(bool value) { if (menu_open.exchange(value)!=value) ++context_revision; }
Status latest() {
    std::lock_guard guard(status_mutex); auto result = published; result.callbacks = callback_count;
    result.work_ms=last_work_ms.load(); result.peak_ms=peak_work_ms.load();
    result.silent_samples=silent_sample_count.load();
    if (native_fault) {
        result.state = State::error; result.supported = result.active = result.has_target = result.target_on_screen = false;
        result.capabilities = {}; result.detail = action_fault ? "Native action call failed; input ownership is uncertain" : "Native aim call failed; reload required";
    }
    return result;
}
const char* state_name(State state) {
    switch (state) {
    case State::disabled: return "Disabled";
    case State::profile_mismatch: return "Build profile mismatch";
    case State::waiting_for_player: return "Waiting for player";
    case State::idle: return "Ready";
    case State::tracking: return "Tracking";
    case State::firing: return "Firing";
    case State::error: return "Native interface error";
    default: return "Runtime unavailable";
    }
}
const char* fire_block_name(FireBlock reason) {
    switch (reason) {
    case FireBlock::none:return "Ready to request fire";
    case FireBlock::not_ready:return "Aim is not ready";
    case FireBlock::auto_fire_off:return "Auto Fire is off";
    case FireBlock::switch_delay:return "Waiting for target switch delay";
    case FireBlock::input_unavailable:return "Native fire input is unavailable";
    case FireBlock::context_changed:return "Player, weapon or input context changed";
    case FireBlock::weapon_state:return "Weapon firing state is unreadable";
    case FireBlock::reloading:return "Weapon is reloading";
    case FireBlock::empty_clip:return "Weapon clip is empty";
    case FireBlock::alignment:return "Waiting for shot alignment";
    case FireBlock::damage:return "Current native damage is below minimum";
    case FireBlock::scope:return "Waiting for scope";
    case FireBlock::stopping:return "Braking movement";
    case FireBlock::chance_unavailable:return "Shot chance is unavailable for this weapon";
    case FireBlock::weapon_cooldown:return "Waiting for weapon cooldown";
    case FireBlock::readiness_unavailable:return "Native weapon readiness is unavailable";
    case FireBlock::sample_shortage:return "Insufficient current hit-chance samples";
    case FireBlock::chance_below_threshold:return "Sampled hit chance is below threshold";
    case FireBlock::visibility:return "Current fire path is blocked";
    case FireBlock::semi_auto_cooldown:return "Waiting for semi-auto fire pulse";
    }
    return "Fire gate unavailable";
}
int weapon_group(int id) {
    switch (id) {
    case 2: case 3: case 4: case 30: case 32: case 36: case 61: case 63: return 1;
    case 1: case 64: return 2;
    case 7: case 8: case 10: case 13: case 16: case 17: case 19: case 23: case 24: case 26: case 33: case 34: case 39: case 60: return 3;
    case 9: return 4; case 40: return 5; case 11: case 38: return 6;
    case 25: case 27: case 29: case 35: return 7; case 14: case 28: return 8;
    default: return 0;
    }
}
const char* group_name(int group) {
    static constexpr const char* names[] = {"Global", "Pistols", "Heavy Pistols", "Rifles / SMGs", "AWP", "Scout", "Auto Snipers", "Shotguns", "Machineguns"};
    return names[std::clamp(group, 0, 8)];
}
void install_runtime() {
    esp::reset();
    clear_target_caches(); sampled_settings_revision=~std::uint64_t{0};
    last_work_ms=peak_work_ms=0; sample_sequence=0;
    closing = false; native_fault = false; callback_count = 0; updates = 0; toggle = key_previous = false; previous_enabled = false;
    silent_sample_count=0;
    previous_activation = previous_key = -1; pending = 0xffffffff;
    sources = game::current_sources(); Status s;
    if (!sources.client.base || !sources.engine.base) { s.detail = "CS2 modules are not loaded"; publish(s); return; }
    if (!game::collect(sources).profile_valid || !code(n::input_frame,n::input_frame_bytes) || !code(n::get_angles,n::get_angles_bytes) ||
        !code(n::set_angles,n::set_angles_bytes) || !code(n::bone_index,n::bone_index_bytes) || !code(n::bone_position,n::bone_position_bytes) ||
        !code(n::trace_line,n::trace_line_bytes) || !code(n::get_spread,n::get_spread_bytes) || !code(n::get_inaccuracy,n::get_inaccuracy_bytes) || !code(n::get_punch,n::get_punch_bytes)) {
        s.state = State::profile_mismatch; s.detail = "Native aim methods differ from the inspected build"; publish(s); return;
    }
    std::uintptr_t table = 0, entry = 0;
    if (!read(sources.client.base + o::csgo_input, table) || table != sources.client.base + n::input_vtable ||
        !read(table + n::input_slot * 8, entry) || entry != sources.client.base + n::input_frame) {
        s.detail = "Native input callback identity mismatch"; publish(s); return;
    }
    get_angles = reinterpret_cast<Angles>(sources.client.base + n::get_angles);
    set_angles = reinterpret_cast<Angles>(sources.client.base + n::set_angles);
    bone_index = reinterpret_cast<BoneIndex>(sources.client.base + n::bone_index);
    bone_position = reinterpret_cast<BonePosition>(sources.client.base + n::bone_position);
    trace_line = reinterpret_cast<TraceLine>(sources.client.base + n::trace_line);
    get_spread = reinterpret_cast<Spread>(sources.client.base + n::get_spread);
    get_inaccuracy = reinterpret_cast<Inaccuracy>(sources.client.base + n::get_inaccuracy);
    get_punch = reinterpret_cast<Punch>(sources.client.base + n::get_punch);
    primary_ready_verified=code(n::primary_tick_ready,n::primary_tick_ready_bytes);
    primary_ready_query=primary_ready_verified ? reinterpret_cast<PrimaryReady>(sources.client.base+n::primary_tick_ready) : nullptr;
    smoke_verified = code(n::through_smoke,n::through_smoke_bytes);
    through_smoke = reinterpret_cast<ThroughSmoke>(sources.client.base+n::through_smoke);
    const auto tier0 = GetModuleHandleW(L"tier0.dll");
    free_hitboxes = tier0 ? reinterpret_cast<GameFree>(GetProcAddress(tier0,"MemAlloc_FreeFunc")) : nullptr;
    hitboxes_verified = free_hitboxes && code(n::get_hitboxes,n::get_hitboxes_bytes) && code(n::get_skeleton,n::get_skeleton_bytes);
    get_hitboxes = reinterpret_cast<GetHitboxes>(sources.client.base+n::get_hitboxes);
    ballistics_verified = code(n::filter_construct,n::filter_construct_bytes) && code(n::trace_construct,n::trace_construct_bytes) &&
        code(n::trace_destruct,n::trace_destruct_bytes) && code(n::bullet_create,n::bullet_create_bytes) &&
        code(n::bullet_penetrate,n::bullet_penetrate_bytes) && code(n::bullet_interval,n::bullet_interval_bytes) &&
        code(n::hit_group,n::hit_group_bytes) && code(n::is_armored,n::is_armored_bytes);
    trace_construct = reinterpret_cast<Construct>(sources.client.base + n::trace_construct);
    trace_destruct = reinterpret_cast<Destruct>(sources.client.base + n::trace_destruct);
    filter_construct = reinterpret_cast<FilterConstruct>(sources.client.base + n::filter_construct);
    bullet_create = reinterpret_cast<BulletCreate>(sources.client.base + n::bullet_create);
    bullet_penetrate = reinterpret_cast<BulletPenetrate>(sources.client.base + n::bullet_penetrate);
    bullet_interval = reinterpret_cast<BulletInterval>(sources.client.base + n::bullet_interval);
    hit_group = reinterpret_cast<HitGroup>(sources.client.base + n::hit_group);
    is_armored = reinterpret_cast<IsArmored>(sources.client.base + n::is_armored);
    std::array<unsigned char,32> action_code{};
    actions_verified = sources.memory.read(sources.engine.base+n::action_delta,action_code.data(),action_code.size()) && action_code == n::action_delta_bytes;
    static constexpr const char* action_names[] = {"attack","attack2","forward","back","left","right"};
    for (unsigned i = 0; i < n::actions.size() && actions_verified; ++i) {
        std::uintptr_t name = 0; int mode = -1; std::string text;
        actions_verified = read(sources.client.base+n::actions[i],name) && read(sources.client.base+n::actions[i]+8,mode) && mode == 0 &&
            sources.memory.string(name,text) && text == action_names[i];
    }
    action_delta = reinterpret_cast<ActionDelta>(sources.engine.base+n::action_delta);
    owned_actions = {}; actions_released = true; action_fault = false; reset_fire_state();
    // Only real outgoing history appends are admitted, including the helper
    // append path. The debug-only temporary remains outside this contract.
    if (code(n::history_parse,n::history_parse_bytes) && history_call(0xcde18c) && history_call(0xd026d9)) {
        auto* history=reinterpret_cast<void*>(sources.client.base+n::history_parse);
        if (MH_CreateHook(history,reinterpret_cast<void*>(history_hook),reinterpret_cast<void**>(&original_history))==MH_OK) {
            history_hook_address=history;
            if (MH_EnableHook(history)!=MH_OK) {
                if (!stop_runtime()) { s.detail="History hook cleanup is pending"; publish(s); return; }
                closing=false;
            }
        } else original_history=nullptr;
    }
    auto* target = reinterpret_cast<void*>(entry);
    if (MH_CreateHook(target,reinterpret_cast<void*>(input_hook),reinterpret_cast<void**>(&original_frame)) != MH_OK) {
        stop_runtime(); s.detail = "Could not create native input hook"; publish(s); return;
    }
    hook_address = target;
    if (MH_EnableHook(target) != MH_OK) { stop_runtime(); s.detail = "Could not enable native input hook"; publish(s); return; }
    s.supported = true; s.capabilities = capabilities(); s.state = State::disabled; s.detail = "Enable Aimbot to begin"; publish(s);
}
bool stop_runtime() {
    if (!hook_address && !history_hook_address) return true;
    { std::lock_guard guard(execution_mutex); closing = true; clear_target_caches(); }
    const auto release_deadline = GetTickCount64() + 2500;
    while (!actions_released && GetTickCount64() < release_deadline) Sleep(5);
    if (!actions_released) return false;
    if (hook_address) {
        const auto disabled = MH_DisableHook(hook_address);
        if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) return false;
    }
    const auto deadline = GetTickCount64() + 2500;
    while (callbacks.load() && GetTickCount64() < deadline) Sleep(5);
    if (callbacks.load()) return false;
    if (history_hook_address) {
        const auto disabled=MH_DisableHook(history_hook_address);
        if (disabled!=MH_OK && disabled!=MH_ERROR_DISABLED) return false;
        const auto history_deadline=GetTickCount64()+2500;
        while (history_callbacks.load() && GetTickCount64()<history_deadline) Sleep(5);
        if (history_callbacks.load() || MH_RemoveHook(history_hook_address)!=MH_OK) return false;
        history_hook_address=nullptr; original_history=nullptr;
    }
    if (hook_address && MH_RemoveHook(hook_address)!=MH_OK) return false;
    hook_address = nullptr; original_frame = nullptr; esp::reset(); publish({}); return true;
}
}
