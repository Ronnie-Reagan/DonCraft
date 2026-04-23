#include "game/construction.hpp"

#include <array>
#include <cmath>

namespace df::game
{
namespace
{
constexpr float kConstructionReachMeters = 10.0f;
constexpr int kFloorHalfSpanCells = 1;
constexpr int kWallHalfWidthCells = 1;
constexpr int kWallHeightCells = 3;

auto DominantAxisStep(const Vec3& normal) -> Int3
{
    const float absX = std::abs(normal.x);
    const float absY = std::abs(normal.y);
    const float absZ = std::abs(normal.z);
    if (absY >= absX && absY >= absZ)
    {
        return {0, normal.y >= 0.0f ? 1 : -1, 0};
    }
    if (absX >= absZ)
    {
        return {normal.x >= 0.0f ? 1 : -1, 0, 0};
    }
    return {0, 0, normal.z >= 0.0f ? 1 : -1};
}

auto RotateStepAroundY(Int3 step, const std::uint8_t quarterTurns) -> Int3
{
    switch (quarterTurns & 3u)
    {
    case 1u:
        return {-step.z, step.y, step.x};
    case 2u:
        return {-step.x, step.y, -step.z};
    case 3u:
        return {step.z, step.y, -step.x};
    default:
        return step;
    }
}

auto FlatForwardCardinal(const Vec3& forward) -> Int3
{
    const Vec3 flat = Normalize(Vec3{forward.x, 0.0f, forward.z});
    if (LengthSquared(flat) <= 1.0e-6f)
    {
        return {0, 0, 1};
    }
    if (std::abs(flat.x) >= std::abs(flat.z))
    {
        return {flat.x >= 0.0f ? 1 : -1, 0, 0};
    }
    return {0, 0, flat.z >= 0.0f ? 1 : -1};
}

auto ReplaceableConstructionMaterial(const world::MaterialId material) -> bool
{
    return material == world::MaterialId::Air || material == world::MaterialId::ShallowWater;
}
}

auto ComputeConstructionPlacement(
    const world::DemoWorld& world,
    const Vec3& origin,
    const Vec3& forward,
    const world::MaterialId material,
    const ConstructionShape shape,
    const std::uint8_t rotationQuarterTurns) -> ConstructionPlacement
{
    ConstructionPlacement placement{};
    placement.shape = shape;
    placement.material = material;

    const world::RaycastHit hit = world.Raycast({origin, Normalize(forward)}, kConstructionReachMeters);
    if (!hit.hit)
    {
        return placement;
    }

    placement.valid = true;

    Int3 surfaceStep = DominantAxisStep(hit.normal);
    Int3 anchor = {
        hit.cell.x + surfaceStep.x,
        hit.cell.y + surfaceStep.y,
        hit.cell.z + surfaceStep.z,
    };

    std::vector<Int3> targetCells;
    if (shape == ConstructionShape::Floor)
    {
        const int y = anchor.y;
        targetCells.reserve(9u);
        for (int dz = -kFloorHalfSpanCells; dz <= kFloorHalfSpanCells; ++dz)
        {
            for (int dx = -kFloorHalfSpanCells; dx <= kFloorHalfSpanCells; ++dx)
            {
                targetCells.push_back({anchor.x + dx, y, anchor.z + dz});
            }
        }
    }
    else
    {
        Int3 wallNormal = surfaceStep;
        if (wallNormal.y != 0)
        {
            wallNormal = RotateStepAroundY(FlatForwardCardinal(forward), rotationQuarterTurns);
        }

        const bool fixedX = wallNormal.x != 0;
        const int yStart = surfaceStep.y != 0 ? anchor.y : (anchor.y - 1);
        targetCells.reserve(static_cast<std::size_t>((kWallHalfWidthCells * 2 + 1) * kWallHeightCells));
        for (int heightIndex = 0; heightIndex < kWallHeightCells; ++heightIndex)
        {
            const int y = yStart + heightIndex;
            for (int lateral = -kWallHalfWidthCells; lateral <= kWallHalfWidthCells; ++lateral)
            {
                if (fixedX)
                {
                    targetCells.push_back({anchor.x, y, anchor.z + lateral});
                }
                else
                {
                    targetCells.push_back({anchor.x + lateral, y, anchor.z});
                }
            }
        }
    }

    placement.expectedCellCount = targetCells.size();
    placement.previewCells.reserve(targetCells.size());
    placement.edits.reserve(targetCells.size());

    for (const Int3& cell : targetCells)
    {
        const bool inBounds =
            cell.x >= 0 && cell.x < world.WorldWidthCells() &&
            cell.y >= 0 && cell.y < world.WorldHeightCells() &&
            cell.z >= 0 && cell.z < world.WorldDepthCells();
        const world::MaterialId existing = inBounds
            ? world.MaterialAtCell(cell.x, cell.y, cell.z)
            : world::MaterialId::Air;
        const bool blocked = !inBounds || !ReplaceableConstructionMaterial(existing);
        placement.previewCells.push_back({cell, blocked});
        if (blocked)
        {
            ++placement.blockedCellCount;
            continue;
        }

        ++placement.placeableCellCount;
        placement.edits.push_back({
            .x = cell.x,
            .y = cell.y,
            .z = cell.z,
            .material = material,
        });
    }

    placement.placeable =
        placement.valid &&
        placement.expectedCellCount > 0u &&
        placement.previewCells.size() == placement.expectedCellCount &&
        placement.placeableCellCount > 0u &&
        placement.edits.size() == placement.placeableCellCount;
    return placement;
}
}
