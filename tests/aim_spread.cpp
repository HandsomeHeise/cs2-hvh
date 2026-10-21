#include "aim_spread.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void require(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
bool close(float a, float b, float tolerance = .00001f) { return std::isfinite(a) && std::isfinite(b) && std::abs(a-b) <= tolerance; }
bool close(aim_core::Vec3 a, aim_core::Vec3 b, float tolerance = .00001f) {
    return close(a.x,b.x,tolerance) && close(a.y,b.y,tolerance) && close(a.z,b.z,tolerance);
}
float radius(aim_spread::Offset value) { return std::hypot(value.x,value.y); }
float mean_radius(int item,int mode,float recoil) {
    aim_spread::Rng random(0x96b4712d);
    double sum=0;
    constexpr int count=40000;
    for (int i=0;i<count;++i) sum+=radius(aim_spread::sample(random,item,mode,recoil,1,0));
    return static_cast<float>(sum/count);
}
void rng_tests() {
    aim_spread::Rng a(0), b(0), independent(7);
    bool same=true, bounded=true, varied=false;
    float previous=-1;
    for (int i=0;i<20000;++i) {
        const float value=a.uniform(); independent.uniform(); independent.uniform();
        same &= value==b.uniform(); bounded &= value>=0 && value<1;
        varied |= previous>=0 && value!=previous; previous=value;
    }
    require(same && varied,"zero seed is deterministic, non-stuck and independent from other RNG streams");
    require(bounded,"uniform samples remain within the half-open unit interval");
    aim_spread::Rng first(12345), reference(12345), other(12346);
    bool different=false;
    for (int i=0;i<32;++i) {
        const auto s=aim_spread::sample(first,7,0,0,.03f,.02f);
        other.uniform();
        const auto r=aim_spread::sample(reference,7,0,0,.03f,.02f);
        require(s.x==r.x && s.y==r.y,"equal local seeds produce equal spread offsets despite unrelated draws");
        different |= s.x!=aim_spread::sample(other,7,0,0,.03f,.02f).x;
    }
    require(different,"distinct seeds produce different streams");
}
void offset_tests() {
    aim_spread::Rng random(42);
    const auto zero=aim_spread::sample(random,7,0,0,0,0);
    require(zero.x==0 && zero.y==0,"zero inaccuracy and spread give a zero offset");
    const auto upward=aim_spread::sample(random,7,0,0,.3f,.2f,true,true);
    require(close(upward.x,0) && close(upward.y,.5f),"debug maximum radius and only-up angles combine both components");
    const auto revolver=aim_spread::sample(random,64,1,0,1,0,true,true);
    const auto negev=aim_spread::sample(random,28,0,0,1,0,true,true);
    require(close(revolver.x,0) && close(revolver.y,1) && close(negev.y,1),"maximum-debug override follows weapon radial transforms");
    bool bounded=true, normalized=true;
    for (int item:{7,28,64}) for (int i=0;i<5000;++i) {
        const auto offset=aim_spread::sample(random,item,item==64 ? 1 : 0,0,.3f,.2f);
        bounded &= aim_spread::finite(offset) && radius(offset)<=.50001f;
        const auto direction=aim_spread::direction({37,-128,0},offset);
        normalized &= aim_core::finite(direction) && close(aim_core::length(direction),1,.00001f);
    }
    require(bounded,"generic, Negev and Revolver offsets stay inside sum-of-radii bound");
    require(normalized,"spread directions remain unit length across transformed samples");
    aim_spread::Rng debug(9), ordinary(9);
    aim_spread::sample(debug,28,0,1,.03f,.02f,true,true);
    aim_spread::sample(ordinary,28,0,1,.03f,.02f);
    require(debug.uniform()==ordinary.uniform(),"debug overrides retain the same local random draw consumption");
    require(close(mean_radius(7,0,0),.5f,.015f),"native generic radial sampling is uniform in radius rather than sqrt-uniform in area");
    require(close(mean_radius(64,1,0),2.f/3,.015f) && close(mean_radius(64,0,0),.5f,.015f),"Revolver secondary mode uses one minus squared radius; primary mode stays generic");
    require(close(mean_radius(28,0,0),8.f/9,.015f) && close(mean_radius(28,0,1),4.f/5,.015f) && close(mean_radius(28,0,2),2.f/3,.015f),"Negev recoil stages apply three, two and one radius squarings");
    require(close(mean_radius(28,0,3),.5f,.015f),"Negev recoil index three returns to generic radial sampling");
    aim_spread::Rng fractional(111), integral(111);
    const auto one=aim_spread::sample(fractional,28,0,1.5f,1,0), two=aim_spread::sample(integral,28,0,1,1,0);
    require(one.x==two.x && one.y==two.y,"fractional Negev recoil obeys the native loop comparison");
}
void direction_tests() {
    constexpr float diagonal=.7071067811865475f;
    require(close(aim_spread::direction({0,0,0},{}),{1,0,0}) && close(aim_spread::direction({30,70,0},{}),aim_core::forward({30,70,0})),"zero offset preserves aim forward vector");
    require(close(aim_spread::direction({}, {1,0}),{diagonal,diagonal,0}),"native positive offset X uses minus right, toward world plus Y at yaw zero");
    require(close(aim_spread::direction({}, {0,1}),{diagonal,0,diagonal}),"native positive offset Y raises the shot");
    require(close(aim_spread::direction({0,90,0},{1,0}),{-diagonal,diagonal,0}),"horizontal spread rotates with camera yaw");
    require(close(aim_spread::direction({45,0,0},{0,1}),{1,0,0}),"vertical spread uses the pitch-dependent up basis");
    require(close(aim_spread::direction({0,450,0},{1,0}),aim_spread::direction({0,90,0},{1,0})),"direction is periodic across yaw revolutions");
}
void invalid_tests() {
    const float invalid=std::numeric_limits<float>::quiet_NaN(), infinity=std::numeric_limits<float>::infinity();
    aim_spread::Rng random(1), untouched(1);
    require(!aim_spread::finite(aim_spread::sample(random,7,0,invalid,.1f,.1f)) &&
        !aim_spread::finite(aim_spread::sample(random,7,0,0,infinity,.1f)) &&
        !aim_spread::finite(aim_spread::sample(random,7,0,0,.1f,-.1f)) &&
        !aim_spread::finite(aim_spread::sample(random,28,0,-1,.1f,.1f)),"non-finite or negative radial inputs are rejected");
    require(random.uniform()==untouched.uniform(),"invalid inputs consume no local RNG state");
    require(!aim_core::finite(aim_spread::direction({invalid,0,0},{})) && !aim_core::finite(aim_spread::direction({}, {0,infinity})),"invalid angle or offset cannot become a shot direction");
}
}
int main() {
    try {
        rng_tests(); offset_tests(); direction_tests(); invalid_tests();
        std::ofstream("aim-spread-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"independent local RNG, native single-bullet spread transforms, direction basis and invalid input handling\"}\n";
        std::cout << "aim spread PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "aim spread FAIL: " << error.what() << '\n'; return 1; }
}
