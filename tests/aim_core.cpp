#include "aim_core.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace aim_core;
unsigned checks = 0;
void require(bool ok, const char* label) { ++checks; if (!ok) throw std::runtime_error(label); }
bool close(float a, float b, float epsilon = .0002f) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= epsilon; }
bool close(Vec3 a, Vec3 b, float epsilon = .0002f) { return close(a.x, b.x, epsilon) && close(a.y, b.y, epsilon) && close(a.z, b.z, epsilon); }
void angle_tests() {
    require(close(add({1,2,3}, {-4,5,-6}), {-3,7,-3}) && close(sub({1,2,3}, {-4,5,-6}), {5,-3,9}) && close(mul({1,-2,3}, 2), {2,-4,6}), "vector arithmetic");
    require(close(length({3,4,12}), 13) && std::isfinite(length({1e30f,1e30f,0})), "length handles ordinary and large vectors without squaring overflow");
    require(close(angle_to({10,20,30}, {20,20,30}), {0,0,0}) && close(angle_to({}, {0,1,0}), {0,90,0}), "translated eye and yaw axes");
    require(close(angle_to({}, {0,0,1}), {-90,0,0}) && close(angle_to({}, {0,0,-1}), {90,0,0}), "vertical targets preserve mathematical pitch before game normalization");
    require(close(normalize_angle({120,350,8}), {89,-10,0}) && close(normalize_angle({-120,-350,-8}), {-89,10,0}), "game pitch clamp, yaw wrap and roll removal");
    require(close(normalize_angle({0,1080,0}), {}) && close(std::abs(wrap_yaw(540)), 180), "multiple revolutions and boundary yaw");
    require(close(forward({0,90,0}), {0,1,0}) && close(forward({-90,0,0}), {0,0,1}), "forward vector agrees with yaw and pitch conventions");
    const Vec3 angle{31,-123,0}, eye{100,-40,80};
    const auto target = add(eye, mul(forward(angle), 200));
    require(close(angle_to(eye,target),angle,.0005f) && close(angular_error(angle,eye,target),0,.001f), "angles and direction invert around a translated eye");
    require(close(angular_error({}, {}, {1,0,0}),0) && close(angular_error({}, {}, {-1,0,0}),180) && close(angular_error({}, {}, {0,0,1}),90), "angular cone front, rear and vertical boundaries");
    require(close(angular_error({0,179,0}, {}, forward({0,-179,0})),2,.001f), "cone error crosses yaw seam without a 358 degree jump");
    require(close(angular_error({89,0,0}, {}, forward({89,180,0})),2,.001f), "cone measures direction near poles instead of Euler coordinate distance");
    require(close(angular_error({}, {}, {10,10,10}),54.73561f,.001f), "combined vertical and horizontal cone angle");
    require(!finite(angle_to({},{})) && std::isinf(angular_error({}, {}, {})), "coincident target has no aim direction");
    const float largest = std::numeric_limits<float>::max();
    require(close(angle_to({-largest,0,0}, {largest,0,0}), {}) && close(angular_error({}, {-largest,0,0}, {largest,0,0}),0), "finite extreme coordinates do not overflow direction subtraction");
    require(!finite(angle_to({invalid_float,0,0}, {1,0,0})) && !finite(forward({0,infinity,0})) && !finite(normalize_angle({invalid_float,0,0})) && std::isinf(angular_error({}, {}, {invalid_float,0,0})), "non-finite aim data fails closed");
    require(close(smooth({0,170,0}, {20,-170,0},2), {10,180,0}) && close(smooth({0,-170,0},{0,170,0},2),{0,-180,0}), "smoothing takes shortest yaw path in either direction");
    require(close(smooth({}, {100,350,1},1), {89,-10,0}) && close(smooth({}, {10,20,0},0),{10,20,0}) && !finite(smooth({}, {}, invalid_float)), "smoothing clamps final pitch and validates factor");
}
void selection_tests() {
    const Candidate centered{{1,0,0},100,8,0,500,1}, near{{1,1,0},80,4,1,100,2}, weak{{1,1,1},5,9,2,800,3};
    require(better(centered,near,0) && better(near,centered,1) && better(weak,near,2), "priorities distinguish cone, distance and health");
    auto tie = centered; tie.index = 2;
    require(better(tie,centered,0) && better(tie,centered,1) && better(tie,centered,2) && !better(centered,tie,0), "entity index deterministically breaks equal target scores");
    auto wide = centered; wide.error = 4; wide.index = 1;
    require(better(centered,wide,1) && better(centered,wide,2), "angular error precedes index for distance and health ties");
    auto body = centered; body.hitbox = 1;
    require(better(centered,body,0) && !better(centered,centered,0), "equal entity targets choose stable hitbox and never outrank themselves");
    Candidate invalid;
    require(better(centered,invalid,0) && !better(invalid,centered,0), "empty candidate cannot become selected");
    invalid = centered; invalid.error = invalid_float;
    require(!better(invalid,centered,0), "NaN target score rejected");
    invalid = centered; invalid.point.z = infinity; invalid.error = 0;
    require(!better(invalid,centered,0), "invalid target geometry rejected despite a better score");
    invalid = centered; invalid.health = 0;
    require(!better(invalid,centered,2), "dead target rejected by lowest-health priority");
    auto points = std::vector<Candidate>{centered,near,weak,tie};
    for (int priority = 0; priority < 3; ++priority) {
        Candidate first;
        for (const auto& p : points) if (better(p,first,priority)) first=p;
        std::reverse(points.begin(),points.end()); Candidate reversed;
        for (const auto& p : points) if (better(p,reversed,priority)) reversed=p;
        require(first.index==reversed.index && first.hitbox==reversed.hitbox, "target choice independent from enumeration order");
    }
}
std::array<float,16> camera() {
    // Camera at the origin, looking along world +X; 90 degree horizontal FOV, 16:9 viewport.
    return {0,-1,0,0, 0,0,16.f/9,0, 1,0,0,-.1f, 1,0,0,0};
}
void projection_tests() {
    auto matrix = camera();
    const auto center = project(matrix,{10,0,0});
    require(center.valid && center.on_screen && close(center.x,.5f) && close(center.y,.5f) && close(center.horizontal_fov,90), "camera center and derived horizontal FOV");
    const auto edge = project(matrix,{10,-10,0});
    require(edge.on_screen && close(edge.x,1) && close(edge.y,.5f), "perspective right edge is normalized to one");
    const auto above = project(matrix,{10,0,2});
    require(above.on_screen && close(above.y,.3222222f) && above.y<.5f, "screen Y flips upward world displacement");
    const auto offscreen = project(matrix,{10,-11,0});
    require(offscreen.valid && !offscreen.on_screen && offscreen.x>1, "off-screen point keeps valid camera data");
    const auto behind = project(matrix,{-10,0,0});
    require(behind.valid && !behind.on_screen && !std::isfinite(behind.x) && close(behind.horizontal_fov,90), "behind-camera point never produces a marker");
    require(!project(matrix,{0,0,0}).on_screen && !project(matrix,{.00001f,0,0}).on_screen, "camera plane and near-zero W rejected");
    // Translating the camera by (10,20,30) must cancel the model-space translation.
    matrix[3]=20; matrix[7]=-30*matrix[6]; matrix[11]=-10.1f; matrix[15]=-10;
    const auto translated = project(matrix,{20,20,30});
    require(translated.valid && translated.on_screen && close(translated.x,.5f) && close(translated.y,.5f), "view-projection translation is included before perspective divide");
    matrix={1,0,0,0, 0,0,16.f/9,0, 0,1,0,-.1f, 0,1,0,0};
    const auto rotated=project(matrix,{5,10,0});
    require(rotated.valid && rotated.on_screen && close(rotated.x,.75f) && close(rotated.y,.5f) && close(rotated.horizontal_fov,90), "camera rotation preserves basis-derived FOV and screen position");
    matrix=camera(); for (auto& f:matrix) f*=7;
    const auto scaled=project(matrix,{10,-2,1}); const auto ordinary=project(camera(),{10,-2,1});
    require(scaled.valid && close(scaled.x,ordinary.x) && close(scaled.y,ordinary.y) && close(scaled.horizontal_fov,ordinary.horizontal_fov), "homogeneous scale does not change coordinates or FOV");
    matrix=camera(); matrix[1]=-2;
    require(close(project(matrix,{10,0,0}).horizontal_fov,53.130102f,.001f), "zoom FOV derives from camera row magnitudes");
    matrix=camera(); matrix[0]=.25f;
    require(!project(matrix,{10,0,0}).valid, "skew between forward and screen-right camera basis rejected");
    matrix=camera(); matrix[5]=.5f;
    require(!project(matrix,{10,0,0}).valid, "non-orthogonal screen axes rejected");
    require(!project({}, {1,0,0}).valid && !project({1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},{1,0,0}).valid, "zero and orthographic matrices have no perspective camera FOV");
    matrix=camera(); matrix[10]=invalid_float;
    require(!project(matrix,{10,0,0}).valid && !project(camera(),{infinity,0,0}).valid, "non-finite matrix or target rejects projection");
}
}
int main() {
    try {
        angle_tests(); selection_tests(); projection_tests();
        std::ofstream("aim-core-tests.json") << "{\"passed\":true,\"checks\":" << checks << ",\"scope\":\"pure aim geometry, cone selection, smoothing and perspective projection\"}\n";
        std::cout << "aim core PASS: " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "aim core FAIL: " << error.what() << '\n'; return 1; }
}
