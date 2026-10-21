// Reuse the owned entity/native fixtures. Smoke and action functions below are
// local stubs; this executable never attaches to or calls a game process.
#define main aim_runtime_fixture_main
#include "aim_runtime.cpp"
#undef main

namespace {
struct SmokeFixture;
SmokeFixture* smoke_current = nullptr;
bool smoke_stub(const Vec3*,const Vec3*);
void smoke_action_stub(void*,int,float,float);

struct SmokeFixture : Fixture {
    struct Segment { Vec3 start,end; };
    struct Action { unsigned index; float delta; };
    std::vector<Segment> segments;
    std::vector<Action> actions;
    std::array<float,6> contributions{};
    bool blocked=false, raise=false, action_arguments_valid=true;
    SmokeFixture() {
        smoke_current=this;
        aim::smoke_verified=false; aim::through_smoke=smoke_stub;
        aim::ballistics_verified=false;
        aim::actions_verified=true; aim::action_delta=smoke_action_stub;
        aim::owned_actions={}; aim::actions_released=true; aim::action_fault=false; aim::reset_fire_state();
    }
    void verified() { aim::smoke_verified=true; }
    void ignore(bool value) {
        auto settings=aim::requested; settings.profiles[0].ignore_smoke=value; aim::configure(settings);
    }
    void own(unsigned index) {
        auto desired=aim::owned_actions; desired[index]=true; aim::sync_actions(desired);
    }
    bool released() const {
        return aim::actions_released && std::none_of(aim::owned_actions.begin(),aim::owned_actions.end(),[](bool value){return value;});
    }
    ~SmokeFixture() {
        smoke_current=nullptr; aim::smoke_verified=false; aim::through_smoke=nullptr;
        aim::actions_verified=false; aim::action_delta=nullptr;
    }
};

bool smoke_stub(const Vec3* start,const Vec3* end) {
    auto& f=*smoke_current;
    f.events.emplace_back("smoke"); f.segments.push_back({*start,*end});
    if (f.raise) RaiseException(0xe0424001,0,0,nullptr);
    return f.blocked;
}
void smoke_action_stub(void* object,int slot,float delta,float fraction) {
    auto& f=*smoke_current;
    unsigned index=6;
    for (unsigned i=0;i<aim_native::actions.size();++i)
        if (reinterpret_cast<std::uintptr_t>(object)==Fixture::client+aim_native::actions[i]) index=i;
    f.action_arguments_valid &= index<6 && slot==0 && (delta==1.f || delta==-1.f) && fraction==1.f;
    if (index>=6) RaiseException(0xe0424002,0,0,nullptr);
    f.actions.push_back({index,delta});
    f.contributions[index]=std::max(0.f,f.contributions[index]+delta);
}

void smoke_capability_and_gate_tests() {
    { SmokeFixture f; f.blocked=true; f.enable(); const auto s=f.run();
      require(!s.capabilities.smoke && f.segments.empty() && s.has_target && f.calls("set")==1,
          "unverified smoke capability never calls predicate and does not pretend filtering was available"); }
    { SmokeFixture f; f.verified(); const auto s=f.run();
      require(s.state==aim::State::disabled && f.segments.empty() && f.native_calls()==0,
          "disabled aim invokes no smoke or other native predicate"); }
    { SmokeFixture f; f.verified(); f.enable(); aim::menu_open=true; f.run();
      require(f.segments.empty() && f.native_calls()==0,"open menu gates smoke before native calls"); }
    { SmokeFixture f; f.verified(); f.enable(); foreground=false; f.run();
      require(f.segments.empty() && f.native_calls()==0,"foreign foreground gates smoke before native calls"); }
    { SmokeFixture f; f.verified(); f.enable(); f.run(false);
      require(f.segments.empty() && f.native_calls()==0,"inactive game input gates smoke predicate"); }
    { SmokeFixture f; f.verified(); f.enable(); f.bone_point={100,-1000,64}; f.run();
      require(f.segments.empty() && f.calls("set")==0,"out-of-FOV bone is rejected before smoke query"); }
}

void smoke_filter_tests() {
    { SmokeFixture f; f.verified(); f.enable(); const auto s=f.run();
      require(s.capabilities.smoke && s.has_target && s.state==aim::State::tracking && f.calls("set")==1,
          "verified false smoke predicate accepts current target");
      require(!f.segments.empty(),"smoke predicate receives at least one current candidate segment");
      for (const auto& line:f.segments)
          require(close(line.start.x,0) && close(line.start.y,0) && close(line.start.z,64) &&
              close(line.end.x,f.bone_point.x) && close(line.end.y,f.bone_point.y) && close(line.end.z,f.bone_point.z),
              "native smoke ABI receives exact local eye and current world-space candidate bone");
      const auto smoke=std::find(f.events.begin(),f.events.end(),"smoke"), trace=std::find(f.events.begin(),f.events.end(),"trace");
      require(smoke!=f.events.end() && trace!=f.events.end() && smoke<trace,"smoke predicate precedes visibility trace"); }
    { SmokeFixture f; f.verified(); f.enable(); f.blocked=true; const auto s=f.run();
      require(s.capabilities.smoke && !s.has_target && s.state==aim::State::idle && f.calls("smoke")>0 && f.calls("trace")==0 && f.calls("set")==0,
          "smoke-blocked candidates never reach visibility or angle mutation"); }
    { SmokeFixture f; f.verified(); f.enable(); f.blocked=true; f.ignore(false); const auto s=f.run();
      require(s.has_target && f.segments.empty() && f.calls("trace")>0 && f.calls("set")==1,
          "turning ignore-smoke off bypasses native predicate and accepts visible target"); }
    { SmokeFixture f; f.verified(); f.enable(); require(f.run().has_target,"unblocked target is acquired before smoke changes");
      f.blocked=true; f.clear_calls(); const auto s=f.run();
      require(!s.has_target && !s.target_on_screen && aim::pending==0xffffffff && f.calls("set")==0,
          "new smoke removes stale selected target and pending switch state"); }
}

void smoke_exception_and_release_tests() {
    { SmokeFixture f; f.verified(); f.raise=true;
      require(aim::smoke_native({1,2,3},{4,5,6}) && aim::native_fault,
          "native smoke exception fails closed as blocked and latches native fault"); }
    { SmokeFixture f; f.verified(); f.enable(); f.raise=true; const auto s=f.run();
      require(aim::native_fault && !s.supported && s.state==aim::State::error && !s.capabilities.smoke && !s.has_target,
          "smoke native exception publishes unavailable error without target");
      require(f.calls("smoke")==1 && f.calls("trace")==0 && f.calls("set")==0,
          "smoke fault stops later predicates and angle updates immediately");
      f.raise=false; f.clear_calls(); f.run();
      require(f.native_calls()==0,"latched smoke fault blocks later callbacks even after stub stops throwing"); }
    { SmokeFixture f; f.verified(); f.enable(); f.contributions[0]=2.f; f.own(0); f.own(1);
      require(!f.released() && close(f.contributions[0],3) && close(f.contributions[1],1),
          "owned actions are active before smoke fault with separate physical contribution");
      f.raise=true; const auto s=f.run();
      require(s.state==aim::State::error && aim::native_fault && !aim::action_fault && f.released(),
          "smoke fault releases all owned actions through independent healthy action interface");
      require(f.actions.size()==4 && f.actions[2].delta==-1 && f.actions[3].delta==-1 &&
          close(f.contributions[0],2) && close(f.contributions[1],0) && f.action_arguments_valid,
          "required releases occur exactly once and preserve physical action contribution");
      f.run(); require(f.actions.size()==4,"latched smoke error does not repeat action releases"); }
    { SmokeFixture f; f.verified(); f.enable(); f.own(0); f.blocked=true; const auto s=f.run();
      require(!s.has_target && f.released() && f.actions.size()==2 && f.actions.back().delta==-1,
          "ordinary smoke-blocked no-target result also releases existing owned fire"); }
    { SmokeFixture f; f.verified(); f.enable(); f.raise=true;
      aim::input_hook(reinterpret_cast<void*>(Fixture::client+o::csgo_input),0,true);
      require(f.calls("original")==1 && f.calls("smoke")==1 && f.calls("set")==0 && aim::callbacks==0,
          "native input callback preserves original call and balances lifetime after smoke exception"); }
}
}

int main() {
    try {
        smoke_capability_and_gate_tests(); smoke_filter_tests(); smoke_exception_and_release_tests();
        std::ofstream("aim-smoke-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"owned smoke predicate fixtures: native segment ABI, filtering gates, exceptions and balanced action cleanup\"}\n";
        std::cout << "aim smoke PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "aim smoke FAIL: " << error.what() << '\n'; return 1; }
}
