#pragma once

#include "core/math.hpp"
#include "game/control_state.hpp"
#include "render/frame_data.hpp"
#include "world/demo_world.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace df::game
{
class TruckController
{
public:
    void Reset(const world::DemoWorld& world);
    void Tick(const ControlState& input, world::DemoWorld& world, float dt, bool occupied);

    [[nodiscard]] auto Position() const -> Vec3
    {
        return position_;
    }

    [[nodiscard]] auto ForwardVector() const -> Vec3;
    [[nodiscard]] auto DriverSeatPosition() const -> Vec3;
    [[nodiscard]] auto CameraPosition() const -> Vec3;
    [[nodiscard]] auto ExitPosition() const -> Vec3;
    [[nodiscard]] auto CanEnter(const Vec3& playerPosition) const -> bool;
    [[nodiscard]] auto SpeedMetersPerSecond() const -> float;
    [[nodiscard]] auto EngineLoad() const -> float
    {
        return engineLoad_;
    }
    [[nodiscard]] auto AverageSink() const -> float
    {
        return averageSink_;
    }
    [[nodiscard]] auto ContactMaterial() const -> world::MaterialId
    {
        return dominantMaterial_;
    }

    void AppendModelTriangles(std::vector<render::ColorVertex3D>& triangles, std::vector<render::ColorVertex3D>& translucentTriangles) const;
    void AppendDebugLines(std::vector<render::ColorVertex3D>& lines) const;

private:
    struct WheelContact
    {
        bool hit = false;
        Vec3 point{};
        world::MaterialId material = world::MaterialId::Air;
    };

    [[nodiscard]] auto RightVector() const -> Vec3;
    [[nodiscard]] auto WorldOffset(const Vec3& localOffset) const -> Vec3;
    [[nodiscard]] auto VisualBodyRightVector() const -> Vec3;
    [[nodiscard]] auto VisualBodyUpVector() const -> Vec3;
    [[nodiscard]] auto VisualBodyForwardVector() const -> Vec3;
    [[nodiscard]] auto BodyHalfExtents() const -> Vec3
    {
        return {0.82f, 0.58f, 1.58f};
    }
    [[nodiscard]] auto WheelLocalOffsets() const -> std::array<Vec3, 4>;
    void SampleGroundContacts(const world::DemoWorld& world);
    void ApplyWheelRuts(world::DemoWorld& world, float throttleMagnitude);

    Vec3 position_{};
    Vec3 velocity_{};
    float yawRadians_ = 0.0f;
    float steering_ = 0.0f;
    float bodyPitchRadians_ = 0.0f;
    float bodyRollRadians_ = 0.0f;
    float engineLoad_ = 0.0f;
    float averageSink_ = 0.0f;
    bool grounded_ = false;
    float rutTravelAccumulator_ = 0.0f;
    world::MaterialId dominantMaterial_ = world::MaterialId::Air;
    std::array<WheelContact, 4> wheelContacts_{};
    std::array<float, 4> wheelSpinRadians_{};
    std::array<float, 4> wheelSuspensionOffset_{};
};
}
