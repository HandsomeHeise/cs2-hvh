#include "esp.hpp"
#include "generated/dump_offsets.hpp"
#include "generated/weapon_names.hpp"
#include "generated/aim_native.hpp"
#include "verified_layout.hpp"
#include "enemy_rules.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace esp {
namespace {
namespace o=dump_offsets;
namespace v=verified_layout;
std::mutex settings_mutex,frame_mutex;
config::Esp requested;
Frame published;
game::Sources published_sources;
std::uint64_t next_sample=0;
bool pointer(std::uintptr_t p) { return p>=0x10000 && p<=0x7fffffffffffULL; }
std::uint64_t microseconds(const NativeApi& api) {
    if (api.clock_us) return api.clock_us();
    static const auto frequency=[] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER current{};
    if (frequency>0 && QueryPerformanceCounter(&current)) return static_cast<std::uint64_t>(1000000.0*current.QuadPart/frequency);
    return GetTickCount64()*1000;
}
std::uintptr_t indexed(const game::Memory& m,std::uintptr_t system,unsigned index,std::uint32_t* out=nullptr) {
    std::uintptr_t page=0,entity=0,back=0; std::uint32_t handle=0xffffffff;
    if (index>v::index_mask || !m.get(system+v::chunk_table+(index/512)*8,page) || !pointer(page)) return 0;
    const auto identity=page+(index%512)*v::identity_stride;
    if (!m.get(identity,entity) || !pointer(entity) || !m.get(identity+v::identity_handle,handle) ||
        (handle&v::index_mask)!=index || !m.get(entity+o::identity,back) || back!=identity) return 0;
    if (out) *out=handle;
    return entity;
}
std::uintptr_t resolve(const game::Memory& m,std::uintptr_t system,std::uint32_t handle) {
    if (handle==0xffffffff) return 0;
    std::uint32_t actual=0xffffffff;
    const auto entity=indexed(m,system,handle&v::index_mask,&actual);
    return actual==handle ? entity : 0;
}
bool scene(const game::Memory& m,std::uintptr_t pawn,Vec3& position) {
    std::uintptr_t node=0,owner=0; std::uint8_t dormant=1;
    return m.get(pawn+o::scene_node,node) && pointer(node) && m.get(node+o::scene_owner,owner) && owner==pawn &&
        m.get(node+o::dormant,dormant) && !dormant && m.get(node+o::abs_origin,position) && aim_core::finite(position);
}
bool world_matches(const game::Sources& s,const Frame& frame) {
    std::uintptr_t local=0,network=0,system=0; int signon=0; std::uint8_t background=1;
    const auto& m=s.memory;
    return m.get(s.client.base+o::local_pawn,local) && local==frame.local &&
        m.get(s.client.base+o::entity_system,system) && system==frame.system &&
        m.get(s.engine.base+o::network_client,network) && network==frame.network &&
        m.get(network+o::sign_on,signon) && signon==6 && m.get(network+o::background_map,background) && !background;
}
std::string clean_name(std::string value) {
    if (value.size()>64) value.resize(64);
    for (auto& c:value) if (static_cast<unsigned char>(c)<32 || c==127) c=' ';
    return value.empty() ? "Player" : value;
}
void publish(Frame frame,const game::Sources& source) {
    std::lock_guard guard(frame_mutex); published=std::move(frame); published_sources=source;
}
}
bool fresh(const Frame& f,std::uint64_t now) { return f.active && f.sampled_at && now>=f.sampled_at && now-f.sampled_at<=250; }
Box project_box(const std::array<float,16>& matrix,const Player& player,float width,float height) {
    Box b;
    if (!std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0) return b;
    const auto top=aim_core::project(matrix,aim_core::add(player.head,{0,0,7}));
    const auto bottom=aim_core::project(matrix,player.feet);
    if (!top.valid || !bottom.valid) return b;
    const float h=(bottom.y-top.y)*height;
    if (!std::isfinite(h) || h<3 || h>height*4) return b;
    const float center=(top.x+bottom.x)*.5f*width;
    b={true,center-h*.23f,top.y*height,center+h*.23f,bottom.y*height};
    if (b.right<0 || b.left>width || b.bottom<0 || b.top>height) b.valid=false;
    return b;
}
Segment project_segment(const std::array<float,16>& matrix,const Player& player,unsigned first,unsigned second,float width,float height) {
    if (first>=bone_names.size() || second>=bone_names.size() || !std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0 ||
        !(player.bone_mask&(1u<<first)) || !(player.bone_mask&(1u<<second))) return {};
    const auto a=aim_core::project(matrix,player.bones[first]),b=aim_core::project(matrix,player.bones[second]);
    if (!a.valid || !b.valid || !std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y) ||
        aim_core::length(aim_core::sub(player.bones[first],player.bones[second]))>100) return {};
    return {true,a.x*width,a.y*height,b.x*width,b.y*height};
}
std::string ammo_text(const Player& p) {
    if (p.ammo<0) return {};
    return std::to_string(p.ammo)+(p.reserve>=0 ? " / "+std::to_string(p.reserve) : "");
}
std::vector<std::string> status_labels(const Player& p,const config::Esp& e) {
    std::vector<std::string> labels;
    if (!e.status) return labels;
    if (e.armor && p.armor>0) labels.push_back("ARMOR "+std::to_string(p.armor));
    if (e.equipment && p.helmet) labels.emplace_back("HELMET");
    if (e.equipment && p.kit) labels.emplace_back("KIT");
    if (e.scoped && p.scoped) labels.emplace_back("SCOPED");
    if (e.reloading && p.reloading) labels.emplace_back("RELOADING");
    if (e.flashed && p.flashed) labels.emplace_back("FLASHED");
    if (e.defusing && p.defusing) labels.emplace_back("DEFUSING");
    if (e.immunity && p.immune) labels.emplace_back("IMMUNE");
    return labels;
}
Frame collect(const game::Sources& source,const NativeApi& api,const config::Esp& input,std::uint64_t now) {
    config::Esp e=input; config::sanitize(e); Frame frame; frame.sampled_at=now;
    if (!e.enabled) { frame.detail="Player ESP is off"; return frame; }
    const auto local_data=game::collect(source);
    if (!local_data.profile_valid) { frame.detail="ESP build profile is unavailable"; return frame; }
    if (!api.bone || !api.visible || (api.healthy && !api.healthy())) { frame.detail="Native ESP queries are unavailable"; return frame; }
    frame.supported=true;
    if (!local_data.in_game || !local_data.local_valid || (e.only_alive && !local_data.alive) ||
        (local_data.team!=2 && local_data.team!=3)) { frame.detail="Waiting for a local player in an active round"; return frame; }
    const auto& m=source.memory;
    Vec3 origin{},offset{}; std::uintptr_t query=0; int max_clients=0;
    if (!m.get(source.client.base+o::local_pawn,frame.local) || !m.get(source.client.base+o::entity_system,frame.system) ||
        !m.get(source.engine.base+o::network_client,frame.network) || !scene(m,frame.local,origin) ||
        !m.get(frame.local+o::view_offset,offset) || !aim_core::finite(offset) || std::abs(offset.x)>32 || std::abs(offset.y)>32 || offset.z<0 || offset.z>100 ||
        !m.get(frame.network+o::max_clients,max_clients) || max_clients<1 || max_clients>64 ||
        !m.get(source.client.base+aim_native::trace_query,query) || !pointer(query) ||
        !m.get(source.client.base+o::view_matrix,frame.matrix)) { frame.detail="ESP world data is unreadable"; return frame; }
    const auto eye=aim_core::add(origin,offset);
    const auto enemy_rule=enemy_rules::read(source);
    const auto skeleton_start=e.skeleton ? microseconds(api) : 0;
    const unsigned start=e.skeleton ? static_cast<unsigned>((now/e.interval_ms)%max_clients) : 0;
    for (int slot=0;slot<max_clients;++slot) {
        const int index=1+(start+slot)%max_clients;
        std::uint32_t controller_handle=0xffffffff,handle=0xffffffff;
        const auto controller=indexed(m,frame.system,index,&controller_handle);
        std::uintptr_t identity=0,name_pointer=0; std::string designer;
        if (!controller || !m.get(controller+o::identity,identity) || !m.get(identity+o::designer_name,name_pointer) ||
            !m.string(name_pointer,designer) || designer!="cs_player_controller" || !m.get(controller+o::pawn_handle,handle)) continue;
        const auto pawn=resolve(m,frame.system,handle);
        if (!pawn || pawn==frame.local) continue;
        Player player; player.handle=handle; std::uint8_t team=0,life=255;
        if (!m.get(pawn+o::health,player.health) || player.health<=0 || player.health>100000 ||
            !m.get(pawn+o::team,team) || (team!=2 && team!=3) || !m.get(pawn+o::life_state,life) || life || !scene(m,pawn,player.feet)) continue;
        player.team=team; player.teammate=!enemy_rules::enemy(local_data.team,team,enemy_rule);
        player.distance=aim_core::length(aim_core::sub(player.feet,origin))*.0254f;
        if ((e.enemies_only && player.teammate) || !std::isfinite(player.distance) || player.distance>e.max_distance) continue;
        if (api.healthy && !api.healthy()) break;
        ++frame.bone_queries;
        if (!api.bone(pawn,"head_0",player.head)) continue;
        if (!aim_core::finite(player.head) || aim_core::length(aim_core::sub(player.head,player.feet))>160) continue;
        player.visible=api.visible(query,frame.local,pawn,eye,player.head);
        if (e.visible_only && !player.visible) continue;
        if (e.skeleton) {
            player.bones[0]=player.head; player.bone_mask=1;
            for (unsigned bone=1;bone<bone_names.size();++bone) {
                const auto current=microseconds(api);
                // Bound additional bone work to a 2ms slice; rotate controller
                // order next sample so crowded rounds do not starve one player.
                if (current<skeleton_start || current-skeleton_start>=2000) { frame.skeleton_limited=true; break; }
                if (api.healthy && !api.healthy()) break;
                Vec3 point{}; ++frame.bone_queries;
                if (api.bone(pawn,bone_names[bone],point) && aim_core::finite(point) &&
                    aim_core::length(aim_core::sub(point,player.feet))<=160) {
                    player.bones[bone]=point; player.bone_mask|=1u<<bone;
                }
            }
            for (const auto& link:bone_links) if ((player.bone_mask&(1u<<link[0])) && (player.bone_mask&(1u<<link[1]))) {
                ++frame.skeleton_players; break;
            }
        }
        const auto flag=[&](std::uintptr_t address) { std::uint8_t value=0; return m.get(address,value) && value==1; };
        if (e.status) {
            if (e.armor) { int value=0; if (m.get(pawn+o::armor,value) && value>=0 && value<=1000) player.armor=value; }
            if (e.equipment) {
                std::uintptr_t items=0;
                if (m.get(pawn+o::item_services,items) && pointer(items)) {
                    player.helmet=flag(items+o::has_helmet); player.kit=flag(items+o::has_defuser);
                    std::uintptr_t current=0;
                    if (!m.get(pawn+o::item_services,current) || current!=items) player.helmet=player.kit=false;
                }
            }
            if (e.scoped) player.scoped=flag(pawn+o::scoped);
            if (e.defusing) player.defusing=flag(pawn+o::defusing);
            if (e.immunity) player.immune=flag(pawn+o::immunity);
            if (e.flashed) { float flash=0; player.flashed=m.get(pawn+o::flash_duration,flash) && std::isfinite(flash) && flash>.05f && flash<120; }
        }
        if (e.names) { m.string(controller+o::player_name,player.name,128); player.name=clean_name(std::move(player.name)); }
        if (e.weapons || e.ammo || e.ammo_bar || (e.status && e.reloading)) {
            std::uintptr_t services=0; std::uint32_t weapon_handle=0xffffffff; std::uint16_t item=0;
            if (m.get(pawn+o::weapon_services,services) && pointer(services) && m.get(services+o::active_weapon,weapon_handle)) {
                const auto weapon=resolve(m,frame.system,weapon_handle);
                if (weapon) {
                    if (e.weapons && m.get(weapon+o::attribute_manager+o::item+o::item_definition,item))
                        if (const char* label=weapon_names::lookup(item)) player.weapon=label;
                    if (e.ammo || e.ammo_bar) {
                        int value=0;
                        if (m.get(weapon+o::clip,value) && value>=0 && value<=1000) player.ammo=value;
                        if (m.get(weapon+o::reserve_ammo,value) && value>=0 && value<=10000) player.reserve=value;
                        std::uintptr_t data=0;
                        if (m.get(weapon+0x388,data) && pointer(data) && m.get(data+o::max_clip,value) && value>0 && value<=1000) player.max_clip=value;
                    }
                    if (e.status && e.reloading) player.reloading=flag(weapon+o::reloading);
                    std::uint32_t current_handle=0xffffffff;
                    if (!m.get(services+o::active_weapon,current_handle) || current_handle!=weapon_handle || resolve(m,frame.system,weapon_handle)!=weapon) {
                        player.weapon.clear(); player.ammo=player.reserve=player.max_clip=-1; player.reloading=false;
                    }
                }
            }
        }
        std::uint32_t final_handle=0xffffffff; int final_health=0; std::uint8_t final_life=255,final_team=0;
        Vec3 final_origin{};
        if (resolve(m,frame.system,controller_handle)!=controller || !m.get(controller+o::pawn_handle,final_handle) || final_handle!=handle ||
            resolve(m,frame.system,handle)!=pawn || !m.get(pawn+o::health,final_health) || final_health<=0 ||
            !m.get(pawn+o::life_state,final_life) || final_life || !m.get(pawn+o::team,final_team) || final_team!=team ||
            !scene(m,pawn,final_origin) || aim_core::length(aim_core::sub(final_origin,player.feet))>2) continue;
        player.health=final_health;
        frame.players.push_back(std::move(player));
    }
    std::uint8_t final_local_team=0;
    if (!world_matches(source,frame) || enemy_rules::read(source)!=enemy_rule ||
        !m.get(frame.local+o::team,final_local_team) || final_local_team!=local_data.team || (api.healthy && !api.healthy())) {
        frame.players.clear(); frame.detail="ESP context changed during sampling"; return frame;
    }
    frame.active=true; frame.detail="Player ESP is ready";
    return frame;
}
void configure(const config::Esp& value) {
    auto clean=value; config::sanitize(clean);
    std::lock_guard guard(settings_mutex);
    if (requested!=clean || !clean.enabled) {
        std::lock_guard frame_guard(frame_mutex); published={};
        published.detail=clean.enabled ? "Waiting for verified game input" : "Player ESP is off";
    }
    requested=clean;
}
void on_input(const game::Sources& source,const NativeApi& api,bool active,bool menu,bool verified) {
    config::Esp settings; { std::lock_guard guard(settings_mutex); settings=requested; }
    if (!settings.enabled || !active || !verified || (menu && settings.hide_in_menu)) {
        Frame f; f.supported=verified; f.detail=!settings.enabled ? "Player ESP is off" : !verified ? "Native ESP queries are unavailable" : "ESP sampling is paused";
        publish(std::move(f),source); next_sample=0; return;
    }
    const auto now=GetTickCount64(); if (now<next_sample) return; next_sample=now+settings.interval_ms;
    LARGE_INTEGER start{},end{},frequency{}; const bool measured=QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0 && QueryPerformanceCounter(&start);
    auto frame=collect(source,api,settings,now);
    if (measured && QueryPerformanceCounter(&end) && end.QuadPart>=start.QuadPart)
        frame.work_ms=static_cast<float>(1000.0*(end.QuadPart-start.QuadPart)/frequency.QuadPart);
    publish(std::move(frame),source);
}
Frame latest() {
    Frame f; game::Sources source;
    { std::lock_guard guard(frame_mutex); f=published; source=published_sources; }
    if (f.active && (!fresh(f,GetTickCount64()) || !world_matches(source,f) || !source.memory.get(source.client.base+o::view_matrix,f.matrix))) {
        f.active=false; f.players.clear(); f.detail="Waiting for fresh ESP data";
    }
    return f;
}
void reset() { publish({},{}); next_sample=0; }
}
