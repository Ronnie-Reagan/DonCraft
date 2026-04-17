#pragma once

#include "game/control_state.hpp"

#include <cstdint>

namespace df::game
{
enum class SessionMode : std::uint8_t
{
    Offline = 0,
    ListenHost = 1,
    Client = 2,
    DedicatedServer = 3,
};

enum class ToolType : std::uint8_t
{
    Rifle = 0,
    Grenade = 1,
    Dig = 2,
};

using PlayerId = std::uint32_t;
inline constexpr PlayerId kInvalidPlayerId = 0u;

struct PlayerCommandFrame
{
    std::uint32_t sequence = 0;
    ControlState control{};
    ToolType selectedTool = ToolType::Rifle;
    bool primaryDown = false;
    bool primaryPressed = false;
    bool secondaryDown = false;
    bool quickGrenadePressed = false;
    bool interactPressed = false;
};
}
