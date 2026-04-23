#pragma once

#include "core/config.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace df::world
{
enum class MaterialId : std::uint8_t
{
    Air = 0,
    DrySand = 1,
    WetMud = 2,
    ShallowWater = 3,
    CompactedSoil = 4,
    BrittleConcrete = 5,
    Grass = 6,
    Gravel = 7,
    BasaltRock = 8,
    WoodPlanks = 9,
};

inline constexpr std::size_t kMaterialCount = static_cast<std::size_t>(MaterialId::WoodPlanks) + 1u;

inline constexpr std::uint32_t kChunkSize = config::kChunkSize;
inline constexpr std::uint32_t kChunkVolume = kChunkSize * kChunkSize * kChunkSize;

inline std::string_view ToString(const MaterialId material)
{
    switch (material)
    {
    case MaterialId::Air:
        return "Air";
    case MaterialId::DrySand:
        return "Dry Sand";
    case MaterialId::WetMud:
        return "Wet Mud";
    case MaterialId::ShallowWater:
        return "Shallow Water";
    case MaterialId::CompactedSoil:
        return "Compacted Soil";
    case MaterialId::BrittleConcrete:
        return "Brittle Concrete";
    case MaterialId::Grass:
        return "Grass";
    case MaterialId::Gravel:
        return "Gravel";
    case MaterialId::BasaltRock:
        return "Basalt Rock";
    case MaterialId::WoodPlanks:
        return "Wood Planks";
    default:
        return "Unknown";
    }
}

struct ChunkCoord
{
    int x = 0;
    int y = 0;
    int z = 0;

    auto operator==(const ChunkCoord&) const -> bool = default;
};

struct ChunkCoordHash
{
    [[nodiscard]] std::size_t operator()(const ChunkCoord& coord) const noexcept
    {
        std::size_t seed = 0xcbf29ce484222325ull;
        seed ^= static_cast<std::size_t>(coord.x) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        seed ^= static_cast<std::size_t>(coord.y) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        seed ^= static_cast<std::size_t>(coord.z) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        return seed;
    }
};

inline constexpr std::size_t LinearIndex(const std::uint32_t x, const std::uint32_t y, const std::uint32_t z)
{
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(y) * kChunkSize +
           static_cast<std::size_t>(z) * kChunkSize * kChunkSize;
}
}
