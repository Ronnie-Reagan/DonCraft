#pragma once

#include "core/math.hpp"
#include "game/control_state.hpp"
#include "world/demo_world.hpp"

#include <array>

namespace df::game
{
class PlayerController
{
public:
    void Spawn(const world::DemoWorld& world);
    void PlaceAt(const Vec3& position, float yawRadians, float pitchRadians);
    void Tick(const ControlState& input, const world::DemoWorld& world, float dt);

    [[nodiscard]] auto Position() const -> Vec3
    {
        return position_;
    }

    [[nodiscard]] auto CameraPosition() const -> Vec3
    {
        return position_ + Vec3{0.0f, 0.55f, 0.0f};
    }

    [[nodiscard]] auto ForwardVector() const -> Vec3;
    [[nodiscard]] auto FlatForwardVector() const -> Vec3;
    [[nodiscard]] auto RightVector() const -> Vec3;
    [[nodiscard]] auto YawRadians() const -> float
    {
        return yawRadians_;
    }
    [[nodiscard]] auto PitchRadians() const -> float
    {
        return pitchRadians_;
    }
    [[nodiscard]] auto HorizontalSpeedMetersPerSecond() const -> float
    {
        return Length(Vec3{velocity_.x, 0.0f, velocity_.z});
    }
    [[nodiscard]] auto WalkCycleRadians() const -> float
    {
        return walkCycleRadians_;
    }
    [[nodiscard]] auto LeftFootPosition() const -> Vec3
    {
        return feet_[0].position;
    }
    [[nodiscard]] auto RightFootPosition() const -> Vec3
    {
        return feet_[1].position;
    }
    [[nodiscard]] auto LeftFootGrounded() const -> bool
    {
        return feet_[0].grounded;
    }
    [[nodiscard]] auto RightFootGrounded() const -> bool
    {
        return feet_[1].grounded;
    }
    [[nodiscard]] auto OnGround() const -> bool
    {
        return onGround_;
    }

    [[nodiscard]] auto MaterialUnderFeet(const world::DemoWorld& world) const -> world::MaterialId;

private:
    [[nodiscard]] auto CollisionHalfExtents() const -> Vec3
    {
        return {0.35f, 0.90f, 0.35f};
    }

    struct FootState
    {
        Vec3 position{};
        world::MaterialId material = world::MaterialId::Air;
        bool grounded = false;
    };

    Vec3 position_{};
    Vec3 velocity_{};
    float yawRadians_ = 0.0f;
    float pitchRadians_ = 0.0f;
    float walkCycleRadians_ = 0.0f;
    std::array<FootState, 2> feet_{};
    bool onGround_ = false;
};
}
