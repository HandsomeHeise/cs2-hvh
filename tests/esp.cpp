// Owned memory/native fixtures only. No game process or live native calls.
#define main esp_reused_runtime_main
#include "aim_runtime.cpp"
#undef main
#include "esp.hpp"
#include <filesystem>

namespace {
int bone_calls=0,visibility_calls=0;
bool shown=true,healthy=true,recycle=false,change_world=false;
std::string missing_bone;
bool invalid_bone=false,move_during_bone=false;
std::uint64_t bone_time=100,bone_cost=0;
std::uint64_t esp_clock() { return bone_time; }
bool esp_bone(std::uintptr_t pawn,const char* name,Vec3& position) {
    ++bone_calls;
    bone_time+=bone_cost;
    if (pawn!=Fixture::enemy || missing_bone==name) return false;
    const auto found=std::find(esp::bone_names.begin(),esp::bone_names.end(),std::string_view(name));
    if (found==esp::bone_names.end()) return false;
    const auto index=static_cast<unsigned>(found-esp::bone_names.begin());
    static const Vec3 offsets[]={{0,0,64},{0,0,57},{0,0,49},{0,0,41},{0,0,33},
        {0,10,51},{0,17,42},{0,20,34},{0,-10,51},{0,-17,42},{0,-20,34},
        {0,7,28},{0,8,15},{0,8,3},{0,-7,28},{0,-8,15},{0,-8,3}};
    position=index ? aim_core::add(Vec3{100,10,0},offsets[index]) : current->bone_point;
    if (invalid_bone && index) position.x=std::numeric_limits<float>::quiet_NaN();
    if (move_during_bone) current->put(Fixture::enemy_node+o::abs_origin,Vec3{104,10,0});
    if (recycle) current->put(current->identity(Fixture::enemy_handle)+v::identity_handle,Fixture::enemy_handle+(1<<15));
    if (change_world) current->put(Fixture::network+o::sign_on,5);
    return true;
}
bool esp_visible(std::uintptr_t query,std::uintptr_t local,std::uintptr_t pawn,const Vec3& eye,const Vec3&) {
    ++visibility_calls;
    require(query==Fixture::query && local==Fixture::pawn && pawn==Fixture::enemy && close(eye.z,64),"ESP native visibility uses current local eye/query/pawn");
    return shown;
}
bool esp_healthy() { return healthy; }
struct EspFixture:Fixture {
    static constexpr std::uintptr_t weapon_data=0x62000000,items=0x62001000;
    config::Esp settings;
    esp::NativeApi api{esp_bone,esp_visible,esp_healthy,esp_clock};
    bool recycle_weapon=false;
    unsigned clip_reads=0;
    static bool detail_read(void* user,std::uintptr_t address,void* out,std::size_t size) {
        auto& f=*static_cast<EspFixture*>(user);
        const bool okay=Fixture::read(user,address,out,size);
        if (address==Fixture::weapon+o::clip && ++f.clip_reads==2 && f.recycle_weapon)
            f.put(f.identity(Fixture::weapon_handle)+v::identity_handle,Fixture::weapon_handle+(1<<15));
        return okay;
    }
    EspFixture() {
        settings.enabled=true; bone_calls=visibility_calls=0; shown=healthy=true; recycle=change_world=false;
        missing_bone.clear(); invalid_bone=move_during_bone=false; bone_time=100; bone_cost=0;
        text(enemy_controller+o::player_name,"Enemy Bot");
        put(enemy+o::weapon_services,service);
        add(weapon_data,0x1000); add(items,0x100); put(weapon+0x388,weapon_data); put(weapon_data+o::max_clip,30);
        put(weapon+o::clip,18); put(weapon+o::reserve_ammo,90); put(enemy+o::armor,75); put(enemy+o::item_services,items);
        aim::sources.memory.function=detail_read;
    }
    esp::Frame collect() { return esp::collect(aim::sources,api,settings,1000); }
    ~EspFixture() { esp::configure({}); esp::reset(); }
};
void collection_tests() {
    { EspFixture f; f.settings.enabled=false; auto s=f.collect(); require(!s.active && s.players.empty() && bone_calls==0,"disabled ESP performs no native collection"); }
    { EspFixture f; auto s=f.collect(); require(s.supported && s.active && s.players.size()==1,"valid world yields one opponent independent of disabled Aim");
      const auto& p=s.players[0]; require(p.health==80 && p.name=="Enemy Bot" && p.weapon=="AK-47" && p.visible && !p.teammate,"ESP contains actual name health active weapon and visibility");
      require(p.handle==Fixture::enemy_handle && p.distance>2.5f && p.distance<2.6f && bone_calls==1,"ESP retains serial and converts distance to meters with one head query"); }
    { EspFixture f; ++aim::sources.client.timestamp; require(!f.collect().supported && bone_calls==0,"foreign module profile rejected before native calls"); }
    { EspFixture f; f.put<std::uint8_t>(Fixture::network+o::background_map,1); require(!f.collect().active && bone_calls==0,"background menu world yields no ESP"); }
    { EspFixture f; f.put<std::uint8_t>(Fixture::enemy+o::team,3); require(f.collect().players.empty() && bone_calls==0,"enemy filter skips teammates before bone work");
      f.settings.enemies_only=false; auto s=f.collect(); require(s.players.size()==1 && s.players[0].teammate,"teammate option includes actual same-team pawn"); }
    { EspFixture f; shown=false; auto s=f.collect(); require(s.players.size()==1 && !s.players[0].visible,"occluded players retained when visible-only is disabled");
      f.settings.visible_only=true; require(f.collect().players.empty(),"visible-only removes occluded players"); }
    { EspFixture f; f.put<std::uint8_t>(Fixture::enemy+o::team,3); f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,1);
      auto s=f.collect(); require(s.players.size()==1 && !s.players[0].teammate && s.players[0].team==3,"FFA enemy-only ESP includes own-team opponents and retains actual team");
      f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,0); require(f.collect().players.empty(),"ESP follows server return to team rules"); }
    { EspFixture f; f.settings.max_distance=1; require(f.collect().players.empty() && bone_calls==0,"distance filter applies before native work"); }
    { EspFixture f; f.put(Fixture::enemy+o::health,0); require(f.collect().players.empty() && bone_calls==0,"dead targets do not produce boxes"); }
    { EspFixture f; f.put<std::uint8_t>(Fixture::enemy_node+o::dormant,1); require(f.collect().players.empty() && bone_calls==0,"dormant targets do not produce boxes"); }
    { EspFixture f; f.put(Fixture::pawn+o::health,0); require(!f.collect().active,"only-alive pauses ESP after local death");
      f.settings.only_alive=false; require(f.collect().players.size()==1,"only-alive off allows a valid dead local pawn as origin"); }
    { EspFixture f; f.put(f.identity(Fixture::enemy_handle)+v::identity_handle,Fixture::enemy_handle+(1<<15));
      require(f.collect().players.empty() && bone_calls==0,"recycled pawn slot is rejected before bone query"); }
    { EspFixture f; recycle=true; require(f.collect().players.empty(),"serial changed during native query invalidates player record"); }
    { EspFixture f; change_world=true; auto s=f.collect(); require(!s.active && s.players.empty(),"world transition during collection invalidates whole frame"); }
    { EspFixture f; healthy=false; require(!f.collect().supported && bone_calls==0,"latched native failure prevents further queries"); }
    { EspFixture f; f.bone_point={100,10,400}; require(f.collect().players.empty(),"implausible head-to-origin displacement rejected"); }
    { EspFixture f; f.text(Fixture::enemy_controller+o::player_name,"Bot\nName"); require(f.collect().players[0].name=="Bot Name","control characters removed from drawn names"); }
    { EspFixture f; f.settings.names=f.settings.weapons=false; const auto s=f.collect(); require(s.players[0].name.empty() && s.players[0].weapon.empty(),"disabled labels avoid their optional reads"); }
    { EspFixture f; f.put(Fixture::network+o::max_clients,65); require(!f.collect().active && bone_calls==0,"invalid player bound rejected"); }
}
void projection_tests() {
    const std::array<float,16> matrix{0,-1,0,0,0,0,16.f/9,-64.f*16/9,1,0,0,-.1f,1,0,0,0};
    esp::Player p; p.feet={500,0,0}; p.head={500,0,64};
    auto b=esp::project_box(matrix,p,1500,845);
    require(b.valid && b.left<750 && b.right>750 && b.top<b.bottom,"perspective box encloses projected head and feet");
    p.feet.x=p.head.x=-500; require(!esp::project_box(matrix,p,1500,845).valid,"behind-camera player never projects a box");
    p.feet={500,10000,0}; p.head={500,10000,64}; require(!esp::project_box(matrix,p,1500,845).valid,"fully offscreen box skipped");
    require(!esp::project_box({},p,1500,845).valid && !esp::project_box(matrix,p,0,845).valid,"invalid matrix and viewport rejected");
    esp::Frame frame; frame.active=true; frame.sampled_at=1000;
    require(esp::fresh(frame,1250) && !esp::fresh(frame,1251) && !esp::fresh(frame,999),"ESP snapshot expiry and clock reversal enforced");
}
void detail_tests() {
    { EspFixture f; f.settings.skeleton=true; const auto s=f.collect();
      require(s.players.size()==1 && s.players[0].bone_mask==((1u<<17)-1) && bone_calls==17 && s.bone_queries==17 && s.skeleton_players==1,
          "full skeleton queries each named bone once and reuses the existing head sample");
      require(!s.skeleton_limited,"ordinary skeleton collection stays within its deterministic budget"); }
    { EspFixture f; f.settings.skeleton=true; missing_bone="arm_lower_R"; const auto s=f.collect();
      require(s.players.size()==1 && !(s.players[0].bone_mask&(1u<<9)) && (s.players[0].bone_mask&(1u<<10)),
          "missing bone is omitted without discarding independently valid hand or player data");
      const auto& p=s.players[0];
      require(!esp::project_segment(s.matrix,p,8,9,1500,845).valid && !esp::project_segment(s.matrix,p,9,10,1500,845).valid,
          "missing elbow never creates a synthetic upper-arm to hand bridge");
      require(esp::project_segment(s.matrix,p,1,5,1500,845).valid,"an unrelated valid left-arm segment survives a missing right elbow"); }
    { EspFixture f; f.settings.skeleton=true; invalid_bone=true; const auto s=f.collect();
      require(s.players.size()==1 && s.players[0].bone_mask==1 && s.skeleton_players==0,"nonfinite optional bones cannot produce skeleton lines or remove base ESP"); }
    { EspFixture f; f.settings.skeleton=true; bone_cost=500; const auto s=f.collect();
      require(s.skeleton_limited && s.players.size()==1 && bone_calls<=4,"extra skeleton work stops at its time slice while labels and player data remain available"); }
    { EspFixture f; move_during_bone=true; require(f.collect().players.empty(),"pose sampling that moves outside the captured origin is discarded"); }
    { EspFixture f; f.settings.ammo=f.settings.ammo_bar=true; f.settings.weapons=false; const auto p=f.collect().players[0];
      require(p.ammo==18 && p.reserve==90 && p.max_clip==30 && p.weapon.empty() && esp::ammo_text(p)=="18 / 90","ammo count and native magazine capacity are collected independently of weapon labels");
      f.put(Fixture::weapon+o::clip,0); require(esp::ammo_text(f.collect().players[0])=="0 / 90","empty magazine displays an actual zero"); }
    { EspFixture f; f.settings.ammo=true; f.put(Fixture::weapon+o::clip,-1); const auto p=f.collect().players[0];
      require(p.ammo==-1 && esp::ammo_text(p).empty(),"knife or grenade with no magazine never displays fabricated zero ammo"); }
    { EspFixture f; f.settings.ammo=true; f.fail_read=Fixture::weapon+o::reserve_ammo;
      require(esp::ammo_text(f.collect().players[0])=="18","unknown reserve ammo does not masquerade as zero"); }
    { EspFixture f; f.settings.ammo_bar=true; f.fail_read=EspFixture::weapon_data+o::max_clip;
      require(f.collect().players[0].max_clip==-1,"unknown magazine capacity suppresses ratio-based ammo bar"); }
    { EspFixture f; f.settings.ammo=true; f.recycle_weapon=true; const auto p=f.collect().players[0];
      require(p.weapon.empty() && p.ammo==-1 && p.reserve==-1 && !p.reloading,"recycled active weapon invalidates all weapon-associated ESP fields"); }
    { EspFixture f; f.settings.status=true;
      f.put<std::uint8_t>(EspFixture::items+o::has_helmet,1); f.put<std::uint8_t>(EspFixture::items+o::has_defuser,1);
      f.put<std::uint8_t>(Fixture::enemy+o::scoped,1); f.put<std::uint8_t>(Fixture::weapon+o::reloading,1);
      f.put(Fixture::enemy+o::flash_duration,1.f); f.put<std::uint8_t>(Fixture::enemy+o::defusing,1); f.put<std::uint8_t>(Fixture::enemy+o::immunity,1);
      const auto p=f.collect().players[0]; const auto labels=esp::status_labels(p,f.settings);
      require(p.armor==75 && p.helmet && p.kit && p.scoped && p.reloading && p.flashed && p.defusing && p.immune && labels.size()==8,
          "native pawn, item services and active weapon fields produce all English status indicators");
      require(labels[0]=="ARMOR 75" && labels[4]=="RELOADING" && labels[7]=="IMMUNE","status labels use stable English ordering");
      f.settings.equipment=f.settings.armor=f.settings.reloading=false;
      require(esp::status_labels(p,f.settings).size()==4,"individual status toggles filter labels independently");
      f.settings.status=false; require(esp::status_labels(p,f.settings).empty(),"master status switch hides every status label"); }
    { EspFixture f; f.settings.status=true; f.put(Fixture::enemy+o::armor,-100); f.put<std::uint8_t>(Fixture::enemy+o::scoped,2);
      f.put(Fixture::enemy+o::flash_duration,std::numeric_limits<float>::quiet_NaN());
      require(esp::status_labels(f.collect().players[0],f.settings).empty(),"invalid optional armor, bool and flash values never render status claims"); }
    { EspFixture f; f.settings.status=true; f.fail_read=Fixture::enemy+o::item_services;
      const auto p=f.collect().players[0]; require(!p.helmet && !p.kit && p.armor==75,"unavailable equipment data leaves independent armor status intact"); }
    { EspFixture f; auto s=f.collect();
      require(s.players[0].ammo==-1 && s.players[0].armor==-1 && s.players[0].bone_mask==0 && s.bone_queries==1,"disabled detail features avoid their optional collection"); }
    { EspFixture f; f.settings.skeleton=true; auto s=f.collect(); auto p=s.players[0];
      p.bones[9].x=-10; require(!esp::project_segment(s.matrix,p,8,9,1500,845).valid,"near-plane crossing does not connect a behind-camera limb");
      require(!esp::project_segment(s.matrix,p,17,0,1500,845).valid && !esp::project_segment(s.matrix,p,0,1,0,845).valid,"invalid bone indices and viewport sizes are rejected"); }
}
void profile_tests() {
    config::Settings s; s.aim.enabled=true; s.server_rules.no_spread=true; auto& e=s.esp;
    e.enabled=true; e.box_style=2; e.enemies_only=false; e.visible_only=true; e.snaplines=true;
    e.skeleton=e.ammo=e.ammo_bar=e.status=true; e.armor=false; e.equipment=false; e.scoped=false; e.reloading=false;
    e.flashed=false; e.defusing=false; e.immunity=false;
    e.max_distance=250; e.interval_ms=16; e.fill_alpha=.2f; e.team_color={.1f,.2f,.3f,.4f};
    const auto path=std::filesystem::absolute("esp-fixture.ini").wstring();
    require(config::save(path,s),"ESP profile saves atomically"); config::Settings copy;
    require(config::load(path,copy) && copy.esp==e && copy.aim.enabled && copy.server_rules.no_spread,"all ESP settings roundtrip without disturbing Aim or server rules");
    std::ofstream(std::filesystem::path(path),std::ios::trunc)<<"[Menu]\nScale=100\n";
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());
    require(config::load(path,copy) && !copy.esp.enabled && copy.esp.names && !copy.esp.skeleton && !copy.esp.ammo && !copy.esp.status && copy.esp.armor,"legacy profile defaults optional details off and preserves default status filters");
    DeleteFileW(path.c_str());
    e.box_style=99; e.interval_ms=-1; e.max_distance=std::numeric_limits<float>::quiet_NaN(); e.visible_color[0]=99;
    config::sanitize(e); require(e.box_style==2 && e.interval_ms==16 && e.max_distance==150 && e.visible_color[0]==1,"ESP ranges and nonfinite values sanitized");
}
void runtime_tests() {
    { EspFixture f; esp::configure(f.settings); aim::input_hook(reinterpret_cast<void*>(Fixture::client+o::csgo_input),0,true);
      require(esp::latest().players.size()==1 && aim::latest().state==aim::State::disabled && f.calls("original")==1,
          "verified input callback samples ESP even when Aim is off and preserves the native callback"); }
    { EspFixture f; esp::configure(f.settings); esp::on_input(aim::sources,f.api,true,false,true); require(esp::latest().players.size()==1,"runtime collection publishes current players without Aim enabled");
      esp::on_input(aim::sources,f.api,true,true,true); require(!esp::latest().active,"menu pause clears published ESP immediately");
      esp::on_input(aim::sources,f.api,false,false,true); require(!esp::latest().active,"inactive input clears published ESP"); }
    { EspFixture f; esp::configure(f.settings); esp::on_input(aim::sources,f.api,true,false,true);
      f.put(Fixture::network+o::sign_on,5); require(!esp::latest().active && esp::latest().players.empty(),"render-side world validation suppresses stale players before next input sample"); }
}
}
int main() {
    try { collection_tests(); projection_tests(); detail_tests(); profile_tests(); runtime_tests();
        std::ofstream("esp-tests.json")<<"{\"passed\":true,\"checks\":"<<checks<<",\"scope\":\"owned entity/native fixtures, ESP filtering, world/serial lifecycle, projection, profiles and runtime pause\"}\n";
        std::cout<<"ESP PASS: "<<checks<<" checks\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<"ESP FAIL: "<<e.what()<<'\n'; return 1; }
}
