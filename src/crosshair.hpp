#pragma once
#include "config.hpp"
#include <array>

namespace crosshair {
enum class Kind { line, ring, dot };
struct Primitive { Kind kind; float x1, y1, x2, y2; };
struct Geometry { std::array<Primitive, 5> items{}; unsigned count = 0; };
inline bool visible(const config::Crosshair& c, bool menu, bool in_game, bool alive) {
    return c.enabled && !(menu && c.hide_in_menu) && (!c.only_in_game || in_game) && (!c.only_alive || alive);
}
inline Geometry geometry(const config::Crosshair& c, float x, float y) {
    Geometry result;
    const auto add = [&](Kind kind, float x1, float y1, float x2, float y2) { result.items[result.count++] = {kind, x1, y1, x2, y2}; };
    if (c.style == 1) add(Kind::dot, x, y, c.dot_radius, 0);
    else if (c.style == 2) add(Kind::ring, x, y, c.length, 0);
    else {
        add(Kind::line, x - c.gap - c.length, y, x - c.gap, y);
        add(Kind::line, x + c.gap, y, x + c.gap + c.length, y);
        if (c.style != 3) add(Kind::line, x, y - c.gap - c.length, x, y - c.gap);
        add(Kind::line, x, y + c.gap, x, y + c.gap + c.length);
    }
    if (c.center_dot && c.style != 1) add(Kind::dot, x, y, c.dot_radius, 0);
    return result;
}
}
