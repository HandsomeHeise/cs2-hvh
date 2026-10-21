// Reuse owned native ballistics buffers, without executing their original main.
// The clock is local and deterministic; no game or engine RNG API is called.
#include <windows.h>
ULONGLONG chance_fixture_clock();
#define GetTickCount64 chance_fixture_clock
#define main aim_ballistics_fixture_main
#include "aim_ballistics.cpp"
#undef main
#undef GetTickCount64
#include <cstdlib>

namespace {
ULONGLONG chance_time=1000, chance_clock_step=0;
unsigned chance_clock_reads=0;
struct ChanceFixture;
ChanceFixture* chance_current=nullptr;
void chance_create_stub(void*,const Vec3*,const Vec3*,void*,int,bool);

struct ChanceFixture : Fixture {
    static constexpr std::uintptr_t maximum_variable=0x50070000, up_variable=0x50070100;
    int item=7;
    float inaccuracy=.02f, spread=.01f;
    Vec3 firing_angles{};
    std::vector<Vec3> ray_directions;
    bool alternate_hits=false;
    ChanceFixture() {
        chance_current=this; chance_time=1000; chance_clock_step=0; chance_clock_reads=0;
        add(weapon+0x1a00,0x100);
        add(client+0x25becb0,8); add(client+0x25becc0,8); add(maximum_variable,0x200);
        put(vdata+0x730,1); put(weapon+0x1a00,0); put(weapon+0x1a28,0.f);
        put(client+0x25becb0,maximum_variable); put(client+0x25becc0,up_variable);
        put<std::uint8_t>(maximum_variable+0x58,0); put<std::uint8_t>(up_variable+0x58,0);
        aim::bullet_create=chance_create_stub;
    }
    aim::ChanceResult chance(float minimum_damage=20,bool penetration=true) {
        return aim::sampled_chance(weapon,item,pawn,shooter_team,target,eye,firing_angles,p,
            inaccuracy,spread,penetration,minimum_damage);
    }
    void clear_sample() { events.clear(); ray_directions.clear(); chance_clock_reads=0; }
    ~ChanceFixture() { chance_current=nullptr; }
};
void chance_create_stub(void* data,const Vec3* eye,const Vec3* delta,void* filter,int maximum,bool append) {
    auto& f=*chance_current;
    f.ray_directions.push_back(aim_core::mul(*delta,1/f.p.range));
    if (f.alternate_hits) f.entities[0]=f.ray_directions.size()%2 ? Fixture::target : 0;
    create_stub(data,eye,delta,filter,maximum,append);
}
bool equal_directions(const std::vector<Vec3>& a,const std::vector<Vec3>& b) {
    if (a.size()!=b.size()) return false;
    for (std::size_t i=0;i<a.size();++i)
        if (a[i].x!=b[i].x || a[i].y!=b[i].y || a[i].z!=b[i].z) return false;
    return true;
}
void chance_completion_tests() {
    { ChanceFixture f; const auto r=f.chance();
      require(r.supported && r.complete && r.percent==100 && r.samples==64,"64 completed native hit rays produce100 percent");
      require(f.calls("construct")==64 && f.calls("create")==64 && f.calls("penetrate")==64 && f.calls("destruct")==64 && f.arguments_valid,
          "every sampled ray uses the recovered native ABI and destroys one opaque trace"); }
    { ChanceFixture f; f.interval_count=0; const auto r=f.chance();
      require(r.supported && r.complete && r.percent==0 && r.samples==64 && f.calls("create")==64 && f.calls("destruct")==64,
          "64 completed no-hit rays produce zero percent rather than an unavailable estimate"); }
    { ChanceFixture f; f.alternate_hits=true; const auto r=f.chance();
      require(r.complete && r.percent==50 && r.samples==64 && f.calls("destruct")==64,"percentage counts actual selected-target native hits for each ray"); }
    { ChanceFixture f; const auto threshold=f.chance(100); f.clear_sample(); const auto too_high=f.chance(100.01f);
      require(threshold.complete && threshold.percent==100 && too_high.complete && too_high.percent==0,"minimum damage includes equality and excludes insufficient per-ray damage"); }
    { ChanceFixture f; f.interval(0,20.f,3); const auto blocked=f.chance(20,false); f.clear_sample(); const auto allowed=f.chance(20,true);
      require(blocked.complete && blocked.percent==0 && allowed.complete && allowed.percent==100,"sampled chance obeys penetration permission on every native ray"); }
    { ChanceFixture f; f.inaccuracy=f.spread=0; const auto r=f.chance();
      bool forward=r.complete && f.ray_directions.size()==64;
      for (const auto ray:f.ray_directions) forward &= approximately(ray.x,1) && approximately(ray.y,0) && approximately(ray.z,0);
      require(forward,"zero spread sends all sampled native rays down firing forward"); }
}
void chance_profile_tests() {
    for (int bullets:{-1,0,2,8}) { ChanceFixture f; f.put(Fixture::vdata+0x730,bullets); const auto r=f.chance();
        require(!r.supported && !r.complete && r.percent==-1 && r.samples==0 && f.events.empty(),"actual VData bullet count excludes unsupported multi-pellet and invalid profiles before native calls"); }
    for (int mode:{-1,2}) { ChanceFixture f; f.put(Fixture::weapon+0x1a00,mode); const auto r=f.chance();
        require(!r.supported && !r.complete && f.events.empty(),"unsupported current weapon mode fails before sampling"); }
    for (float recoil:{-1.f,1000.01f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        ChanceFixture f; f.put(Fixture::weapon+0x1a28,recoil); const auto r=f.chance();
        require(!r.supported && !r.complete && f.events.empty(),"invalid or non-finite current recoil index fails before sampling"); }
    for (const auto address:{Fixture::weapon+0x388,Fixture::vdata+0x730,Fixture::weapon+0x1a00,Fixture::weapon+0x1a28,
            Fixture::client+0x25becb0,ChanceFixture::maximum_variable+0x58,Fixture::client+0x25becc0,ChanceFixture::up_variable+0x58}) {
        ChanceFixture f; f.fail_read=address; const auto r=f.chance();
        require(!r.supported && !r.complete && r.samples==0 && f.events.empty(),"each unreadable recovered weapon/debug-cvar input fails before native trace construction"); }
    for (const auto address:{Fixture::client+0x25becb0,Fixture::client+0x25becc0}) {
        ChanceFixture f; f.put(address,std::uintptr_t{0}); const auto r=f.chance();
        require(!r.supported && !r.complete && f.events.empty(),"null current debug-cvar object cannot be treated as disabled"); }
    for (const auto address:{ChanceFixture::maximum_variable+0x58,ChanceFixture::up_variable+0x58}) {
        ChanceFixture f; f.put<std::uint8_t>(address,2); const auto r=f.chance();
        require(!r.supported && !r.complete && f.events.empty(),"out-of-range debug bool is rejected rather than treated as enabled"); }
    { ChanceFixture f; f.inaccuracy=std::numeric_limits<float>::quiet_NaN(); const auto r=f.chance();
      require(!r.complete && r.percent==-1 && r.samples==0 && f.events.empty(),"NaN spread input cannot publish a completed chance estimate"); }
    { ChanceFixture f; f.spread=-1; const auto r=f.chance();
      require(!r.complete && r.percent==-1 && r.samples==0 && f.events.empty(),"negative spread cannot construct a native sampling ray"); }
    for (bool inaccuracy:{false,true}) { ChanceFixture f; (inaccuracy ? f.inaccuracy : f.spread)=10;
        const auto r=f.chance(); require(!r.supported && !r.complete && f.events.empty(),"spread and inaccuracy at native invalid upper bound fail before traces"); }
    for (float minimum:{-1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        ChanceFixture f; const auto r=f.chance(minimum);
        require(!r.supported && !r.complete && r.percent==-1 && f.events.empty(),"invalid minimum damage cannot authorize or publish a chance estimate"); }
    { ChanceFixture f; f.firing_angles.y=std::numeric_limits<float>::infinity(); const auto r=f.chance();
      require(!r.complete && r.percent==-1 && r.samples==0 && f.events.empty(),"invalid firing angle cannot construct a native sampling ray"); }
    { ChanceFixture f; f.item=64; f.put(Fixture::weapon+0x1a00,1); f.inaccuracy=.3f; f.spread=.2f;
      f.put<std::uint8_t>(ChanceFixture::maximum_variable+0x58,1); f.put<std::uint8_t>(ChanceFixture::up_variable+0x58,1);
      const auto r=f.chance(); bool correct=r.complete && f.ray_directions.size()==64;
      for (const auto ray:f.ray_directions) correct &= approximately(ray.x,2/std::sqrt(5.f)) && approximately(ray.y,0) && approximately(ray.z,1/std::sqrt(5.f));
      require(correct,"current debug cvars override Revolver transformed radii and native bullet direction to maximum only-up spread"); }
}
void chance_budget_and_fault_tests() {
    { ChanceFixture f; chance_clock_step=4; const auto r=f.chance();
      require(r.supported && !r.complete && r.percent==-1 && r.samples==4 && f.calls("create")==4 && f.calls("destruct")==4,
          "four millisecond budget expiry returns an incomplete estimate with unknown percentage and balanced cleanup"); }
    { ChanceFixture f; chance_clock_step=1; const auto r=f.chance();
      require(r.supported && !r.complete && r.percent==-1 && r.samples==16 && f.calls("destruct")==16,
          "budget is measured from controlled elapsed time rather than native ray count"); }
    for (const auto fault:{Fault::construct,Fault::filter,Fault::create,Fault::penetrate,Fault::interval,Fault::group,Fault::armor,Fault::destruct}) {
        ChanceFixture f; f.fault=fault; const auto r=f.chance();
        require(aim::native_fault && !r.complete && r.percent==-1 && r.samples==0 && f.calls("construct")==1 &&
            f.calls("destruct")==static_cast<unsigned>(fault==Fault::construct ? 0 : 1),"sampling latches native SEH failure and cleans up exactly one completed constructor");
        require(f.events.back()==(fault==Fault::construct ? "construct" : "destruct"),"failed sampling stops native processing after required destructor cleanup");
        f.fault=Fault::none; f.clear_sample(); const auto later=f.chance();
        require(!later.complete && later.percent==-1 && later.samples==0 && f.events.empty(),"latched fault prevents subsequent chance calls from executing native rays");
    }
    { ChanceFixture f; f.fail_read=reinterpret_cast<std::uintptr_t>(f.intervals.data()); const auto r=f.chance();
      require(!r.complete && r.percent==-1 && r.samples==0 && !aim::native_fault && f.calls("construct")==1 && f.calls("destruct")==1,
          "unreadable native interval keeps chance incomplete with destructor cleanup without inventing a native fault"); }
}
void chance_rng_baseline_tests() {
    ChanceFixture f;
    std::srand(1977); const int expected_first=std::rand(), expected_second=std::rand();
    std::srand(1977); require(std::rand()==expected_first,"process CRT RNG baseline is reproducible before sampling");
    const auto a=f.chance(); const auto rays=f.ray_directions;
    require(a.complete && std::rand()==expected_second,"chance sampling does not consume or reseed the process CRT RNG baseline");
    aim_spread::Rng unrelated(91); for (int i=0;i<137;++i) unrelated.uniform();
    f.clear_sample(); const auto b=f.chance();
    require(b.complete && equal_directions(rays,f.ray_directions),"chance uses a deterministic independent local stream despite unrelated random draws");
    // Native engine RandomSeed/RandomFloat are intentionally absent from this
    // fixture. The CRT baseline and ray determinism do not measure engine RNG state.
}
}

ULONGLONG chance_fixture_clock() { return chance_time + chance_clock_step * chance_clock_reads++; }
int main() {
    try {
        checks=0; chance_completion_tests(); chance_profile_tests(); chance_budget_and_fault_tests(); chance_rng_baseline_tests();
        std::ofstream("aim-chance-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"owned native64-ray sampling, damage gates, weapon/debug profiles, deterministic budget and SEH cleanup\",\"rng_limit\":\"CRT baseline and local determinism tested; engine global RNG API/state unavailable and never called\"}\n";
        std::cout << "aim chance PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "aim chance FAIL: " << error.what() << '\n'; return 1; }
}
