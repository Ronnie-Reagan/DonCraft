#include "game/truck_controller.hpp"

#include "game/model_primitives.hpp"
#include "world/material_properties.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace df::game
{
namespace
{
auto MoveToward(const float current, const float target, const float maxDelta) -> float
{
    if (current < target)
    {
        return std::min(current + maxDelta, target);
    }

    return std::max(current - maxDelta, target);
}

auto WrapAngle(const float radians) -> float
{
    return std::remainder(radians, kPi * 2.0f);
}

auto TryStepMove(
    const world::DemoWorld& world,
    Vec3& position,
    const Vec3& moveDelta,
    const Vec3& halfExtents,
    const float stepHeight) -> bool
{
    if (LengthSquared(moveDelta) <= 1.0e-8f)
    {
        return true;
    }

    const auto tryMove = [&](const Vec3& delta, const float lift) -> bool
    {
        const Vec3 raised = position + Vec3{0.0f, lift, 0.0f};
        if (lift > 0.0f && world.OverlapsBlocking(raised, halfExtents))
        {
            return false;
        }

        const Vec3 candidate = raised + delta;
        if (world.OverlapsBlocking(candidate, halfExtents))
        {
            return false;
        }

        position = candidate;
        return true;
    };

    if (tryMove(moveDelta, 0.0f))
    {
        return true;
    }

    if (stepHeight > 0.0f)
    {
        for (const float fraction : {1.0f, 0.75f, 0.5f})
        {
            if (tryMove(moveDelta, stepHeight * fraction))
            {
                return true;
            }
        }
    }

    return false;
}
}

void TruckController::Reset(const world::DemoWorld& world)
{
    const Vec3 minimum = world.WorldMin();
    const Vec3 maximum = world.WorldMax();
    position_ = {
        Lerp(minimum.x, maximum.x, 0.72f),
        maximum.y * 0.70f,
        Lerp(minimum.z, maximum.z, 0.42f),
    };

    const world::RaycastHit groundHit = world.Raycast({position_, Vec3{0.0f, -1.0f, 0.0f}}, maximum.y + world.CellSize() * 2.0f);
    if (groundHit.hit)
    {
        position_.y = groundHit.position.y + BodyHalfExtents().y + 0.32f;
    }

    velocity_ = {};
    yawRadians_ = DegreesToRadians(-90.0f);
    steering_ = 0.0f;
    bodyPitchRadians_ = 0.0f;
    bodyRollRadians_ = 0.0f;
    engineLoad_ = 0.0f;
    averageSink_ = 0.0f;
    grounded_ = false;
    rutTravelAccumulator_ = 0.0f;
    dominantMaterial_ = world::MaterialId::CompactedSoil;
    wheelContacts_ = {};
    wheelSpinRadians_.fill(0.0f);
    wheelSuspensionOffset_.fill(0.0f);
    SampleGroundContacts(world);
}

void TruckController::Tick(const ControlState& input, world::DemoWorld& world, const float dt, const bool occupied)
{
    const float throttle = occupied
        ? (input.moveForward ? 1.0f : 0.0f) - (input.moveBackward ? 1.0f : 0.0f)
        : 0.0f;
    const float steeringTarget = occupied
        ? (input.moveRight ? 1.0f : 0.0f) - (input.moveLeft ? 1.0f : 0.0f)
        : 0.0f;
    steering_ = MoveToward(steering_, steeringTarget, dt * 4.0f);

    SampleGroundContacts(world);

    float gripSum = 0.0f;
    float sinkSum = 0.0f;
    int contactCount = 0;
    std::array<int, world::kMaterialCount> materialCounts{};
    for (const WheelContact& contact : wheelContacts_)
    {
        if (!contact.hit)
        {
            continue;
        }

        const world::MaterialProperties properties = world::GetMaterialProperties(contact.material);
        gripSum += properties.wheelGrip;
        sinkSum += properties.wheelSink;
        ++contactCount;
        ++materialCounts[static_cast<std::size_t>(contact.material)];
    }

    grounded_ = contactCount >= 2;
    const float averageGrip = contactCount > 0 ? gripSum / static_cast<float>(contactCount) : 0.08f;
    averageSink_ = contactCount > 0 ? sinkSum / static_cast<float>(contactCount) : 0.0f;

    dominantMaterial_ = world::MaterialId::Air;
    int dominantCount = -1;
    for (int materialIndex = 0; materialIndex < static_cast<int>(materialCounts.size()); ++materialIndex)
    {
        if (materialCounts[static_cast<std::size_t>(materialIndex)] > dominantCount)
        {
            dominantCount = materialCounts[static_cast<std::size_t>(materialIndex)];
            dominantMaterial_ = static_cast<world::MaterialId>(materialIndex);
        }
    }

    const Vec3 forward = ForwardVector();
    const Vec3 right = RightVector();
    const float forwardSpeed = Dot(velocity_, forward);
    const float lateralSpeed = Dot(velocity_, right);

    const float maxSpeed = occupied ? 17.0f : 0.0f;
    const float engineAcceleration = 18.0f * averageGrip;
    const float brakeAcceleration = 26.0f * std::max(averageGrip, 0.35f);

    float targetForwardSpeed = throttle * maxSpeed;
    if (!occupied)
    {
        targetForwardSpeed = 0.0f;
    }

    float nextForwardSpeed = forwardSpeed;
    if (std::abs(throttle) > 0.001f)
    {
        const float accel = ((forwardSpeed * throttle) >= 0.0f) ? engineAcceleration : brakeAcceleration;
        nextForwardSpeed = MoveToward(forwardSpeed, targetForwardSpeed, accel * dt);
    }
    else
    {
        nextForwardSpeed = MoveToward(forwardSpeed, 0.0f, (grounded_ ? 7.0f : 1.5f) * dt);
    }

    const float nextLateralSpeed = MoveToward(lateralSpeed, 0.0f, (grounded_ ? (15.0f * averageGrip + 2.0f) : 1.5f) * dt);
    velocity_ = forward * nextForwardSpeed + right * nextLateralSpeed + Vec3{0.0f, velocity_.y, 0.0f};

    if (grounded_)
    {
        const float steeringStrength = (0.8f + averageGrip * 1.5f) * Clamp(std::abs(nextForwardSpeed) / 8.0f, 0.2f, 1.0f);
        yawRadians_ += steering_ * steeringStrength * dt * (nextForwardSpeed >= 0.0f ? 1.0f : -1.0f);
    }

    velocity_.y -= 18.0f * dt;

    const Vec3 halfExtents = BodyHalfExtents();
    const Vec3 horizontalDelta{velocity_.x * dt, 0.0f, velocity_.z * dt};
    const float stepHeight = grounded_ ? 0.52f + averageSink_ * world.CellSize() * 0.45f : 0.0f;
    if (!TryStepMove(world, position_, horizontalDelta, halfExtents, stepHeight))
    {
        const float previousX = position_.x;
        const float previousZ = position_.z;
        if (!TryStepMove(world, position_, Vec3{horizontalDelta.x, 0.0f, 0.0f}, halfExtents, stepHeight))
        {
            velocity_.x = 0.0f;
        }
        if (!TryStepMove(world, position_, Vec3{0.0f, 0.0f, horizontalDelta.z}, halfExtents, stepHeight))
        {
            velocity_.z = 0.0f;
        }
        if (position_.x == previousX)
        {
            velocity_.x = 0.0f;
        }
        if (position_.z == previousZ)
        {
            velocity_.z = 0.0f;
        }
    }

    Vec3 candidate = position_;
    candidate.y += velocity_.y * dt;
    if (world.OverlapsBlocking(candidate, halfExtents))
    {
        if (velocity_.y < 0.0f)
        {
            grounded_ = true;
        }
        velocity_.y = 0.0f;
    }
    else
    {
        position_.y = candidate.y;
    }

    const float wheelRadius = 0.32f;
    constexpr float suspensionTravel = 0.18f;
    const float wheelAngularDelta = (wheelRadius > 0.001f ? nextForwardSpeed / wheelRadius : 0.0f) * dt;
    for (float& wheelSpinRadians : wheelSpinRadians_)
    {
        wheelSpinRadians = WrapAngle(wheelSpinRadians + wheelAngularDelta);
    }

    SampleGroundContacts(world);
    const auto wheelOffsets = WheelLocalOffsets();
    const float suspensionBlend = Clamp(dt * 9.0f, 0.0f, 1.0f);
    for (std::size_t wheelIndex = 0; wheelIndex < wheelContacts_.size(); ++wheelIndex)
    {
        float desiredOffset = -suspensionTravel;
        if (wheelContacts_[wheelIndex].hit)
        {
            const float desiredWheelLocalY = Clamp(
                (wheelContacts_[wheelIndex].point.y + wheelRadius) - position_.y,
                wheelOffsets[wheelIndex].y - suspensionTravel,
                wheelOffsets[wheelIndex].y + suspensionTravel);
            desiredOffset = desiredWheelLocalY - wheelOffsets[wheelIndex].y;
        }
        wheelSuspensionOffset_[wheelIndex] = Lerp(wheelSuspensionOffset_[wheelIndex], desiredOffset, suspensionBlend);
    }

    if (grounded_)
    {
        float heightSum = 0.0f;
        int heightCount = 0;
        for (const WheelContact& contact : wheelContacts_)
        {
            if (!contact.hit)
            {
                continue;
            }

            heightSum += contact.point.y;
            ++heightCount;
        }

        if (heightCount > 0)
        {
            const float targetHeight = (heightSum / static_cast<float>(heightCount)) + BodyHalfExtents().y + 0.28f - averageSink_ * world.CellSize() * 0.65f;
            position_.y = Lerp(position_.y, targetHeight, Clamp(dt * 8.0f, 0.0f, 1.0f));
            velocity_.y = std::max(velocity_.y, 0.0f);
        }
    }

    float frontHeightSum = 0.0f;
    float rearHeightSum = 0.0f;
    int frontCount = 0;
    int rearCount = 0;
    float leftHeightSum = 0.0f;
    float rightHeightSum = 0.0f;
    int leftCount = 0;
    int rightCount = 0;
    for (std::size_t wheelIndex = 0; wheelIndex < wheelContacts_.size(); ++wheelIndex)
    {
        if (!wheelContacts_[wheelIndex].hit)
        {
            continue;
        }

        if (wheelIndex < 2u)
        {
            frontHeightSum += wheelContacts_[wheelIndex].point.y;
            ++frontCount;
        }
        else
        {
            rearHeightSum += wheelContacts_[wheelIndex].point.y;
            ++rearCount;
        }

        if ((wheelIndex % 2u) == 0u)
        {
            leftHeightSum += wheelContacts_[wheelIndex].point.y;
            ++leftCount;
        }
        else
        {
            rightHeightSum += wheelContacts_[wheelIndex].point.y;
            ++rightCount;
        }
    }

    float targetPitch = 0.0f;
    if (frontCount > 0 && rearCount > 0)
    {
        targetPitch = std::atan2(
            (frontHeightSum / static_cast<float>(frontCount)) - (rearHeightSum / static_cast<float>(rearCount)),
            std::abs(WheelLocalOffsets()[0].z - WheelLocalOffsets()[2].z));
    }

    float targetRoll = 0.0f;
    if (leftCount > 0 && rightCount > 0)
    {
        targetRoll = std::atan2(
            (leftHeightSum / static_cast<float>(leftCount)) - (rightHeightSum / static_cast<float>(rightCount)),
            std::abs(WheelLocalOffsets()[0].x - WheelLocalOffsets()[1].x));
    }

    const float orientationBlend = Clamp(dt * 5.0f, 0.0f, 1.0f);
    bodyPitchRadians_ = Lerp(bodyPitchRadians_, targetPitch, orientationBlend);
    bodyRollRadians_ = Lerp(bodyRollRadians_, targetRoll, orientationBlend);

    engineLoad_ = Clamp(std::abs(throttle) * (1.0f - averageGrip * 0.35f) + std::abs(nextForwardSpeed) / 16.0f, 0.0f, 1.0f);
    if (grounded_ && occupied)
    {
        rutTravelAccumulator_ += std::abs(nextForwardSpeed) * dt;
        if (rutTravelAccumulator_ >= world.CellSize() * 0.30f)
        {
            ApplyWheelRuts(world, std::abs(throttle));
            rutTravelAccumulator_ = 0.0f;
        }
    }
    else
    {
        rutTravelAccumulator_ = 0.0f;
    }
}

auto TruckController::ForwardVector() const -> Vec3
{
    return Normalize({std::cos(yawRadians_), 0.0f, std::sin(yawRadians_)});
}

auto TruckController::RightVector() const -> Vec3
{
    return Normalize(Cross(ForwardVector(), Vec3{0.0f, 1.0f, 0.0f}));
}

auto TruckController::WorldOffset(const Vec3& localOffset) const -> Vec3
{
    return RightVector() * localOffset.x + Vec3{0.0f, 1.0f, 0.0f} * localOffset.y + ForwardVector() * localOffset.z;
}

auto TruckController::VisualBodyRightVector() const -> Vec3
{
    Vec3 right = RightVector();
    right = model::RotateAroundAxis(right, ForwardVector(), bodyRollRadians_);
    return Normalize(right);
}

auto TruckController::VisualBodyUpVector() const -> Vec3
{
    Vec3 up{0.0f, 1.0f, 0.0f};
    up = model::RotateAroundAxis(up, RightVector(), bodyPitchRadians_);
    up = model::RotateAroundAxis(up, ForwardVector(), bodyRollRadians_);
    return Normalize(up);
}

auto TruckController::VisualBodyForwardVector() const -> Vec3
{
    Vec3 forward = ForwardVector();
    forward = model::RotateAroundAxis(forward, RightVector(), bodyPitchRadians_);
    return Normalize(forward);
}

auto TruckController::WheelLocalOffsets() const -> std::array<Vec3, 4>
{
    return {
        Vec3{-0.72f, -0.44f,  1.08f},
        Vec3{ 0.72f, -0.44f,  1.08f},
        Vec3{-0.72f, -0.44f, -1.08f},
        Vec3{ 0.72f, -0.44f, -1.08f},
    };
}

void TruckController::SampleGroundContacts(const world::DemoWorld& world)
{
    const auto wheelOffsets = WheelLocalOffsets();
    for (std::size_t wheelIndex = 0; wheelIndex < wheelContacts_.size(); ++wheelIndex)
    {
        const Vec3 wheelOrigin = position_ + WorldOffset(wheelOffsets[wheelIndex]) + Vec3{0.0f, 0.55f, 0.0f};
        const world::RaycastHit hit = world.Raycast({wheelOrigin, Vec3{0.0f, -1.0f, 0.0f}}, 1.8f);

        wheelContacts_[wheelIndex].hit = hit.hit;
        wheelContacts_[wheelIndex].point = hit.position;
        wheelContacts_[wheelIndex].material = hit.material;
    }
}

void TruckController::ApplyWheelRuts(world::DemoWorld& world, const float throttleMagnitude)
{
    for (const WheelContact& contact : wheelContacts_)
    {
        if (!contact.hit)
        {
            continue;
        }

        switch (contact.material)
        {
        case world::MaterialId::WetMud:
            world.ApplyDig(contact.point - Vec3{0.0f, world.CellSize() * 0.08f, 0.0f}, world.CellSize() * 0.45f, 0.42f + throttleMagnitude * 0.18f);
            break;
        case world::MaterialId::DrySand:
            world.ApplyDig(contact.point - Vec3{0.0f, world.CellSize() * 0.05f, 0.0f}, world.CellSize() * 0.40f, 0.26f + throttleMagnitude * 0.12f);
            break;
        case world::MaterialId::CompactedSoil:
        {
            const Int3 cell = world.CellAtWorldPosition(contact.point - Vec3{0.0f, world.CellSize() * 0.12f, 0.0f});
            world.EditCell(cell.x, cell.y, cell.z, world::MaterialId::WetMud);
            break;
        }
        default:
            break;
        }
    }
}

auto TruckController::DriverSeatPosition() const -> Vec3
{
    return position_ +
        VisualBodyRightVector() * 0.0f +
        VisualBodyUpVector() * (0.58f - averageSink_ * 0.18f) +
        VisualBodyForwardVector() * -0.20f;
}

auto TruckController::CameraPosition() const -> Vec3
{
    return DriverSeatPosition() + Vec3{0.0f, 0.32f, 0.0f};
}

auto TruckController::ExitPosition() const -> Vec3
{
    return position_ + VisualBodyRightVector() * 1.8f + Vec3{0.0f, 0.3f, 0.0f};
}

auto TruckController::CanEnter(const Vec3& playerPosition) const -> bool
{
    return Length(DriverSeatPosition() - playerPosition) <= 2.8f;
}

auto TruckController::SpeedMetersPerSecond() const -> float
{
    return Length(Vec3{velocity_.x, 0.0f, velocity_.z});
}

void TruckController::AppendModelTriangles(std::vector<render::ColorVertex3D>& triangles, std::vector<render::ColorVertex3D>& translucentTriangles) const
{
    using namespace model;

    Basis3 bodyBasis = MakeBasis(position_, VisualBodyForwardVector(), VisualBodyUpVector());
    bodyBasis.right = VisualBodyRightVector();
    bodyBasis.up = VisualBodyUpVector();
    bodyBasis.forward = VisualBodyForwardVector();

    const Vec4 chassisColor = grounded_ ? MakeColor(0.81f, 0.25f, 0.14f, 1.0f) : MakeColor(0.90f, 0.46f, 0.16f, 1.0f);
    const Vec4 trimColor = MakeColor(0.16f, 0.18f, 0.20f, 1.0f);
    const Vec4 cabinColor = MakeColor(0.72f, 0.20f, 0.14f, 1.0f);
    const Vec4 lightColor = MakeColor(0.98f, 0.84f, 0.34f, 1.0f);
    const Vec4 wheelColor = MakeColor(0.10f, 0.11f, 0.13f, 1.0f);
    const Vec4 hubColor = MakeColor(0.52f, 0.55f, 0.58f, 1.0f);
    const Vec4 glassColor = MakeColor(0.64f, 0.88f, 1.0f, 0.34f);

    Basis3 lowerBasis = bodyBasis;
    lowerBasis.origin = TransformPoint(bodyBasis, Vec3{0.0f, -0.06f, -0.02f});
    AppendBox(triangles, lowerBasis, Vec3{0.86f, 0.22f, 1.44f}, chassisColor);

    Basis3 bedBasis = bodyBasis;
    bedBasis.origin = TransformPoint(bodyBasis, Vec3{0.0f, 0.20f, -0.42f});
    AppendBox(triangles, bedBasis, Vec3{0.80f, 0.25f, 0.72f}, chassisColor);

    Basis3 cabBasis = bodyBasis;
    cabBasis.origin = TransformPoint(bodyBasis, Vec3{0.0f, 0.32f, 0.36f});
    AppendBox(triangles, cabBasis, Vec3{0.76f, 0.34f, 0.58f}, cabinColor);

    Basis3 roofBasis = bodyBasis;
    roofBasis.origin = TransformPoint(bodyBasis, Vec3{0.0f, 0.70f, 0.24f});
    AppendBox(triangles, roofBasis, Vec3{0.68f, 0.10f, 0.42f}, cabinColor);

    Basis3 hoodBasis = bodyBasis;
    hoodBasis.origin = TransformPoint(bodyBasis, Vec3{0.0f, 0.16f, 1.02f});
    AppendBox(triangles, hoodBasis, Vec3{0.72f, 0.16f, 0.44f}, chassisColor);

    Basis3 bumperBasis = bodyBasis;
    bumperBasis.origin = TransformPoint(bodyBasis, Vec3{0.0f, -0.10f, 1.60f});
    AppendBox(triangles, bumperBasis, Vec3{0.82f, 0.10f, 0.12f}, trimColor);

    Basis3 rearBumperBasis = bodyBasis;
    rearBumperBasis.origin = TransformPoint(bodyBasis, Vec3{0.0f, -0.10f, -1.56f});
    AppendBox(triangles, rearBumperBasis, Vec3{0.82f, 0.10f, 0.12f}, trimColor);

    Basis3 windshieldBasis = bodyBasis;
    windshieldBasis.origin = TransformPoint(bodyBasis, Vec3{0.0f, 0.44f, 0.70f});
    AppendBox(translucentTriangles, windshieldBasis, Vec3{0.60f, 0.22f, 0.05f}, glassColor);

    Basis3 rearWindowBasis = bodyBasis;
    rearWindowBasis.origin = TransformPoint(bodyBasis, Vec3{0.0f, 0.44f, -0.04f});
    AppendBox(translucentTriangles, rearWindowBasis, Vec3{0.54f, 0.20f, 0.04f}, glassColor);

    Basis3 leftWindowBasis = bodyBasis;
    leftWindowBasis.origin = TransformPoint(bodyBasis, Vec3{-0.70f, 0.42f, 0.28f});
    AppendBox(translucentTriangles, leftWindowBasis, Vec3{0.04f, 0.18f, 0.40f}, glassColor);

    Basis3 rightWindowBasis = bodyBasis;
    rightWindowBasis.origin = TransformPoint(bodyBasis, Vec3{0.70f, 0.42f, 0.28f});
    AppendBox(translucentTriangles, rightWindowBasis, Vec3{0.04f, 0.18f, 0.40f}, glassColor);

    Basis3 leftHeadlightBasis = bodyBasis;
    leftHeadlightBasis.origin = TransformPoint(bodyBasis, Vec3{-0.44f, 0.02f, 1.48f});
    AppendBox(triangles, leftHeadlightBasis, Vec3{0.12f, 0.06f, 0.03f}, lightColor);

    Basis3 rightHeadlightBasis = bodyBasis;
    rightHeadlightBasis.origin = TransformPoint(bodyBasis, Vec3{0.44f, 0.02f, 1.48f});
    AppendBox(triangles, rightHeadlightBasis, Vec3{0.12f, 0.06f, 0.03f}, lightColor);

    constexpr float wheelRadius = 0.32f;
    constexpr float wheelHalfWidth = 0.13f;
    const auto wheelOffsets = WheelLocalOffsets();
    for (std::size_t wheelIndex = 0; wheelIndex < wheelOffsets.size(); ++wheelIndex)
    {
        Vec3 suspendedOffset = wheelOffsets[wheelIndex];
        suspendedOffset.y += wheelSuspensionOffset_[wheelIndex];
        Vec3 wheelCenter = TransformPoint(bodyBasis, suspendedOffset);

        Vec3 wheelForward = bodyBasis.forward;
        Vec3 wheelRight = bodyBasis.right;
        Vec3 wheelUp = bodyBasis.up;
        if (wheelIndex < 2u)
        {
            const float steeringAngle = steering_ * DegreesToRadians(28.0f);
            wheelForward = Normalize(RotateAroundAxis(wheelForward, bodyBasis.up, steeringAngle));
            wheelRight = Normalize(RotateAroundAxis(wheelRight, bodyBasis.up, steeringAngle));
        }

        wheelUp = Normalize(RotateAroundAxis(wheelUp, wheelRight, wheelSpinRadians_[wheelIndex]));
        wheelForward = Normalize(RotateAroundAxis(wheelForward, wheelRight, wheelSpinRadians_[wheelIndex]));

        Basis3 wheelBasis{
            wheelCenter,
            wheelRight,
            wheelUp,
            wheelForward,
        };
        AppendCylinder(triangles, wheelBasis, wheelHalfWidth, wheelRadius, 10, wheelColor, true, true);

        Basis3 hubBasis = wheelBasis;
        AppendBox(triangles, hubBasis, Vec3{0.04f, 0.14f, 0.14f}, hubColor);

        Basis3 spokeABasis = wheelBasis;
        AppendBox(triangles, spokeABasis, Vec3{0.03f, 0.23f, 0.03f}, hubColor);

        Basis3 spokeBBasis = wheelBasis;
        AppendBox(triangles, spokeBBasis, Vec3{0.03f, 0.03f, 0.23f}, hubColor);
    }
}

void TruckController::AppendDebugLines(std::vector<render::ColorVertex3D>& lines) const
{
    const Vec4 bodyColor = grounded_ ? MakeColor(0.93f, 0.36f, 0.22f, 1.0f) : MakeColor(0.98f, 0.72f, 0.22f, 1.0f);
    const Vec4 wheelColor = MakeColor(0.14f, 0.16f, 0.18f, 1.0f);
    const Vec3 half = BodyHalfExtents();

    const std::array<Vec3, 8> corners = {
        position_ + WorldOffset(Vec3{-half.x, -half.y, -half.z}),
        position_ + WorldOffset(Vec3{ half.x, -half.y, -half.z}),
        position_ + WorldOffset(Vec3{ half.x,  half.y, -half.z}),
        position_ + WorldOffset(Vec3{-half.x,  half.y, -half.z}),
        position_ + WorldOffset(Vec3{-half.x, -half.y,  half.z}),
        position_ + WorldOffset(Vec3{ half.x, -half.y,  half.z}),
        position_ + WorldOffset(Vec3{ half.x,  half.y,  half.z}),
        position_ + WorldOffset(Vec3{-half.x,  half.y,  half.z}),
    };

    constexpr std::array<std::array<int, 2>, 12> edges = {
        std::array<int, 2>{0, 1}, std::array<int, 2>{1, 2}, std::array<int, 2>{2, 3}, std::array<int, 2>{3, 0},
        std::array<int, 2>{4, 5}, std::array<int, 2>{5, 6}, std::array<int, 2>{6, 7}, std::array<int, 2>{7, 4},
        std::array<int, 2>{0, 4}, std::array<int, 2>{1, 5}, std::array<int, 2>{2, 6}, std::array<int, 2>{3, 7},
    };

    for (const auto& edge : edges)
    {
        lines.push_back({corners[edge[0]], bodyColor});
        lines.push_back({corners[edge[1]], bodyColor});
    }

    for (std::size_t wheelIndex = 0; wheelIndex < wheelContacts_.size(); ++wheelIndex)
    {
        const Vec3 wheelCenter = position_ + WorldOffset(WheelLocalOffsets()[wheelIndex]);
        lines.push_back({wheelCenter + Vec3{-0.18f, 0.0f, 0.0f}, wheelColor});
        lines.push_back({wheelCenter + Vec3{ 0.18f, 0.0f, 0.0f}, wheelColor});
        lines.push_back({wheelCenter + Vec3{0.0f, -0.18f, 0.0f}, wheelColor});
        lines.push_back({wheelCenter + Vec3{0.0f,  0.18f, 0.0f}, wheelColor});
        lines.push_back({wheelCenter + Vec3{0.0f, 0.0f, -0.18f}, wheelColor});
        lines.push_back({wheelCenter + Vec3{0.0f, 0.0f,  0.18f}, wheelColor});

        if (wheelContacts_[wheelIndex].hit)
        {
            lines.push_back({wheelCenter, MakeColor(0.45f, 0.9f, 1.0f, 1.0f)});
            lines.push_back({wheelContacts_[wheelIndex].point, MakeColor(0.45f, 0.9f, 1.0f, 1.0f)});
        }
    }
}
}
