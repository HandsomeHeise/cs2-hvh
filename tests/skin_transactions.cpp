// Same-TU tests exercise the actual private transaction implementation on owned pages.
// This executable never opens CS2, installs hooks, or calls native game functions.
#include "../src/skins.cpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace {
unsigned checks = 0, frees = 0, invalidations = 0, model_changes = 0;
std::unordered_set<void*> allocations;
std::uintptr_t material_entity = 0;
std::vector<std::string> material_events;
std::vector<std::uintptr_t> clear_headers, rebuild_headers;
std::vector<int> rebuild_paints;
bool legacy_policy = false, reject_mesh_write = false;
void require(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
void* heap_allocate(std::size_t bytes) {
    auto* data = std::malloc(bytes);
    if (data) allocations.insert(data);
    return data;
}
void heap_release(void* data) {
    if (!data || allocations.erase(data) != 1) throw std::runtime_error("double-free or foreign storage release");
    ++frees;
    if (material_entity) material_events.push_back("free");
    std::free(data);
}
void invalidate_stub(void*) { ++invalidations; if (material_entity) material_events.push_back("invalidate"); }
void model_stub(void*, const char*) { ++model_changes; if (material_entity) material_events.push_back("model"); }
void changed_stub(void*, int flags) { require(flags == 1, "native changed flag preserved"); material_events.push_back("changed"); }
const char* weapon_model_stub(void*, int) { return cosmetics::find_item(61)->model; }
void attribute_stub(void*, const char*, float) { material_events.push_back("attribute"); }
bool legacy_stub(void*) { return legacy_policy; }
void mesh_stub(void* node, std::uint64_t mask) {
    if (material_entity) material_events.push_back("mesh");
    if (!reject_mesh_write) std::memcpy(static_cast<unsigned char*>(node) + dump_offsets::model_state + dump_offsets::model_mesh_mask, &mask, sizeof(mask));
}
int body_index_stub(void*, const char* name) { require(std::strcmp(name, "body") == 0, "native body lookup receives named group"); return 0; }
void choice_stub(void* node, int index, int choice) {
    const auto vector = reinterpret_cast<std::uintptr_t>(node) + dump_offsets::model_state + dump_offsets::model_body_choices;
    int count = 0; std::uintptr_t data = 0;
    if (!skins::memory.get(vector, count) || !skins::memory.get(vector + 8, data) || index < 0 || index >= count)
        throw std::runtime_error("choice setter must never resize or access a missing entry");
    if (material_entity) material_events.push_back("choice");
    std::memcpy(reinterpret_cast<void*>(data + index * 4), &choice, sizeof(choice));
}
std::uintptr_t live_attribute_data() {
    skins::Vector header;
    if (!skins::memory.get(material_entity + dump_offsets::attribute_manager + dump_offsets::item +
        dump_offsets::attribute_list + dump_offsets::attribute_vector, header)) throw std::runtime_error("material header unavailable");
    return header.data;
}
void clear_stub(void* owner, bool notify) {
    require(reinterpret_cast<std::uintptr_t>(owner) == material_entity + 0x610 && notify, "clear receives native owner and notify flag");
    material_events.push_back("clear"); clear_headers.push_back(live_attribute_data());
}
void rebuild_stub(void* entity, bool high_resolution) {
    require(reinterpret_cast<std::uintptr_t>(entity) == material_entity && !high_resolution, "rebuild receives weapon and native false flag");
    material_events.push_back("rebuild"); rebuild_headers.push_back(live_attribute_data());
    int paint = -1;
    if (!skins::memory.get(material_entity + dump_offsets::fallback_paint, paint)) throw std::runtime_error("material paint unavailable");
    rebuild_paints.push_back(paint);
}
struct Fixture {
    std::uintptr_t system = 0, page = 0, entity = 0, node = 0;
    std::uintptr_t fail_read = 0;
    unsigned fail_reads = 0;
    static constexpr std::uint32_t handle = (7 << 15) | 1;
    skins::Vector original;
    std::array<std::uintptr_t, 12> table{};
    std::string model = "weapons/models/ak47/weapon_rif_ak47.vmdl";
    static std::uintptr_t reserve(std::size_t size) {
        auto result = reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!result) throw std::runtime_error("fixture VirtualAlloc failed");
        return result;
    }
    template<class T> static void store(std::uintptr_t where, const T& value) { std::memcpy(reinterpret_cast<void*>(where), &value, sizeof(T)); }
    static bool read(void* user, std::uintptr_t where, void* output, std::size_t bytes) {
        auto& f = *static_cast<Fixture*>(user);
        if (where == f.fail_read && f.fail_reads) { --f.fail_reads; return false; }
        if (bytes == sizeof(std::uintptr_t) &&
            (where == skins::client_base + skin_native::skeleton_vtable + 14*8 ||
             where == skins::client_base + skin_native::networked_skeleton_vtable + 14*8)) {
            const auto getter = skins::client_base + skin_native::skeleton_get;
            std::memcpy(output, &getter, sizeof(getter)); return true;
        }
        SIZE_T copied = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(where), output, bytes, &copied) && copied == bytes;
    }
    Fixture() {
        material_entity = 0; material_events.clear(); clear_headers.clear(); rebuild_headers.clear(); rebuild_paints.clear();
        legacy_policy = false; reject_mesh_write = false;
        system = reserve(0x1000); page = reserve(0xe000); entity = reserve(0x4000); node = reserve(0x1000);
        store(system + verified_layout::chunk_table, page);
        const auto identity = page + verified_layout::identity_stride;
        store(identity, entity); store(identity + verified_layout::identity_handle, handle);
        store(entity + dump_offsets::identity, identity);
        store(entity + dump_offsets::scene_node, node);
        store(node, skins::client_base + skin_native::skeleton_vtable);
        store(node + dump_offsets::scene_owner, entity);
        store(node + dump_offsets::model_state + dump_offsets::model_mesh_mask, std::uint64_t{1});
        store(node + dump_offsets::model_state + dump_offsets::model_body_choices, 1);
        store(node + dump_offsets::model_state + dump_offsets::model_body_choices + 8, node + 0x800);
        store(node + dump_offsets::model_state + dump_offsets::model_name, reinterpret_cast<std::uintptr_t>(model.c_str()));
        original.count = 1; original.capacity = 2; original.padding = 0xdecafbad;
        original.data = reinterpret_cast<std::uintptr_t>(heap_allocate(original.capacity * skin_native::attribute_stride));
        std::memset(reinterpret_cast<void*>(original.data), 0x42, original.capacity * skin_native::attribute_stride);
        skins::memory = {read, this}; skins::allocate = heap_allocate; skins::release = heap_release;
        skins::invalidate = invalidate_stub; skins::set_model = model_stub;
        skins::weapon_changed = changed_stub; skins::weapon_model = weapon_model_stub;
        skins::attribute = attribute_stub; skins::clear_materials = clear_stub; skins::rebuild_materials = rebuild_stub;
        skins::uses_legacy_model = legacy_stub; skins::set_mesh_mask = mesh_stub;
        skins::body_group_index = body_index_stub; skins::set_body_choice = choice_stub;
        table[skin_native::weapon_changed_slot] = skins::client_base + skin_native::weapon_changed;
        store(entity, reinterpret_cast<std::uintptr_t>(table.data()));
    }
    skins::Record record(bool glove = true) {
        skins::Record r;
        r.entity = entity; r.handle = handle; r.glove = glove;
        r.item = glove ? entity + dump_offsets::gloves : entity + dump_offsets::attribute_manager + dump_offsets::item;
        r.vector_address = r.item + dump_offsets::attribute_list + dump_offsets::attribute_vector;
        store(r.vector_address, original);
        std::memset(reinterpret_cast<void*>(r.vector_address + sizeof(skins::Vector)), 0xa5, 0x68 - sizeof(skins::Vector));
        return r;
    }
    bool metadata(const skins::Record& r) {
        const auto* p = reinterpret_cast<const unsigned char*>(r.vector_address + sizeof(skins::Vector));
        for (std::size_t i = 0; i < 0x68 - sizeof(skins::Vector); ++i) if (p[i] != 0xa5) return false;
        return true;
    }
    bool original_header(const skins::Record& r) {
        skins::Vector live; return skins::memory.get(r.vector_address, live) && skins::same_header(live, original);
    }
    void protect(DWORD access) { DWORD previous = 0; if (!VirtualProtect(reinterpret_cast<void*>(entity), 0x4000, access, &previous)) throw std::runtime_error("fixture protect failed"); }
    ~Fixture() {
        material_entity = 0;
        for (auto address : {system, page, entity, node}) if (address) VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE);
        for (auto* data : allocations) std::free(data);
        allocations.clear();
    }
};

void normal_roundtrip() {
    Fixture f; auto r = f.record(); const auto start_frees = frees;
    require(skins::prepare_attributes(r) && !r.original_detached && f.original_header(r), "preparation keeps game storage ownership");
    require(skins::publish_attributes(r) && r.original_detached, "verified publication detaches original storage");
    require(f.metadata(r), "publication preserves network owner and notification metadata after byte24");
    require(skins::restore(r, f.system) && f.original_header(r) && !r.original_detached, "original full storage header restored");
    require(frees == start_frees + 1 && allocations.contains(reinterpret_cast<void*>(f.original.data)), "restore frees only clone and returns original to engine");
    require(f.metadata(r), "restoration preserves network metadata");
}
void unreadable_and_recycled() {
    Fixture f; auto r = f.record();
    require(skins::prepare_attributes(r) && skins::publish_attributes(r), "prepare unreadable lookup fixture");
    const auto start_frees = frees;
    f.fail_read = f.system + verified_layout::chunk_table; f.fail_reads = 2;
    require(skins::lookup(f.system, r.handle).state == skins::LookupState::unavailable, "failed table read is unavailable");
    require(!skins::restore(r, f.system) && r.original_detached && frees == start_frees, "unreadable table retains record and original backup");
    f.fail_reads = 0;
    const auto clone = r.clone;
    heap_release(reinterpret_cast<void*>(clone)); // Simulate the engine destroying its installed clone.
    Fixture::store(f.page + verified_layout::identity_stride + verified_layout::identity_handle, Fixture::handle + (1 << 15));
    require(skins::restore(r, f.system) && frees == start_frees + 2 && !r.original_detached, "recycled identity releases detached original without double-freeing engine clone");
}
void failed_publication() {
    Fixture f; auto r = f.record();
    require(skins::prepare_attributes(r), "prepare failed publication fixture");
    const auto start_frees = frees;
    f.protect(PAGE_READONLY);
    require(!skins::publish_attributes(r) && !r.original_detached, "readonly storage rejects publication without claiming original");
    require(skins::restore(r, f.system) && f.original_header(r), "failed publication rollback verifies original header");
    require(frees == start_frees + 1 && allocations.contains(reinterpret_cast<void*>(f.original.data)), "failed publication never frees game original");
    f.protect(PAGE_READWRITE);
}
void partial_and_unconfirmed_publication() {
    Fixture f; auto r = f.record();
    require(skins::prepare_attributes(r), "prepare partial header fixture");
    r.header_attempted = true;
    std::memcpy(reinterpret_cast<void*>(r.vector_address), &r.replacement, 16); // Pointer installed; capacity/flags not installed.
    require(skins::restore(r, f.system) && f.original_header(r) && allocations.contains(reinterpret_cast<void*>(f.original.data)), "partial header is fully rolled back before clone release");
    r = f.record(); require(skins::prepare_attributes(r), "prepare unavailable publication readback fixture");
    f.fail_read = r.vector_address; f.fail_reads = 1;
    require(!skins::publish_attributes(r) && !r.original_detached, "unavailable publication readback does not claim detached original");
    require(skins::restore(r, f.system) && f.original_header(r), "unconfirmed completed publication is reversibly rolled back");
}
void failed_restore_retry() {
    Fixture f; auto r = f.record();
    require(skins::prepare_attributes(r) && skins::publish_attributes(r), "prepare retry fixture");
    const auto field_address = f.entity + 0x100;
    Fixture::store(field_address, std::uint32_t{12});
    require(skins::field(r, field_address, std::uint32_t{99}), "tracked scalar changed");
    const auto start_frees = frees;
    f.fail_read = field_address; f.fail_reads = 1;
    require(!skins::restore(r, f.system) && f.original_header(r) && !r.original_detached && !r.clone, "failed scalar restore retains transaction after header ownership returned");
    require(frees == start_frees + 1 && allocations.contains(reinterpret_cast<void*>(f.original.data)), "first partial restore frees clone only");
    require(skins::restore(r, f.system) && frees == start_frees + 1, "retry never frees returned original or clone twice");
    std::uint32_t value = 0; skins::memory.get(field_address, value);
    require(value == 12, "retry restores pending scalar");
}
void failed_header_restore_retry() {
    Fixture f; auto r = f.record();
    require(skins::prepare_attributes(r) && skins::publish_attributes(r), "prepare failed header restore fixture");
    const auto start_frees = frees;
    f.protect(PAGE_READONLY);
    require(!skins::restore(r, f.system) && r.original_detached && r.clone && frees == start_frees, "failed header restoration retains both ownership records");
    f.protect(PAGE_READWRITE);
    require(skins::restore(r, f.system) && f.original_header(r) && frees == start_frees + 1, "header restoration retries before clone release");
}
void server_changes() {
    Fixture f; auto r = f.record(false);
    require(skins::prepare_attributes(r) && skins::publish_attributes(r), "prepare server change fixture");
    const auto field_address = f.entity + 0x100;
    Fixture::store(field_address, std::uint32_t{12});
    require(skins::field(r, field_address, std::uint32_t{99}), "track server scalar fixture");
    Fixture::store(field_address, std::uint32_t{123});
    r.model = "weapons/models/ak47/weapon_rif_ak47.vmdl";
    r.applied_model = "weapons/models/knife/weapon_knife_bayonet.vmdl";
    f.model = "weapons/models/awp/weapon_snip_awp.vmdl";
    Fixture::store(f.node + dump_offsets::model_state + dump_offsets::model_name, reinterpret_cast<std::uintptr_t>(f.model.c_str()));
    const auto changes = model_changes;
    require(skins::restore(r, f.system), "server-change transaction ends without undoing newer values");
    std::uint32_t value = 0; skins::memory.get(field_address, value);
    require(value == 123 && model_changes == changes, "new server scalar and model preserved");
    r = f.record(false); require(skins::prepare_attributes(r) && skins::publish_attributes(r), "prepare isolated model guard fixture");
    r.model = "weapons/models/ak47/weapon_rif_ak47.vmdl"; r.applied_model = "weapons/models/knife/weapon_knife_bayonet.vmdl";
    require(skins::restore(r, f.system) && model_changes == changes, "new server model preserved even without scalar conflict");
}
void stattrak_filter() {
    Fixture f; auto r = f.record();
    f.original.count = 2; Fixture::store(r.vector_address, f.original);
    Fixture::store(f.original.data + dump_offsets::attribute_definition, std::uint16_t{80});
    Fixture::store(f.original.data + skin_native::attribute_stride + dump_offsets::attribute_definition, std::uint16_t{8});
    r.strip_stattrak = true;
    require(skins::prepare_attributes(r) && r.replacement.count == 1, "StatTrak off removes copied counter attributes");
    std::uint16_t definition = 0;
    std::memcpy(&definition, reinterpret_cast<const void*>(r.clone + dump_offsets::attribute_definition), sizeof(definition));
    require(definition == 8 && *reinterpret_cast<const unsigned char*>(r.clone) == 0x42, "filter preserves remaining attribute prefix and value payload");
    require(skins::publish_attributes(r) && skins::restore(r, f.system) && f.original_header(r), "filtered clone restores original count and storage");
    std::memcpy(&definition, reinterpret_cast<const void*>(f.original.data + dump_offsets::attribute_definition), sizeof(definition));
    require(definition == 80 && f.metadata(r), "filter leaves original StatTrak data and network metadata intact");
}
void material_roundtrip(bool fail_restore) {
    Fixture f; auto initial = f.record(false);
    f.model = cosmetics::find_item(61)->model;
    Fixture::store(f.node + dump_offsets::model_state + dump_offsets::model_name, reinterpret_cast<std::uintptr_t>(f.model.c_str()));
    Fixture::store(initial.item + dump_offsets::item_definition, std::uint16_t{61});
    Fixture::store(f.entity + dump_offsets::fallback_paint, 12);
    config::Finish finish; finish.item_id = 61; finish.paint_id = 705; finish.enabled = true;
    legacy_policy = true;
    material_entity = f.entity;
    require(skins::apply(f.entity, Fixture::handle, false, finish) && skins::records.size() == 1, "actual weapon apply completes tracked transaction");
    auto& r = skins::records.back(); const auto clone = r.clone;
    require(clear_headers == std::vector<std::uintptr_t>{f.original.data, clone}, "apply cancels old jobs before swap and notification jobs after geometry");
    require(rebuild_headers == std::vector<std::uintptr_t>{clone} && rebuild_paints == std::vector<int>{705}, "native rebuild sees selected paint and installed clone");
    require(material_events.front() == "clear" && std::find(material_events.begin(), material_events.end(), "model") < std::find(material_events.begin(), material_events.end(), "mesh") && material_events.back() == "rebuild", "apply rebuild follows attributes, model and legacy geometry update");
    require(*reinterpret_cast<std::uint64_t*>(f.node + dump_offsets::model_state + dump_offsets::model_mesh_mask) == 2, "actual apply uses native legacy decision and asset geometry");
    require(*reinterpret_cast<std::uint32_t*>(f.node + 0x800) == 1 && material_events[material_events.size()-2] == "clear", "body choice and final cache clear precede material generation");
    if (fail_restore) {
        f.protect(PAGE_READONLY);
        require(!skins::restore(r, f.system) && r.clone == clone && rebuild_headers.size() == 1 && allocations.contains(reinterpret_cast<void*>(clone)), "failed header rollback cancels jobs and retains clone without rebuilding");
        f.protect(PAGE_READWRITE);
    }
    require(skins::restore(r, f.system) && f.original_header(r), "material rollback restores original attribute storage");
    require(*reinterpret_cast<std::uint64_t*>(f.node + dump_offsets::model_state + dump_offsets::model_mesh_mask) == 1, "actual rollback restores original geometry");
    require(*reinterpret_cast<std::uint32_t*>(f.node + 0x800) == 0, "actual rollback restores original body choice");
    require(clear_headers[clear_headers.size()-2] == clone && clear_headers.back() == f.original.data && rebuild_headers.back() == f.original.data && rebuild_paints.back() == 12, "restore cancels clone and notification jobs before rebuilding original paint");
    require(material_events[material_events.size()-2] == "rebuild" && material_events.back() == "free", "clone released only after native rebuild reads restored storage");
    require(!allocations.contains(reinterpret_cast<void*>(clone)) && allocations.contains(reinterpret_cast<void*>(f.original.data)), "material rollback preserves engine original allocation");
    skins::records.clear();
}
void mesh_roundtrip() {
    Fixture f; auto r = f.record(false); r.applied_model = f.model;
    const auto mask_address = f.node + dump_offsets::model_state + dump_offsets::model_mesh_mask;
    Fixture::store(mask_address, std::uint64_t{0x81});
    require(skins::apply_mesh(r, 1, 2, true) && r.mesh_applied == 0x82, "legacy mesh switch preserves unrelated group bits");
    require(skins::restore_mesh(r) && !r.mesh_touched && *reinterpret_cast<std::uint64_t*>(mask_address) == 0x81, "native mesh restoration returns original mask");
    Fixture::store(f.node, skins::client_base + skin_native::networked_skeleton_vtable);
    require(skins::apply_mesh(r, 1, 2, true) && skins::restore_mesh(r), "networked weapon skeleton subtype uses the same verified native getter");
    require(skins::apply_mesh(r, 1, 2, true), "prepare changed unrelated group fixture");
    Fixture::store(mask_address, std::uint64_t{0x102});
    require(skins::restore_mesh(r) && *reinterpret_cast<std::uint64_t*>(mask_address) == 0x101, "mesh restoration preserves newer unrelated group bits");
    Fixture::store(mask_address, std::uint64_t{4});
    require(skins::apply_mesh(r, 1, 2, true) && !r.mesh_touched && *reinterpret_cast<std::uint64_t*>(mask_address) == 4, "hidden body is not forced visible");
    Fixture::store(mask_address, std::uint64_t{2});
    require(skins::apply_mesh(r, 1, 2, false) && r.mesh_applied == 1 && skins::restore_mesh(r), "modern finish can select modern geometry and restore legacy original");
    Fixture::store(mask_address, std::uint64_t{1});
    require(skins::apply_mesh(r, 1, 2, true), "prepare newer mask fixture");
    Fixture::store(mask_address, std::uint64_t{0x10});
    require(skins::restore_mesh(r) && *reinterpret_cast<std::uint64_t*>(mask_address) == 0x10, "newer engine mask is preserved");
    Fixture::store(mask_address, std::uint64_t{1});
    require(skins::apply_mesh(r, 1, 2, true), "prepare mesh read retry fixture");
    f.fail_read = mask_address; f.fail_reads = 1;
    require(!skins::restore_mesh(r) && r.mesh_touched && skins::restore_mesh(r), "unavailable mesh read retains restoration state for retry");
    reject_mesh_write = true;
    require(!skins::apply_mesh(r, 1, 2, true) && r.mesh_touched && skins::restore_mesh(r), "failed native mesh update is detected without losing original mask");
    reject_mesh_write = false;
    require(skins::apply_mesh(r, 1, 2, true), "prepare replaced node fixture");
    Fixture::store(f.entity + dump_offsets::scene_node, std::uintptr_t{0});
    require(skins::restore_mesh(r) && *reinterpret_cast<std::uint64_t*>(mask_address) == 2, "replaced scene node does not receive stale mask restoration");
}
void choice_roundtrip() {
    Fixture f; auto r = f.record(false); r.applied_model = f.model;
    require(skins::apply_choice(r, 0, 1) && r.choice_original == 0, "body choice application captures original entry");
    require(skins::restore_choice(r) && *reinterpret_cast<std::uint32_t*>(f.node + 0x800) == 0, "body choice rollback restores existing engine entry");
    require(!skins::apply_choice(r, 1, 1) && !r.choice_touched, "missing body entry is rejected without resizing engine vector");
    require(skins::apply_choice(r, 0, 1), "prepare newer body choice fixture");
    Fixture::store(f.node + 0x800, std::uint32_t{2});
    require(skins::restore_choice(r) && *reinterpret_cast<std::uint32_t*>(f.node + 0x800) == 2, "newer engine body choice is preserved");
    Fixture::store(f.node + 0x800, std::uint32_t{0});
    require(skins::apply_choice(r, 0, 1), "prepare body choice read retry");
    f.fail_read = f.node + 0x800; f.fail_reads = 1;
    require(!skins::restore_choice(r) && r.choice_touched && skins::restore_choice(r), "unreadable choice preserves restoration state until retry");
}
void hud_roundtrip() {
    Fixture f;
    struct Pages {
        std::uintptr_t arms = Fixture::reserve(0x2000), arm_node = Fixture::reserve(0x1000);
        std::uintptr_t hud = Fixture::reserve(0x2000), hud_node = Fixture::reserve(0x1000);
        ~Pages() { for (auto p : {arms, arm_node, hud, hud_node}) VirtualFree(reinterpret_cast<void*>(p), 0, MEM_RELEASE); }
    } p;
    constexpr auto arms_handle = (7u << 15) | 2u, hud_handle = (7u << 15) | 3u;
    for (auto [entity, handle] : {std::pair{p.arms, arms_handle}, std::pair{p.hud, hud_handle}}) {
        const auto identity = f.page + (handle & 0x1ff) * verified_layout::identity_stride;
        Fixture::store(identity, entity); Fixture::store(identity + verified_layout::identity_handle, handle);
        Fixture::store(entity + dump_offsets::identity, identity);
    }
    Fixture::store(f.entity + dump_offsets::hud_arms, arms_handle);
    Fixture::store(p.arms, skins::client_base + skin_native::hud_arms_vtable);
    Fixture::store(p.arms + dump_offsets::scene_node, p.arm_node);
    Fixture::store(p.arm_node + dump_offsets::scene_owner, p.arms);
    Fixture::store(p.arm_node + dump_offsets::scene_child, p.hud_node);
    Fixture::store(p.hud, skins::client_base + skin_native::hud_weapon_vtable);
    Fixture::store(p.hud + dump_offsets::scene_node, p.hud_node);
    Fixture::store(p.hud + dump_offsets::owner_handle, Fixture::handle);
    Fixture::store(p.hud_node, skins::client_base + skin_native::networked_skeleton_vtable);
    Fixture::store(p.hud_node + dump_offsets::scene_owner, p.hud);
    Fixture::store(p.hud_node + dump_offsets::scene_parent, p.arm_node);
    Fixture::store(p.hud_node + dump_offsets::model_state + dump_offsets::model_name, reinterpret_cast<std::uintptr_t>(cosmetics::find_item(61)->model));
    const auto mask = p.hud_node + dump_offsets::model_state + dump_offsets::model_mesh_mask;
    Fixture::store(mask, std::uint64_t{1});
    const auto choices = p.hud_node + dump_offsets::model_state + dump_offsets::model_body_choices;
    Fixture::store(choices, 1); Fixture::store(choices + 8, p.hud_node + 0x800);
    skins::Record source; source.entity = f.entity; source.handle = Fixture::handle; source.target = 61;
    source.applied_model = cosmetics::find_item(61)->model;
    skins::records.push_back(source); legacy_policy = true;
    require(skins::sync_hud(f.entity, f.system) && skins::hud_records.size() == 1, "local arms hierarchy resolves separate HUD weapon via exact source handle");
    require(*reinterpret_cast<std::uint64_t*>(mask) == 2 && *reinterpret_cast<std::uint32_t*>(p.hud_node + 0x800) == 1, "HUD geometry and body choice use source weapon legacy policy");
    require(skins::restore_all(f.system) && skins::hud_records.empty() && *reinterpret_cast<std::uint64_t*>(mask) == 1 && *reinterpret_cast<std::uint32_t*>(p.hud_node + 0x800) == 0, "HUD geometry restores before source transaction teardown");
    skins::records.push_back(source);
    Fixture::store(p.hud + dump_offsets::owner_handle, Fixture::handle + 10);
    require(skins::sync_hud(f.entity, f.system) && skins::hud_records.empty() && *reinterpret_cast<std::uint64_t*>(mask) == 1, "HUD proxy for an unconfigured source is left unchanged");
    Fixture::store(p.hud + dump_offsets::owner_handle, Fixture::handle);
    require(skins::sync_hud(f.entity, f.system), "prepare reused HUD source fixture");
    Fixture::store(p.hud + dump_offsets::owner_handle, Fixture::handle + 10);
    require(skins::sync_hud(f.entity, f.system) && skins::hud_records.empty() && *reinterpret_cast<std::uint64_t*>(mask) == 2, "reused HUD proxy retains newer source geometry during cleanup");
    Fixture::store(p.hud + dump_offsets::owner_handle, Fixture::handle);
    Fixture::store(mask, std::uint64_t{1}); Fixture::store(p.hud_node + 0x800, std::uint32_t{0});
    Fixture::store(p.hud_node + dump_offsets::scene_sibling, p.hud_node);
    require(!skins::sync_hud(f.entity, f.system), "cyclic scene hierarchy is bounded and rejected");
    require(skins::restore_all(f.system) && skins::hud_records.empty(), "HUD geometry remains restorable after traversal failure");
}
}
int main() {
    try {
        normal_roundtrip(); unreadable_and_recycled(); failed_publication(); partial_and_unconfirmed_publication(); failed_restore_retry(); failed_header_restore_retry(); server_changes(); stattrak_filter();
        material_roundtrip(false); material_roundtrip(true);
        mesh_roundtrip();
        choice_roundtrip();
        hud_roundtrip();
        std::cout << "skin transactions PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "skin transactions FAIL: " << error.what() << '\n'; return 1;
    }
}
