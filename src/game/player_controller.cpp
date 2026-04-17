#include "game/player_controller.hpp"

#include "world/material_properties.hpp"

#include <algorithm>
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

auto SnapDownToGround(
    const world::DemoWorld& world,
    Vec3& position,
    const Vec3& halfExtents,
    const float maxDrop) -> bool
{
    if (maxDrop <= 1.0e-4f)
    {
        return false;
    }

    const auto isClearAtDrop = [&](const float drop) -> bool
    {
        return !world.OverlapsBlocking(position - Vec3{0.0f, drop, 0.0f}, halfExtents);
    };

    if (!isClearAtDrop(0.0f))
    {
        return false;
    }

    float low = 0.0f;
    float high = maxDrop;
    if (isClearAtDrop(high))
    {
        position.y -= high;
        return true;
    }

    for (int iteration = 0; iteration < 8; ++iteration)
    {
        const float mid = (low + high) * 0.5f;
        if (isClearAtDrop(mid))
        {
            low = mid;
        }
        else
        {
            high = mid;
        }
    }

    if (low <= 1.0e-4f)
    {
        return false;
    }

    position.y -= low;
    return true;
}
}

void PlayerController::Spawn(const world::DemoWorld& world)
{
    const Vec3 minimum = world.WorldMin();
    const Vec3 maximum = world.WorldMax();
    const Vec3 spawnProbe{
        Lerp(minimum.x, maximum.x, 0.22f),
        maximum.y - world.CellSize() * 1.5f,
        Lerp(minimum.z, maximum.z, 0.20f),
    };

    const world::RaycastHit groundHit = world.Raycast({spawnProbe, Vec3{0.0f, -1.0f, 0.0f}}, maximum.y + world.CellSize() * 2.0f);
    position_ = groundHit.hit
        ? groundHit.position + Vec3{0.0f, CollisionHalfExtents().y + 0.85f, 0.0f}
        : Vec3{spawnProbe.x, maximum.y * 0.55f, spawnProbe.z};
    velocity_ = {};
    yawRadians_ = DegreesToRadians(20.0f);
    pitchRadians_ = DegreesToRadians(-12.0f);
    walkCycleRadians_ = 0.0f;
    feet_.fill({});
    onGround_ = false;
}

void PlayerController::PlaceAt(const Vec3& position, const float yawRadians, const float pitchRadians)
{
    position_ = position;
    velocity_ = {};
    yawRadians_ = yawRadians;
    pitchRadians_ = pitchRadians;
    walkCycleRadians_ = 0.0f;
    feet_.fill({});
    onGround_ = false;
}

void PlayerController::Tick(const ControlState& input, const world::DemoWorld& world, const float dt)
{
    yawRadians_ += input.lookYawDelta * 0.0026f;
    pitchRadians_ = Clamp(pitchRadians_ - input.lookPitchDelta * 0.0022f, DegreesToRadians(-89.0f), DegreesToRadians(89.0f));
    const bool wasGrounded = onGround_;

    const Vec3 flatForward = FlatForwardVector();
    const Vec3 right = RightVector();

    Vec3 moveInput{};
    if (input.moveForward)
    {
        moveInput += flatForward;
    }
    if (input.moveBackward)
    {
        moveInput -= flatForward;
    }
    if (input.moveRight)
    {
        moveInput += right;
    }
    if (input.moveLeft)
    {
        moveInput -= right;
    }
    moveInput = Normalize(moveInput);

    const world::MaterialId footingMaterial = MaterialUnderFeet(world);
    const world::MaterialProperties footingProperties = world::GetMaterialProperties(footingMaterial);
    const float moveScale = footingProperties.movementSlowdown;
    const float targetSpeed = (input.sprint ? 8.5f : 6.0f) * moveScale;
    const float acceleration = onGround_ ? 38.0f : 18.0f;

    velocity_.x = MoveToward(velocity_.x, moveInput.x * targetSpeed, acceleration * dt);
    velocity_.z = MoveToward(velocity_.z, moveInput.z * targetSpeed, acceleration * dt);

    if (onGround_ && input.jumpPressed)
    {
        velocity_.y = footingMaterial == world::MaterialId::ShallowWater ? 5.2f : 7.4f;
        onGround_ = false;
    }

    const float gravity = footingMaterial == world::MaterialId::ShallowWater ? 12.0f : 20.0f;
    velocity_.y -= gravity * dt;

    const Vec3 halfExtents = CollisionHalfExtents();
    constexpr float kFootToCenterHeight = 0.86f;
    constexpr Vec3 kFootHalfExtents{0.12f, 0.08f, 0.19f};
    const float stepHeight = wasGrounded ? std::max(0.22f, world.CellSize() * 0.95f) : 0.0f;
    const Vec3 horizontalDelta{velocity_.x * dt, 0.0f, velocity_.z * dt};
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

    struct FootSample
    {
        Vec3 footCenter{};
        world::MaterialId material = world::MaterialId::Air;
        bool grounded = false;
    };

    const auto sampleFeet = [&](const float bodyCenterY) -> std::array<FootSample, 2>
    {
        std::array<FootSample, 2> samples{};
        const std::array<Vec3, 2> supportOffsets = {
            Vec3{-0.19f, 0.0f, 0.04f},
            Vec3{ 0.19f, 0.0f, 0.04f},
        };

        for (std::size_t footIndex = 0; footIndex < samples.size(); ++footIndex)
        {
            const Vec3 supportAnchor =
                position_ +
                right * supportOffsets[footIndex].x +
                flatForward * supportOffsets[footIndex].z;
            const Vec3 probeOrigin = supportAnchor + Vec3{0.0f, stepHeight + world.CellSize() * 0.35f, 0.0f};
            const float probeDistance = kFootToCenterHeight + stepHeight + world.CellSize() * 0.70f;
            const world::RaycastHit hit = world.Raycast({probeOrigin, Vec3{0.0f, -1.0f, 0.0f}}, probeDistance);
            if (!hit.hit || !world::BlocksMovement(hit.material))
            {
                samples[footIndex].footCenter = supportAnchor + Vec3{0.0f, bodyCenterY - kFootToCenterHeight, 0.0f};
                continue;
            }

            const Vec3 candidateCenter{supportAnchor.x, hit.position.y + kFootHalfExtents.y, supportAnchor.z};
            if (world.OverlapsBlocking(candidateCenter, kFootHalfExtents))
            {
                samples[footIndex].footCenter = supportAnchor + Vec3{0.0f, bodyCenterY - kFootToCenterHeight, 0.0f};
                continue;
            }

            samples[footIndex].footCenter = candidateCenter;
            samples[footIndex].material = hit.material;
            samples[footIndex].grounded = true;
        }

        return samples;
    };

    const auto applyFootSamples = [&](const std::array<FootSample, 2>& samples, const float bodyCenterY)
    {
        for (std::size_t footIndex = 0; footIndex < samples.size(); ++footIndex)
        {
            feet_[footIndex].grounded = samples[footIndex].grounded;
            feet_[footIndex].material = samples[footIndex].material;
            feet_[footIndex].position = samples[footIndex].footCenter;
            if (!samples[footIndex].grounded)
            {
                feet_[footIndex].position.y = bodyCenterY - kFootToCenterHeight;
            }
        }
    };

    auto footSamples = sampleFeet(position_.y);
    int groundedFeet = 0;
    float groundedTargetCenterY = 0.0f;
    for (const FootSample& sample : footSamples)
    {
        if (!sample.grounded)
        {
            continue;
        }
        groundedTargetCenterY += sample.footCenter.y + kFootToCenterHeight;
        ++groundedFeet;
    }

    onGround_ = false;
    if (groundedFeet > 0 && velocity_.y <= 0.0f)
    {
        groundedTargetCenterY /= static_cast<float>(groundedFeet);
        const float supportDelta = groundedTargetCenterY - position_.y;
        const float maxSupportRise = wasGrounded ? stepHeight + 0.08f : 0.10f;
        const float maxSupportDrop = wasGrounded ? stepHeight + world.CellSize() * 0.35f : std::max(0.12f, world.CellSize() * 0.35f);
        if (supportDelta >= -maxSupportDrop && supportDelta <= maxSupportRise)
        {
            position_.y = groundedTargetCenterY;
            velocity_.y = 0.0f;
            onGround_ = true;
        }
    }

    if (!onGround_ || velocity_.y > 0.0f)
    {
        Vec3 candidate = position_;
        candidate.y += velocity_.y * dt;
        if (world.OverlapsBlocking(candidate, halfExtents))
        {
            if (velocity_.y < 0.0f)
            {
                onGround_ = true;
            }
            velocity_.y = 0.0f;
        }
        else
        {
            position_.y = candidate.y;
            onGround_ = false;
        }

        footSamples = sampleFeet(position_.y);
        groundedFeet = 0;
        groundedTargetCenterY = 0.0f;
        for (const FootSample& sample : footSamples)
        {
            if (!sample.grounded)
            {
                continue;
            }
            groundedTargetCenterY += sample.footCenter.y + kFootToCenterHeight;
            ++groundedFeet;
        }

        if (velocity_.y <= 0.0f && groundedFeet > 0)
        {
            groundedTargetCenterY /= static_cast<float>(groundedFeet);
            const float catchDistance = wasGrounded ? (stepHeight + world.CellSize() * 0.35f) : std::max(0.12f, world.CellSize() * 0.35f);
            if ((position_.y - groundedTargetCenterY) >= -0.05f && (position_.y - groundedTargetCenterY) <= catchDistance)
            {
                position_.y = groundedTargetCenterY;
                velocity_.y = 0.0f;
                onGround_ = true;
            }
        }
    }

    footSamples = sampleFeet(position_.y);
    applyFootSamples(footSamples, position_.y);

    walkCycleRadians_ = WrapAngle(walkCycleRadians_ + HorizontalSpeedMetersPerSecond() * dt * (onGround_ ? 3.2f : 1.4f));
}

auto PlayerController::ForwardVector() const -> Vec3
{
    const float cosPitch = std::cos(pitchRadians_);
    return Normalize({
        std::cos(yawRadians_) * cosPitch,
        std::sin(pitchRadians_),
        std::sin(yawRadians_) * cosPitch,
    });
}

auto PlayerController::FlatForwardVector() const -> Vec3
{
    const Vec3 flatForward = Normalize(Vec3{std::cos(yawRadians_), 0.0f, std::sin(yawRadians_)});
    return LengthSquared(flatForward) > 1.0e-6f ? flatForward : Vec3{1.0f, 0.0f, 0.0f};
}

auto PlayerController::RightVector() const -> Vec3
{
    return Normalize(Cross(FlatForwardVector(), Vec3{0.0f, 1.0f, 0.0f}));
}

auto PlayerController::MaterialUnderFeet(const world::DemoWorld& world) const -> world::MaterialId
{
    world::MaterialId bestMaterial = world::MaterialId::Air;
    float bestSlowdown = 10.0f;
    for (const FootState& foot : feet_)
    {
        if (!foot.grounded)
        {
            continue;
        }

        const float slowdown = world::GetMaterialProperties(foot.material).movementSlowdown;
        if (slowdown < bestSlowdown)
        {
            bestSlowdown = slowdown;
            bestMaterial = foot.material;
        }
    }

    if (bestMaterial != world::MaterialId::Air)
    {
        return bestMaterial;
    }

    return world.MaterialAtWorldPosition(position_ - Vec3{0.0f, CollisionHalfExtents().y + 0.15f, 0.0f});
}
}
