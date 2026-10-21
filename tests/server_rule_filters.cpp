// Reuse the same-TU authority fixture; all native originals below are local
// stubs. No game process, real detour installation or game calls are used.
#define main server_rules_authority_fixture_main
#include "server_rules.cpp"
#undef main
#include <bit>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
unsigned filter_checks = 0;
void filter_check(bool value,const char* message) {
    ++filter_checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool same_float(float a,float b) {
    return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);
}
struct FilterFixture;
FilterFixture* filter_current = nullptr;
float client_spread_original(void*);
float server_spread_original(void*);
float client_inaccuracy_original(void*,float*,float*);
float server_inaccuracy_original(void*,float*,float*);
void client_recoil_original(void*,const void*,float,float);
void server_recoil_original(void*,const void*,float,float);

struct FilterFixture : Fixture {
    struct Record {
        unsigned calls = 0, callbacks_during = 0;
        void* receiver = nullptr;
        const void* time = nullptr;
        float* out_a = nullptr;
        float* out_b = nullptr;
        float angle = 0, magnitude = 0;
    };
    std::array<Record,6> records{};
    std::array<float,2> spread_returns{.125f,.875f};
    std::array<float,2> inaccuracy_returns{.375f,.625f};
    bool throw_original = false, block_original = false, entered = false, release = false;
    std::mutex block_mutex;
    std::condition_variable block_cv;
    static constexpr std::uintptr_t weapon_address = 0x65000000;
    static constexpr std::uintptr_t recoil_address = 0x66000000;
    std::uint64_t shot_time = 0x3f00000000001a91ULL;
    FilterFixture() {
        filter_current=this;
        prime();
        const std::array<void*,6> originals{
            reinterpret_cast<void*>(client_spread_original),reinterpret_cast<void*>(client_inaccuracy_original),reinterpret_cast<void*>(client_recoil_original),
            reinterpret_cast<void*>(server_spread_original),reinterpret_cast<void*>(server_inaccuracy_original),reinterpret_cast<void*>(server_recoil_original)};
        for (unsigned i=0;i<6;++i) server_rules::hooks[i]={nullptr,originals[i]};
    }
    ~FilterFixture() { filter_current=nullptr; }
    Record& record(unsigned i,void* receiver) {
        auto& r=records[i]; ++r.calls; r.receiver=receiver;
        r.callbacks_during=server_rules::callbacks.load();
        if (throw_original) throw std::runtime_error("controlled native-original exception");
        if (block_original) {
            std::unique_lock lock(block_mutex); entered=true; block_cv.notify_all();
            block_cv.wait(lock,[this]{return release;});
        }
        return r;
    }
    float spread(unsigned side) {
        return side ? server_rules::server_spread_hook(reinterpret_cast<void*>(weapon_address))
                    : server_rules::client_spread_hook(reinterpret_cast<void*>(weapon_address));
    }
    float inaccuracy(unsigned side,float* a,float* b) {
        return side ? server_rules::server_inaccuracy_hook(reinterpret_cast<void*>(weapon_address),a,b)
                    : server_rules::client_inaccuracy_hook(reinterpret_cast<void*>(weapon_address),a,b);
    }
    void recoil(unsigned side,float angle=123.25f,float magnitude=7.5f) {
        auto* receiver=reinterpret_cast<void*>(recoil_address);
        if (side) server_rules::server_recoil_hook(receiver,&shot_time,angle,magnitude);
        else server_rules::client_recoil_hook(receiver,&shot_time,angle,magnitude);
    }
    void expect_original_behavior(const char* reason) {
        for (unsigned side=0;side<2;++side) {
            filter_check(same_float(spread(side),spread_returns[side]),reason);
            filter_check(same_float(inaccuracy(side,nullptr,nullptr),inaccuracy_returns[side]),reason);
            recoil(side);
            filter_check(same_float(records[side*3+2].magnitude,7.5f),reason);
        }
    }
    void expect_filtered_behavior(const char* reason) {
        for (unsigned side=0;side<2;++side) {
            filter_check(same_float(spread(side),0.f),reason);
            filter_check(same_float(inaccuracy(side,nullptr,nullptr),0.f),reason);
            recoil(side);
            filter_check(same_float(records[side*3+2].magnitude,0.f),reason);
        }
    }
};

float spread_original(unsigned side,void* receiver) {
    auto& f=*filter_current; f.record(side*3,receiver); return f.spread_returns[side];
}
float inaccuracy_original(unsigned side,void* receiver,float* a,float* b) {
    auto& f=*filter_current; auto& r=f.record(side*3+1,receiver); r.out_a=a; r.out_b=b;
    if (a) *a=13.25f+side;
    if (b) *b=-4.5f-static_cast<float>(side);
    return f.inaccuracy_returns[side];
}
void recoil_original(unsigned side,void* receiver,const void* time,float angle,float magnitude) {
    auto& r=filter_current->record(side*3+2,receiver); r.time=time; r.angle=angle; r.magnitude=magnitude;
}
float client_spread_original(void* p) { return spread_original(0,p); }
float server_spread_original(void* p) { return spread_original(1,p); }
float client_inaccuracy_original(void* p,float* a,float* b) { return inaccuracy_original(0,p,a,b); }
float server_inaccuracy_original(void* p,float* a,float* b) { return inaccuracy_original(1,p,a,b); }
void client_recoil_original(void* p,const void* t,float a,float m) { recoil_original(0,p,t,a,m); }
void server_recoil_original(void* p,const void* t,float a,float m) { recoil_original(1,p,t,a,m); }

void active_filter_contracts() {
    FilterFixture f;
    for (unsigned side=0;side<2;++side) {
        const unsigned base=side*3;
        filter_check(same_float(f.spread(side),0.f),"active permitted spread result is float zero on both modules");
        filter_check(f.records[base].calls==1 && f.records[base].receiver==reinterpret_cast<void*>(f.weapon_address),"spread calls the original once with unchanged weapon");
        float a=-100,b=-200;
        filter_check(same_float(f.inaccuracy(side,&a,&b),0.f),"active permitted inaccuracy result is float zero on both modules");
        const auto& i=f.records[base+1];
        filter_check(i.calls==1 && i.receiver==reinterpret_cast<void*>(f.weapon_address) && i.out_a==&a && i.out_b==&b,"inaccuracy original receives exact receiver and optional output pointers once");
        filter_check(same_float(a,13.25f+side) && same_float(b,-4.5f-static_cast<float>(side)),"inaccuracy preserves both native optional output writes");
        f.recoil(side);
        const auto& r=f.records[base+2];
        filter_check(r.calls==1 && r.receiver==reinterpret_cast<void*>(f.recoil_address) && r.time==&f.shot_time,"recoil original runs once with exact service and native time pointer");
        filter_check(same_float(r.angle,123.25f) && same_float(r.magnitude,0.f) && f.shot_time==0x3f00000000001a91ULL,"recoil preserves angle and time bytes while zeroing only new impulse magnitude");
        for (unsigned j=0;j<3;++j) filter_check(f.records[base+j].callbacks_during==1,"original executes inside one counted native callback");
    }
    const auto status=server_rules::latest();
    filter_check(status.client_calls==3 && status.server_calls==3,"status accounts for all three paired native filters per module");
    filter_check(same_float(status.observed_spread,0.f) && same_float(status.observed_inaccuracy,0.f) && same_float(status.observed_recoil,0.f),"native gain observations report actual forwarded/returned values");
    filter_check(server_rules::callbacks.load()==0,"every native callback drains after originals return");
}

void independent_switches_and_optional_outputs() {
    { FilterFixture f; f.prime(false,true);
      for (unsigned side=0;side<2;++side) {
        filter_check(same_float(f.spread(side),f.spread_returns[side]),"NoRecoil alone preserves native spread");
        filter_check(same_float(f.inaccuracy(side,nullptr,nullptr),f.inaccuracy_returns[side]),"NoRecoil alone preserves native inaccuracy and null outputs");
        f.recoil(side); filter_check(same_float(f.records[side*3+2].magnitude,0.f),"NoRecoil alone suppresses only firing impulse");
      } }
    { FilterFixture f; f.prime(true,false);
      for (unsigned side=0;side<2;++side) {
        filter_check(same_float(f.spread(side),0.f) && same_float(f.inaccuracy(side,nullptr,nullptr),0.f),"NoSpread alone zeroes both independent coefficients");
        f.recoil(side); filter_check(same_float(f.records[side*3+2].magnitude,7.5f),"NoSpread alone preserves recoil magnitude");
      } }
    { FilterFixture f;
      for (unsigned side=0;side<2;++side) {
        float a=0,b=0;
        f.inaccuracy(side,&a,nullptr); filter_check(f.records[side*3+1].out_b==nullptr && same_float(a,13.25f+side),"one optional output remains valid with the other null");
        f.inaccuracy(side,nullptr,&b); filter_check(f.records[side*3+1].out_a==nullptr && same_float(b,-4.5f-static_cast<float>(side)),"second optional output remains valid with first null");
      } }
}

void immediate_gate_fallbacks() {
    { FilterFixture f; config::ServerRules disabled{}; server_rules::configure(disabled); f.expect_original_behavior("disabled settings immediately restore originals without another input poll"); }
    { FilterFixture f; f.put<int>(Fixture::channel+0x8eb0,3); f.expect_original_behavior("remote channel immediately restores original behavior"); }
    { FilterFixture f; f.put<std::uint8_t>(Fixture::cheats+0x58,0); f.expect_filtered_behavior("sv_cheats off preserves all local native filters"); }
    { FilterFixture f; f.put<std::uint8_t>(Fixture::cheats+0x58,0); f.prime(); f.expect_filtered_behavior("local native filters activate with sv_cheats already off"); }
    { FilterFixture f; f.put<std::uintptr_t>(Fixture::engine+n::cheats,0); f.prime(); f.expect_filtered_behavior("missing sv_cheats never blocks local native filters"); }
    { FilterFixture f; f.fail_read=Fixture::engine+n::cheats; f.prime(); f.expect_filtered_behavior("unreadable sv_cheats never blocks local native filters"); }
    { FilterFixture f; f.put<std::uintptr_t>(Fixture::engine+n::cheats,0); f.put<int>(Fixture::channel+0x8eb0,3); f.expect_original_behavior("missing sv_cheats never relaxes strict remote rejection"); }
    { FilterFixture f; clock_value+=101; f.expect_original_behavior("stale authority immediately restores original behavior"); }
    { FilterFixture f; server_rules::last_authority=clock_value+1; f.expect_original_behavior("future authority sample fails closed on clock reversal"); }
    { FilterFixture f; server_rules::closing=true; f.expect_original_behavior("closing immediately forwards original native behavior"); }
    { FilterFixture f; server_rules::ready=false; f.expect_original_behavior("not-ready runtime forwards original behavior"); }
    { FilterFixture f; f.put<std::uintptr_t>(Fixture::service+0x150,Fixture::server+0x80); f.expect_original_behavior("changed server identity immediately revokes cached permission"); }
}

void native_callback_cleanup() {
    { FilterFixture f; f.throw_original=true;
      for (unsigned side=0;side<2;++side) for (unsigned kind=0;kind<3;++kind) {
        bool caught=false;
        try { if (kind==0) f.spread(side); else if (kind==1) f.inaccuracy(side,nullptr,nullptr); else f.recoil(side); }
        catch (const std::runtime_error&) { caught=true; }
        filter_check(caught && server_rules::callbacks.load()==0,"counted callback unwinds when controlled original throws a C++ exception");
      } }
    { FilterFixture f; f.block_original=true; std::atomic<bool> stopped=false; bool stop_result=false;
      std::thread native([&]{f.spread(0);});
      { std::unique_lock lock(f.block_mutex); f.block_cv.wait(lock,[&]{return f.entered;}); }
      filter_check(server_rules::callbacks.load()==1,"blocked native original keeps its callback counted");
      std::thread stopper([&]{stop_result=server_rules::stop_runtime(); stopped=true;});
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while (!server_rules::closing && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
      filter_check(server_rules::closing && !stopped,"shutdown waits while an original native callback is active");
      { std::lock_guard lock(f.block_mutex); f.release=true; } f.block_cv.notify_all();
      native.join(); stopper.join();
      filter_check(stop_result && stopped && server_rules::callbacks.load()==0 && !server_rules::ready,"shutdown completes after the final callback drains");
      filter_check(f.records[0].calls==1,"draining shutdown never repeats the original native call");
    }
}
}

int main() {
    active_filter_contracts(); independent_switches_and_optional_outputs();
    immediate_gate_fallbacks(); native_callback_cleanup();
    std::cout << "Server-rule native filters: " << filter_checks << " checks passed\n";
    return 0;
}
