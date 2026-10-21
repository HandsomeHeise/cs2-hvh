// Reuse the owned entity/native fixtures and their same-TU inclusion of aim.cpp.
// Only the action callback below is added; no process, hooks or desktop input run.
#define main aim_runtime_fixture_main
#include "aim_runtime.cpp"
#undef main

namespace {
struct ActionFixture;
ActionFixture* action_current = nullptr;
void action_stub(void*, int, float, float);
struct ActionFixture : Fixture {
    struct Call { unsigned index; float delta, fraction; };
    std::array<float,6> contributions{};
    std::vector<Call> actions;
    bool arguments_valid = true, raise_before = false, raise_after = false;
    int fault_index = -1;
    static constexpr std::uintptr_t weapon_data = 0x600b0000;
    ActionFixture() {
        action_current = this;
        punch = {}; // Auto-fire validates the actual shooting angle even when recoil correction is off.
        aim::actions_verified = true; aim::action_delta = action_stub;
        aim::owned_actions = {}; aim::actions_released = true;
        aim::action_fault = false; aim::reset_fire_state();
        aim::ballistics_verified = false;
        add(weapon_data,0x1000); put(weapon+0x388,weapon_data);
        put<std::uint8_t>(weapon_data+0x72d,1);
    }
    void desire(std::array<bool,6> desired) { aim::sync_actions(desired); }
    void hold(unsigned index = 0) { std::array<bool,6> desired{}; desired[index]=true; desire(desired); }
    void enable_fire() {
        enable(); auto settings=aim::requested;
        settings.profiles[0].auto_fire=true;
        settings.profiles[0].hitchance=0; // This action lifecycle fixture intentionally has no ballistic sampler.
        aim::configure(settings);
    }
    bool released() const {
        return aim::actions_released && std::none_of(aim::owned_actions.begin(),aim::owned_actions.end(),[](bool held){return held;});
    }
    ~ActionFixture() { action_current=nullptr; aim::actions_verified=false; aim::action_delta=nullptr; }
};
void action_stub(void* object,int slot,float delta,float fraction) {
    auto& f=*action_current;
    unsigned index=6;
    for (unsigned i=0;i<aim_native::actions.size();++i)
        if (reinterpret_cast<std::uintptr_t>(object)==Fixture::client+aim_native::actions[i]) index=i;
    f.arguments_valid &= index<6 && slot==0 && (delta==1.f || delta==-1.f) && fraction==1.f;
    if (index>=6) RaiseException(0xe0423001,0,0,nullptr);
    f.actions.push_back({index,delta,fraction});
    if (f.fault_index==static_cast<int>(index) && f.raise_before) RaiseException(0xe0423002,0,0,nullptr);
    f.contributions[index]=std::max(0.f,f.contributions[index]+delta);
    if (f.fault_index==static_cast<int>(index) && f.raise_after) RaiseException(0xe0423003,0,0,nullptr);
}

void balanced_action_tests() {
    { ActionFixture f; f.hold();
      require(f.actions.size()==1 && f.actions[0].delta==1 && !f.released(),"one owned press creates exactly one native contribution");
      for (int i=0;i<20;++i) f.hold();
      require(f.actions.size()==1 && close(f.contributions[0],1),"held callbacks never accumulate repeated native press references");
      f.desire({}); f.desire({});
      require(f.actions.size()==2 && f.actions.back().delta==-1 && close(f.contributions[0],0) && f.released(),"release is balanced once and repeated empty frames are inert");
      require(f.arguments_valid,"native action ABI uses exact object, slot, delta and fraction"); }
    { ActionFixture f; f.contributions={1,2,.5f,3,1,4}; const auto manual=f.contributions;
      f.desire({true,true,true,true,true,true});
      for (unsigned i=0;i<6;++i) require(close(f.contributions[i],manual[i]+1),"owned contribution is additive to physical contributions");
      f.desire({});
      require(f.contributions==manual && f.actions.size()==12 && f.released(),"all six owned releases preserve physical contributions exactly"); }
    { ActionFixture f; f.hold(1); { aim::ActionFrame frame; }
      require(f.released() && f.actions.back().index==1 && f.actions.back().delta==-1,"default frame scope releases a previously held scope action"); }
    { ActionFixture f; aim::actions_verified=false; f.hold();
      require(f.actions.empty() && f.released(),"unverified action interface never calls a native stub"); }
}

void action_gate_tests() {
    for (unsigned gate=0;gate<7;++gate) {
        ActionFixture f; f.enable(); f.hold();
        switch (gate) {
        case 0: aim::menu_open=true; break;
        case 1: foreground=false; break;
        case 2: foreign_process=true; break;
        case 3: { auto value=aim::requested; value.enabled=false; aim::configure(value); break; }
        case 4: f.put(Fixture::enemy+o::health,0); break;
        case 5: f.put(Fixture::pawn+o::health,0); break;
        case 6: ++aim::sources.client.timestamp; break;
        }
        f.run();
        require(f.actions.size()==2 && f.actions.back().delta==-1 && f.released(),"menu/focus/disabled/no-target/death/profile gates release owned fire");
    }
    { ActionFixture f; f.enable(); f.hold(); f.run(false);
      require(f.released() && f.actions.size()==2,"inactive native game callback releases owned fire"); }
    { ActionFixture f; f.enable(1); f.run(); key_down=true; f.run(); key_down=false; f.run(); f.hold(); key_down=true; f.run();
      require(f.released() && f.actions.size()==2 && !aim::latest().active,"second toggle edge releases owned fire"); }
    { ActionFixture f; f.enable_fire(); f.run(); require(aim::owned_actions[0],"valid aligned visible target creates native auto-fire press");
      f.run(); require(f.actions.size()==1,"full-auto continuation retains one native press");
      f.open_menu=true; f.run();
      require(f.released() && f.actions.size()==2,"menu opening during final target evaluation releases existing fire"); }
    { ActionFixture f; f.enable_fire(); f.trace_fraction=.5f; f.run();
      require(f.released() && f.actions.empty(),"occluded target cannot create automatic fire"); }
}

void action_callback_and_fault_tests() {
    { ActionFixture f; f.desire({true,true,true,true,true,true}); aim::closing=true;
      aim::input_hook(reinterpret_cast<void*>(Fixture::client+o::csgo_input),0,true);
      require(f.released() && f.actions.size()==12 && f.calls("original")==1 && aim::callbacks==0,"closing input callback releases all owned actions before preserving original callback"); }
    { ActionFixture f; f.enable(); f.hold(); f.fault=Fault::trace; f.run();
      require(aim::native_fault && !aim::action_fault && f.released() && f.actions.size()==2,"independent trace fault still releases owned actions through verified action API"); }
    { ActionFixture f; f.hold(); aim::native_fault=true; f.desire({});
      require(f.released() && f.actions.size()==2,"latched unrelated native fault never suppresses required release"); }
    for (bool after:{false,true}) {
        ActionFixture f; f.fault_index=0; f.raise_before=!after; f.raise_after=after;
        f.hold();
        require(aim::native_fault && aim::action_fault && aim::owned_actions[0] && !aim::actions_released,"failed press retains uncertain ownership and blocks unload");
        require(close(f.contributions[0],after?1.f:0.f),"action fixture distinguishes exception before and after native mutation");
        f.raise_before=f.raise_after=false; f.desire({});
        aim::closing=true; aim::input_hook(reinterpret_cast<void*>(Fixture::client+o::csgo_input),0,true);
        require(f.actions.size()==1 && !aim::actions_released && f.calls("original")==1,"uncertain action fault is never retried while original input callback remains intact");
    }
    { ActionFixture f; f.hold(); f.fault_index=0; f.raise_after=true; f.desire({});
      require(aim::action_fault && aim::owned_actions[0] && !aim::actions_released && close(f.contributions[0],0),"failed release remains conservatively uncertain even after native contribution disappeared"); }
}
void weapon_pulse_tests() {
    { ActionFixture f; f.enable_fire(); f.put<std::uint8_t>(ActionFixture::weapon_data+0x72d,0); f.run();
      require(aim::owned_actions[0],"single-shot starts its first eligible pulse immediately");
      clock_value+=10; f.run(); require(aim::owned_actions[0] && f.actions.size()==1,"single-shot pulse remains held for its20ms interval");
      clock_value+=10; f.run(); require(!aim::owned_actions[0] && f.actions.size()==2,"single-shot pulse serializes a balanced release at20ms");
      clock_value+=69; f.run(); require(!aim::owned_actions[0],"repeat cadence does not start early");
      ++clock_value; f.run(); require(aim::owned_actions[0] && f.actions.size()==3,"next single-shot pulse starts at90ms"); }
    { ActionFixture f; f.enable_fire(); f.run(); const auto replacement=Fixture::weapon_handle+(1<<15);
      f.entity(replacement,Fixture::weapon); f.put(Fixture::service+o::active_weapon,replacement);
      ++clock_value; f.run(); require(!aim::owned_actions[0] && f.actions.size()==2,"weapon serial replacement releases the old held attack first");
      ++clock_value; f.run(); require(aim::owned_actions[0] && f.actions.size()==3,"replacement weapon can request fire in next callback without inherited timer"); }
    { ActionFixture f; f.enable_fire(); f.put<std::uint8_t>(ActionFixture::weapon_data+0x72d,0); f.run();
      clock_value+=5; f.weapon_ready=false; auto s=f.run();
      require(!aim::owned_actions[0] && s.fire_block==aim::FireBlock::weapon_cooldown,"native cooldown releases the owned pulse and exposes its reason");
      ++clock_value; f.weapon_ready=true; f.run();
      require(aim::owned_actions[0],"native readiness resumes immediately without carrying the old90ms retry deadline");
      aim::set_menu_open(true); f.run(); require(!aim::owned_actions[0],"menu pause cancels a live semi-auto pulse");
      aim::set_menu_open(false); ++clock_value; f.run(); require(aim::owned_actions[0],"resume uses fresh fire state without a stale pulse deadline"); }
}
}

int main() {
    try {
        balanced_action_tests(); action_gate_tests(); action_callback_and_fault_tests(); weapon_pulse_tests();
        std::ofstream("aim-actions-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"owned native action fixtures: balanced references, physical input preservation, input gates, callback release and exception uncertainty\"}\n";
        std::cout << "aim actions PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "aim actions FAIL: " << error.what() << '\n'; return 1; }
}
