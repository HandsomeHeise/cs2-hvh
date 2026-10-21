#include "server_rules.hpp"
#include "game_data.hpp"
#include "generated/dump_offsets.hpp"
#include "generated/server_rules_native.hpp"
#include "MinHook.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

namespace server_rules {
namespace {
namespace n = server_rules_native;
namespace o = dump_offsets;
using Spread = float (*)(void*);
using Inaccuracy = float (*)(void*,float*,float*);
using Recoil = void (*)(void*,const void*,float,float);
struct Hook { void* address = nullptr; void* original = nullptr; };
std::array<Hook,6> hooks;
std::array<std::atomic<std::uint64_t>,6> calls{};
std::array<std::atomic<float>,3> observed{{-1.f,-1.f,-1.f}};
std::atomic<unsigned> callbacks{0};
std::atomic<bool> closing{false}, ready{false}, spread_active{false}, recoil_active{false};
std::atomic<std::uint64_t> last_authority{0};
std::atomic<std::uintptr_t> checked_service{0},checked_server{0},checked_client{0},checked_channel{0};
std::mutex settings_mutex, status_mutex, lifecycle_mutex;
config::ServerRules requested;
Status published;
game::Sources sources;
game::Module server_module, network_module;
std::uint64_t updates = 0, next_poll = 0, next_retry = 0;

bool pointer(std::uintptr_t p) { return p>=0x10000 && p<=0x7fffffffffffULL; }
template<class T> bool read(std::uintptr_t p,T& value) { return sources.memory.get(p,value); }
void publish(Status s) { std::lock_guard guard(status_mutex); published = std::move(s); }
struct Authority { bool local = false, cheats = false; std::uintptr_t service=0,server=0,client=0,channel=0,cheats_data=0; };
Authority authority() {
    Authority a; std::uintptr_t table=0,name=0; int state=0,signon=0,kind=-1; std::uint8_t background=1,cheats=0; std::uint16_t type=0xffff;
    std::string variable_name;
    if (!read(sources.engine.base+n::server_service,a.service) || !pointer(a.service) || !read(a.service,table) || table!=sources.engine.base+n::server_service_vtable ||
        !read(a.service+0x150,a.server) || !pointer(a.server) || !read(a.server,table) || table!=sources.engine.base+n::game_server_vtable ||
        !read(a.server+0x38,state) || (state!=3 && state!=4) || !read(sources.engine.base+o::network_client,a.client) || !pointer(a.client) ||
        !read(a.client,table) || table!=sources.engine.base+n::network_client_vtable || !read(a.client+o::sign_on,signon) || signon!=6 ||
        !read(a.client+o::background_map,background) || background || !read(a.client+0xf0,a.channel) || !pointer(a.channel) ||
        !read(a.channel,table) || table!=network_module.base+n::channel_vtable || !read(a.channel+0x8eb0,kind) || kind!=4) return a;
    a.local = true;
    // Diagnostic observation only. Local weapon rules do not depend on this
    // console variable being present, readable or enabled.
    if (read(sources.engine.base+n::cheats,a.cheats_data) && pointer(a.cheats_data) && read(a.cheats_data,name) &&
        sources.memory.string(name,variable_name) && variable_name=="sv_cheats" && read(a.cheats_data+0x28,type) && type==0 &&
        read(a.cheats_data+0x58,cheats) && cheats==1) a.cheats = true;
    return a;
}
bool permitted_now() {
    if (!ready || closing) return false;
    const auto sampled = last_authority.load(); const auto now=GetTickCount64();
    if (!sampled || now<sampled || now-sampled>100) return false;
    std::uintptr_t service=0,server=0,client=0,channel=0,table=0; int state=0,kind=-1,signon=-1;
    std::uint8_t background=1;
    return read(sources.engine.base+n::server_service,service) && service==checked_service && read(service,table) && table==sources.engine.base+n::server_service_vtable &&
        read(service+0x150,server) && server==checked_server && read(server,table) && table==sources.engine.base+n::game_server_vtable &&
        read(server+0x38,state) && (state==3 || state==4) && read(sources.engine.base+o::network_client,client) && client==checked_client &&
        read(client,table) && table==sources.engine.base+n::network_client_vtable && read(client+o::sign_on,signon) && signon==6 &&
        read(client+o::background_map,background) && !background && read(client+0xf0,channel) && channel==checked_channel &&
        read(channel,table) && table==network_module.base+n::channel_vtable && read(channel+0x8eb0,kind) && kind==4;
}
struct Callback {
    unsigned index;
    explicit Callback(unsigned i):index(i) { ++callbacks; ++calls[i]; }
    ~Callback() { --callbacks; }
};
float spread_hook(unsigned index,void* weapon) {
    Callback callback(index);
    const float original=reinterpret_cast<Spread>(hooks[index].original)(weapon);
    const float value=spread_active && permitted_now() ? 0.f : original;
    observed[0]=value; return value;
}
float inaccuracy_hook(unsigned index,void* weapon,float* movement,float* airborne) {
    Callback callback(index);
    // Original execution preserves optional outputs and native housekeeping.
    const float original=reinterpret_cast<Inaccuracy>(hooks[index].original)(weapon,movement,airborne);
    const float value=spread_active && permitted_now() ? 0.f : original;
    observed[1]=value; return value;
}
void recoil_hook(unsigned index,void* service,const void* time,float angle,float magnitude) {
    Callback callback(index);
    const float value=recoil_active && permitted_now() ? 0.f : magnitude;
    observed[2]=value;
    // This is the actual firearm impulse, not landing/view-punch. Time and angle
    // remain native; old recoil decays normally and no new impulse is added.
    reinterpret_cast<Recoil>(hooks[index].original)(service,time,angle,value);
}
float client_spread_hook(void* weapon) { return spread_hook(0,weapon); }
float client_inaccuracy_hook(void* weapon,float* a,float* b) { return inaccuracy_hook(1,weapon,a,b); }
void client_recoil_hook(void* service,const void* time,float angle,float magnitude) { recoil_hook(2,service,time,angle,magnitude); }
float server_spread_hook(void* weapon) { return spread_hook(3,weapon); }
float server_inaccuracy_hook(void* weapon,float* a,float* b) { return inaccuracy_hook(4,weapon,a,b); }
void server_recoil_hook(void* service,const void* time,float angle,float magnitude) { recoil_hook(5,service,time,angle,magnitude); }
template<std::size_t N> bool code(std::uintptr_t base,std::uint32_t rva,const std::array<unsigned char,N>& expected) {
    std::array<unsigned char,N> bytes{};
    return sources.memory.read(base+rva,bytes.data(),bytes.size()) && bytes==expected;
}
game::Module module(const wchar_t* name) {
    game::Module result; result.base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(name));
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!result.base || !read(result.base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 ||
        !read(result.base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return {};
    result.timestamp=nt.FileHeader.TimeDateStamp; result.image_size=nt.OptionalHeader.SizeOfImage; return result;
}
bool versions() {
    int build=0;
    return sources.client.timestamp==n::client_timestamp && sources.client.image_size==n::client_image_size &&
        sources.engine.timestamp==n::engine_timestamp && sources.engine.image_size==n::engine_image_size &&
        server_module.timestamp==n::server_timestamp && server_module.image_size==n::server_image_size &&
        network_module.timestamp==n::network_timestamp && network_module.image_size==n::network_image_size &&
        read(sources.engine.base+o::build_number,build) && build==static_cast<int>(o::expected_build);
}
bool detach_hooks() {
    for (const auto& hook:hooks) if (hook.address) {
        const auto result=MH_QueueDisableHook(hook.address);
        if (result!=MH_OK && result!=MH_ERROR_DISABLED) return false;
    }
    if (std::any_of(hooks.begin(),hooks.end(),[](const Hook& h){return h.address;}) && MH_ApplyQueued()!=MH_OK) return false;
    const auto deadline=GetTickCount64()+2500;
    while (callbacks && GetTickCount64()<deadline) Sleep(5);
    if (callbacks) return false;
    for (auto& hook:hooks) if (hook.address) { if (MH_RemoveHook(hook.address)!=MH_OK) return false; hook={}; }
    return true;
}
void install() {
    if (ready || closing || std::any_of(hooks.begin(),hooks.end(),[](const Hook& h){return h.address;})) return;
    sources=game::current_sources(); server_module=module(L"server.dll"); network_module=module(L"networksystem.dll"); Status s;
    if (!sources.client.base || !sources.engine.base) { s.detail="CS2 modules are not loaded"; publish(s); return; }
    if (!server_module.base || !network_module.base) { s.state=State::waiting_for_server; s.detail="Waiting for a local listen server"; publish(s); return; }
    if (!versions() || !code(sources.client.base,n::client_spread,n::client_spread_bytes) || !code(sources.client.base,n::client_inaccuracy,n::client_inaccuracy_bytes) ||
        !code(sources.client.base,n::client_recoil,n::client_recoil_bytes) || !code(server_module.base,n::server_spread,n::server_spread_bytes) ||
        !code(server_module.base,n::server_inaccuracy,n::server_inaccuracy_bytes) || !code(server_module.base,n::server_recoil,n::server_recoil_bytes) ||
        !code(sources.engine.base,n::server_active,n::server_active_bytes) || !code(network_module.base,n::loopback,n::loopback_bytes)) {
        s.state=State::profile_mismatch; s.detail="Paired weapon filters differ from the inspected build"; publish(s); return;
    }
    const std::array<std::uintptr_t,6> targets{sources.client.base+n::client_spread,sources.client.base+n::client_inaccuracy,sources.client.base+n::client_recoil,
        server_module.base+n::server_spread,server_module.base+n::server_inaccuracy,server_module.base+n::server_recoil};
    const std::array<void*,6> replacements{reinterpret_cast<void*>(client_spread_hook),reinterpret_cast<void*>(client_inaccuracy_hook),reinterpret_cast<void*>(client_recoil_hook),
        reinterpret_cast<void*>(server_spread_hook),reinterpret_cast<void*>(server_inaccuracy_hook),reinterpret_cast<void*>(server_recoil_hook)};
    bool ok=true;
    for (unsigned i=0;i<hooks.size();++i) {
        auto* address=reinterpret_cast<void*>(targets[i]);
        if (MH_CreateHook(address,replacements[i],&hooks[i].original)!=MH_OK) { ok=false; break; }
        hooks[i].address=address;
    }
    if (ok) {
        for (const auto& hook:hooks) if (MH_QueueEnableHook(hook.address)!=MH_OK) ok=false;
        if (ok) ok=MH_ApplyQueued()==MH_OK;
    }
    if (!ok) {
        // A failed queued enable can have activated an earlier entry. Keep every
        // trampoline until those callbacks have returned, including rollback.
        detach_hooks();
        s.state=State::error; s.detail="Could not install all paired weapon filters"; publish(s); return;
    }
    ready=true; s.supported=s.no_spread_supported=s.no_recoil_supported=true;
    s.state=State::disabled; s.detail="Select rules for your local listen server"; publish(s);
}
}
void configure(const config::ServerRules& value) {
    { std::lock_guard guard(settings_mutex); requested=value; }
    if (!value.no_spread) spread_active=false;
    if (!value.no_recoil) recoil_active=false;
}
void install_runtime() {
    std::lock_guard guard(lifecycle_mutex); closing=false; spread_active=false; recoil_active=false; last_authority=0; next_poll=next_retry=0; updates=0;
    for (auto& count:calls) count=0;
    for (auto& value:observed) value=-1;
    install();
}
void poll_runtime() {
    const auto now=GetTickCount64();
    if (closing || ready || now<next_retry) return;
    std::lock_guard guard(lifecycle_mutex); next_retry=now+1000; install();
}
void update_on_input() {
    if (!ready || closing) return;
    const auto now=GetTickCount64(); if (now<next_poll) return; next_poll=now+16;
    config::ServerRules settings;
    { std::lock_guard guard(settings_mutex); settings=requested; }
    const auto a=authority();
    checked_service=a.service; checked_server=a.server; checked_client=a.client; checked_channel=a.channel;
    last_authority=now;
    spread_active=a.local && settings.no_spread;
    recoil_active=a.local && settings.no_recoil;
    Status s; s.supported=s.no_spread_supported=s.no_recoil_supported=true; s.local_server=a.local; s.cheats=a.cheats; s.updates=++updates;
    s.spread_value=spread_active ? 0.f : 1.f; s.recoil_value=recoil_active ? 0.f : 1.f;
    if (!a.local) { s.state=State::waiting_for_server; s.detail="Requires this process's active loopback listen server"; }
    else if (spread_active || recoil_active) { s.state=State::applied; s.detail="Weapon rules active on your local listen server"; }
    else { s.state=State::ready; s.detail="Original weapon behavior; both rules are off"; }
    publish(s);
}
Status latest() {
    std::lock_guard guard(status_mutex); auto s=published;
    s.observed_spread=observed[0]; s.observed_inaccuracy=observed[1]; s.observed_recoil=observed[2];
    for (unsigned i=0;i<3;++i) s.client_calls+=calls[i].load();
    for (unsigned i=3;i<6;++i) s.server_calls+=calls[i].load();
    return s;
}
const char* state_name(State state) {
    switch (state) {
    case State::disabled:return "Disabled"; case State::waiting_for_server:return "Waiting for local server"; case State::ready:return "Ready";
    case State::applied:return "Applied"; case State::permission_denied:return "Local server required"; case State::error:return "Filter error";
    case State::profile_mismatch:return "Build profile mismatch"; default:return "Runtime unavailable";
    }
}
bool stop_runtime() {
    std::lock_guard guard(lifecycle_mutex);
    closing=true; spread_active=false; recoil_active=false; last_authority=0;
    if (!detach_hooks()) return false;
    ready=false; publish({}); return true;
}
}
