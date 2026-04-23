#pragma once

#include "core/math.hpp"
#include "world/demo_world.hpp"

#include <algorithm>
#include <cmath>

namespace df::game::movement
{
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
