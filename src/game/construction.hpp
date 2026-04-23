#pragma once

#include "core/math.hpp"
#include "world/demo_world.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace df::game
{
enum class ConstructionShape : std::uint8_t
{
    Floor = 0,
    Wall = 1,
};

struct ConstructionPreviewCell
{
    Int3 cell{};
    bool blocked = false;
};

struct ConstructionPlacement
{
    bool valid = false;
    bool placeable = false;
    std::size_t expectedCellCount = 0u;
    std::size_t placeableCellCount = 0u;
    std::size_t blockedCellCount = 0u;
    ConstructionShape shape = ConstructionShape::Floor;
    world::MaterialId material = world::MaterialId::Air;
    std::vector<ConstructionPreviewCell> previewCells;
    std::vector<world::DemoWorld::CellMaterialEdit> edits;
};

[[nodiscard]] auto ComputeConstructionPlacement(
    const world::DemoWorld& world,
    const Vec3& origin,
    const Vec3& forward,
    world::MaterialId material,
    ConstructionShape shape,
    std::uint8_t rotationQuarterTurns) -> ConstructionPlacement;
}
