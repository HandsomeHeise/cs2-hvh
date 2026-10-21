#pragma once
#include "hvh_api.h"
#include "game_data.hpp"
#include <string>

namespace menu {
void initialize(const std::wstring& directory);
void draw(bool& visible, const HvhStats& stats, const game::Snapshot& data);
void draw_watermark();
void draw_overlay(const game::Snapshot& data, bool menu_visible);
}
