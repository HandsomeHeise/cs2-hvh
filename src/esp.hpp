#pragma once
#include "aim_core.hpp"
#include "config.hpp"
#include "game_data.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace esp {
using aim_core::Vec3;
inline constexpr std::array<const char*,17> bone_names{"head_0","neck_0","spine_2","spine_1","pelvis",
    "arm_upper_L","arm_lower_L","hand_L","arm_upper_R","arm_lower_R","hand_R",
    "leg_upper_L","leg_lower_L","ankle_L","leg_upper_R","leg_lower_R","ankle_R"};
inline constexpr std::array<std::array<unsigned,2>,16> bone_links{{{0,1},{1,2},{2,3},{3,4},
    {1,5},{5,6},{6,7},{1,8},{8,9},{9,10},{4,11},{11,12},{12,13},{4,14},{14,15},{15,16}}};
struct Player {
    Vec3 feet{}, head{};
    std::uint32_t handle=0xffffffff;
    int health=0, team=0;
    bool teammate=false, visible=false;
    float distance=0;
    std::string name, weapon;
    std::array<Vec3,bone_names.size()> bones{};
    std::uint32_t bone_mask=0;
    int ammo=-1,reserve=-1,max_clip=-1,armor=-1;
    bool helmet=false,kit=false,scoped=false,reloading=false,flashed=false,defusing=false,immune=false;
};
struct Frame {
    bool supported=false, active=false;
    std::uintptr_t local=0, network=0, system=0;
    std::uint64_t sampled_at=0;
    float work_ms=0;
    unsigned bone_queries=0,skeleton_players=0;
    bool skeleton_limited=false;
    std::array<float,16> matrix{};
    std::vector<Player> players;
    std::string detail="Waiting for verified game input";
};
struct NativeApi {
    bool (*bone)(std::uintptr_t,const char*,Vec3&)=nullptr;
    bool (*visible)(std::uintptr_t,std::uintptr_t,std::uintptr_t,const Vec3&,const Vec3&)=nullptr;
    bool (*healthy)()=nullptr;
    std::uint64_t (*clock_us)()=nullptr; // Optional deterministic clock for owned fixtures.
};
struct Box { bool valid=false; float left=0,top=0,right=0,bottom=0; };
struct Segment { bool valid=false; float x1=0,y1=0,x2=0,y2=0; };
Box project_box(const std::array<float,16>&,const Player&,float width,float height);
Segment project_segment(const std::array<float,16>&,const Player&,unsigned first,unsigned second,float width,float height);
std::string ammo_text(const Player&);
std::vector<std::string> status_labels(const Player&,const config::Esp&);
Frame collect(const game::Sources&,const NativeApi&,const config::Esp&,std::uint64_t now);
bool fresh(const Frame&,std::uint64_t now);
void configure(const config::Esp&);
void on_input(const game::Sources&,const NativeApi&,bool active,bool menu_open,bool verified);
Frame latest(); // Pure memory reads only; refreshes the camera matrix for this render.
void reset();
}
