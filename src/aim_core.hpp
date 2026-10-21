#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace aim_core {
struct Vec3 { float x = 0, y = 0, z = 0; };
inline constexpr double pi = 3.1415926535897932384626433832795;
inline constexpr double radians = pi / 180.0;
inline constexpr double degrees = 180.0 / pi;
inline constexpr float infinity = std::numeric_limits<float>::infinity();
inline constexpr float invalid_float = std::numeric_limits<float>::quiet_NaN();
inline bool finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline Vec3 invalid() { return {invalid_float, invalid_float, invalid_float}; }
inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 mul(Vec3 a, float factor) { return {a.x * factor, a.y * factor, a.z * factor}; }
inline float length(Vec3 a) { return static_cast<float>(std::hypot(static_cast<double>(a.x), a.y, a.z)); }
inline float wrap_yaw(float yaw) {
    return std::isfinite(yaw) ? static_cast<float>(std::remainder(static_cast<double>(yaw), 360.0)) : invalid_float;
}
inline Vec3 normalize_angle(Vec3 angle) {
    if (!finite(angle)) return invalid();
    return {std::clamp(angle.x, -89.f, 89.f), wrap_yaw(angle.y), 0};
}
inline Vec3 angle_to(Vec3 eye, Vec3 point) {
    if (!finite(eye) || !finite(point)) return invalid();
    const double x = static_cast<double>(point.x) - eye.x;
    const double y = static_cast<double>(point.y) - eye.y;
    const double z = static_cast<double>(point.z) - eye.z;
    const double horizontal = std::hypot(x, y);
    if (horizontal == 0 && z == 0) return invalid();
    return {static_cast<float>(std::atan2(-z, horizontal) * degrees),
        static_cast<float>(std::atan2(y, x) * degrees), 0};
}
inline Vec3 forward(Vec3 angles) {
    if (!finite(angles)) return invalid();
    const double pitch = std::remainder(static_cast<double>(angles.x), 360.0) * radians;
    const double yaw = std::remainder(static_cast<double>(angles.y), 360.0) * radians;
    const double cp = std::cos(pitch);
    return {static_cast<float>(cp * std::cos(yaw)), static_cast<float>(cp * std::sin(yaw)), static_cast<float>(-std::sin(pitch))};
}
inline float angular_error(Vec3 view, Vec3 eye, Vec3 point) {
    if (!finite(view) || !finite(eye) || !finite(point)) return infinity;
    const double x = static_cast<double>(point.x) - eye.x;
    const double y = static_cast<double>(point.y) - eye.y;
    const double z = static_cast<double>(point.z) - eye.z;
    const double distance = std::hypot(x, y, z);
    if (!(distance > 0) || !std::isfinite(distance)) return infinity;
    const double pitch = std::remainder(static_cast<double>(view.x), 360.0) * radians;
    const double yaw = std::remainder(static_cast<double>(view.y), 360.0) * radians;
    const double dot = (std::cos(pitch) * std::cos(yaw) * x + std::cos(pitch) * std::sin(yaw) * y - std::sin(pitch) * z) / distance;
    return static_cast<float>(std::acos(std::clamp(dot, -1.0, 1.0)) * degrees);
}
inline Vec3 smooth(Vec3 current, Vec3 wanted, float factor) {
    if (!finite(current) || !finite(wanted) || !std::isfinite(factor)) return invalid();
    current = normalize_angle(current); wanted = normalize_angle(wanted);
    const float divisor = std::max(1.f, factor);
    return normalize_angle({current.x + (wanted.x - current.x) / divisor,
        current.y + wrap_yaw(wanted.y - current.y) / divisor, 0});
}
struct Candidate {
    Vec3 point;
    int health = 0, index = -1, hitbox = 0;
    float distance = infinity, error = infinity;
};
inline bool valid_candidate(const Candidate& a) {
    return finite(a.point) && a.health > 0 && a.index >= 0 && std::isfinite(a.distance) && a.distance >= 0 &&
        std::isfinite(a.error) && a.error >= 0 && a.error <= 180;
}
inline bool better(const Candidate& a, const Candidate& b, int priority) {
    if (!valid_candidate(a)) return false;
    if (!valid_candidate(b)) return true;
    if (priority == 1 && a.distance != b.distance) return a.distance < b.distance;
    if (priority == 2 && a.health != b.health) return a.health < b.health;
    if (a.error != b.error) return a.error < b.error;
    if (a.index != b.index) return a.index < b.index;
    return a.hitbox < b.hitbox;
}
struct Projection {
    bool valid = false, on_screen = false;
    float x = invalid_float, y = invalid_float, horizontal_fov = 0;
};
inline Projection project(const std::array<float, 16>& matrix, Vec3 point) {
    Projection result;
    if (!finite(point)) return result;
    for (const float value : matrix) if (!std::isfinite(value)) return result;
    const auto norm = [&](int row) { return std::hypot(static_cast<double>(matrix[row]), matrix[row + 1], matrix[row + 2]); };
    const double nx = norm(0), ny = norm(4), nw = norm(12);
    if (!(nx > 0 && ny > 0 && nw > 0)) return result; // Orthographic/degenerate transforms have no perspective FOV.
    const auto orthogonal = [&](int a, int b, double na, double nb) {
        const double dot = static_cast<double>(matrix[a]) * matrix[b] + static_cast<double>(matrix[a + 1]) * matrix[b + 1] +
            static_cast<double>(matrix[a + 2]) * matrix[b + 2];
        return std::abs(dot / na / nb) <= .001;
    };
    if (!orthogonal(0, 4, nx, ny) || !orthogonal(0, 12, nx, nw) || !orthogonal(4, 12, ny, nw)) return result;
    const float fov = static_cast<float>(2 * std::atan(nw / nx) * degrees);
    if (!std::isfinite(fov) || !(fov > 0 && fov < 180)) return result;
    result.valid = true; result.horizontal_fov = fov;
    const auto clip = [&](int row) {
        return static_cast<double>(matrix[row]) * point.x + static_cast<double>(matrix[row + 1]) * point.y +
            static_cast<double>(matrix[row + 2]) * point.z + matrix[row + 3];
    };
    const double w = clip(12);
    if (!(w > nw * .0001)) return result; // Near-plane tolerance remains unchanged by homogeneous matrix scaling.
    const double x = .5 * (1 + clip(0) / w), y = .5 * (1 - clip(4) / w);
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<float>::max() || std::abs(y) > std::numeric_limits<float>::max()) return result;
    result.x = static_cast<float>(x); result.y = static_cast<float>(y);
    result.on_screen = x >= 0 && x <= 1 && y >= 0 && y <= 1;
    return result;
}
}
