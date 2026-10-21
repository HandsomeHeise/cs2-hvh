// Owned-memory authority fixtures. No game modules, hooks or input are used.
#include <windows.h>
ULONGLONG fixture_time();
#define GetTickCount64 fixture_time
#include "../src/server_rules.cpp"
#undef GetTickCount64
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace o = dump_offsets;
namespace n = server_rules_native;
unsigned checks=0;
ULONGLONG clock_value=5000;
void require(bool value,const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
struct Block { std::uintptr_t address; std::vector<unsigned char> bytes; };
struct Fixture {
    static constexpr std::uintptr_t client=0x10000000,engine=0x20000000,network=0x30000000,
        service=0x60000000,server=0x60010000,game_client=0x61000000,channel=0x62000000,
        cheats=0x63000000,names=0x64000000;
    std::vector<Block> blocks;
    std::uintptr_t fail_read=0;
    unsigned reads=0;
    void add(std::uintptr_t address,std::size_t size) { blocks.push_back({address,std::vector<unsigned char>(size)}); }
    template<class T> void put(std::uintptr_t address,T value) {
        for (auto& b:blocks) if (address>=b.address && address-b.address<=b.bytes.size() && sizeof(T)<=b.bytes.size()-(address-b.address)) {
            std::memcpy(b.bytes.data()+address-b.address,&value,sizeof(value)); return;
        }
        throw std::runtime_error("fixture write outside owned memory");
    }
    void text(std::uintptr_t address,const char* value) { for (std::size_t i=0;i<=std::strlen(value);++i) put(address+i,value[i]); }
    static bool read(void* user,std::uintptr_t address,void* output,std::size_t bytes) {
        auto& f=*static_cast<Fixture*>(user); ++f.reads;
        if (address==f.fail_read) return false;
        for (const auto& b:f.blocks) if (address>=b.address && address-b.address<=b.bytes.size() && bytes<=b.bytes.size()-(address-b.address)) {
            std::memcpy(output,b.bytes.data()+address-b.address,bytes); return true;
        }
        return false;
    }
    Fixture() {
        clock_value=5000;
        add(engine+n::server_service,8); add(engine+o::network_client,8); add(engine+n::cheats,8);
        add(engine+o::build_number,4); add(service,0x160); add(server,0x300);
        add(game_client,o::background_map+8); add(channel,0x8ed0); add(cheats,0x80); add(names,256);
        put(engine+n::server_service,service); put(service,engine+n::server_service_vtable); put(service+0x150,server);
        put(server,engine+n::game_server_vtable); put(server+0x38,3); put(server+0x2bc,64);
        put(engine+o::network_client,game_client); put(game_client,engine+n::network_client_vtable);
        put(game_client+o::sign_on,6); put<std::uint8_t>(game_client+o::background_map,0); put(game_client+0xf0,channel);
        put(channel,network+n::channel_vtable); put(channel+0x8eb0,4);
        put(engine+n::cheats,cheats); put(cheats,names); text(names,"sv_cheats");
        put<std::uint16_t>(cheats+0x28,0); put<std::uint64_t>(cheats+0x30,0x400082100ULL); put<std::uint8_t>(cheats+0x58,1);
        put(engine+o::build_number,o::expected_build);
        server_rules::sources={{read,this},{client,n::client_image_size,n::client_timestamp},{engine,n::engine_image_size,n::engine_timestamp}};
        server_rules::server_module={0x40000000,n::server_image_size,n::server_timestamp};
        server_rules::network_module={network,n::network_image_size,n::network_timestamp};
        server_rules::ready=true; server_rules::closing=false; server_rules::callbacks=0;
        server_rules::spread_active=false; server_rules::recoil_active=false; server_rules::last_authority=0;
        server_rules::checked_service=server_rules::checked_server=server_rules::checked_client=server_rules::checked_channel=0;
        server_rules::next_poll=server_rules::next_retry=server_rules::updates=0;
        server_rules::requested={}; server_rules::published={};
        for (auto& h:server_rules::hooks) h={};
        for (auto& c:server_rules::calls) c=0;
        for (auto& v:server_rules::observed) v=-1;
    }
    server_rules::Status run() { server_rules::update_on_input(); return server_rules::latest(); }
    void prime(bool no_spread=true,bool no_recoil=true) {
        config::ServerRules settings; settings.no_spread=no_spread; settings.no_recoil=no_recoil;
        server_rules::configure(settings); server_rules::next_poll=0; run();
    }
    ~Fixture() { server_rules::sources={}; server_rules::ready=false; server_rules::last_authority=0; }
};

void authority_tests() {
    { Fixture f; auto a=server_rules::authority(); require(a.local && a.cheats,"valid active loopback authority"); require(server_rules::versions(),"exact module metadata and build accepted"); }
    for (auto field:{Fixture::service,Fixture::server,Fixture::game_client,Fixture::channel}) {
        Fixture f; f.put<std::uintptr_t>(field,0x11111111); require(!server_rules::authority().local,"foreign vtable rejected");
    }
    for (auto field:{Fixture::engine+n::server_service,Fixture::service+0x150,Fixture::engine+o::network_client,Fixture::game_client+0xf0}) {
        Fixture f; f.put<std::uintptr_t>(field,0); require(!server_rules::authority().local,"missing authority object rejected");
    }
    for (int state:{-1,0,1,2,5,9,64}) { Fixture f; f.put(Fixture::server+0x38,state); require(!server_rules::authority().local,"dead/waiting/loading/unknown server states rejected"); }
    { Fixture f; f.put(Fixture::server+0x38,4); require(server_rules::authority().local,"native ss_paused state accepted"); }
    for (int clients:{1,2,64}) { Fixture f; f.put(Fixture::server+0x2bc,clients); require(server_rules::authority().local,"max-clients field never interpreted as server state"); }
    { Fixture f; f.put(Fixture::game_client+o::sign_on,5); require(!server_rules::authority().local,"connecting client rejected"); }
    { Fixture f; f.put<std::uint8_t>(Fixture::game_client+o::background_map,1); require(!server_rules::authority().local,"background client rejected"); }
    for (int kind:{-1,0,1,2,3,5}) { Fixture f; f.put(Fixture::channel+0x8eb0,kind); f.put<std::uint32_t>(Fixture::channel+0x8e98,0x0100007f); require(!server_rules::authority().local,"only in-process loopback, including localhost IP rejected"); }
    for (auto field:{Fixture::engine+n::server_service,Fixture::service,Fixture::service+0x150,Fixture::server,Fixture::server+0x38,
         Fixture::engine+o::network_client,Fixture::game_client,Fixture::game_client+o::sign_on,Fixture::game_client+o::background_map,
         Fixture::game_client+0xf0,Fixture::channel,Fixture::channel+0x8eb0}) {
        Fixture f; f.fail_read=field; require(!server_rules::authority().local,"unreadable local authority fails closed");
    }
    { Fixture f; f.text(Fixture::names,"sv_other"); auto a=server_rules::authority(); require(a.local && !a.cheats,"wrong convar name changes only cheats observation"); }
    { Fixture f; f.put<std::uint16_t>(Fixture::cheats+0x28,1); auto a=server_rules::authority(); require(a.local && !a.cheats,"non-bool variable changes only cheats observation"); }
    for (std::uint8_t enabled:{0,2,255}) { Fixture f; f.put(Fixture::cheats+0x58,enabled); auto a=server_rules::authority(); require(a.local && !a.cheats,"cheats observation accepts only exact enabled bool without gating local authority"); }
    for (auto field:{Fixture::engine+n::cheats,Fixture::cheats,Fixture::names,Fixture::cheats+0x28,Fixture::cheats+0x58}) {
        Fixture f; f.fail_read=field; auto a=server_rules::authority(); require(a.local && !a.cheats,"unreadable cheats observation never blocks local authority");
    }
}
void fast_tests() {
    { Fixture f; require(!server_rules::permitted_now(),"unsampled authority denied"); f.prime(); require(server_rules::permitted_now(),"fresh authority permitted"); clock_value+=100; require(server_rules::permitted_now(),"100ms boundary permitted"); ++clock_value; require(!server_rules::permitted_now(),"expired authority denied"); }
    { Fixture f; f.prime(); --clock_value; require(!server_rules::permitted_now(),"clock reversal denied"); }
    { Fixture f; f.prime(); server_rules::closing=true; require(!server_rules::permitted_now(),"shutdown immediately denied"); }
    { Fixture f; f.prime(); server_rules::ready=false; require(!server_rules::permitted_now(),"uninstalled runtime denied"); }
    for (auto field:{Fixture::engine+n::server_service,Fixture::service+0x150,Fixture::engine+o::network_client,Fixture::game_client+0xf0}) {
        Fixture f; f.prime(); f.put<std::uintptr_t>(field,0x77770000); require(!server_rules::permitted_now(),"authority identity swap immediately denied");
    }
    { Fixture f; f.prime(); f.put<std::uint8_t>(Fixture::cheats+0x58,0); require(server_rules::permitted_now(),"sv_cheats off does not revoke local authority"); }
    { Fixture f; f.prime(); f.put(Fixture::channel+0x8eb0,0); require(!server_rules::permitted_now(),"remote transport immediately denied"); }
    { Fixture f; f.prime(); f.put(Fixture::server+0x38,2); require(!server_rules::permitted_now(),"server loading transition immediately denied"); }
    { Fixture f; f.prime(); f.put(Fixture::server+0x38,0); require(!server_rules::permitted_now(),"server stopped immediately denied"); }
    { Fixture f; f.prime(); f.put(Fixture::server+0x38,4); require(server_rules::permitted_now(),"server paused remains local permitted instance"); }
    for (auto field:{Fixture::service,Fixture::server,Fixture::game_client,Fixture::channel}) {
        Fixture f; f.prime(); f.put<std::uintptr_t>(field,0x11111111); require(!server_rules::permitted_now(),"same-pointer foreign vtable immediately denied");
    }
    { Fixture f; f.prime(); f.put(Fixture::game_client+o::sign_on,1); require(!server_rules::permitted_now(),"disconnect state immediately denied"); }
    { Fixture f; f.prime(); f.put<std::uint8_t>(Fixture::game_client+o::background_map,1); require(!server_rules::permitted_now(),"background transition immediately denied"); }
    { Fixture f; f.prime(); f.put<std::uint16_t>(Fixture::cheats+0x28,7); require(server_rules::permitted_now(),"cheats type replacement does not revoke local authority"); }
    { Fixture f; f.prime(); f.text(Fixture::names,"sv_other"); require(server_rules::permitted_now(),"cheats identity replacement does not revoke local authority"); }
    { Fixture f; f.put<std::uintptr_t>(Fixture::engine+n::cheats,0); f.prime(); require(server_rules::permitted_now(),"absent sv_cheats never blocks sampled local authority"); }
    for (auto field:{Fixture::engine+n::cheats,Fixture::cheats,Fixture::names,Fixture::cheats+0x28,Fixture::cheats+0x58}) {
        Fixture f; f.fail_read=field; f.prime(); require(server_rules::permitted_now(),"unreadable cheats data never blocks sampled local authority");
    }
    { Fixture f; f.prime(); f.fail_read=Fixture::channel+0x8eb0; require(!server_rules::permitted_now(),"fast unreadable field denied"); }
}
void configuration_tests() {
    { Fixture f; auto s=f.run(); require(s.state==server_rules::State::ready && s.local_server && s.cheats,"defaults leave original behavior ready"); require(s.spread_value==1 && s.recoil_value==1,"native gain is one with defaults off"); require(!server_rules::spread_active && !server_rules::recoil_active,"defaults never activate either filter"); }
    { Fixture f; f.prime(true,false); auto s=server_rules::latest(); require(server_rules::spread_active && !server_rules::recoil_active,"spread can activate independently"); require(s.spread_value==0 && s.recoil_value==1 && s.state==server_rules::State::applied,"spread status reports native gain0/1"); }
    { Fixture f; f.prime(false,true); require(!server_rules::spread_active && server_rules::recoil_active,"recoil can activate independently"); auto s=server_rules::latest(); require(s.spread_value==1 && s.recoil_value==0,"recoil status reports native gain1/0"); server_rules::configure({}); require(!server_rules::spread_active && !server_rules::recoil_active,"config off takes effect without next input sample"); }
    { Fixture f; f.prime(); f.put<std::uint8_t>(Fixture::cheats+0x58,0); clock_value+=16; auto s=f.run(); require(s.state==server_rules::State::applied && s.local_server && !s.cheats,"sv_cheats off remains observational while local rules are applied"); require(s.spread_value==0 && s.recoil_value==0,"local native gains stay zero without sv_cheats"); }
    { Fixture f; f.fail_read=Fixture::engine+n::cheats; f.prime(); const auto s=server_rules::latest(); require(s.state==server_rules::State::applied && s.local_server && !s.cheats,"unreadable cheats does not produce a permission error"); }
    { Fixture f; f.put(Fixture::channel+0x8eb0,0); f.prime(); auto s=server_rules::latest(); require(s.state==server_rules::State::waiting_for_server && !s.local_server,"remote status waits for local server"); }
    { Fixture f; f.put<std::uintptr_t>(Fixture::engine+n::cheats,0); f.put(Fixture::channel+0x8eb0,0); f.prime(); require(!server_rules::spread_active && !server_rules::recoil_active && !server_rules::permitted_now(),"absent cheats never relaxes remote transport rejection"); }
    { Fixture f; config::Settings settings; settings.aim.enabled=false; settings.server_rules={true,true}; config::sanitize(settings);
      server_rules::configure(settings.server_rules); f.run(); require(server_rules::spread_active && server_rules::recoil_active,"rules remain independent of disabled aim after profile sanitation");
      settings.aim.enabled=true; settings.aim.profiles[0].recoil=true; settings.server_rules={}; config::sanitize(settings); server_rules::configure(settings.server_rules);
      require(!server_rules::spread_active && !server_rules::recoil_active,"aim recoil setting never enables server weapon rules"); }
    require(std::strcmp(server_rules::state_name(server_rules::State::permission_denied),"Local server required")==0,"compatibility state describes the actual local server requirement");
}
void profile_tests() {
    { Fixture f; ++server_rules::sources.client.timestamp; require(!server_rules::versions(),"foreign client build rejected"); }
    { Fixture f; ++server_rules::sources.engine.image_size; require(!server_rules::versions(),"foreign engine image rejected"); }
    { Fixture f; ++server_rules::server_module.timestamp; require(!server_rules::versions(),"foreign server build rejected"); }
    { Fixture f; ++server_rules::network_module.image_size; require(!server_rules::versions(),"foreign network build rejected"); }
    { Fixture f; f.put(Fixture::engine+o::build_number,14190); require(!server_rules::versions(),"foreign game build rejected"); }
    { Fixture f; f.fail_read=Fixture::engine+o::build_number; require(!server_rules::versions(),"unreadable build rejected"); }
    { Fixture f; f.add(Fixture::client+n::client_spread,n::client_spread_bytes.size());
      for (std::size_t i=0;i<n::client_spread_bytes.size();++i) f.put(Fixture::client+n::client_spread+i,n::client_spread_bytes[i]);
      require(server_rules::code(Fixture::client,n::client_spread,n::client_spread_bytes),"exact native prologue accepted");
      f.put<unsigned char>(Fixture::client+n::client_spread,0xcc); require(!server_rules::code(Fixture::client,n::client_spread,n::client_spread_bytes),"modified native prologue rejected"); }
}
}
ULONGLONG fixture_time() { return clock_value; }
int main() {
    try { authority_tests(); fast_tests(); configuration_tests(); profile_tests(); std::cout<<checks<<" server-rule authority checks passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n'; return 1; }
}
