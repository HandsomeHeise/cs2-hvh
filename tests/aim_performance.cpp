// Entirely owned memory/native fixtures. No CS2 process, hook installation,
// desktop input, native engine RNG or actual game function is accessed.
#include <windows.h>
BOOL performance_counter(LARGE_INTEGER*);
BOOL performance_frequency(LARGE_INTEGER*);
#define QueryPerformanceCounter performance_counter
#define QueryPerformanceFrequency performance_frequency
#define main aim_runtime_fixture_main
#include "aim_runtime.cpp"
#undef main
#undef QueryPerformanceCounter
#undef QueryPerformanceFrequency

namespace {
LONGLONG qpc_time=0, ray_cost=0;
bool qpc_available=true;
ULONGLONG ray_clock_cost=0;
struct PerformanceFixture;
PerformanceFixture* performance_current=nullptr;
void* performance_construct(void*);
void performance_destruct(void*);
void* performance_filter(void*,void*,std::uint64_t,std::uint8_t,std::uint8_t);
void performance_create(void*,const Vec3*,const Vec3*,void*,int,bool);
void performance_penetrate(void*,float,float,float,int,int,void*);
void performance_interval(void*,void*,void*);
int performance_group(void*);
bool performance_armor(void*,int);
void performance_action(void*,int,float,float);

struct PerformanceFixture : Fixture {
    static constexpr std::uintptr_t data=0x61000000, intervals=0x61001000, records=0x61002000,
        damage_scale=0x61003000, maximum=0x61004000, only_up=0x61004100,
        geometry=0x61005000, other_player=0x61009000, other_controller=0x6100d000;
    static constexpr std::uint32_t geometry_handle=(9u<<15)|200, other_handle=(10u<<15)|201, other_controller_handle=(11u<<15)|3;
    aim::BallisticParameters parameters{100,4,1.5f,2,4096,.98f};
    bool hit=true, raise_native=false, alternate=false;
    bool geometry_first=false;
    std::uintptr_t blocker=geometry;
    float damage=20;
    std::vector<Vec3> rays;
    PerformanceFixture() {
        performance_current=this; qpc_time=ray_cost=0; qpc_available=true; ray_clock_cost=0;
        aim::clear_target_caches(); aim::ballistics_verified=true; aim::smoke_verified=false;
        aim::hitboxes_verified=false; aim::actions_verified=false;
        aim::owned_actions={}; aim::actions_released=true; aim::action_fault=false;
        add(data,0x1000); add(intervals,48); add(records,112); add(damage_scale,0x100); add(maximum,0x200);
        add(geometry,0x4000); add(other_player,0x4000); add(other_controller,0x1000);
        entity(geometry_handle,geometry); put(identity(geometry_handle)+o::designer_name,names+384); text(names+384,"prop_dynamic");
        for (const auto rva:{0x24bca80u,0x24bca90u,0x24bcaa0u,0x24bcab0u,0x25becb0u,0x25becc0u}) add(client+rva,8);
        for (const auto rva:{0x24bca80u,0x24bca90u,0x24bcaa0u,0x24bcab0u}) put(client+rva,damage_scale);
        put(damage_scale+0x58,5.f); put(client+0x25becb0,maximum); put(client+0x25becc0,only_up);
        put<std::uint8_t>(maximum+0x58,0); put<std::uint8_t>(only_up+0x58,0);
        put(weapon+0x388,data); put(data+0x730,1); put(data+0x820,parameters.damage);
        put(data+0x824,parameters.head); put(data+0x828,parameters.armor); put(data+0x82c,parameters.penetration);
        put(data+0x830,parameters.range); put(data+0x834,parameters.modifier);
        put(weapon+0x1a00,0); put(weapon+0x1a28,0.f); put<std::uint8_t>(data+0x72d,1);
        put(intervals+8,damage); put(intervals+12,4); put<std::uint16_t>(intervals+0x12,0);
        put(intervals+24+8,damage); put(intervals+24+12,4); put<std::uint16_t>(intervals+24+0x12,1);
        put(enemy+0x1ec8,0);
        aim::trace_construct=performance_construct; aim::trace_destruct=performance_destruct;
        aim::filter_construct=performance_filter; aim::bullet_create=performance_create;
        aim::bullet_penetrate=performance_penetrate; aim::bullet_interval=performance_interval;
        aim::hit_group=performance_group; aim::is_armored=performance_armor;
        punch={};
    }
    aim::ChanceContext context(int threshold=60) {
        aim::ChanceContext c;
        c.pawn=pawn; c.weapon=weapon; c.target=enemy; c.local_handle=pawn_handle;
        c.weapon_handle=weapon_handle; c.target_handle=enemy_handle; c.item=7; c.team=3;
        c.health=80; c.hitbox=1; c.threshold=threshold; c.eye={0,0,64}; c.point=bone_point;
        c.origin={100,10,0}; c.angles=aim_core::angle_to(c.eye,c.point); c.parameters=parameters;
        c.inaccuracy=.002f; c.spread=.001f; c.minimum_damage=20;
        return c;
    }
    void clear_sample() { clear_calls(); rays.clear(); }
    void player_blocker() {
        entity(other_handle,other_player); entity(other_controller_handle,other_controller);
        put(identity(other_controller_handle)+o::designer_name,names+256);
        put(other_controller+o::pawn_handle,other_handle);
        blocker=other_player; geometry_first=true;
    }
    void fire_settings() {
        config::Aim settings; settings.enabled=true; settings.activation=2;
        settings.profiles[0].recoil=false; settings.profiles[0].auto_fire=true; settings.profiles[0].switch_delay_ms=0;
        aim::actions_verified=true; aim::action_delta=performance_action; aim::configure(settings);
    }
    ~PerformanceFixture() {
        aim::clear_target_caches(); aim::ballistics_verified=aim::actions_verified=false;
        aim::owned_actions={}; aim::actions_released=true; performance_current=nullptr;
    }
};
template<class T> void performance_store(void* object,std::size_t offset,T value) {
    std::memcpy(static_cast<unsigned char*>(object)+offset,&value,sizeof(value));
}
void* performance_construct(void* opaque) {
    auto& f=*performance_current; f.events.emplace_back("ballistic-construct");
    performance_store(opaque,0,2); performance_store(opaque,8,PerformanceFixture::records);
    performance_store(opaque,0x1c20,(f.geometry_first ? 1 : 0)+(f.hit ? 1 : 0)); performance_store(opaque,0x1c28,PerformanceFixture::intervals);
    return opaque;
}
void performance_destruct(void*) { performance_current->events.emplace_back("ballistic-destruct"); }
void* performance_filter(void* filter,void*,std::uint64_t,std::uint8_t,std::uint8_t) { return filter; }
void performance_create(void*,const Vec3*,const Vec3* delta,void*,int,bool) {
    auto& f=*performance_current; f.events.emplace_back("ballistic-create"); qpc_time+=ray_cost; clock_value+=ray_clock_cost;
    f.rays.push_back(aim_core::mul(*delta,1.f/f.parameters.range));
    if (f.raise_native) RaiseException(0xe0423001,0,0,nullptr);
}
void performance_penetrate(void*,float,float,float,int,int,void*) {}
void performance_interval(void*,void* output,void* interval) {
    const auto& f=*performance_current;
    const auto entity=f.geometry_first && reinterpret_cast<std::uintptr_t>(interval)==PerformanceFixture::intervals ? f.blocker :
        f.alternate && f.rays.size()%2==0 ? std::uintptr_t{0} : PerformanceFixture::enemy;
    performance_store(output,8,entity); performance_store(output,0xac,.25f);
}
int performance_group(void*) { return 2; }
bool performance_armor(void*,int) { return false; }
void performance_action(void*,int,float delta,float) { performance_current->events.emplace_back(delta>0 ? "attack-down" : "attack-up"); }

void sampler_performance_tests() {
    { PerformanceFixture f; auto c=f.context(); c.inaccuracy=c.spread=0;
      const auto r=aim::fresh_chance(c);
      require(r.supported && r.complete && r.decision && r.threshold_met && r.percent==100 && r.samples==1,
          "zero spread and inaccuracy need exactly one authoritative native ray for the deterministic distribution");
      require(f.calls("ballistic-create")==1 && f.calls("ballistic-destruct")==1,"deterministic fast path constructs and destroys one native trace");
      f.hit=false; const auto miss=aim::fresh_chance(c);
      require(miss.complete && !miss.threshold_met && miss.percent==0 && miss.samples==1,"deterministic miss never becomes a zero-spread hit"); }
    { PerformanceFixture f; auto c=f.context(); c.inaccuracy=c.spread=0; c.minimum_damage=101;
      const auto r=aim::fresh_chance(c); require(r.complete && r.percent==0 && !r.threshold_met,"deterministic fast path retains native minimum damage"); }
    { PerformanceFixture f; const auto c=f.context(); const auto r=aim::fresh_chance(c);
      require(r.samples==16 && r.rays==16 && r.complete && r.threshold_met && r.percent==100,"a fresh callback estimates all16 actual native samples when affordable");
      const auto next=aim::fresh_chance(c);
      require(next.samples==16 && next.complete && f.rays.size()==32,"the next callback traces16 current rays instead of reusing earlier geometry or authorization"); }
    { PerformanceFixture f; f.alternate=true; auto c=f.context(50); const auto r=aim::fresh_chance(c);
      require(r.complete && r.samples==16 && r.percent==50 && r.threshold_met,"empirical chance uses actual hits divided by actual N, including threshold equality");
      c.threshold=51; const auto reject=aim::fresh_chance(c); require(reject.complete && !reject.threshold_met && reject.percent==50,"a completed below-threshold estimate cannot authorize fire"); }
    { PerformanceFixture f; auto c=f.context(100); const auto r=aim::fresh_chance(c);
      require(r.complete && r.threshold_met && r.percent==100 && r.samples==16,"100 means all current samples hit, without claiming an underlying probability guarantee"); }
    { PerformanceFixture f; f.hit=false; const auto r=aim::fresh_chance(f.context());
      require(r.complete && !r.threshold_met && r.samples==16 && r.percent==0,"16 completed current misses publish zero rather than a stale or unavailable estimate"); }
    { PerformanceFixture f; ray_cost=100; const auto c=f.context(); const auto r=aim::fresh_chance(c);
      require(r.samples==8 && r.complete && r.threshold_met && f.calls("ballistic-create")==8 && qpc_time==800,
          "at least8 current samples can authorize an empirical estimate when the QPC slice prevents more"); }
    { PerformanceFixture f; ray_cost=125; const auto c=f.context(); auto r=aim::fresh_chance(c);
      require(r.samples==6 && !r.complete && !r.threshold_met && r.percent==-1 && qpc_time==750,
          "fewer than8 rays withhold chance and stop before another ray at the0.75ms budget");
      clock_value+=8; r=aim::fresh_chance(c);
      require(r.samples==6 && !r.complete && !r.threshold_met && f.rays.size()==12,"incomplete samples never accumulate across callbacks"); }
    { PerformanceFixture f; ray_cost=1200; const auto r=aim::fresh_chance(f.context());
      require(r.samples==1 && f.calls("ballistic-destruct")==1 && !r.threshold_met,"a slow opaque native call is cleaned up and no second over-budget ray begins"); }
    { PerformanceFixture f; ray_clock_cost=50; const auto r=aim::fresh_chance(f.context());
      require(!r.threshold_met && !r.complete && f.calls("ballistic-destruct")==1,"an individual native call crossing the absolute stall guard cannot authorize its result"); }
    { PerformanceFixture f; qpc_available=false; const auto r=aim::fresh_chance(f.context());
      require(!r.threshold_met && !r.complete && f.rays.empty(),"unavailable performance clock rejects the estimate before native calls"); }
    { PerformanceFixture f; f.raise_native=true; const auto r=aim::fresh_chance(f.context());
      require(aim::native_fault && !r.threshold_met && !r.complete && f.calls("ballistic-destruct")==1,"native fault latches failure and destroys opaque storage before rejecting the estimate");
      const auto calls=f.rays.size(); aim::fresh_chance(f.context()); require(f.rays.size()==calls,"latched fault prevents all subsequent native chance calls"); }
}
void sampler_context_tests() {
    for (int mutation=0;mutation<17;++mutation) {
        PerformanceFixture f; auto c=f.context(100); const auto first=aim::fresh_chance(c);
        switch (mutation) {
        case 0: ++c.target_handle; break; case 1: ++c.weapon_handle; break; case 2: ++c.local_handle; break;
        case 3: c.point.y+=2; break; case 4: c.origin.y+=2; break; case 5: c.eye.y+=2; break;
        case 6: c.angles.y+=.1f; break; case 7: c.inaccuracy*=1.1f; break; case 8: c.spread*=1.1f; break;
        case 9: f.put(Fixture::weapon+0x1a28,1.f); break; case 10: f.put(Fixture::weapon+0x1a00,1); break;
        case 11: f.put<std::uint8_t>(PerformanceFixture::maximum+0x58,1); break;
        case 12: f.put<std::uint8_t>(PerformanceFixture::only_up+0x58,1); break;
        case 13: --c.health; break; case 14: c.minimum_damage+=1; break;
        case 15: c.penetration=!c.penetration; break; case 16: c.parameters.modifier=.9f; break;
        }
        clock_value+=8; f.hit=false; const auto r=aim::fresh_chance(c);
        require(first.threshold_met && r.samples==16 && r.complete && !r.threshold_met && r.percent==0,
            "each changed context traces only current geometry and cannot inherit any previous positive authorization");
    }
    { PerformanceFixture f; const auto c=f.context(); aim::fresh_chance(c); f.put(PerformanceFixture::data+0x730,8);
      const auto r=aim::fresh_chance(c); require(!r.supported && !r.threshold_met && f.rays.size()==16,"unsupported pellet count rejects prior authorization without a trace"); }
    { PerformanceFixture f; const auto c=f.context(); aim::fresh_chance(c); f.fail_read=PerformanceFixture::maximum+0x58;
      const auto r=aim::fresh_chance(c); require(!r.supported && !r.threshold_met,"unreadable native input cannot inherit a prior supported result"); }
    { PerformanceFixture f; auto c=f.context(); c.inaccuracy=std::numeric_limits<float>::quiet_NaN();
      require(!aim::fresh_chance(c).supported && f.rays.empty(),"nonfinite accuracy cannot enter the deterministic fast path"); }
    { PerformanceFixture f; aim_spread::Rng baseline(12345), independent(12345); aim::fresh_chance(f.context());
      require(baseline.uniform()==independent.uniform() && baseline.uniform()==independent.uniform(),"fresh sampler owns its PRNG and does not consume another generator's sequence"); }
}
void blocker_roster_tests() {
    { PerformanceFixture f; f.geometry_first=true;
      for (int i=0;i<16;++i) require(!aim::player_entity(PerformanceFixture::geometry),"baseline recognizes the owned prop as a non-player");
      const auto repeated_reads=f.reads; f.clear_sample(); aim::BlockerScope scope;
      const auto r=aim::fresh_chance(f.context());
      require(r.complete && r.percent==100 && scope.own.builds==1 && scope.own.cache_hits==15,
          "16 repeated prop impacts build one current roster and reuse only serial-validated classification");
      require(f.reads<repeated_reads/2,"callback-local classification uses less than half the memory reads of16 repeated controller scans including actual ballistic work"); }
    { PerformanceFixture f; f.player_blocker(); aim::BlockerScope scope; const auto r=aim::fresh_chance(f.context());
      require(r.complete && r.percent==0 && !r.threshold_met && scope.own.builds==1,"another resolved player before the target still blocks every native ray"); }
    { PerformanceFixture f; f.geometry_first=true; f.fail_read=PerformanceFixture::geometry+o::identity;
      const auto r=aim::fresh_chance(f.context()); require(r.complete && r.percent==0 && !r.threshold_met,"an encountered impact with unreadable identity fails closed instead of being skipped as world geometry"); }
    { PerformanceFixture f; f.geometry_first=true; f.fail_read=Fixture::page+64*v::identity_stride;
      const auto r=aim::fresh_chance(f.context()); require(r.complete && r.percent==100,"an unrelated sparse unreadable controller slot does not blanket-block a positively identified prop"); }
    { PerformanceFixture f; f.geometry_first=true; aim::BlockerScope scope;
      require(aim::fresh_chance(f.context()).percent==100,"first classification accepts verified current prop geometry");
      f.entity(PerformanceFixture::geometry_handle+(1u<<15),PerformanceFixture::geometry); f.text(Fixture::names+384,"cs_player_pawn");
      const auto r=aim::fresh_chance(f.context()); require(r.complete && r.percent==0,"recycled prop identity cannot reuse a negative classification when it becomes a new player pawn"); }
}
void fire_gate_tests() {
    { PerformanceFixture f; f.fire_settings(); auto settings=aim::requested;
      settings.profiles[0].penetration=true; aim::configure(settings); f.trace_fraction=.4f;
      f.put(PerformanceFixture::intervals+0xc,3); const auto s=f.run();
      require(settings.profiles[0].visible_only && s.has_target && s.state==aim::State::firing && f.calls("attack-down")==1 && s.penetrations==1 && s.damage>=20,
          "Autowall overrides saved visible-only and fires a covered shot after native damage and chance pass"); }
    { PerformanceFixture f; f.fire_settings(); f.trace_fraction=.4f;
      require(!f.run().has_target && f.calls("attack-down")==0,"covered target remains excluded with Autowall off"); }
    { PerformanceFixture f; f.fire_settings(); auto settings=aim::requested;
      settings.profiles[0].penetration=true; settings.profiles[0].min_damage=150; aim::configure(settings);
      f.trace_fraction=.4f; f.damage=1; f.put(PerformanceFixture::intervals+8,1.f); f.put(PerformanceFixture::intervals+0xc,3);
      require(!f.run().has_target && f.calls("attack-down")==0,"Autowall never authorizes a covered hit below native minimum damage"); }
    { PerformanceFixture f; f.fire_settings(); f.put<std::uint8_t>(Fixture::enemy+o::team,3);
      f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,1); const auto s=f.run();
      require(s.has_target && s.state==aim::State::firing && f.calls("attack-down")==1,"FFA authorizes native Auto Fire at an otherwise valid same-team opponent");
      f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,0); ++clock_value; f.clear_sample(); f.run();
      require(!aim::latest().has_target && f.calls("attack-up")==1 && f.calls("attack-down")==0,"switching to team rules releases held Auto Fire at same-team player"); }
    { PerformanceFixture f; f.enable(); require(f.run().fire_block==aim::FireBlock::auto_fire_off,"manual aim reports Auto Fire off explicitly"); }
    { PerformanceFixture f; f.fire_settings(); auto settings=aim::requested; settings.profiles[0].smoothing=10; aim::configure(settings);
      const auto s=f.run(); require(s.fire_block==aim::FireBlock::alignment && s.alignment_error>=.15f && !s.chance_complete && s.chance_samples==0,
          "unaligned smooth aim reports its actual shot-angle error rather than generic tracking or sample shortage"); }
    { PerformanceFixture f; f.fire_settings(); f.put<std::uint8_t>(Fixture::weapon+o::reloading,1);
      require(f.run().fire_block==aim::FireBlock::reloading,"reloading is reported separately from aim alignment"); }
    { PerformanceFixture f; f.fire_settings(); f.put(Fixture::weapon+o::clip,0);
      require(f.run().fire_block==aim::FireBlock::empty_clip,"empty magazine has a distinct fire gate"); }
    { PerformanceFixture f; f.fire_settings(); f.fail_read=PerformanceFixture::data+0x72d;
      require(f.run().fire_block==aim::FireBlock::weapon_state,"unreadable native full-auto field cannot silently look like tracking"); }
    { PerformanceFixture f; f.fire_settings(); ray_cost=125; const auto s=f.run();
      require(s.fire_block==aim::FireBlock::sample_shortage && !s.chance_complete && s.chance_samples==6,"QPC sample shortage has its own exact reason and actual ray count"); }
    { PerformanceFixture f; f.fire_settings(); f.alternate=true; const auto s=f.run();
      require(s.fire_block==aim::FireBlock::chance_below_threshold && s.chance_complete && s.hit_chance==50,
          "completed below-threshold chance is distinct from an insufficient sample batch"); }
    { PerformanceFixture f; f.fire_settings(); auto settings=aim::requested; settings.profiles[0].auto_scope=true; aim::configure(settings);
      f.put<std::uint16_t>(Fixture::weapon+o::attribute_manager+o::item+o::item_definition,9);
      require(f.run().fire_block==aim::FireBlock::scope,"scope requirement is reported before shot sampling"); }
    { PerformanceFixture f; f.fire_settings(); auto settings=aim::requested; settings.profiles[0].auto_stop=true; aim::configure(settings);
      f.put(Fixture::pawn+o::velocity,Vec3{100,0,0}); require(f.run().fire_block==aim::FireBlock::stopping,"movement braking is a distinct fire gate"); }
    { PerformanceFixture f; f.fire_settings(); f.open_menu=true;
      require(f.run().fire_block==aim::FireBlock::context_changed,"input context invalidation during a native visibility call reports a context fire gate"); }
    { PerformanceFixture f; f.fire_settings(); f.put<std::uint8_t>(PerformanceFixture::data+0x72d,0); f.run(); clock_value+=20;
      require(f.run().fire_block==aim::FireBlock::semi_auto_cooldown,"semi-auto pulse ownership has an explicit cooldown gate"); }
    for (int reason=0;reason<=static_cast<int>(aim::FireBlock::semi_auto_cooldown);++reason)
      require(std::strlen(aim::fire_block_name(static_cast<aim::FireBlock>(reason)))>3,"each public fire gate has an English diagnostic label");
}
struct SilentFixture;
SilentFixture* silent_current=nullptr;
void silent_parser(const void*,void*,bool,float,float,void*);
struct SilentFixture:PerformanceFixture {
    aim::SilentRequest request;
    alignas(16) std::array<unsigned char,0x60> message{},output{};
    unsigned parser_calls=0;
    bool parser_arguments=true,throw_parser=false;
    SilentFixture() {
        silent_current=this;
        for (unsigned i=0;i<message.size();++i) message[i]=static_cast<unsigned char>(i+1);
        std::memcpy(message.data()+0x10,&view,sizeof(view));
        add(reinterpret_cast<std::uintptr_t>(message.data()),message.size());
        put(reinterpret_cast<std::uintptr_t>(message.data()),message);
        aim::history_hook_address=reinterpret_cast<void*>(0x12346000); aim::original_history=silent_parser;
        aim::silent_sample_count=0; aim::active_silent=nullptr;
    }
    aim::Status stage(bool automatic=false) {
        if (automatic) fire_settings(); else enable();
        auto settings=aim::requested; settings.profiles[0].silent=true; settings.profiles[0].smoothing=30;
        aim::configure(settings);
        aim::update(reinterpret_cast<void*>(client+o::csgo_input),true,&request);
        return aim::latest();
    }
    void parse(std::uintptr_t return_rva=0xd026d9) {
        aim::SilentScope scope(request);
        aim::history_dispatch(message.data(),output.data(),true,.25f,.75f,this,client+return_rva);
    }
    ~SilentFixture() { aim::active_silent=nullptr; aim::history_hook_address=nullptr; aim::original_history=nullptr; silent_current=nullptr; }
};
void silent_parser(const void* source,void* destination,bool extra,float fraction,float time,void* context) {
    auto& f=*silent_current; ++f.parser_calls;
    if (f.throw_parser) throw std::runtime_error("owned native-parser fixture exception");
    f.parser_arguments &= destination==f.output.data() && extra && fraction==.25f && time==.75f && context==&f;
    std::memcpy(destination,source,f.output.size());
}
void silent_tests() {
    { SilentFixture f; const auto original=f.message; const auto s=f.stage();
      require(s.has_target && f.request.valid && f.calls("set")==0,"Silent Aim stages a real target without calling the camera/input angle setter");
      require(close(f.request.angles.y,5.710593f) && close(f.request.angles.x,0),"Silent Aim uses direct shot angles while retaining saved smoothing");
      f.parse(); Vec3 shot{}; std::memcpy(&shot,f.output.data()+0x10,sizeof(shot));
      require(close(shot.y,5.710593f) && f.message==original && close(f.view.x,0) && close(f.view.y,0) && close(f.view.z,0),"native history parser receives a changed shot angle and original source/camera remain unchanged");
      require(std::memcmp(f.output.data(),original.data(),0x10)==0 && std::memcmp(f.output.data()+0x1c,original.data()+0x1c,0x60-0x1c)==0,
          "only twelve angle bytes change; ticks interpolation origin indices and other native metadata are identical");
      require(f.parser_calls==1 && f.parser_arguments && aim::latest().silent_samples==1 && aim::history_callbacks==0,"six native arguments forward once and history callback drains");
      f.parse(0xcde18c); require(f.parser_calls==2 && aim::latest().silent_samples==2,"helper append caller shares the same verified shot-history contract");
      require(aim::active_silent==nullptr,"silent scope does not retain requests after the synchronous command build"); }
    { SilentFixture f; const auto s=f.stage(true);
      require(s.state==aim::State::firing && f.request.valid && f.calls("attack-down")==1 && f.calls("set")==0,"Silent Auto Fire evaluates chance and shot alignment without visible aim or inherited smoothing delay"); }
    { SilentFixture f; f.stage(true); f.clear_sample(); ++clock_value; f.weapon_ready=false;
      aim::update(reinterpret_cast<void*>(Fixture::client+o::csgo_input),true,&f.request);
      require(f.request.valid && aim::latest().fire_block==aim::FireBlock::weapon_cooldown && f.calls("attack-down")==0,
          "cooldown keeps valid silent direction available for previously captured native fire messages without authorizing another shot");
      f.parse(); Vec3 shot{}; std::memcpy(&shot,f.output.data()+0x10,sizeof(shot));
      require(close(shot.y,5.710593f) && aim::silent_sample_count==1,"history serialization in a later cooldown update retains the target direction instead of reverting to camera angle"); }
    { SilentFixture f; f.stage(); auto settings=aim::requested; settings.profiles[0].recoil=true; aim::configure(settings); f.punch={1,2,0};
      aim::update(reinterpret_cast<void*>(Fixture::client+o::csgo_input),true,&f.request);
      require(f.request.valid && close(f.request.angles.x,-1) && close(f.request.angles.y,3.710593f) && f.calls("set")==0,
          "Silent shot angles retain native recoil compensation without moving camera"); }
    { SilentFixture f; f.stage(); auto settings=aim::requested; settings.profiles[0].silent=false; aim::configure(settings);
      aim::update(reinterpret_cast<void*>(Fixture::client+o::csgo_input),true,&f.request);
      require(!f.request.valid && f.calls("set")==1,"disabling Silent Aim restores normal aim and clears staged history changes"); }
    for (int invalid=0;invalid<12;++invalid) {
      SilentFixture f; f.stage();
      if (invalid==0) aim::menu_open=true;
      if (invalid==1) foreground=false;
      if (invalid==2) clock_value+=33;
      if (invalid==3) --clock_value;
      if (invalid==4) f.put(f.identity(Fixture::enemy_handle)+v::identity_handle,Fixture::enemy_handle+(1<<15));
      if (invalid==5) f.put(Fixture::enemy+o::health,0);
      if (invalid==6) f.put<std::uint8_t>(Fixture::enemy+o::immunity,1);
      if (invalid==7) f.put(Fixture::service+o::active_weapon,Fixture::weapon_handle+(1<<15));
      if (invalid==8) { auto settings=aim::requested; settings.enabled=false; aim::configure(settings); }
      if (invalid==9) f.put<std::uint8_t>(Fixture::enemy_rule_data+0x58,1);
      if (invalid==10) f.put(Fixture::enemy_node+o::abs_origin,Vec3{101,10,0});
      if (invalid==11) aim::closing=true;
      f.parse(); require(f.output==f.message && f.parser_calls==1 && aim::silent_sample_count==0,
          "expired changed paused or invalid silent context passes original input to native parser once");
    }
    { SilentFixture f; f.stage(); f.parse(0xd02cfd); require(f.output==f.message && aim::silent_sample_count==0,"debug-only temporary history caller is never rewritten");
      f.parse(0x111111); require(f.output==f.message && aim::silent_sample_count==0,"unknown native caller is never rewritten"); }
    { SilentFixture f; f.stage(); aim::history_dispatch(f.message.data(),f.output.data(),true,.25f,.75f,&f,Fixture::client+0xd026d9);
      require(f.output==f.message && aim::silent_sample_count==0,"parser calls outside the verified synchronous input scope remain unchanged"); }
    { SilentFixture f; f.stage(); f.fail_read=reinterpret_cast<std::uintptr_t>(f.message.data()); f.parse();
      require(f.output==f.message && f.parser_calls==1 && aim::silent_sample_count==0,"unreadable source-copy guard leaves native parser arguments unchanged"); }
    { SilentFixture f; aim::history_hook_address=nullptr; const auto s=f.stage();
      require(!f.request.valid && !s.has_target && f.calls("set")==0 && f.calls("attack-down")==0,"unavailable native history support cannot silently fall back to visible aim or fire"); }
    { SilentFixture f; f.stage(); f.throw_parser=true; bool caught=false;
      try { f.parse(); } catch(const std::runtime_error&) { caught=true; }
      require(caught && aim::history_callbacks==0 && aim::active_silent==nullptr,"native fixture exceptions release callback counter and synchronous scope before propagation"); }
}
void selection_refresh_tests() {
    { PerformanceFixture f; f.enable(); auto s=f.run(); const auto full=f.calls("bone-position");
      require(s.has_target && full==3 && f.calls("ballistic-create")>0,"initial selection evaluates the configured current bones and native damage");
      f.clear_sample(); clock_value+=8; f.bone_point={100,11,64}; s=f.run();
      require(s.has_target && f.calls("bone-position")==1 && f.calls("ballistic-create")==1 && close(f.last_written.y,aim_core::angle_to({0,0,64},f.bone_point).y),
          "16 ms identity reuse refreshes the selected bone and native damage before writing current aim");
      f.clear_sample(); clock_value+=8; f.run(); require(f.calls("bone-position")==full,"selection cache expires at16ms and performs the configured full scan"); }
    { PerformanceFixture f; f.enable(); f.run(); f.clear_sample(); clock_value+=1; f.trace_fraction=.5f; f.run();
      require(f.calls("set")==0 && !aim::latest().has_target && !aim::selection_cache.valid,"new obstruction rejects a cached target before any view write"); }
    { PerformanceFixture f; f.enable(); f.run(); f.clear_sample(); clock_value+=1; f.put(Fixture::enemy+o::health,0); f.run();
      require(f.calls("set")==0 && !aim::latest().has_target,"cached selection never substitutes stale health"); }
    { PerformanceFixture f; f.enable(); f.run(); f.clear_sample(); clock_value+=1;
      f.put(f.identity(Fixture::enemy_handle)+v::identity_handle,Fixture::enemy_handle+(1<<15)); f.run();
      require(f.calls("bone-position")==0 && f.calls("set")==0 && !aim::latest().has_target,"cached target serial is resolved before native skeleton access"); }
    { PerformanceFixture f; f.fire_settings(); f.run(); aim::set_menu_open(true); f.clear_sample(); f.run();
      require(!aim::selection_cache.valid && f.rays.empty() && f.calls("attack-up")==1,"menu pause clears cached identity and releases owned attack without sampling"); }
    { PerformanceFixture f; f.fire_settings(); f.run(); foreground=false; f.clear_sample(); f.run();
      require(!aim::selection_cache.valid && f.rays.empty() && f.calls("attack-up")==1,"focus pause releases attack and invalidates cached identity"); }
    { PerformanceFixture f; f.fire_settings(); f.run();
      require(f.calls("attack-down")==1 && aim::latest().state==aim::State::firing && aim::latest().chance_sample_count==16,
          "fresh sufficient hit chance starts Auto Fire immediately in the first eligible callback");
      for (int i=0;i<3;++i) { clock_value+=10; f.bone_point.y+=2; f.put(Fixture::enemy_node+o::abs_origin,Vec3{100,f.bone_point.y,0}); f.run(); }
      require(f.calls("attack-down")==1 && f.calls("attack-up")==0 && aim::latest().state==aim::State::firing,
          "moving target100units per second keeps full-auto held across consecutive fresh eligible callbacks without pending-sample pulses"); }
    { PerformanceFixture f; f.fire_settings(); ray_cost=125; f.run(); clock_value+=8; f.run();
      require(f.calls("attack-down")==0 && aim::latest().chance_sample_count==0 && aim::latest().chance_samples==6,
          "Auto Fire stays withheld when fewer than8 rays fit and does not accumulate them across frames"); }
    { PerformanceFixture f; f.fire_settings(); ray_cost=100;
      aim::input_hook(reinterpret_cast<void*>(Fixture::client+o::csgo_input),0,true); const auto s=aim::latest();
      require(std::isfinite(s.work_ms) && s.work_ms>=.9f && s.peak_ms>=s.work_ms && s.chance_samples==8 && s.chance_sample_count==8,
          "input callback telemetry measures actual aim work and the actual current estimate sample count with QPC");
      const auto peak=s.peak_ms; f.clear_sample(); aim::input_hook(reinterpret_cast<void*>(Fixture::client+o::csgo_input),1,true);
      require(aim::latest().peak_ms==peak && f.calls("original")==1 && f.calls("get")==0,"unverified input slots keep the engine callback and do not add aim work"); }
}

}
BOOL performance_counter(LARGE_INTEGER* value) { if (!qpc_available) return FALSE; value->QuadPart=qpc_time; return TRUE; }
BOOL performance_frequency(LARGE_INTEGER* value) { if (!qpc_available) return FALSE; value->QuadPart=1000000; return TRUE; }
int main() {
    try {
        sampler_performance_tests(); sampler_context_tests(); blocker_roster_tests(); fire_gate_tests(); selection_refresh_tests(); silent_tests();
        std::ofstream("aim-performance-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"owned native fixtures, fresh empirical native samples, QPC budget, moving targets, continuous full-auto and current target selection\"}\n";
        std::cout << "aim performance PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "aim performance FAIL: " << error.what() << '\n'; return 1; }
}
