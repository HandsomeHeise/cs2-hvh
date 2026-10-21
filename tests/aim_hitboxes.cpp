// Native hitbox fixtures own every descriptor, transform and geometry byte.
// No game attachment, game function, hook or desktop input is used.
#include <windows.h>
#include "../src/aim.cpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using aim_core::Vec3;
unsigned checks=0;
void require(bool ok,const char* text) { ++checks; if (!ok) throw std::runtime_error(text); }
bool approximately(float a,float b) { return std::isfinite(a) && std::abs(a-b)<.001f; }
bool approximately(Vec3 a,Vec3 b) { return approximately(a.x,b.x)&&approximately(a.y,b.y)&&approximately(a.z,b.z); }
struct Descriptor { std::uintptr_t hitbox; int mesh, padding; };
static_assert(sizeof(Descriptor)==16);
struct Fixture;
Fixture* current=nullptr;
int query_stub(void*,void*,void*,std::uint32_t);
void free_stub(void*);

struct Fixture {
    static constexpr std::uintptr_t node=0x10000000;
    std::array<Descriptor,128> descriptors{};
    alignas(16) std::array<aim::BoneTransform,128> transforms{};
    std::array<std::array<unsigned char,0x70>,128> records{};
    struct Output { std::uint64_t before=0x1234567812345678ULL; std::array<aim::TargetPoint,256> points{}; std::uint64_t after=0x8765432187654321ULL; } output;
    std::size_t count=99;
    int native_count=1, descriptor_count=1, transform_count=1, descriptor_capacity=128, transform_capacity=128;
    std::uint32_t descriptor_flags=0, transform_flags=0;
    unsigned queries=0;
    bool arguments_valid=true, raise_before=false, raise_after=false, fail_free=false, null_descriptors=false, null_transforms=false;
    std::uintptr_t fail_read=0;
    std::vector<std::uintptr_t> freed;
    template<class T> void field(unsigned index,std::size_t offset,T value) { std::memcpy(records[index].data()+offset,&value,sizeof(value)); }
    void geometry(unsigned index,std::uint8_t shape,Vec3 low,Vec3 high,float radius,int group=1) {
        field(index,0x18,low); field(index,0x24,high); field(index,0x30,radius); field(index,0x38,group); field(index,0x3c,shape);
    }
    static bool read(void* user,std::uintptr_t address,void* destination,std::size_t bytes) {
        auto& f=*static_cast<Fixture*>(user);
        if (address==f.fail_read) return false;
        for (const auto region:{std::pair{reinterpret_cast<std::uintptr_t>(f.descriptors.data()),sizeof(f.descriptors)},
                               std::pair{reinterpret_cast<std::uintptr_t>(f.transforms.data()),sizeof(f.transforms)},
                               std::pair{reinterpret_cast<std::uintptr_t>(f.records.data()),sizeof(f.records)}})
            if (address>=region.first && address-region.first<=region.second && bytes<=region.second-(address-region.first)) {
                std::memcpy(destination,reinterpret_cast<const void*>(address),bytes); return true;
            }
        return false;
    }
    Fixture() {
        current=this; aim::native_fault=false;
        aim::sources.memory={read,this}; aim::get_hitboxes=query_stub; aim::free_hitboxes=free_stub;
        for (unsigned i=0;i<128;++i) {
            descriptors[i]={reinterpret_cast<std::uintptr_t>(records[i].data()),static_cast<int>(i%3),0};
            transforms[i]={{0,0,0},1,0,0,0,1};
            geometry(i,1,{0,0,0},{0,0,0},2);
        }
    }
    bool run(int mask=31,float scale=.5f) { return aim::hitbox_points_native(node,mask,scale,output.points,count); }
    bool freed_both() const {
        return freed.size()==2 && freed[0]==reinterpret_cast<std::uintptr_t>(descriptors.data()) && freed[1]==reinterpret_cast<std::uintptr_t>(transforms.data());
    }
    ~Fixture() { aim::sources={}; aim::get_hitboxes=nullptr; aim::free_hitboxes=nullptr; current=nullptr; }
};
int query_stub(void* node,void* descriptor_output,void* transform_output,std::uint32_t hash) {
    auto& f=*current; ++f.queries;
    const auto* d=static_cast<const aim::HitboxVector*>(descriptor_output);
    const auto* t=static_cast<const aim::TransformVector*>(transform_output);
    const auto zero=[](const void* p,std::size_t size) { const auto* bytes=static_cast<const unsigned char*>(p); return std::all_of(bytes,bytes+size,[](unsigned char v){return v==0;}); };
    f.arguments_valid &= reinterpret_cast<std::uintptr_t>(node)==Fixture::node && hash==0 &&
        !(reinterpret_cast<std::uintptr_t>(transform_output)&15) && zero(d,sizeof(*d)) && zero(t,sizeof(*t));
    if (f.raise_before) RaiseException(0xe0424001,0,0,nullptr);
    auto* dw=static_cast<aim::HitboxVector*>(descriptor_output); auto* tw=static_cast<aim::TransformVector*>(transform_output);
    dw->count=f.descriptor_count; dw->data=f.null_descriptors?0:reinterpret_cast<std::uintptr_t>(f.descriptors.data()); dw->capacity=f.descriptor_capacity; dw->flags=f.descriptor_flags;
    tw->count=f.transform_count; tw->data=f.null_transforms?0:reinterpret_cast<std::uintptr_t>(f.transforms.data()); tw->capacity=f.transform_capacity; tw->flags=f.transform_flags;
    if (f.raise_after) RaiseException(0xe0424002,0,0,nullptr);
    return f.native_count;
}
void free_stub(void* pointer) {
    auto& f=*current; const auto address=reinterpret_cast<std::uintptr_t>(pointer);
    f.arguments_valid &= address==reinterpret_cast<std::uintptr_t>(f.descriptors.data()) || address==reinterpret_cast<std::uintptr_t>(f.transforms.data());
    f.freed.push_back(address);
    if (f.fail_free) RaiseException(0xe0424003,0,0,nullptr);
}

void geometry_tests() {
    { Fixture f; f.geometry(0,1,{4,5,6},{100,200,300},2);
      require(f.run() && f.count==7 && approximately(f.output.points[0].point,{4,5,6}),"sphere uses native minimum as center rather than midpoint of unused maximum");
      require(approximately(f.output.points[1].point,{5,5,6}) && approximately(f.output.points[2].point,{3,5,6}),"sphere radial samples use actual radius and requested scale");
      require(f.freed_both() && f.arguments_valid,"query receives zeroed aligned native headers and current-set hash, then frees exactly both arrays"); }
    { Fixture f; f.geometry(0,2,{-2,-4,-6},{2,4,6},3); f.transforms[0]={{100,200,300},2,0,0,0,1};
      require(f.run() && f.count==9 && approximately(f.output.points[0].point,{100,200,300}),"capsule midpoint is transformed through real world position and scale");
      require(approximately(f.output.points[1].point,{103,200,300}) && approximately(f.output.points[7].point,{102,204,306}) && approximately(f.output.points[8].point,{98,196,294}),"capsule radial and segment samples retain native geometry"); }
    { Fixture f; f.geometry(0,1,{1,0,0},{1,0,0},2); const float q=std::sqrt(.5f); f.transforms[0]={{10,20,30},2,0,0,q,q};
      require(f.run() && approximately(f.output.points[0].point,{10,22,30}) && approximately(f.output.points[1].point,{10,24,30}),"quaternion rotates scaled hitbox samples once into world space"); }
    { Fixture f; f.geometry(0,0,{-2,-4,-6},{2,4,6},0,3);
      require(f.run(4,1) && f.count==7 && approximately(f.output.points[1].point,{2,0,0}) && approximately(f.output.points[5].point,{0,0,6}) && f.output.points[0].bit==4,"box samples use bounds extents and stomach selection bit"); }
    { Fixture f; f.geometry(0,0,{2,3,4},{2,3,4},0);
      require(f.run() && f.count==1 && approximately(f.output.points[0].point,{2,3,4}),"degenerate box produces only its actual point"); }
    { Fixture f; f.geometry(0,2,{-2,0,0},{2,0,0},0);
      require(f.run() && f.count==3 && approximately(f.output.points[1].point,{1,0,0}) && approximately(f.output.points[2].point,{-1,0,0}),"zero-radius capsule retains bounded axis samples"); }
    { Fixture f; f.native_count=f.descriptor_count=f.transform_count=9;
      const int groups[]{1,2,8,3,4,5,6,7,0};
      for (unsigned i=0;i<9;++i) f.geometry(i,0,{0,0,0},{0,0,0},0,groups[i]);
      require(f.run() && f.count==8,"hitgroup mask maps only supported head/chest/stomach/arm/leg groups");
      const int bits[]{1,2,2,4,8,8,16,16}; for (unsigned i=0;i<8;++i) require(f.output.points[i].bit==bits[i],"native hitgroups preserve requested selection categories"); }
    { Fixture f; f.native_count=f.descriptor_count=f.transform_count=128;
      require(f.run() && f.count==256 && f.output.before==0x1234567812345678ULL && f.output.after==0x8765432187654321ULL && f.freed_both(),"large native lists remain bounded by output capacity without overwriting guards"); }
}

void failure_and_cleanup_tests() {
    for (unsigned invalid=0;invalid<8;++invalid) {
        Fixture f;
        switch(invalid) {
        case 0: f.native_count=0; break;
        case 1: f.native_count=2; break;
        case 2: f.descriptor_count=-1; break;
        case 3: f.native_count=f.descriptor_count=f.transform_count=129; break;
        case 4: f.transform_count=0; break;
        case 5: f.descriptor_capacity=0; break;
        case 6: f.transform_capacity=0; break;
        case 7: f.descriptors[0].hitbox=0; break;
        }
        require(!f.run() && f.count==0 && f.freed_both(),"failed native return/header/count/record validation discards output and frees both allocations");
    }
    { Fixture f; f.null_descriptors=true;
      require(!f.run() && f.count==0 && f.freed.size()==1 && f.freed[0]==reinterpret_cast<std::uintptr_t>(f.transforms.data()),"null descriptor allocation never frees null and still frees transform allocation"); }
    { Fixture f; f.descriptor_flags=0x80000000u; f.transform_flags=0x40000000u;
      require(f.run() && f.freed.empty(),"native external/fixed allocation flags prohibit freeing borrowed vector buffers"); }
    for (unsigned invalid=0;invalid<8;++invalid) {
        Fixture f;
        switch(invalid) {
        case 0: f.field(0,0x30,-1.f); break;
        case 1: f.field(0,0x30,std::numeric_limits<float>::quiet_NaN()); break;
        case 2: f.field(0,0x18,Vec3{600,0,0}); break;
        case 3: f.transforms[0].scale=0; break;
        case 4: f.transforms[0].w=2; break;
        case 5: f.transforms[0].position.x=std::numeric_limits<float>::infinity(); break;
        case 6: f.geometry(0,0,{2,0,0},{-2,0,0},0); break;
        case 7: f.fail_read=reinterpret_cast<std::uintptr_t>(f.records[0].data())+0x30; break;
        }
        require(!f.run() && f.count==0 && f.freed_both() && !aim::native_fault,"invalid geometry/transforms/read failures fail closed with balanced native cleanup");
    }
    { Fixture f; f.native_count=f.descriptor_count=f.transform_count=2; f.field(1,0x30,-1.f);
      require(!f.run() && f.count==0 && f.freed_both(),"failure after previously sampled hitbox never publishes partial geometry"); }
    for (unsigned invalid=0;invalid<6;++invalid) {
        Fixture f; const int mask=invalid==0?0:invalid==1?32:31;
        const float scale=invalid==2?0.f:invalid==3?1.1f:invalid==4?std::numeric_limits<float>::quiet_NaN():invalid==5?std::numeric_limits<float>::infinity():.5f;
        require(!f.run(mask,scale) && f.count==0 && f.queries==0 && f.freed.empty(),"invalid masks/scales reject before native allocation");
    }
    for (bool after:{false,true}) {
        Fixture f; f.raise_before=!after; f.raise_after=after;
        require(!f.run() && f.count==0 && aim::native_fault && (after?f.freed_both():f.freed.empty()),"native exception before/after output allocation latches fault and cleans completed vector allocations");
    }
    { Fixture f; f.fail_free=true;
      require(!f.run() && f.count==0 && aim::native_fault && f.freed.size()==1,"native allocator fault invalidates output and stops uncertain native cleanup"); }
}
}

int main() {
    try {
        geometry_tests(); failure_and_cleanup_tests();
        std::ofstream("aim-hitboxes-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"owned native hitbox geometry, aligned vector ABI, allocator flags, bounds and exception cleanup\"}\n";
        std::cout << "aim hitboxes PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "aim hitboxes FAIL: " << error.what() << '\n'; return 1; }
}
