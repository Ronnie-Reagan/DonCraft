#pragma once

#include "core/math.hpp"
#include "world/demo_world.hpp"
#include "world/material_properties.hpp"

#include <algorithm>
#include <cmath>

namespace df::game::movement
{
inline auto SnapDownToGround(
    const world::DemoWorld& world,
    Vec3& position,
    const Vec3& halfExtents,
    float maxDrop) -> bool;

inline auto MoveToward(const float current, const float target, const float maxDelta) -> float
{
    if (current < target)
    {
        return std::min(current + maxDelta, target);
    }

    return std::max(current - maxDelta, target);
}

inline auto WrapAngleRadians(const float radians) -> float
{
    return std::remainder(radians, kPi * 2.0f);
}

inline auto TryStepMove(
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

        Vec3 settled = candidate;
        if (stepHeight > 0.1f)
        {
            const float maxDrop = std::max(stepHeight * 0.38f, 0.05f);
            (void)SnapDownToGround(world, settled, halfExtents, maxDrop);
        }

        position = settled;
        return true;
    };

    if (tryMove(moveDelta, 0.0f))
    {
        return true;
    }

    if (stepHeight > 0.1f)
    {
        for (const float fraction : {0.35f, 0.7f, 1.0f})
        {
            if (tryMove(moveDelta, stepHeight * fraction))
            {
                return true;
            }
        }
    }

    return false;
}

inline auto SnapDownToGround(
    const world::DemoWorld& world,
    Vec3& position,
    const Vec3& halfExtents,
    const float maxDrop) -> bool
{
    if (maxDrop <= 1.0e-4f)
    {
        return false;
    }

    if (world.OverlapsBlocking(position, halfExtents))
    {
        return false;
    }

    const Vec3 probeOrigin = position + Vec3{0.0f, halfExtents.y + 0.05f, 0.0f};
    const float probeDistance = halfExtents.y * 2.0f + maxDrop + 0.12f;
    const world::RaycastHit hit = world.Raycast({probeOrigin, Vec3{0.0f, -1.0f, 0.0f}}, probeDistance);
    if (!hit.hit || !world::BlocksMovement(hit.material))
    {
        return false;
    }

    const float targetCenterY = hit.position.y + halfExtents.y;
    const float drop = position.y - targetCenterY;
    if (drop < -0.05f || drop > maxDrop + 0.08f)
    {
        return false;
    }

    Vec3 snapped = position;
    snapped.y = targetCenterY;
    if (world.OverlapsBlocking(snapped, halfExtents))
    {
        return false;
    }

    position = snapped;
    return true;
}
}
