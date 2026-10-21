#include "skins.hpp"
#include "skin_runtime.hpp"
#include "game_data.hpp"
#include "generated/cosmetics.hpp"
#include "generated/cosmetic_geometry.hpp"
#include "generated/dump_offsets.hpp"
#include "generated/skin_native.hpp"
#include "verified_layout.hpp"
#include "MinHook.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <vector>

namespace skins {
namespace {
namespace o = dump_offsets;
namespace n = skin_native;
namespace v = verified_layout;
using Frame = void (*)(void*, int);
using Attribute = void (*)(void*, const char*, float);
using Invalidate = void (*)(void*);
using Model = const char* (*)(void*, int);
using SetModel = void (*)(void*, const char*);
using Changed = void (*)(void*, int);
using Materials = void (*)(void*, bool);
using Legacy = bool (*)(void*);
using MeshMask = void (*)(void*, std::uint64_t);
using BodyGroup = int (*)(void*, const char*);
using BodyChoice = void (*)(void*, int, int);
using Allocate = void* (*)(std::size_t);
using Free = void (*)(void*);
struct Vector {
    std::int32_t count = 0;
    std::uint32_t padding = 0;
    std::uintptr_t data = 0;
    std::int32_t capacity = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(Vector) == 24 && offsetof(Vector, data) == 8 && offsetof(Vector, flags) == 20);
struct Field {
    std::uintptr_t address;
    std::vector<unsigned char> original, applied;
};
struct Record {
    std::uint32_t handle = 0;
    std::uint32_t source_weapon = 0xffffffff;
    std::uintptr_t entity = 0, item = 0, clone = 0, vector_address = 0;
    std::uintptr_t mesh_node = 0;
    std::uint64_t mesh_original = 0, mesh_applied = 0, mesh_variants = 0;
    bool mesh_touched = false;
    std::uintptr_t choice_node = 0;
    int choice_index = -1;
    std::uint32_t choice_original = 0, choice_applied = 0;
    bool choice_touched = false;
    bool glove = false, strip_stattrak = false, materials_touched = false;
    bool original_detached = false, header_attempted = false, uncertain_clone_escaped = false;
    Vector original, replacement;
    std::vector<Field> fields;
    std::string model, applied_model;
    int target = 0;
};
std::mutex settings_mutex, status_mutex, execution_mutex;
config::Skins requested;
std::atomic<std::uint64_t> revision{1}, edited_at{0};
std::atomic<bool> closing{false}, restored{true};
std::atomic<unsigned> callbacks{0};
std::atomic<std::uint64_t> frame_callbacks{0};
Status published;
Frame original_frame = nullptr;
void* hook_address = nullptr;
Attribute attribute = nullptr;
Invalidate invalidate = nullptr;
Model weapon_model = nullptr;
SetModel set_model = nullptr;
Changed weapon_changed = nullptr;
Materials clear_materials = nullptr, rebuild_materials = nullptr;
Legacy uses_legacy_model = nullptr;
MeshMask set_mesh_mask = nullptr;
BodyGroup body_group_index = nullptr;
BodyChoice set_body_choice = nullptr;
Allocate allocate = nullptr;
Free release = nullptr;
std::uintptr_t client_base = 0;
std::vector<Record> records; // Accessed only by the intercepted game callback.
std::vector<Record> hud_records; // Geometry only; HUD entities are not C_EconEntity itemviews.
game::Memory memory;
std::uint64_t applied_revision = 0, next_sample = 0;
int applied_team = -1;

void publish(Status s) { std::lock_guard guard(status_mutex); published = std::move(s); }
bool address(std::uintptr_t p) { return p >= 0x10000 && p < 0x00007ffffffff000ULL; }
bool write(std::uintptr_t p, const void* data, std::size_t bytes) {
    if (!address(p) || !data || !bytes || bytes > 4096) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(p), &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        p - reinterpret_cast<std::uintptr_t>(region.BaseAddress) + bytes > region.RegionSize) return false;
    SIZE_T copied = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), data, bytes, &copied) && copied == bytes;
}
template<class T> bool put(std::uintptr_t p, const T& value) { return write(p, &value, sizeof(value)); }
template<class T> bool field(Record& record, std::uintptr_t p, const T& value) {
    Field f; f.address = p; f.original.resize(sizeof(T)); f.applied.resize(sizeof(T));
    if (!memory.read(p, f.original.data(), sizeof(T))) return false;
    std::memcpy(f.applied.data(), &value, sizeof(T));
    record.fields.push_back(std::move(f));
    put(p, value);
    auto& tracked = record.fields.back();
    std::vector<unsigned char> actual(sizeof(T));
    if (!memory.read(p, actual.data(), actual.size())) return false;
    const bool complete = actual == tracked.applied;
    if (!complete) tracked.applied = std::move(actual); // Track even a partial scalar write for rollback.
    return complete;
}
enum class LookupState { unavailable, empty, present };
struct Lookup { LookupState state = LookupState::unavailable; std::uintptr_t entity = 0; };
Lookup lookup(std::uintptr_t system, std::uint32_t handle) {
    if (!address(system) || handle == 0xffffffff) return {};
    const auto index = handle & v::index_mask;
    std::uintptr_t page = 0, instance = 0, backlink = 0;
    std::uint32_t serial = 0;
    if (!memory.get(system + v::chunk_table + index / v::entries_per_chunk * 8, page)) return {};
    if (!page) return {LookupState::empty};
    if (!address(page)) return {};
    const auto identity = page + (index % v::entries_per_chunk) * v::identity_stride;
    if (!memory.get(identity + v::identity_handle, serial)) return {};
    if (serial != handle) return {LookupState::empty};
    if (!memory.get(identity, instance)) return {};
    if (!instance) return {LookupState::empty};
    if (!address(instance) || !memory.get(instance + o::identity, backlink) || backlink != identity) return {};
    return {LookupState::present, instance};
}
std::uintptr_t resolve(std::uintptr_t system, std::uint32_t handle) {
    const auto result = lookup(system, handle);
    return result.state == LookupState::present ? result.entity : 0;
}
bool handle_for(std::uintptr_t entity, std::uint32_t& handle) {
    std::uintptr_t identity = 0;
    return memory.get(entity + o::identity, identity) && address(identity) && memory.get(identity + v::identity_handle, handle);
}
bool changed_method(std::uintptr_t entity) {
    std::uintptr_t table = 0, fn = 0;
    return memory.get(entity, table) && memory.get(table + n::weapon_changed_slot * 8, fn) && fn == client_base + n::weapon_changed;
}
void changed(std::uintptr_t entity) {
    // Primary slot 11 is independently corroborated on C_CSWeaponBase and C_Knife.
    weapon_changed(reinterpret_cast<void*>(entity), 1);
}
void clear_weapon_materials(std::uintptr_t entity) {
    // The native global refresh cancels pending jobs and releases active materials
    // through the composite owner, before rebuilding the persistent weapon descriptor.
    clear_materials(reinterpret_cast<void*>(entity + 0x610), true);
}
void rebuild_weapon_materials(std::uintptr_t entity) {
    // This member obtains the item and the other descriptor arguments itself.
    rebuild_materials(reinterpret_cast<void*>(entity), false);
}
void free_original(Record& r) {
    // A captured header does not transfer ownership. Only a verified swap detaches it.
    if (!r.original_detached) return;
    if (r.original.data && !(r.original.flags & 0xc0000000U)) release(reinterpret_cast<void*>(r.original.data));
    r.original.data = 0; r.original_detached = false;
}
bool same_header(const Vector& a, const Vector& b) { return std::memcmp(&a, &b, sizeof(Vector)) == 0; }
bool write_header(std::uintptr_t destination, const Vector& header) {
    put(destination, header);
    Vector actual;
    return memory.get(destination, actual) && same_header(actual, header);
}
bool prepare_attributes(Record& r) {
    r.vector_address = r.item + o::attribute_list + o::attribute_vector;
    if (!memory.get(r.vector_address, r.original) || r.original.count < 0 || r.original.count > 48 ||
        r.original.capacity < r.original.count || r.original.capacity > 4096 || (r.original.count && !address(r.original.data))) return false;
    Vector clone = r.original;
    clone.capacity = std::max(8, r.original.count + 8);
    clone.flags = 0;
    const auto bytes = static_cast<std::size_t>(clone.capacity) * n::attribute_stride;
    void* data = allocate(bytes);
    if (!data) return false;
    std::memset(data, 0, bytes);
    if (clone.count && !memory.read(r.original.data, data, static_cast<std::size_t>(clone.count) * n::attribute_stride)) {
        release(data); return false;
    }
    if (r.strip_stattrak) {
        // Verified local item schema: definitions 80/81 are integer StatTrak attributes.
        auto* entries = static_cast<unsigned char*>(data);
        int retained = 0;
        for (int i = 0; i < clone.count; ++i) {
            std::uint16_t definition = 0;
            std::memcpy(&definition, entries + i * n::attribute_stride + o::attribute_definition, sizeof(definition));
            if (definition == 80 || definition == 81) continue;
            if (retained != i) std::memmove(entries + retained * n::attribute_stride, entries + i * n::attribute_stride, n::attribute_stride);
            ++retained;
        }
        clone.count = retained;
    }
    clone.data = reinterpret_cast<std::uintptr_t>(data);
    r.clone = clone.data; r.replacement = clone;
    return true;
}
bool publish_attributes(Record& r) {
    // The record is already tracked, including a possibly partial header write.
    r.header_attempted = true;
    if (!write_header(r.vector_address, r.replacement)) return false;
    r.original_detached = true;
    return true;
}
bool original_model(std::uintptr_t entity, std::string& model) {
    std::uintptr_t node = 0, name = 0;
    return memory.get(entity + o::scene_node, node) && address(node) &&
        memory.get(node + o::model_state + o::model_name, name) && memory.string(name, model) &&
        (model.rfind("models/", 0) == 0 || model.rfind("weapons/", 0) == 0) && model.ends_with(".vmdl");
}
void mark_gloves(std::uintptr_t pawn) {
    // Both fields are bytes. The native consumer clears reapply itself.
    put(pawn + o::reapply_gloves, std::uint8_t{1});
    put(pawn + o::gloves_changed, std::uint8_t{1});
}
bool skeleton(std::uintptr_t entity, std::uintptr_t& node) {
    std::uintptr_t table = 0, owner = 0, getter = 0;
    return memory.get(entity + o::scene_node, node) && address(node) &&
        memory.get(node, table) && (table == client_base + n::skeleton_vtable || table == client_base + n::networked_skeleton_vtable) &&
        memory.get(table + 14*8, getter) && getter == client_base + n::skeleton_get &&
        memory.get(node + o::scene_owner, owner) && owner == entity;
}
bool apply_mesh(Record& r, std::uint64_t modern, std::uint64_t legacy, bool use_legacy, bool* modified = nullptr) {
    std::uintptr_t node = 0;
    std::uint64_t current = 0;
    if (!skeleton(r.entity, node) || !memory.get(node + o::model_state + o::model_mesh_mask, current)) return false;
    // Only replace the asset's two visible body variants. Hidden body and other
    // mesh groups retain their current bits; an entirely hidden body stays hidden.
    const auto variants = modern | legacy;
    const auto desired = (current & ~variants) | ((current & variants) ? (use_legacy ? legacy : modern) : 0);
    if (r.mesh_touched && r.mesh_node != node) return false;
    if (current == desired) { if (r.mesh_touched) r.mesh_applied = desired; return true; }
    if (!r.mesh_touched) { r.mesh_node = node; r.mesh_original = current; r.mesh_variants = variants; }
    r.mesh_applied = desired; r.mesh_touched = true;
    if (modified) *modified = true;
    set_mesh_mask(reinterpret_cast<void*>(node), desired);
    return memory.get(node + o::model_state + o::model_mesh_mask, current) && current == desired;
}
bool restore_mesh(Record& r) {
    if (!r.mesh_touched) return true;
    std::uintptr_t node = 0;
    if (!memory.get(r.entity + o::scene_node, node)) return false;
    if (node != r.mesh_node) { r.mesh_touched = false; return true; }
    std::string model;
    if (!original_model(r.entity, model)) return false;
    if (model != r.applied_model || !skeleton(r.entity, node)) { r.mesh_touched = false; return true; }
    std::uint64_t current = 0;
    if (!memory.get(node + o::model_state + o::model_mesh_mask, current)) return false;
    if ((current & r.mesh_variants) == (r.mesh_applied & r.mesh_variants)) {
        const auto desired = (current & ~r.mesh_variants) | (r.mesh_original & r.mesh_variants);
        set_mesh_mask(reinterpret_cast<void*>(node), desired);
        if (!memory.get(node + o::model_state + o::model_mesh_mask, current) || current != desired) return false;
    }
    r.mesh_touched = false; // A newer game mask is preserved.
    return true;
}
bool choice_address(std::uintptr_t node, int index, std::uintptr_t& location) {
    int count = 0;
    std::uintptr_t data = 0;
    const auto choices = node + o::model_state + o::model_body_choices;
    if (!memory.get(choices, count) || count < 0 || count > 64 || index < 0 || index >= count ||
        !memory.get(choices + 8, data) || !address(data)) return false;
    location = data + index * 4;
    return true;
}
bool apply_choice(Record& r, int index, int desired, bool* modified = nullptr) {
    std::uintptr_t node = 0, location = 0;
    std::uint32_t current = 0;
    if (!skeleton(r.entity, node) || !choice_address(node, index, location) || !memory.get(location, current)) return false;
    if (r.choice_touched && (r.choice_node != node || r.choice_index != index)) return false;
    if (current == static_cast<std::uint32_t>(desired)) { if (r.choice_touched) r.choice_applied = current; return true; }
    if (!r.choice_touched) { r.choice_node = node; r.choice_index = index; r.choice_original = current; }
    r.choice_applied = static_cast<std::uint32_t>(desired); r.choice_touched = true;
    if (modified) *modified = true;
    set_body_choice(reinterpret_cast<void*>(node), index, desired);
    return memory.get(location, current) && current == r.choice_applied;
}
bool restore_choice(Record& r) {
    if (!r.choice_touched) return true;
    std::uintptr_t node = 0, location = 0;
    if (!memory.get(r.entity + o::scene_node, node)) return false;
    if (node != r.choice_node) { r.choice_touched = false; return true; }
    std::string model;
    if (!original_model(r.entity, model)) return false;
    if (model != r.applied_model || !skeleton(r.entity, node)) { r.choice_touched = false; return true; }
    std::uint32_t current = 0;
    if (!choice_address(node, r.choice_index, location) || !memory.get(location, current)) return false;
    if (current == r.choice_applied) {
        set_body_choice(reinterpret_cast<void*>(node), r.choice_index, static_cast<int>(r.choice_original));
        if (!memory.get(location, current) || current != r.choice_original) return false;
    }
    r.choice_touched = false;
    return true;
}
bool apply_geometry(Record& r, bool refresh = false) {
    const auto* geometry = cosmetic_geometry::find(r.target);
    const auto* item = cosmetics::find_item(r.target);
    if (!geometry || !geometry->eligible || !item || r.applied_model != item->model) return true;
    const bool legacy = uses_legacy_model(reinterpret_cast<void*>(r.entity));
    bool modified = false;
    if (!apply_mesh(r, geometry->modern_mask, geometry->legacy_mask, legacy, &modified) ||
        !apply_choice(r, body_group_index(reinterpret_cast<void*>(r.entity), "body"),
            legacy ? geometry->legacy_choice : geometry->modern_choice, &modified)) return false;
    if (refresh && modified) { clear_weapon_materials(r.entity); rebuild_weapon_materials(r.entity); }
    return true;
}
bool capture_geometry(Record& r) {
    const auto* geometry = cosmetic_geometry::find(r.target);
    const auto* item = cosmetics::find_item(r.target);
    if (!geometry || !geometry->eligible || !item || r.model != item->model) return true;
    std::uintptr_t node = 0, location = 0;
    const int index = body_group_index(reinterpret_cast<void*>(r.entity), "body");
    if (!skeleton(r.entity, node) || !choice_address(node, index, location) ||
        !memory.get(node + o::model_state + o::model_mesh_mask, r.mesh_original) ||
        !memory.get(location, r.choice_original)) return false;
    // Capture before attribute/model notifications can change these same fields.
    r.mesh_node = r.choice_node = node; r.mesh_variants = geometry->switch_mask;
    r.mesh_applied = r.mesh_original; r.mesh_touched = true;
    r.choice_index = index; r.choice_applied = r.choice_original; r.choice_touched = true;
    return true;
}
bool restore(Record& r, std::uintptr_t system) {
    const auto found = lookup(system, r.handle);
    if (found.state == LookupState::unavailable) return false;
    if (found.state == LookupState::empty || found.entity != r.entity) {
        // A positively replaced/empty identity differs from an unreadable entity table.
        // An installed clone belongs to the engine; an unpublished preparation remains ours.
        if (r.clone && !r.header_attempted) release(reinterpret_cast<void*>(r.clone));
        free_original(r); return true;
    }
    if (r.source_weapon != 0xffffffff) {
        std::uint32_t owner = 0xffffffff;
        if (!memory.get(r.entity + o::owner_handle, owner)) return false;
        if (owner != r.source_weapon) return true; // Reused HUD proxy now belongs to another weapon.
    }
    if (r.materials_touched) {
        if (!changed_method(r.entity)) return false;
        // Cancel work using the installed attributes before returning their storage.
        clear_weapon_materials(r.entity);
    }
    Vector live;
    std::uintptr_t clone_to_free = 0;
    if (r.clone) {
        if (!r.header_attempted) clone_to_free = r.clone;
        else {
            if (!memory.get(r.vector_address, live)) return false;
            if (!r.original_detached) {
                // Publication failed before any native setter. Restore and verify every byte
                // before freeing an allocation that a partial pointer write might reference.
                if (r.uncertain_clone_escaped && live.data != r.clone && live.data != r.original.data) return false;
                if (!(r.uncertain_clone_escaped && live.data == r.original.data) &&
                    !same_header(live, r.original) && !write_header(r.vector_address, r.original)) return false;
                if (live.data == r.clone || !r.uncertain_clone_escaped) clone_to_free = r.clone;
                // If an uncertain publication survived a native frame and the pointer changed,
                // ownership is no longer provable. Retain that allocation rather than double-free.
            } else if (live.data == r.clone) {
                if (!write_header(r.vector_address, r.original)) return false;
                r.original_detached = false; // Ownership returned to the engine only after readback.
                if (!(live.flags & 0xc0000000U)) clone_to_free = r.clone;
            } else if (live.data == r.original.data) {
                // A previous rollback may have succeeded despite an unavailable readback.
                r.original_detached = false;
            } else free_original(r); // A newer engine-owned buffer replaced the installed clone.
        }
    }
    bool ok = true, server_conflict = false;
    for (auto it = r.fields.rbegin(); it != r.fields.rend(); ++it) {
        std::vector<unsigned char> now(it->applied.size());
        if (!memory.read(it->address, now.data(), now.size())) { ok = false; continue; }
        // Do not undo a newer game/server value.
        if (now == it->applied) {
            write(it->address, it->original.data(), it->original.size());
            if (!memory.read(it->address, now.data(), now.size()) || now != it->original) ok = false;
        } else if (now != it->original) server_conflict = true;
    }
    if (r.header_attempted || !r.fields.empty()) invalidate(reinterpret_cast<void*>(r.item));
    if (ok && !server_conflict) {
        if (r.glove) mark_gloves(r.entity);
        else {
            ok = restore_mesh(r);
            if (!restore_choice(r)) ok = false;
            std::string current_model;
            if (!r.model.empty() && !r.applied_model.empty() && original_model(r.entity, current_model) && current_model == r.applied_model)
                set_model(reinterpret_cast<void*>(r.entity), r.model.c_str());
            if (r.source_weapon == 0xffffffff && changed_method(r.entity)) changed(r.entity);
        }
    }
    if (ok && r.materials_touched) { clear_weapon_materials(r.entity); rebuild_weapon_materials(r.entity); }
    if (clone_to_free) release(reinterpret_cast<void*>(clone_to_free));
    r.clone = 0;
    return ok;
}
bool restore_all(std::uintptr_t system) {
    bool ok = true;
    std::erase_if(hud_records, [&](Record& r) { if (restore(r, system)) return true; ok = false; return false; });
    std::erase_if(records, [&](Record& r) { if (restore(r, system)) return true; ok = false; return false; });
    return ok;
}
bool sync_hud(std::uintptr_t pawn, std::uintptr_t system) {
    std::uint32_t arms_handle = 0xffffffff;
    if (!memory.get(pawn + o::hud_arms, arms_handle)) return false;
    const auto arms = resolve(system, arms_handle);
    std::uintptr_t table = 0, root = 0, owner = 0, child = 0;
    std::vector<std::uint32_t> seen;
    bool ok = true;
    if (arms && memory.get(arms, table) && table == client_base + n::hud_arms_vtable &&
        memory.get(arms + o::scene_node, root) && address(root) &&
        memory.get(root + o::scene_owner, owner) && owner == arms && memory.get(root + o::scene_child, child)) {
        std::vector<std::uintptr_t> visited;
        for (unsigned index = 0; child && index < 32; ++index) {
            if (!address(child) || std::find(visited.begin(), visited.end(), child) != visited.end()) { ok = false; break; }
            visited.push_back(child);
            std::uintptr_t entity = 0, parent = 0, next = 0;
            std::uint32_t handle = 0xffffffff, source = 0xffffffff;
            if (!memory.get(child + o::scene_sibling, next)) { ok = false; break; }
            if (memory.get(child + o::scene_parent, parent) && parent == root &&
                memory.get(child + o::scene_owner, entity) && address(entity) &&
                handle_for(entity, handle) && resolve(system, handle) == entity &&
                memory.get(entity, table) && table == client_base + n::hud_weapon_vtable &&
                memory.get(entity + o::owner_handle, source)) {
                const auto weapon = std::find_if(records.begin(), records.end(), [&](const Record& r) {
                    return !r.glove && r.handle == source && resolve(system, source) == r.entity;
                });
                const auto* geometry = weapon != records.end() ? cosmetic_geometry::find(weapon->target) : nullptr;
                std::string model;
                if (geometry && geometry->eligible && original_model(entity, model) && model == weapon->applied_model) {
                    auto hud = std::find_if(hud_records.begin(), hud_records.end(), [=](const Record& r) { return r.handle == handle; });
                    if (hud != hud_records.end() && (hud->entity != entity || hud->source_weapon != source || hud->applied_model != model)) {
                        if (!restore(*hud, system)) { ok = false; break; }
                        hud_records.erase(hud); hud = hud_records.end();
                    }
                    if (hud == hud_records.end()) {
                        Record r; r.entity = entity; r.handle = handle; r.source_weapon = source;
                        r.target = weapon->target; r.applied_model = model;
                        hud_records.push_back(std::move(r)); hud = std::prev(hud_records.end());
                    }
                    seen.push_back(handle);
                    const bool legacy = uses_legacy_model(reinterpret_cast<void*>(weapon->entity));
                    if (!apply_mesh(*hud, geometry->modern_mask, geometry->legacy_mask, legacy) ||
                        !apply_choice(*hud, body_group_index(reinterpret_cast<void*>(entity), "body"),
                            legacy ? geometry->legacy_choice : geometry->modern_choice)) { ok = false; break; }
                }
            }
            child = next;
        }
    }
    std::erase_if(hud_records, [&](Record& r) {
        if (std::find(seen.begin(), seen.end(), r.handle) != seen.end()) return false;
        if (restore(r, system)) return true; ok = false; return false;
    });
    return ok;
}
bool apply(std::uintptr_t entity, std::uint32_t handle, bool glove, const config::Finish& finish) {
    Record r; r.entity = entity; r.handle = handle; r.glove = glove; r.target = finish.item_id; r.strip_stattrak = finish.stattrak < 0;
    r.item = glove ? entity + o::gloves : entity + o::attribute_manager + o::item;
    if (!glove && (!changed_method(entity) || !original_model(entity, r.model))) return false;
    if (!glove && !capture_geometry(r)) return false;
    if (!prepare_attributes(r)) return false;
    records.push_back(std::move(r));
    auto& record = records.back();
    if (!glove) {
        record.materials_touched = true;
        clear_weapon_materials(entity);
    }
    if (!publish_attributes(record)) return false;
    bool ok = field(record, record.item + o::item_definition, static_cast<std::uint16_t>(finish.item_id)) &&
        field(record, record.item + o::item_id_high, std::uint32_t{0xffffffff}) &&
        field(record, record.item + o::item_initialized, std::uint8_t{1}) &&
        field(record, record.item + o::item_disallow_soc, std::uint8_t{1});
    if (!glove) {
        ok = ok && field(record, entity + o::fallback_paint, finish.paint_id) && field(record, entity + o::fallback_seed, finish.seed) &&
            field(record, entity + o::fallback_wear, finish.wear) && field(record, entity + o::fallback_stattrak, finish.stattrak);
        if (cosmetics::find_item(finish.item_id)->category == cosmetics::Category::knife)
            ok = ok && field(record, record.item + o::item_quality, 3);
        else if (finish.stattrak >= 0) ok = ok && field(record, record.item + o::item_quality, 9);
        else {
            int quality = 0;
            if (!memory.get(record.item + o::item_quality, quality)) ok = false;
            else if (quality == 9) ok = ok && field(record, record.item + o::item_quality, finish.paint_id ? 4 : 0);
        }
    }
    if (!ok) return false; // The caller restores the tracked transaction.
    auto* item = reinterpret_cast<void*>(record.item);
    attribute(item, "set item texture prefab", static_cast<float>(finish.paint_id));
    attribute(item, "set item texture seed", static_cast<float>(finish.seed));
    attribute(item, "set item texture wear", finish.wear);
    if (!glove) {
        // Integer attributes use float register storage with the integer bits preserved.
        if (finish.stattrak >= 0) {
            attribute(item, "kill eater", std::bit_cast<float>(finish.stattrak));
            attribute(item, "kill eater score type", std::bit_cast<float>(std::int32_t{0}));
        }
        changed(entity);
        const char* model = weapon_model(item, 0);
        std::string checked;
        if (model && memory.string(reinterpret_cast<std::uintptr_t>(model), checked) &&
            (checked.rfind("models/", 0) == 0 || checked.rfind("weapons/", 0) == 0) && checked.ends_with(".vmdl"))
        { set_model(reinterpret_cast<void*>(entity), checked.c_str()); record.applied_model = checked; }
        else return false;
        if (!apply_geometry(record)) return false;
        // Model/geometry notifications can enqueue their own material request.
        // The final native clear/rebuild pair must follow all those updates.
        clear_weapon_materials(entity);
        rebuild_weapon_materials(entity);
    } else mark_gloves(entity);
    Vector live;
    if (!memory.get(record.vector_address, live) || live.data != record.clone || live.count < record.replacement.count || live.count > record.replacement.count + 8) return false;
    return true;
}
void update() {
    const auto source = game::current_sources(); memory = source.memory;
    std::uintptr_t system = 0;
    memory.get(source.client.base + o::entity_system, system);
    if (closing.load()) { restored = restore_all(system); return; }
    const auto now = GetTickCount64();
    if (now < next_sample) return;
    next_sample = now + 100;
    config::Skins config;
    std::uint64_t wanted;
    { std::lock_guard guard(settings_mutex); config = requested; wanted = revision.load(); }
    Status s; s.supported = s.weapons_supported = s.knives_supported = s.gloves_supported = true;
    const auto data = game::collect(source);
    if (!data.profile_valid) {
        s.state = State::profile_mismatch; s.detail = data.detail; publish(std::move(s)); return;
    }
    if (wanted != applied_revision && now - edited_at.load() < 150) return;
    if (!config.enabled || !data.in_game || !data.local_valid || !data.alive) {
        if (!restore_all(system)) { s.state = State::error; s.detail = "Waiting to restore an unavailable item"; }
        else { s.state = !config.enabled ? State::disabled : State::waiting_for_player; s.detail = !config.enabled ? "Original appearance restored" : "Waiting for a living local player"; }
        applied_revision = wanted; publish(std::move(s)); return;
    }
    if (wanted != applied_revision || applied_team != data.team) {
        if (!restore_all(system)) { s.state = State::error; s.detail = "Could not restore previous cosmetic transaction"; publish(std::move(s)); return; }
        applied_revision = wanted;
        applied_team = data.team;
    }
    std::uintptr_t pawn = 0, service = 0, handles = 0;
    std::uint32_t pawn_handle = 0xffffffff;
    int count = 0;
    if (!memory.get(source.client.base + o::local_pawn, pawn) || !handle_for(pawn, pawn_handle) || resolve(system, pawn_handle) != pawn ||
        !memory.get(pawn + o::weapon_services, service) || !address(service) || !memory.get(service + o::weapons, count) || count < 0 || count > 64 ||
        !memory.get(service + o::weapons + 8, handles) || (count && !address(handles))) {
        s.state = State::waiting_for_player; s.detail = "Local loadout changed; waiting for the next frame"; publish(std::move(s)); return;
    }
    std::vector<std::uint32_t> owned;
    for (int i = 0; i < count; ++i) { std::uint32_t h = 0xffffffff; if (memory.get(handles + i*4, h)) owned.push_back(h); }
    // A dropped/replaced weapon is restored while its serial still resolves.
    bool ok = true;
    std::erase_if(records, [&](Record& r) {
        bool unchanged = true;
        Vector current;
        if (!memory.get(r.vector_address, current) || current.data != r.clone) unchanged = false;
        for (const auto& f : r.fields) {
            std::array<unsigned char, 8> value{};
            if (f.applied.size() > value.size() || !memory.read(f.address, value.data(), f.applied.size()) ||
                !std::equal(f.applied.begin(), f.applied.end(), value.begin())) { unchanged = false; break; }
        }
        if (unchanged && ((r.glove && r.entity == pawn) || (!r.glove && std::find(owned.begin(), owned.end(), r.handle) != owned.end() && resolve(system, r.handle) == r.entity))) return false;
        if (restore(r, system)) return true; ok = false; return false;
    });
    const int team = data.team == 2 ? 0 : data.team == 3 ? 1 : -1;
    for (const auto h : owned) {
        const auto entity = resolve(system, h);
        std::uint16_t definition = 0;
        if (entity && memory.get(entity + o::attribute_manager + o::item + o::item_definition, definition)) s.carried_items.push_back(definition);
    }
    for (const auto h : owned) {
        if (std::any_of(records.begin(), records.end(), [=](const Record& r) { return !r.glove && r.handle == h; })) continue;
        const auto entity = resolve(system, h);
        std::uint32_t owner = 0xffffffff;
        std::uint16_t id = 0;
        if (!entity || !memory.get(entity + o::owner_handle, owner) || owner != pawn_handle ||
            !memory.get(entity + o::attribute_manager + o::item + o::item_definition, id)) continue;
        const auto* item = cosmetics::find_item(id);
        if (!item || item->category == cosmetics::Category::gloves) continue;
        const int target = item->category == cosmetics::Category::knife && team >= 0 && config.knives[team] ? config.knives[team] : id;
        const auto* finish = config::find_finish(config, target);
        if (finish && finish->enabled && !apply(entity, h, false, *finish)) { ok = false; break; }
    }
    // Native animation can change body visibility or other mesh groups. Recheck
    // just the visible variant while retaining the first captured original bits.
    if (ok) for (auto& r : records) if (!r.glove && !apply_geometry(r, true)) { ok = false; break; }
    if (ok) ok = sync_hud(pawn, system);
    if (ok && team >= 0 && config.gloves[team] && !std::any_of(records.begin(), records.end(), [](const Record& r) { return r.glove; })) {
        const auto* finish = config::find_finish(config, config.gloves[team]);
        if (finish && finish->enabled) ok = apply(pawn, pawn_handle, true, *finish);
    }
    if (!ok) {
        restore_all(system); s.state = State::error; s.detail = "Cosmetic change rejected; previous appearance restored";
    } else {
        for (const auto& r : records) {
            s.applied_items.push_back(r.target);
            if (r.glove) s.gloves_applied = true;
            else if (cosmetics::find_item(r.target)->category == cosmetics::Category::knife) s.knife_applied = true;
            else ++s.applied_weapons;
        }
        s.state = records.empty() ? State::ready : State::applied;
        s.detail = records.empty() ? "Choose a finish for your current loadout" : "Local item values applied; material update requested";
    }
    restored = records.empty() && hud_records.empty(); publish(std::move(s));
}
void frame_hook(void* self, int stage) {
    callbacks.fetch_add(1);
    frame_callbacks.fetch_add(1);
    // Stage 6 runs at the native FramePostDataUpdate callback, on the game thread.
    if (stage == 6 || closing.load()) {
        std::lock_guard guard(execution_mutex); update();
        for (auto& r : records) if (r.clone && r.header_attempted && !r.original_detached) r.uncertain_clone_escaped = true;
    }
    original_frame(self, stage);
    callbacks.fetch_sub(1);
}
template<std::size_t N> bool code(std::uint32_t rva, const std::array<unsigned char,N>& expected) {
    std::array<unsigned char,N> actual{};
    return memory.read(client_base + rva, actual.data(), N) && actual == expected;
}
}
void configure(const config::Skins& value) {
    auto clean = value; config::sanitize(clean);
    std::lock_guard guard(settings_mutex);
    if (clean == requested) return;
    requested = std::move(clean); edited_at = GetTickCount64(); ++revision;
}
void request_apply() { edited_at = 0; ++revision; }
Status latest() { std::lock_guard guard(status_mutex); auto result = published; result.frame_callbacks = frame_callbacks.load(); return result; }
const char* state_name(State state) {
    switch (state) {
    case State::disabled: return "Disabled";
    case State::profile_mismatch: return "Build profile mismatch";
    case State::waiting_for_player: return "Waiting for local player";
    case State::ready: return "Ready";
    case State::applied: return "Local values applied";
    case State::error: return "Update failed";
    default: return "Runtime unavailable";
    }
}
void install_runtime() {
    closing = false; restored = true; applied_revision = 0; next_sample = 0; applied_team = -1; frame_callbacks = 0;
    Status s;
    const auto source = game::current_sources(); memory = source.memory; client_base = source.client.base;
    if (!client_base || !source.engine.base) { s.detail = "CS2 modules are not loaded"; publish(std::move(s)); return; }
    if (!game::collect(source).profile_valid || !code(n::frame, n::frame_bytes) || !code(n::set_attribute, n::set_attribute_bytes) ||
        !code(n::invalidate_item, n::invalidate_item_bytes) || !code(n::weapon_model, n::weapon_model_bytes) ||
        !code(n::set_model, n::set_model_bytes) || !code(n::weapon_changed, n::weapon_changed_bytes) ||
        !code(n::clear_materials, n::clear_materials_bytes) || !code(n::rebuild_materials, n::rebuild_materials_bytes) ||
        !code(n::uses_legacy_model, n::uses_legacy_model_bytes) || !code(n::set_mesh_mask, n::set_mesh_mask_bytes) ||
        !code(n::body_group_index, n::body_group_index_bytes) || !code(n::set_body_choice, n::set_body_choice_bytes)) {
        s.state = State::profile_mismatch; s.detail = "Native cosmetic methods differ from the inspected build"; publish(std::move(s)); return;
    }
    std::uintptr_t table = 0, entry = 0;
    if (!memory.get(client_base + n::client_interface, table) || table != client_base + n::client_vtable ||
        !memory.get(table + n::frame_slot * 8, entry) || entry != client_base + n::frame) {
        s.detail = "Source2Client frame callback identity mismatch"; publish(std::move(s)); return;
    }
    const auto tier0 = GetModuleHandleW(L"tier0.dll");
    allocate = reinterpret_cast<Allocate>(GetProcAddress(tier0, "MemAlloc_AllocFunc"));
    release = reinterpret_cast<Free>(GetProcAddress(tier0, "MemAlloc_FreeFunc"));
    if (!allocate || !release) { s.detail = "Game allocator exports unavailable"; publish(std::move(s)); return; }
    attribute = reinterpret_cast<Attribute>(client_base + n::set_attribute);
    invalidate = reinterpret_cast<Invalidate>(client_base + n::invalidate_item);
    weapon_model = reinterpret_cast<Model>(client_base + n::weapon_model);
    set_model = reinterpret_cast<SetModel>(client_base + n::set_model);
    weapon_changed = reinterpret_cast<Changed>(client_base + n::weapon_changed);
    clear_materials = reinterpret_cast<Materials>(client_base + n::clear_materials);
    rebuild_materials = reinterpret_cast<Materials>(client_base + n::rebuild_materials);
    uses_legacy_model = reinterpret_cast<Legacy>(client_base + n::uses_legacy_model);
    set_mesh_mask = reinterpret_cast<MeshMask>(client_base + n::set_mesh_mask);
    body_group_index = reinterpret_cast<BodyGroup>(client_base + n::body_group_index);
    set_body_choice = reinterpret_cast<BodyChoice>(client_base + n::set_body_choice);
    auto* target = reinterpret_cast<void*>(entry);
    if (MH_CreateHook(target, reinterpret_cast<void*>(frame_hook), reinterpret_cast<void**>(&original_frame)) != MH_OK) {
        s.detail = "Could not create cosmetic frame hook"; publish(std::move(s)); return;
    }
    hook_address = target;
    if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); hook_address = nullptr; s.detail = "Could not enable cosmetic frame hook"; publish(std::move(s)); return; }
    s.supported = s.weapons_supported = s.knives_supported = s.gloves_supported = true;
    s.state = State::disabled; s.detail = "Select a finish, then enable Skin Changer"; publish(std::move(s));
}
bool stop_runtime() {
    if (!hook_address) return true;
    { std::lock_guard guard(execution_mutex); closing = true; restored = records.empty() && hud_records.empty(); }
    const auto restore_deadline = GetTickCount64() + 2500;
    while (!restored.load() && GetTickCount64() < restore_deadline) Sleep(5);
    if (!restored.load()) return false;
    const auto disabled = MH_DisableHook(hook_address);
    if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) return false;
    const auto deadline = GetTickCount64() + 2500;
    while (callbacks.load() && GetTickCount64() < deadline) Sleep(5);
    if (callbacks.load() || MH_RemoveHook(hook_address) != MH_OK) return false;
    hook_address = nullptr; original_frame = nullptr;
    publish({}); return true;
}
}
