#pragma once
#include "aim_core.hpp"
#include <cstdint>

namespace aim_spread {
// An independent stream for local sampling. This never seeds or consumes the
// game's global RNG and does not claim to predict its exact per-shot seed.
class Rng {
    std::uint32_t state;
public:
    explicit Rng(std::uint32_t seed) : state(seed ? seed : 0x6d2b79f5U) {}
    float uniform() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return static_cast<float>(state >> 8) * 0x1p-24f;
    }
};
struct Offset { float x = 0, y = 0; };
inline bool finite(Offset a) { return std::isfinite(a.x) && std::isfinite(a.y); }
inline Offset invalid() { return {aim_core::invalid_float, aim_core::invalid_float}; }

// Native single-bullet radial transforms. Shotgun pellet patterns require their
// separate native pattern model and are outside this sampler's contract.
inline Offset sample(Rng& random, int item, int mode, float recoil, float inaccuracy, float spread,
    bool maxInaccuracy = false, bool onlyUp = false) {
    if (!std::isfinite(recoil) || recoil < 0 || !std::isfinite(inaccuracy) || inaccuracy < 0 || !std::isfinite(spread) || spread < 0)
        return invalid();
    const auto radius = [&](float value) {
        if (item == 64 && mode == 1) value = 1 - value * value;
        else if (item == 28 && recoil < 3) {
            int i = 3;
            do { --i; value *= value; } while (static_cast<float>(i) > recoil);
            value = 1 - value;
        }
        return maxInaccuracy ? 1.f : value;
    };
    const float radiusA = radius(random.uniform());
    float angleA = random.uniform() * static_cast<float>(2 * aim_core::pi);
    const float radiusB = radius(random.uniform());
    float angleB = random.uniform() * static_cast<float>(2 * aim_core::pi);
    if (onlyUp) angleA = angleB = static_cast<float>(aim_core::pi / 2);
    const double x = static_cast<double>(std::cos(angleA)) * radiusA * inaccuracy + static_cast<double>(std::cos(angleB)) * radiusB * spread;
    const double y = static_cast<double>(std::sin(angleA)) * radiusA * inaccuracy + static_cast<double>(std::sin(angleB)) * radiusB * spread;
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<float>::max() || std::abs(y) > std::numeric_limits<float>::max()) return invalid();
    return {static_cast<float>(x), static_cast<float>(y)};
}

inline aim_core::Vec3 direction(aim_core::Vec3 angles, Offset offset) {
    if (!aim_core::finite(angles) || !finite(offset)) return aim_core::invalid();
    const auto forward = aim_core::forward(angles);
    const double pitch = std::remainder(static_cast<double>(angles.x), 360.0) * aim_core::radians;
    const double yaw = std::remainder(static_cast<double>(angles.y), 360.0) * aim_core::radians;
    const double sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
    // Source right=(sinYaw,-cosYaw,0), up=(sinPitch*cosYaw,sinPitch*sinYaw,cosPitch).
    // The inspected bullet routine uses forward MINUS right*x PLUS up*y.
    const double x = forward.x - sy * offset.x + sp * cy * offset.y;
    const double y = forward.y + cy * offset.x + sp * sy * offset.y;
    const double z = forward.z + cp * offset.y;
    const double magnitude = std::hypot(x, y, z);
    if (!(magnitude > 0) || !std::isfinite(magnitude)) return aim_core::invalid();
    return {static_cast<float>(x / magnitude), static_cast<float>(y / magnitude), static_cast<float>(z / magnitude)};
}
}
