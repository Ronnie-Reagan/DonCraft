#pragma once

namespace df::game
{
struct ControlState
{
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool sprint = false;
    bool jumpPressed = false;
    float lookYawDelta = 0.0f;
    float lookPitchDelta = 0.0f;
};
}
