// Same-TU fixtures exercise private runtime gates using owned byte buffers and
// fake native methods. No game process, hooks, desktop input or game memory is used.
#include <windows.h>
#include <cstdint>

HWND fixture_foreground();
DWORD fixture_window_process(HWND, LPDWORD);
SHORT fixture_key_state(int);
ULONGLONG fixture_time();
#define GetForegroundWindow fixture_foreground
#define GetWindowThreadProcessId fixture_window_process
#define GetAsyncKeyState fixture_key_state
#define GetTickCount64 fixture_time
#include "../src/aim.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetAsyncKeyState
#undef GetTickCount64

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using aim_core::Vec3;
namespace o = dump_offsets;
namespace v = verified_layout;
unsigned checks = 0;
bool foreground = true, foreign_process = false, key_down = false;
ULONGLONG clock_value = 5000;
void require(bool ok, const char* text) { ++checks; if (!ok) throw std::runtime_error(text); }
bool close(float a, float b) { return std::isfinite(a) && std::abs(a-b) < .001f; }
struct Block { std::uintptr_t address; std::vector<unsigned char> bytes; };
enum class Fault { none, get, bone_index, bone_position, trace, spread, inaccuracy, punch, set, ready };
struct Fixture;
Fixture* current = nullptr;
void angles_get(void*, int, Vec3*);
void angles_set(void*, int, Vec3*);
int bone_index_stub(void*, const char*);
void bone_position_stub(void*, int, Vec3*, Vec3*);
void trace_stub(void*, const Vec3*, const Vec3*, void*, std::uint64_t, void*);
float spread_stub(void*);
float inaccuracy_stub(void*, float*, float*);
Vec3* punch_stub(void*, Vec3*, bool);
bool primary_ready_stub(void*);
void original_stub(void*, int, bool);

struct Fixture {
    static constexpr std::uintptr_t client=0x10000000, engine=0x20000000, network=0x30000000,
        system=0x40000000, page=0x50000000, controller=0x60000000, pawn=0x60010000,
        enemy_controller=0x60020000, enemy=0x60030000, weapon=0x60040000, service=0x60050000,
        node=0x60060000, enemy_node=0x60070000, names=0x60080000, query=0x60090000, punch_service=0x600a0000,
        input_queue=0x600c0000, replacement_queue=0x600f0000,
        globals=0x60200000, rules=0x60201000, enemy_rule_data=0x60202000, enemy_rule_name=0x60202100;
    static constexpr std::uint32_t controller_handle=(3<<15)|1, pawn_handle=(4<<15)|65,
        enemy_controller_handle=(5<<15)|2, enemy_handle=(6<<15)|66, weapon_handle=(7<<15)|129;
    std::vector<Block> blocks;
    std::vector<std::string> events;
    unsigned reads=0;
    Fault fault=Fault::none;
    float trace_fraction=1;
    std::uintptr_t trace_hit=0, obsolete_hit=0;
    bool all_solid=false, recycle_enemy=false, swap_local=false, open_menu=false, replace_input_queue=false;
    bool native_arguments_valid=true;
    bool weapon_ready=true;
    bool end_ffa_during_trace=false;
    std::uintptr_t ready_weapon=weapon;
    Vec3 view{}, last_written{}, bone_point{100,10,64}, punch{1,2,0};
    std::uintptr_t fail_read=0;
    void add(std::uintptr_t address, std::size_t size) { blocks.push_back({address,std::vector<unsigned char>(size)}); }
    template<class T> void put(std::uintptr_t address, T value) {
        for (auto& b:blocks) if (address>=b.address && address-b.address<=b.bytes.size() && sizeof(T)<=b.bytes.size()-(address-b.address)) {
            std::memcpy(b.bytes.data()+address-b.address,&value,sizeof(value)); return;
        }
        throw std::runtime_error("fixture write outside owned block");
    }
    void text(std::uintptr_t address, const char* value) { for (std::size_t i=0;i<=std::strlen(value);++i) put(address+i,value[i]); }
    static bool read(void* user,std::uintptr_t address,void* output,std::size_t bytes) {
        auto& f=*static_cast<Fixture*>(user); ++f.reads;
        if (address==f.fail_read) return false;
        for (const auto& b:f.blocks) if (address>=b.address && address-b.address<=b.bytes.size() && bytes<=b.bytes.size()-(address-b.address)) {
            std::memcpy(output,b.bytes.data()+address-b.address,bytes); return true;
        }
        return false;
    }
    void entity(std::uint32_t handle,std::uintptr_t address) {
        const auto identity=page+(handle&v::index_mask)*v::identity_stride;
        put(identity,address); put(identity+v::identity_handle,handle); put(address+o::identity,identity);
    }
    std::uintptr_t identity(std::uint32_t handle) { return page+(handle&v::index_mask)*v::identity_stride; }
    void clear_calls() { events.clear(); reads=0; }
    void queued_input(int count,std::uintptr_t address=input_queue) {
        add(address,static_cast<std::size_t>(std::max(count,1))*0x440);
        put(client+o::csgo_input+0xb50,count); put(client+o::csgo_input+0xb58,address);
        if (count>0) put(address+static_cast<std::uintptr_t>(count)*0x440-0x10,view);
    }
    unsigned calls(const char* name) const { return static_cast<unsigned>(std::count(events.begin(),events.end(),name)); }
    unsigned native_calls() const { return static_cast<unsigned>(events.size())-calls("original"); }
    void event(const char* name,Fault trigger) {
        events.emplace_back(name);
        if (fault==trigger) RaiseException(0xe0421001,0,0,nullptr);
    }
    Fixture() {
        current=this; foreground=true; foreign_process=false; key_down=false; clock_value=5000;
        add(client+o::local_controller,8); add(client+o::local_pawn,8); add(client+o::entity_system,8);
        add(client+o::view_matrix,64); add(client+aim_native::trace_query,8);
        add(client+o::csgo_input,0xb70); add(client+0x223bd00,8);
        add(client+aim_native::global_vars,8); add(globals,0x100);
        add(client+aim_native::teammates_are_enemies,8); add(enemy_rule_data,0x100); add(enemy_rule_name,64);
        put(client+aim_native::teammates_are_enemies,enemy_rule_data); put(enemy_rule_data,enemy_rule_name);
        text(enemy_rule_name,"mp_teammates_are_enemies"); put<std::uint16_t>(enemy_rule_data+0x28,0);
        put<std::uint8_t>(enemy_rule_data+0x58,0);
        add(client+aim_native::game_rules,8); add(rules,8);
        add(client+aim_native::game_rules_vtable,67*8);
        put(client+aim_native::global_vars,globals); put(client+aim_native::game_rules,rules);
        put(rules,client+aim_native::game_rules_vtable);
        put(client+aim_native::game_rules_vtable+66*8,client+aim_native::game_rules_tick);
        add(engine+o::build_number,4); add(engine+o::network_client,8);
        add(network,o::background_map+64); add(system,0x1000); add(page,512*v::identity_stride);
        for (auto address:{controller,enemy_controller}) add(address,0x1000);
        for (auto address:{pawn,enemy,weapon}) add(address,0x4000);
        add(service,0x100); add(punch_service,0x100); add(node,0x200); add(enemy_node,0x200); add(names,512);
        put(client+o::local_controller,controller); put(client+o::local_pawn,pawn); put(client+o::entity_system,system);
        put(client+aim_native::trace_query,query);
        put(client+0x223bd00,client+o::csgo_input);
        put(client+o::csgo_input+0xb50,0); put(client+o::csgo_input+0x688,view);
        put(engine+o::network_client,network); put(engine+o::build_number,o::expected_build);
        put(network+o::sign_on,6); put<std::uint8_t>(network+o::background_map,0); put(network+o::max_clients,2);
        put(network+v::map_short,names); text(names,"de_fixture"); text(names+128,"weapon_ak47"); text(names+256,"cs_player_controller");
        put(system+v::chunk_table,page);
        entity(controller_handle,controller); entity(pawn_handle,pawn); entity(enemy_controller_handle,enemy_controller);
        entity(enemy_handle,enemy); entity(weapon_handle,weapon);
        put(identity(controller_handle)+o::designer_name,names+256); put(identity(enemy_controller_handle)+o::designer_name,names+256);
        put(controller+o::pawn_handle,pawn_handle); put(enemy_controller+o::pawn_handle,enemy_handle);
        text(controller+o::player_name,"Owned Fixture");
        put(pawn+o::health,100); put<std::uint8_t>(pawn+o::team,3); put<std::uint8_t>(pawn+o::life_state,0);
        put(pawn+o::weapon_services,service); put(service+o::active_weapon,weapon_handle);
        put(identity(weapon_handle)+o::designer_name,names+128); put(weapon+o::clip,30);
        put<std::uint16_t>(weapon+o::attribute_manager+o::item+o::item_definition,7);
        put(pawn+o::scene_node,node); put(node+o::scene_owner,pawn); put<std::uint8_t>(node+o::dormant,0);
        put(node+o::abs_origin,Vec3{}); put(pawn+o::view_offset,Vec3{0,0,64}); put(pawn+o::flash_duration,0.f);
        put(pawn+o::velocity,Vec3{});
        put(pawn+o::aim_punch_services,punch_service); put(punch_service+0x38,pawn);
        put(enemy+o::health,80); put<std::uint8_t>(enemy+o::team,2); put<std::uint8_t>(enemy+o::life_state,0);
        put<std::uint8_t>(enemy+o::immunity,0); put<std::uint32_t>(enemy+o::flags,1);
        put(enemy+o::scene_node,enemy_node); put(enemy_node+o::scene_owner,enemy); put<std::uint8_t>(enemy_node+o::dormant,0);
        put(enemy_node+o::abs_origin,Vec3{100,10,0});
        put(client+o::view_matrix,std::array<float,16>{0,-1,0,0, 0,0,16.f/9,-64.f*16/9, 1,0,0,-.1f, 1,0,0,0});
        aim::sources={{read,this},{client,o::client_image_size,o::client_timestamp},{engine,o::engine_image_size,o::engine_timestamp}};
        aim::hook_address=reinterpret_cast<void*>(0x12345000); aim::original_frame=original_stub;
        aim::get_angles=angles_get; aim::set_angles=angles_set;
        aim::bone_index=bone_index_stub; aim::bone_position=bone_position_stub; aim::trace_line=trace_stub;
        aim::get_spread=spread_stub; aim::get_inaccuracy=inaccuracy_stub;
        aim::get_punch=punch_stub;
        aim::primary_ready_verified=true; aim::primary_ready_query=primary_ready_stub; aim::reset_fire_state();
        aim::closing=false; aim::native_fault=false; aim::menu_open=false; aim::callbacks=0; aim::callback_count=0;
        aim::updates=0; aim::pending=0xffffffff; aim::selected_at=0;
        aim::toggle=false; aim::key_previous=false; aim::previous_activation=-1; aim::previous_key=-1; aim::previous_enabled=false;
        aim::published={}; aim::requested={};
    }
    void enable(int activation=2,int delay=0) {
        config::Aim value; value.enabled=true; value.activation=activation; value.profiles[0].switch_delay_ms=delay; value.profiles[0].recoil=false;
        aim::configure(value);
    }
    aim::Status run(bool active=true) {
        aim::update(reinterpret_cast<void*>(client+o::csgo_input),active); return aim::latest();
    }
    ~Fixture() { aim::sources={}; aim::hook_address=nullptr; aim::original_frame=nullptr; current=nullptr; }
};
void angles_get(void* self,int slot,Vec3* output) {
    current->event("get",Fault::get);
    current->native_arguments_valid &= reinterpret_cast<std::uintptr_t>(self)==Fixture::client+o::csgo_input && slot==0;
    *output=current->view;
}
void angles_set(void* self,int slot,Vec3* value) {
    current->event("set",Fault::set);
    current->native_arguments_valid &= reinterpret_cast<std::uintptr_t>(self)==Fixture::client+o::csgo_input && slot==0 && aim_core::finite(*value);
    current->last_written=*value;
}
int bone_index_stub(void* entity,const char*) {
    current->event("bone-index",Fault::bone_index);
    current->native_arguments_valid &= reinterpret_cast<std::uintptr_t>(entity)==Fixture::enemy;
    return 5;
}
void bone_position_stub(void*,int index,Vec3* point,Vec3* angles) {
    current->event("bone-position",Fault::bone_position); current->native_arguments_valid &= index==5;
    *point=current->bone_point; *angles={};
}
void trace_stub(void* query,const Vec3* eye,const Vec3*,void* ignored,std::uint64_t mask,void* output) {
    current->event("trace",Fault::trace);
    current->native_arguments_valid &= reinterpret_cast<std::uintptr_t>(query)==Fixture::query &&
        reinterpret_cast<std::uintptr_t>(ignored)==Fixture::pawn && mask==1 && aim_core::finite(*eye);
    auto* bytes=static_cast<unsigned char*>(output);
    std::memcpy(bytes+0x08,&current->trace_hit,8); // Source-derived CGameTrace hit-entity member.
    std::memcpy(bytes+0x18,&current->obsolete_hit,8); // Deliberately distinct old, incorrect offset.
    std::memcpy(bytes+0xac,&current->trace_fraction,4); bytes[0xbb]=current->all_solid;
    if (current->recycle_enemy) current->put(current->identity(Fixture::enemy_handle)+v::identity_handle,Fixture::enemy_handle+(1<<15));
    if (current->swap_local) current->put(Fixture::client+o::local_pawn,Fixture::enemy);
    if (current->open_menu) aim::menu_open=true;
    if (current->end_ffa_during_trace) current->put<std::uint8_t>(Fixture::enemy_rule_data+0x58,0);
    if (current->replace_input_queue) {
        current->put(Fixture::client+o::csgo_input+0xb50,2);
        current->put(Fixture::client+o::csgo_input+0xb58,Fixture::replacement_queue);
    }
}
float spread_stub(void*) { current->event("spread",Fault::spread); return .001f; }
float inaccuracy_stub(void*,float*,float*) { current->event("inaccuracy",Fault::inaccuracy); return .002f; }
Vec3* punch_stub(void* pawn,Vec3* value,bool enabled) {
    current->event("punch",Fault::punch);
    current->native_arguments_valid &= reinterpret_cast<std::uintptr_t>(pawn)==Fixture::pawn && enabled;
    *value=current->punch; return value;
}
bool primary_ready_stub(void* weapon) {
    current->event("primary-ready",Fault::ready);
    current->native_arguments_valid &= reinterpret_cast<std::uintptr_t>(weapon)==current->ready_weapon;
    return current->weapon_ready;
}
void original_stub(void*,int,bool) { current->events.emplace_back("original"); }

void gate_tests() {
    { Fixture f; f.enable(); auto settings=aim::requested; settings.profiles[0].penetration=true; aim::configure(settings);
      aim::ballistics_verified=false; const auto s=f.run();
      require(!s.has_target && !s.active && f.calls("set")==0,"saved Autowall fails closed when native penetration queries are unavailable"); }
    { Fixture f; const auto s=f.run(); require(s.state==aim::State::disabled && !s.active && !s.has_target && f.native_calls()==0 && f.reads==0,"default OFF touches no native methods or entity memory"); }
    { Fixture f; f.enable(); aim::menu_open=true; const auto s=f.run(); require(s.state==aim::State::idle && f.native_calls()==0 && f.reads==0,"open menu gates before native calls and memory reads"); }
    { Fixture f; f.enable(); foreground=false; const auto s=f.run(); require(s.state==aim::State::idle && f.native_calls()==0 && f.reads==0,"missing foreground window gates input"); }
    { Fixture f; f.enable(); foreign_process=true; const auto s=f.run(); require(s.state==aim::State::idle && f.native_calls()==0 && f.reads==0,"foreign foreground process gates input"); }
    { Fixture f; f.enable(); const auto s=f.run(false); require(s.state==aim::State::idle && f.native_calls()==0 && f.reads==0,"inactive game callback gates input"); }
    { Fixture f; f.enable(); aim::native_fault=true; const auto s=f.run(); require(s.state==aim::State::error && !s.supported && f.native_calls()==0 && f.reads==0,"latched native fault blocks all further calls"); }
    { Fixture f; f.enable(); aim::closing=true; f.run(); require(f.native_calls()==0 && f.reads==0,"shutdown gate runs before reads or native calls"); }
    { Fixture f; f.enable(); ++aim::sources.client.timestamp; const auto s=f.run(); require(s.state==aim::State::profile_mismatch && f.native_calls()==0,"module profile mismatch never calls game interfaces"); }
    { Fixture f; f.enable(); f.put(Fixture::engine+o::build_number,o::expected_build+1); const auto s=f.run(); require(s.state==aim::State::profile_mismatch && f.native_calls()==0,"game build mismatch never calls game interfaces"); }
    { Fixture f; f.enable(); f.put<std::uint16_t>(Fixture::weapon+o::attribute_manager+o::item+o::item_definition,42); const auto s=f.run(); require(s.state==aim::State::idle && f.native_calls()==0 && !s.has_target,"knife is excluded before native calls"); }
    { Fixture f; f.enable(); f.put(Fixture::pawn+o::health,0); const auto s=f.run(); require(s.state==aim::State::waiting_for_player && f.native_calls()==0,"dead local player never reaches native calls"); }
    { Fixture f; f.enable(); f.put<std::uint8_t>(Fixture::pawn+o::team,1); const auto s=f.run(); require(!s.has_target && f.native_calls()==0,"spectator or invalid local team never reaches native calls"); }
    { Fixture f; f.enable(); f.put(Fixture::controller+o::pawn_handle,Fixture::pawn_handle+(1<<15)); const auto s=f.run(); require(s.state==aim::State::waiting_for_player && f.native_calls()==0,"recycled local pawn serial gates native calls"); }
    { Fixture f; f.enable(); f.fail_read=Fixture::client+aim_native::trace_query; const auto s=f.run(); require(s.state==aim::State::error && f.native_calls()==0,"unreadable trace query fails before angle getter"); }
}
void enemy_rule_tests() {
    { Fixture f; f.enable(); f.put<std::uint8_t>(Fixture::enemy+o::team,3);
      require(!f.run().has_target && f.calls("set")==0,"team-based rules exclude own-team players");
      f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,1); f.clear_calls();
      require(f.run().has_target && f.calls("set")==1 && f.native_arguments_valid,"FFA targets own-team opponents through selection and final validation");
      f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,0); f.clear_calls(); ++clock_value;
      require(!f.run().has_target && f.calls("set")==0,"disabling FFA invalidates same-team cached selection immediately"); }
    { Fixture f; f.enable(); f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,1);
      require(f.run().has_target,"FFA retains opposite-team opponents");
      f.put(Fixture::enemy_controller+o::pawn_handle,Fixture::pawn_handle); f.clear_calls();
      require(!f.run().has_target && f.calls("bone-index")==0,"FFA never targets the local pawn"); }
    for (int invalid=0;invalid<4;++invalid) {
      Fixture f; f.enable(); f.put<std::uint8_t>(Fixture::enemy+o::team,3); f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,1);
      if (invalid==0) f.text(Fixture::enemy_rule_name,"other_variable");
      if (invalid==1) f.put<std::uint16_t>(Fixture::enemy_rule_data+0x28,1);
      if (invalid==2) f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,2);
      if (invalid==3) f.fail_read=Fixture::client+aim_native::teammates_are_enemies;
      require(!enemy_rules::read(aim::sources).known && !f.run().has_target && f.calls("set")==0,"invalid rule identity/type/value or unreadable pointer cannot enable friendly targeting");
    }
    for (int invalid=0;invalid<4;++invalid) {
      Fixture f; f.enable(); f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,1); f.put<std::uint8_t>(Fixture::enemy+o::team,3);
      if (invalid==0) f.put(Fixture::enemy+o::health,0);
      if (invalid==1) f.put<std::uint8_t>(Fixture::enemy+o::life_state,1);
      if (invalid==2) f.put<std::uint8_t>(Fixture::enemy+o::team,1);
      if (invalid==3) f.put<std::uint8_t>(Fixture::enemy+o::immunity,1);
      require(!f.run().has_target && f.calls("set")==0,"FFA preserves health life spectator and spawn-immunity filters");
    }
    { Fixture f; f.enable(); f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,1); f.put<std::uint8_t>(Fixture::enemy+o::team,3);
      f.end_ffa_during_trace=true; const auto s=f.run();
      require(!s.has_target && f.calls("set")==0,"server rule changed during trace blocks stale same-team angle writes"); }
    { Fixture f; ++aim::sources.client.timestamp;
      require(!enemy_rules::read(aim::sources).known,"foreign client metadata blocks rule reads"); }
}
void native_and_trace_tests() {
    { Fixture f; f.enable(); const auto s=f.run(); require(s.has_target && s.state==aim::State::tracking && f.calls("set")==1 && close(f.last_written.y,5.710593f) && close(f.last_written.x,0) && f.native_arguments_valid,"valid owned target runs real selection and writes expected aim angle");
      require(s.target_index==2 && s.target_health==80 && s.projection_valid && s.target_on_screen && s.sampled_at==clock_value,"published target and projection are derived from current fixture"); }
    { Fixture f; f.trace_fraction=.4f; f.trace_hit=Fixture::enemy;
      require(aim::visible_native(Fixture::query,Fixture::pawn,Fixture::enemy,{0,0,64},f.bone_point),"fractional trace directly hitting target uses hit entity at +0x08");
      f.trace_hit=0; f.obsolete_hit=Fixture::enemy;
      require(!aim::visible_native(Fixture::query,Fixture::pawn,Fixture::enemy,{0,0,64},f.bone_point),"obsolete +0x18 field cannot fake a target hit");
      f.trace_fraction=1;
      require(aim::visible_native(Fixture::query,Fixture::pawn,Fixture::enemy,{0,0,64},f.bone_point),"unobstructed full fraction is visible without an entity hit");
      f.all_solid=true;
      require(!aim::visible_native(Fixture::query,Fixture::pawn,Fixture::enemy,{0,0,64},f.bone_point),"all-solid trace is not visible");
      f.all_solid=false; f.trace_fraction=std::numeric_limits<float>::quiet_NaN();
      require(!aim::visible_native(Fixture::query,Fixture::pawn,Fixture::enemy,{0,0,64},f.bone_point),"NaN trace fraction fails closed");
      f.trace_fraction=1.1f;
      require(!aim::visible_native(Fixture::query,Fixture::pawn,Fixture::enemy,{0,0,64},f.bone_point),"out-of-range trace fraction fails closed"); }
    { Fixture f; f.enable(); f.trace_fraction=.5f; const auto s=f.run(); require(!s.has_target && f.calls("set")==0,"obstruction cannot become a selected visible target"); }
    { Fixture f; f.enable(); f.put(f.identity(Fixture::enemy_handle)+v::identity_handle,Fixture::enemy_handle+(1<<15)); const auto s=f.run(); require(!s.has_target && f.calls("bone-index")==0 && f.calls("trace")==0 && f.calls("set")==0,"recycled enemy serial rejects skeleton and trace calls"); }
    { Fixture f; f.enable(); f.put(f.identity(Fixture::enemy_controller_handle)+o::designer_name,Fixture::names+128); const auto s=f.run();
      require(!s.has_target && f.calls("bone-index")==0 && f.calls("set")==0,"non-controller designer class cannot be interpreted as a target controller"); }
    { Fixture f; f.enable(); f.recycle_enemy=true; const auto s=f.run(); require(!s.has_target && f.calls("set")==0,"target recycled during trace is discarded before angle write"); }
    { Fixture f; f.enable(); f.swap_local=true; const auto s=f.run(); require(s.state==aim::State::idle && !s.has_target && !s.active && aim::pending==0xffffffff && f.calls("set")==0,"local pawn swap during trace clears target and prevents stale-context angle write"); }
    { Fixture f; f.enable(); f.open_menu=true; const auto s=f.run(); require(s.state==aim::State::idle && !s.has_target && !s.active && f.calls("set")==0,"menu opening during target evaluation clears target and prevents angle write"); }
    for (const auto fault:{Fault::get,Fault::bone_index,Fault::bone_position,Fault::trace,Fault::set}) {
        Fixture f; f.enable(); f.fault=fault; const auto s=f.run();
        require(aim::native_fault && s.state==aim::State::error && !s.supported && !s.has_target && !s.active && !s.capabilities.view_angles,"native exception is latched and clears published capabilities and target");
        require((fault!=Fault::bone_index || f.calls("bone-index")==1) && (fault!=Fault::bone_position || f.calls("bone-position")==1) &&
            (fault!=Fault::trace || f.calls("trace")==1) && (fault==Fault::set || f.calls("set")==0),"no further native calls follow a failed native method");
        f.clear_calls(); f.fault=Fault::none; f.run(); require(f.native_calls()==0,"native fault remains latched across later callbacks");
    }
    { Fixture f; f.enable(); f.fault=Fault::spread; const auto s=f.run();
      require(f.calls("spread")==0 ? f.calls("set")==1 : aim::native_fault && s.state==aim::State::error && f.calls("inaccuracy")==0 && f.calls("set")==0,
          "unused accuracy probe is avoided or a native accuracy fault stops subsequent calls"); }
    { Fixture f; f.enable(); auto settings=aim::requested; settings.profiles[0].recoil=true; aim::configure(settings); const auto s=f.run();
      require(s.state==aim::State::tracking && f.calls("punch")==1 && f.calls("set")==1 && close(f.last_written.x,-1) && close(f.last_written.y,3.710593f) && f.native_arguments_valid,
          "verified recoil owner enables native punch compensation before angle write"); }
    { Fixture f; f.enable(); auto settings=aim::requested; settings.profiles[0].recoil=true; aim::configure(settings); f.put(Fixture::punch_service+0x38,Fixture::enemy); const auto s=f.run();
      require(s.state==aim::State::error && f.calls("punch")==0 && f.calls("set")==0,"foreign recoil service owner is rejected before native punch getter"); }
    { Fixture f; f.enable(); auto settings=aim::requested; settings.profiles[0].recoil=true; aim::configure(settings); f.punch.x=std::numeric_limits<float>::quiet_NaN(); const auto s=f.run();
      require(s.state==aim::State::error && f.calls("punch")==1 && f.calls("set")==0,"non-finite recoil result leaves view unchanged"); }
    { Fixture f; f.enable(); auto settings=aim::requested; settings.profiles[0].recoil=true; aim::configure(settings); f.fault=Fault::punch; const auto s=f.run();
      require(aim::native_fault && s.state==aim::State::error && f.calls("punch")==1 && f.calls("set")==0,"native recoil getter fault stops view update"); }
}
void activation_and_stale_tests() {
    { Fixture f; f.enable(); auto settings=aim::requested; settings.profiles[0].fov=4;
      settings.profiles[3].fov=10; settings.profiles[3].switch_delay_ms=0; settings.profiles[3].recoil=false;
      aim::configure(settings); auto s=f.run();
      require(!s.has_target && f.calls("set")==0,"firearm inherits narrow global FOV until group override is enabled");
      settings.profiles[3].override_global=true; aim::configure(settings); f.clear_calls(); s=f.run();
      require(s.has_target && s.group==3 && s.fov_degrees==10 && f.calls("set")==1,"runtime selection honors the active weapon-group override");
      settings.per_weapon=false; aim::configure(settings); f.clear_calls(); s=f.run();
      require(!s.has_target && s.fov_degrees==4 && f.calls("set")==0,"global profile mode bypasses preserved weapon override"); }
    { Fixture f; f.enable(0); f.run(); require(f.native_calls()==0,"hold activation remains idle without key");
      key_down=true; f.run(); require(f.calls("set")==1,"hold activation tracks only while pressed");
      f.clear_calls(); key_down=false; const auto s=f.run(); require(!s.active && !s.has_target && f.native_calls()==0,"release clears active target immediately"); }
    { Fixture f; f.enable(1); f.run(); key_down=true; f.run(); require(f.calls("set")==1,"toggle activation starts on rising key edge");
      f.clear_calls(); key_down=false; f.run(); require(f.calls("set")==1,"toggle remains active after key release");
      f.clear_calls(); key_down=true; f.run(); require(f.native_calls()==0,"second toggle edge disables input");
      aim::menu_open=true; f.run(); aim::menu_open=false; f.clear_calls(); f.run(); require(f.native_calls()==0,"closing menu while key held does not reactivate toggle"); }
    { Fixture f; f.enable(2,100); auto s=f.run(); require(s.has_target && f.calls("set")==0,"new target honors switch delay");
      clock_value+=99; f.run(); require(f.calls("set")==0,"switch delay does not expire early");
      ++clock_value; f.run(); require(f.calls("set")==1,"switch delay expires at configured boundary");
      f.clear_calls(); f.put(f.identity(Fixture::enemy_handle)+v::identity_handle,Fixture::enemy_handle+(1<<15)); s=f.run();
      require(!s.has_target && aim::pending==0xffffffff && f.calls("set")==0,"missing target clears published target and switch state"); }
    { Fixture f; f.enable(2,100); f.run(); clock_value+=200;
      f.put<std::uint16_t>(Fixture::weapon+o::attribute_manager+o::item+o::item_definition,42); f.run();
      require(aim::pending==0xffffffff,"unsupported weapon clears previous target delay state");
      f.put<std::uint16_t>(Fixture::weapon+o::attribute_manager+o::item+o::item_definition,7); f.clear_calls(); f.run();
      require(f.calls("set")==0,"returning to firearm reacquires target with a fresh delay"); }
    { Fixture f; f.enable(2,100); f.run(); ++aim::sources.client.timestamp; f.run();
      require(aim::pending==0xffffffff,"profile mismatch clears stale target delay state"); }
    { Fixture f; f.enable(2,100); f.run(); f.fail_read=Fixture::client+aim_native::trace_query; f.run();
      require(aim::pending==0xffffffff,"unreadable input context clears stale target delay state"); }
    { Fixture f; f.enable(); const auto self=reinterpret_cast<void*>(Fixture::client+o::csgo_input);
      aim::input_hook(self,1,true); require(f.calls("original")==1 && f.native_calls()==0 && aim::callbacks==0,"other input slot still calls original and skips aim");
      f.clear_calls(); aim::input_hook(reinterpret_cast<void*>(0x12345),0,true);
      require(f.calls("original")==1 && f.native_calls()==0 && aim::callbacks==0,"foreign input instance still calls original and skips aim");
      f.clear_calls(); aim::input_hook(self,0,true);
      require(f.calls("original")==1 && f.calls("set")==1 && aim::callbacks==0 && aim::callback_count==3,"verified input slot runs aim and balances callback lifetime"); }
}

void input_context_tests() {
    { Fixture f; f.enable(); std::uintptr_t storage=0;
      require(aim::input_context(Fixture::client+o::csgo_input,storage) && storage==Fixture::client+o::csgo_input+0x688,
          "empty native input queue validates direct finite angle storage"); }
    for (int count:{-1,129}) {
        Fixture f; f.enable(); f.put(Fixture::client+o::csgo_input+0xb50,count); const auto s=f.run();
        require(s.state==aim::State::error && f.native_calls()==0 && !s.has_target,
            "negative and oversized input queue counts fail before native methods");
    }
    { Fixture f; f.enable(); f.fail_read=Fixture::client+0x223bd00; const auto s=f.run();
      require(s.state==aim::State::error && f.native_calls()==0,"unreadable current input global rejects native getters and setters"); }
    { Fixture f; f.enable(); f.put(Fixture::client+0x223bd00,Fixture::client+o::csgo_input+8); const auto s=f.run();
      require(s.state==aim::State::error && f.native_calls()==0,"current native input instance must equal verified callback self"); }
    { Fixture f; f.enable(); aim::update(reinterpret_cast<void*>(Fixture::client+o::csgo_input+8),true); const auto s=aim::latest();
      require(s.state==aim::State::error && f.native_calls()==0,"foreign self is rejected before angle getter even with active fixture player"); }
    { Fixture f; f.enable(); f.fail_read=Fixture::client+o::csgo_input+0xb50; const auto s=f.run();
      require(s.state==aim::State::error && f.native_calls()==0,"unreadable input queue count fails before native methods"); }
    { Fixture f; f.enable(); f.put(Fixture::client+o::csgo_input+0x688,Vec3{0,std::numeric_limits<float>::quiet_NaN(),0}); const auto s=f.run();
      require(s.state==aim::State::error && f.native_calls()==0,"non-finite direct input angles are rejected before getter"); }
    { Fixture f; f.enable(); f.fail_read=Fixture::client+o::csgo_input+0x688; const auto s=f.run();
      require(s.state==aim::State::error && f.native_calls()==0,"unreadable direct angle storage fails before getter"); }
    for (std::uintptr_t data:{std::uintptr_t{0},std::uintptr_t{0x1234},std::uintptr_t{0xffffffffffffffffULL},std::uintptr_t{0x61000000}}) {
        Fixture f; f.enable(); f.put(Fixture::client+o::csgo_input+0xb50,1); f.put(Fixture::client+o::csgo_input+0xb58,data); const auto s=f.run();
        require(s.state==aim::State::error && f.native_calls()==0,"invalid or unreadable queued input pointer fails before native getter");
    }
    { Fixture f; f.enable(); f.put(Fixture::client+o::csgo_input+0xb50,1); f.fail_read=Fixture::client+o::csgo_input+0xb58; const auto s=f.run();
      require(s.state==aim::State::error && f.native_calls()==0,"unreadable queued input pointer is rejected"); }
    { Fixture f; f.enable(); f.queued_input(2); const auto last=Fixture::input_queue+2*0x440-0x10;
      f.put(Fixture::input_queue+0x440-0x10,Vec3{0,std::numeric_limits<float>::quiet_NaN(),0});
      std::uintptr_t storage=0;
      require(aim::input_context(Fixture::client+o::csgo_input,storage) && storage==last,
          "two-entry native queue selects the final record at count times 0x440 minus 0x10, not earlier stale angles");
      const auto s=f.run(); require(s.has_target && f.calls("get")==1 && f.calls("set")==1 && f.native_arguments_valid,
          "valid queued angle storage supports native target tracking and view update"); }
    { Fixture f; f.enable(); f.queued_input(128); const auto s=f.run();
      require(s.has_target && f.calls("set")==1,"maximum supported input queue count remains valid"); }
    { Fixture f; f.enable(); f.queued_input(2); f.put(Fixture::input_queue+2*0x440-0x10,Vec3{std::numeric_limits<float>::infinity(),0,0}); const auto s=f.run();
      require(s.state==aim::State::error && f.native_calls()==0,"non-finite final queued angle storage blocks all native calls"); }
    { Fixture f; f.enable(); f.queued_input(2); f.fail_read=Fixture::input_queue+2*0x440-0x10; const auto s=f.run();
      require(s.state==aim::State::error && f.native_calls()==0,"unreadable final queued angle storage blocks native calls"); }
    { Fixture f; f.enable(); f.queued_input(2); f.add(Fixture::replacement_queue,2*0x440);
      f.put(Fixture::replacement_queue+2*0x440-0x10,Vec3{}); f.replace_input_queue=true; const auto s=f.run();
      require(f.calls("get")==1 && f.calls("trace")>0 && f.calls("set")==0 && s.state==aim::State::idle && !s.has_target,
          "queue replacement during trace is rejected before setter even when new queued storage is valid");
      require(aim::pending==0xffffffff,"changed input storage clears pending target state"); }
}
}

HWND fixture_foreground() { return foreground ? reinterpret_cast<HWND>(0x1234) : nullptr; }
DWORD fixture_window_process(HWND window,LPDWORD process) { if (!window) return 0; *process=GetCurrentProcessId()+(foreign_process ? 1 : 0); return 1; }
SHORT fixture_key_state(int) { return key_down ? static_cast<SHORT>(0x8000) : 0; }
ULONGLONG fixture_time() { return clock_value; }

int main() {
    try {
        gate_tests(); enemy_rule_tests(); native_and_trace_tests(); activation_and_stale_tests(); input_context_tests();
        std::ofstream("aim-runtime-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"owned memory fixtures, native input gates, trace layout, serial validation, activation and stale target cleanup\"}\n";
        std::cout << "aim runtime PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "aim runtime FAIL: " << error.what() << '\n'; return 1; }
}
