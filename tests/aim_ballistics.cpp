// Native ABI fixtures use owned buffers and fake functions only. No game calls,
// process attachment, hook installation or desktop input occur in these tests.
#include <windows.h>
#include "../src/aim.cpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using aim_core::Vec3;
unsigned checks = 0;
void require(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}
bool approximately(float a, float b) { return std::isfinite(a) && std::abs(a-b) < .001f; }
template<class T> void store(void* destination, std::size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(destination)+offset, &value, sizeof(value));
}
template<class T> T load(const void* source, std::size_t offset) {
    T value{}; std::memcpy(&value,static_cast<const unsigned char*>(source)+offset,sizeof(value)); return value;
}
enum class Fault { none, construct, filter, create, penetrate, interval, group, armor, destruct };
struct Fixture;
Fixture* current = nullptr;
void* construct_stub(void*);
void destruct_stub(void*);
void* filter_stub(void*,void*,std::uint64_t,std::uint8_t,std::uint8_t);
void create_stub(void*,const Vec3*,const Vec3*,void*,int,bool);
void penetrate_stub(void*,float,float,float,int,int,void*);
void interval_stub(void*,void*,void*);
int group_stub(void*);
bool armor_stub(void*,int);

struct Block { std::uintptr_t address; std::vector<unsigned char> bytes; };
struct Fixture {
    static constexpr std::uintptr_t client=0x10000000, target=0x50000000, pawn=0x50010000,
        weapon=0x50020000, vdata=0x50030000, ct_body=0x50040000, ct_head=0x50040100,
        t_body=0x50040200, t_head=0x50040300, other=0x50050000, geometry=0x50060000,
        system=0x51000000, page=0x52000000, controller=0x53000000, name=0x53010000;
    std::vector<Block> blocks;
    std::vector<std::string> events;
    std::array<std::array<unsigned char,0x18>,4> intervals{};
    std::array<unsigned char,4*0x38> records{};
    std::array<std::uintptr_t,4> entities{target,0,0,0};
    std::array<int,4> groups{2,2,2,2};
    std::array<bool,4> armored{};
    Fault fault=Fault::none;
    int interval_count=1, record_count=4, current_interval=0, shooter_team=3;
    std::uintptr_t interval_pointer_override=0, record_pointer_override=0, fail_read=0;
    bool override_interval_pointer=false, override_record_pointer=false, arguments_valid=true;
    void* constructed_data=nullptr;
    aim::BallisticParameters p{100,4.f,1.5f,2.f,4096.f,.98f};
    aim::BallisticResult result{};
    Vec3 eye{1,2,64}, point{101,2,64};

    void add(std::uintptr_t address,std::size_t bytes) { blocks.push_back({address,std::vector<unsigned char>(bytes)}); }
    template<class T> void put(std::uintptr_t address,T value) {
        for (auto& block:blocks) if (address>=block.address && address-block.address<=block.bytes.size() && sizeof(T)<=block.bytes.size()-(address-block.address)) {
            std::memcpy(block.bytes.data()+address-block.address,&value,sizeof(value)); return;
        }
        throw std::runtime_error("ballistic fixture write outside owned memory");
    }
    void text(std::uintptr_t address,const char* value) {
        for (std::size_t i=0;i<=std::strlen(value);++i) put(address+i,value[i]);
    }
    static bool read(void* user,std::uintptr_t address,void* output,std::size_t bytes) {
        auto& f=*static_cast<Fixture*>(user);
        if (address==f.fail_read) return false;
        for (const auto& block:f.blocks) if (address>=block.address && address-block.address<=block.bytes.size() && bytes<=block.bytes.size()-(address-block.address)) {
            std::memcpy(output,block.bytes.data()+address-block.address,bytes); return true;
        }
        for (const auto region: {std::pair{reinterpret_cast<std::uintptr_t>(f.intervals.data()),sizeof(f.intervals)},
                                std::pair{reinterpret_cast<std::uintptr_t>(f.records.data()),sizeof(f.records)}}) {
            if (address>=region.first && address-region.first<=region.second && bytes<=region.second-(address-region.first)) {
                std::memcpy(output,reinterpret_cast<const void*>(address),bytes); return true;
            }
        }
        return false;
    }
    void event(const char* name,Fault at) {
        events.emplace_back(name);
        if (fault==at) RaiseException(0xe0422001,0,0,nullptr);
    }
    unsigned calls(const char* name) const { return static_cast<unsigned>(std::count(events.begin(),events.end(),name)); }
    void interval(unsigned index,float damage,int remaining,bool material=false) {
        store(intervals[index].data(),0, .1f*index);
        store(intervals[index].data(),4,.1f*(index+1));
        store(intervals[index].data(),8,damage);
        store(intervals[index].data(),0xc,remaining);
        store<std::uint16_t>(intervals[index].data(),0x10,static_cast<std::uint16_t>(index));
        store<std::uint16_t>(intervals[index].data(),0x12,static_cast<std::uint16_t>(index));
        intervals[index][0x14]=material ? 1 : 0;
    }
    void reset_parameters() {
        put(weapon+0x388,vdata); put(vdata+0x820,p.damage); put(vdata+0x824,p.head);
        put(vdata+0x828,p.armor); put(vdata+0x82c,p.penetration); put(vdata+0x830,p.range); put(vdata+0x834,p.modifier);
    }
    void other_is_player() {
        constexpr std::uint32_t controller_handle=(3u<<15)|1, pawn_handle=(4u<<15)|65;
        const auto controller_identity=page+verified_layout::identity_stride;
        const auto pawn_identity=page+65*verified_layout::identity_stride;
        put(controller_identity,controller); put(controller_identity+verified_layout::identity_handle,controller_handle);
        put(controller+dump_offsets::identity,controller_identity);
        put(controller_identity+dump_offsets::designer_name,name); text(name,"cs_player_controller");
        put(controller+dump_offsets::pawn_handle,pawn_handle);
        put(pawn_identity,other); put(pawn_identity+verified_layout::identity_handle,pawn_handle);
        put(other+dump_offsets::identity,pawn_identity);
    }
    Fixture() {
        current=this;
        add(target,0x4000); add(pawn,0x4000); add(weapon,0x1000); add(vdata,0x1000);
        add(other,0x4000); add(geometry,0x4000);
        add(client+dump_offsets::entity_system,8); add(system,0x1000);
        add(page,512*verified_layout::identity_stride); add(controller,0x1000); add(name,64);
        put(client+dump_offsets::entity_system,system); put(system+verified_layout::chunk_table,page);
        add(ct_body,0x400);
        for (const auto rva:{0x24bca80u,0x24bca90u,0x24bcaa0u,0x24bcab0u}) add(client+rva,8);
        put(client+0x24bca80,ct_body); put(client+0x24bca90,ct_head);
        put(client+0x24bcaa0,t_body); put(client+0x24bcab0,t_head);
        put(ct_body+0x58,2.f); put(ct_head+0x58,3.f); put(t_body+0x58,5.f); put(t_head+0x58,7.f);
        put<std::uint8_t>(target+dump_offsets::team,2); put(target+0x1ec8,100);
        interval(0,20.f,4);
        reset_parameters();
        aim::sources={{read,this},{client,dump_offsets::client_image_size,dump_offsets::client_timestamp},{}};
        aim::trace_construct=construct_stub; aim::trace_destruct=destruct_stub; aim::filter_construct=filter_stub;
        aim::bullet_create=create_stub; aim::bullet_penetrate=penetrate_stub; aim::bullet_interval=interval_stub;
        aim::hit_group=group_stub; aim::is_armored=armor_stub; aim::native_fault=false;
    }
    bool run(bool penetration=true) {
        return aim::ballistic_native(pawn,shooter_team,target,eye,point,p,penetration,result);
    }
    ~Fixture() { aim::sources={}; current=nullptr; }
};

void* construct_stub(void* data) {
    current->event("construct",Fault::construct);
    current->arguments_valid &= (reinterpret_cast<std::uintptr_t>(data)&15)==0;
    current->constructed_data=data;
    store(data,0,current->record_count);
    store(data,8,current->override_record_pointer ? current->record_pointer_override : reinterpret_cast<std::uintptr_t>(current->records.data()));
    store(data,0x1c20,current->interval_count);
    store(data,0x1c28,current->override_interval_pointer ? current->interval_pointer_override : reinterpret_cast<std::uintptr_t>(current->intervals.data()));
    return data;
}
void destruct_stub(void* data) {
    current->arguments_valid &= data==current->constructed_data;
    current->event("destruct",Fault::destruct);
}
void* filter_stub(void* filter,void* ignored,std::uint64_t mask,std::uint8_t layer,std::uint8_t flags) {
    current->event("filter",Fault::filter);
    current->arguments_valid &= reinterpret_cast<std::uintptr_t>(ignored)==Fixture::pawn && mask==0x1c300b && layer==3 && flags==0x0f;
    return filter;
}
void create_stub(void* data,const Vec3* eye,const Vec3* delta,void* filter,int maximum,bool append) {
    current->event("create",Fault::create);
    current->arguments_valid &= data==current->constructed_data && approximately(eye->x,current->eye.x) && approximately(eye->y,current->eye.y) &&
        approximately(eye->z,current->eye.z) && approximately(aim_core::length(*delta),current->p.range) && maximum==4 && append &&
        (static_cast<unsigned char*>(filter)[0x39]&2) && (load<std::uint64_t>(filter,0x10)&0x4000000000ULL);
}
void penetrate_stub(void* data,float damage,float penetration,float modifier,int maximum,int team,void* diagnostics) {
    current->event("penetrate",Fault::penetrate);
    current->arguments_valid &= data==current->constructed_data && approximately(damage,static_cast<float>(current->p.damage)) &&
        approximately(penetration,current->p.penetration) && approximately(modifier,current->p.modifier) && maximum==4 && team==current->shooter_team && !diagnostics;
}
void interval_stub(void* data,void* trace,void* interval) {
    current->event("interval",Fault::interval);
    const auto address=reinterpret_cast<std::uintptr_t>(interval), first=reinterpret_cast<std::uintptr_t>(current->intervals.data());
    current->arguments_valid &= data==current->constructed_data && address>=first && address-first<sizeof(current->intervals) && (address-first)%0x18==0;
    current->current_interval=static_cast<int>((address-first)/0x18);
    if (current->current_interval<0 || current->current_interval>=4) RaiseException(0xe0422002,0,0,nullptr);
    store(trace,8,current->entities[current->current_interval]);
    store(trace,0x18,Fixture::other); // The obsolete wrong field must not influence target matching.
    store(trace,0xac,.25f); store(trace,0x84,Vec3{101,2,64});
}
int group_stub(void*) { current->event("group",Fault::group); return current->groups[current->current_interval]; }
bool armor_stub(void* target,int group) {
    current->event("armor",Fault::armor);
    current->arguments_valid &= reinterpret_cast<std::uintptr_t>(target)==Fixture::target && group==current->groups[current->current_interval];
    return current->armored[current->current_interval];
}

void abi_and_lifetime_tests() {
    { Fixture f; require(f.run() && f.result.hit && approximately(f.result.damage,100.f),"native pipeline uses interval damage+8 and target entity+8");
      require(f.arguments_valid,"opaque buffers, native ABI, bullet mask, flags, delta, team and maximum penetration arguments match recovered contracts");
      require(f.events==std::vector<std::string>{"construct","filter","create","penetrate","interval","group","armor","destruct"},"normal call sequence ends in exactly one destructor"); }
    { Fixture f; f.point=f.eye; require(!f.run() && f.calls("destruct")==1 && f.calls("create")==0,"zero-distance rejection still destroys completed native data"); }
    { Fixture f; f.point={10000,0,0}; require(!f.run() && f.calls("destruct")==1 && f.calls("create")==0,"out-of-range target is rejected before trace and destructor runs once"); }
    for (const auto fault:{Fault::construct,Fault::filter,Fault::create,Fault::penetrate,Fault::interval,Fault::group,Fault::armor,Fault::destruct}) {
        Fixture f; f.fault=fault; const auto ok=f.run();
        require(!ok && aim::native_fault,"each native SEH failure returns false and latches fault");
        require(f.calls("construct")==1 && f.calls("destruct")==(fault==Fault::construct ? 0u : 1u),"native destructor is attempted once after completed construction, never after failed construction");
        require(f.events.back()==(fault==Fault::construct ? "construct" : "destruct"),"native fault cleanup does not call later processing methods");
    }
    { Fixture f; f.interval_count=0; require(f.run() && !f.result.hit && f.calls("destruct")==1,"empty trace is a completed no-hit with one destructor"); }
    { Fixture f; f.result={true,999.f,3,1}; f.interval_count=0;
      require(f.run() && !f.result.hit && f.result.damage==0,"completed no-hit clears reused result instead of publishing a stale hit"); }
    { Fixture f; f.fail_read=reinterpret_cast<std::uintptr_t>(f.intervals.data()); require(!f.run() && f.calls("interval")==0 && f.calls("destruct")==1,"unreadable interval fails before native conversion and still destroys data"); }
}

void damage_and_mapping_tests() {
    for (const auto team:{2,3}) for (const auto group:{0,1,2,3,4,5,6,7,8}) {
        Fixture f; f.put<std::uint8_t>(Fixture::target+dump_offsets::team,static_cast<std::uint8_t>(team)); f.groups[0]=group;
        const float body=team==3 ? 2.f : 5.f, head=team==3 ? 3.f : 7.f;
        const float expected=group==0 ? 20.f : group==1 ? 20.f*4.f*head : group==3 ? 20.f*1.25f*body :
            (group==6 || group==7) ? 20.f*.75f*body : 20.f*body;
        require(f.run() && f.result.hit && f.result.group==group && approximately(f.result.damage,expected),"actual native hitgroup and TARGET team select current scale; generic group has no team multiplier");
    }
    { Fixture f; f.groups[0]=1; f.armored[0]=true; f.put(Fixture::target+0x1ec8,1000);
      require(f.run() && approximately(f.result.damage,420.f),"helmet-eligible head damage uses native armor ratio after head/team scaling"); }
    { Fixture f; f.armored[0]=true; f.put(Fixture::target+0x1ec8,5);
      require(f.run() && approximately(f.result.damage,90.f),"insufficient armor subtracts twice available armor from scaled damage"); }
    { Fixture f; f.armored[0]=true; f.put(Fixture::target+0x1ec8,100);
      require(f.run() && approximately(f.result.damage,75.f),"sufficient armor applies half of weapon armor ratio"); }
    { Fixture f; f.interval(0,3.5f,2); require(f.run() && f.result.hit && approximately(f.result.damage,17.5f) && f.result.penetrations==2,"remaining interval damage replaces base weapon damage and remaining count maps to penetration usage"); }
    { Fixture f; f.interval(0,3.5f,2); require(f.run(false) && !f.result.hit,"disabled wall penetration rejects a penetrated hit"); }
    { Fixture f; f.interval(0,20.f,4); require(f.run(false) && f.result.hit,"nonpenetrated hit remains available with wall penetration disabled"); }
    { Fixture f; f.interval_count=2; f.interval(0,80.f,3,true); f.entities[0]=Fixture::other; f.interval(1,6.f,3); f.entities[1]=Fixture::target;
      require(f.run() && f.result.hit && approximately(f.result.damage,30.f) && f.calls("interval")==1,"material bit at+0x14 skips geometry interval while following impact uses its own damage"); }
    { Fixture f; f.interval_count=2; f.entities[0]=0; f.interval(1,9.f,4); f.entities[1]=Fixture::target;
      require(f.run() && f.result.hit && approximately(f.result.damage,45.f) && f.calls("interval")==2,"empty native impact does not hide later target interval"); }
    { Fixture f; f.entities[0]=Fixture::other; require(f.run() && !f.result.hit,"wrong entity at correct trace field cannot hit selected target"); }
    { Fixture f; f.interval_count=2; f.entities[0]=Fixture::geometry; f.interval(1,9.f,3); f.entities[1]=Fixture::target;
      require(f.run() && f.result.hit && approximately(f.result.damage,45.f),"world or prop boundary does not hide later target after native penetration"); }
    { Fixture f; f.other_is_player(); f.interval_count=2; f.entities[0]=Fixture::other; f.interval(1,9.f,3); f.entities[1]=Fixture::target;
      require(f.run() && !f.result.hit && f.calls("interval")==1,"another resolved player before selected target blocks authorization"); }
    { Fixture f; f.groups[0]=0; f.fail_read=Fixture::client+0x24bcaa0;
      require(f.run() && f.result.hit && approximately(f.result.damage,20.f),"generic impact does not require or use an unrelated body-scale cvar"); }
}

void invalid_data_tests() {
    for (int count:{-1,257}) { Fixture f; f.interval_count=count;
        require(!f.run() && f.calls("interval")==0 && f.calls("destruct")==1,"invalid interval count fails before native conversion with cleanup"); }
    for (int count:{-1,4097}) { Fixture f; f.record_count=count;
        require(!f.run() && f.calls("interval")==0 && f.calls("destruct")==1,"invalid native record count fails before conversion with cleanup"); }
    { Fixture f; store<std::uint16_t>(f.intervals[0].data(),0x12,4);
      require(!f.run() && f.calls("interval")==0 && f.calls("destruct")==1,"exit record index equal to native record count is rejected before native getter"); }
    { Fixture f; f.record_count=0;
      require(!f.run() && f.calls("interval")==0 && f.calls("destruct")==1,"nonempty interval cannot reference an empty native record vector"); }
    { Fixture f; f.fail_read=reinterpret_cast<std::uintptr_t>(f.records.data());
      require(!f.run() && f.calls("interval")==0 && f.calls("destruct")==1,"unreadable native exit record is rejected before getter"); }
    { Fixture f; f.override_record_pointer=true; f.record_pointer_override=0;
      require(!f.run() && f.calls("interval")==0 && f.calls("destruct")==1,"null native record vector fails closed"); }
    for (auto pointer:{std::uintptr_t{0},std::uintptr_t{0x1234},std::uintptr_t{0xffffffffffffffffULL}}) {
        Fixture f; f.override_interval_pointer=true; f.interval_pointer_override=pointer;
        require(!f.run() && f.calls("interval")==0 && f.calls("destruct")==1,"invalid interval pointers fail closed with cleanup");
    }
    for (float damage:{-1.f,10001.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        Fixture f; f.interval(0,damage,4); require(!f.run() && !f.result.hit && f.calls("destruct")==1,"invalid remaining damage cannot authorize target");
    }
    for (int remaining:{-1,5}) { Fixture f; f.interval(0,20.f,remaining);
        require(!f.run() && !f.result.hit && f.calls("destruct")==1,"invalid remaining penetration count fails closed"); }
    for (int group:{-1,9}) { Fixture f; f.groups[0]=group;
        require(!f.run() && !f.result.hit && f.calls("destruct")==1,"invalid native hitgroup fails closed"); }
    for (int team:{0,1,4}) { Fixture f; f.put<std::uint8_t>(Fixture::target+dump_offsets::team,static_cast<std::uint8_t>(team));
        require(!f.run() && !f.result.hit && f.calls("destruct")==1,"unsupported target team cannot select a damage-scale cvar"); }
    for (int armor:{-1,1001}) { Fixture f; f.put(Fixture::target+0x1ec8,armor);
        require(!f.run() && !f.result.hit && f.calls("destruct")==1,"invalid target armor value fails closed"); }
    for (float scale:{-1.f,101.f,std::numeric_limits<float>::quiet_NaN()}) { Fixture f; f.put(Fixture::t_body+0x58,scale);
        require(!f.run() && !f.result.hit && f.calls("destruct")==1,"invalid current damage-scale value fails closed"); }
    { Fixture f; f.fail_read=Fixture::client+0x24bcaa0;
      require(!f.run() && !f.result.hit && f.calls("destruct")==1,"unreadable target-team cvar fails closed"); }
}

void parameter_tests() {
    { Fixture f; aim::BallisticParameters value;
      require(aim::parameters(Fixture::weapon,value) && value.damage==100 && approximately(value.head,4) && approximately(value.armor,1.5f) && approximately(value.penetration,2) && approximately(value.range,4096) && approximately(value.modifier,.98f),"current weapon+0x388 VData layout reads all six native ballistic parameters"); }
    for (const auto offset:{0x820u,0x824u,0x828u,0x82cu,0x830u,0x834u}) {
        Fixture f; f.fail_read=Fixture::vdata+offset; aim::BallisticParameters value;
        require(!aim::parameters(Fixture::weapon,value),"each unreadable native parameter rejects the ballistic profile");
    }
    for (int damage:{0,1000,-1}) { Fixture f; f.put(Fixture::vdata+0x820,damage); aim::BallisticParameters value;
        require(!aim::parameters(Fixture::weapon,value),"invalid VData damage rejects parameter profile"); }
    for (auto offset:{0x824u,0x828u,0x82cu,0x830u,0x834u}) {
        Fixture f; f.put(Fixture::vdata+offset,std::numeric_limits<float>::quiet_NaN()); aim::BallisticParameters value;
        require(!aim::parameters(Fixture::weapon,value),"non-finite native ballistic float rejects parameter profile");
    }
    for (float range:{0.f,-1.f,100001.f}) { Fixture f; f.put(Fixture::vdata+0x830,range); aim::BallisticParameters value;
        require(!aim::parameters(Fixture::weapon,value),"invalid weapon range rejects parameter profile"); }
    for (float modifier:{0.f,-1.f,1.01f}) { Fixture f; f.put(Fixture::vdata+0x834,modifier); aim::BallisticParameters value;
        require(!aim::parameters(Fixture::weapon,value),"invalid range modifier rejects parameter profile"); }
}
}

int main() {
    try {
        abi_and_lifetime_tests(); damage_and_mapping_tests(); invalid_data_tests(); parameter_tests();
        std::ofstream("aim-ballistics-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"owned native ABI stubs, lifetime cleanup, interval target/damage mapping, penetration and current hitgroup/armor math\"}\n";
        std::cout << "aim ballistics PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "aim ballistics FAIL: " << error.what() << '\n'; return 1; }
}
