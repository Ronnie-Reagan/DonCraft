#pragma once

#include "core/math.hpp"
#include "world/material.hpp"

#include <string_view>

namespace df::world
{
struct MaterialProperties
{
    MaterialId id = MaterialId::Air;
    std::string_view name = "Air";
    Vec4 color = MakeColor(0.0f, 0.0f, 0.0f, 0.0f);
    bool blocksMovement = false;
    bool supportsTerrain = false;
    bool isLoose = false;
    bool isLiquid = false;
    float movementSlowdown = 1.0f;
    float wheelGrip = 1.0f;
    float wheelSink = 0.0f;
    float digResistance = 0.0f;
    float rifleResistance = 0.0f;
    float blastResistance = 0.0f;
};

[[nodiscard]] inline auto GetMaterialProperties(const MaterialId material) -> MaterialProperties
{
    switch (material)
    {
    case MaterialId::DrySand:
        return {
            .id = material,
            .name = "Dry Sand",
            .color = MakeColor(0.86f, 0.76f, 0.48f, 1.0f),
            .blocksMovement = true,
            .supportsTerrain = true,
            .isLoose = true,
            .movementSlowdown = 0.92f,
            .wheelGrip = 0.60f,
            .wheelSink = 0.28f,
            .digResistance = 0.20f,
            .rifleResistance = 0.12f,
            .blastResistance = 0.18f,
        };

    case MaterialId::WetMud:
        return {
            .id = material,
            .name = "Wet Mud",
            .color = MakeColor(0.33f, 0.23f, 0.16f, 1.0f),
            .blocksMovement = true,
            .supportsTerrain = true,
            .isLoose = true,
            .movementSlowdown = 0.52f,
            .wheelGrip = 0.26f,
            .wheelSink = 0.54f,
            .digResistance = 0.35f,
            .rifleResistance = 0.24f,
            .blastResistance = 0.28f,
        };

    case MaterialId::ShallowWater:
        return {
            .id = material,
            .name = "Shallow Water",
            .color = MakeColor(0.22f, 0.44f, 0.78f, 1.0f),
            .blocksMovement = false,
            .supportsTerrain = false,
            .isLoose = true,
            .isLiquid = true,
            .movementSlowdown = 0.60f,
            .wheelGrip = 0.18f,
            .wheelSink = 0.20f,
            .digResistance = 0.05f,
            .rifleResistance = 0.02f,
            .blastResistance = 0.08f,
        };

    case MaterialId::CompactedSoil:
        return {
            .id = material,
            .name = "Compacted Soil",
            .color = MakeColor(0.48f, 0.35f, 0.22f, 1.0f),
            .blocksMovement = true,
            .supportsTerrain = true,
            .movementSlowdown = 1.0f,
            .wheelGrip = 0.86f,
            .wheelSink = 0.06f,
            .digResistance = 0.62f,
            .rifleResistance = 0.40f,
            .blastResistance = 0.46f,
        };

    case MaterialId::BrittleConcrete:
        return {
            .id = material,
            .name = "Brittle Concrete",
            .color = MakeColor(0.70f, 0.72f, 0.73f, 1.0f),
            .blocksMovement = true,
            .supportsTerrain = true,
            .movementSlowdown = 1.0f,
            .wheelGrip = 1.0f,
            .wheelSink = 0.02f,
            .digResistance = 0.92f,
            .rifleResistance = 0.68f,
            .blastResistance = 0.58f,
        };

    case MaterialId::Grass:
        return {
            .id = material,
            .name = "Grass",
            .color = MakeColor(0.34f, 0.56f, 0.24f, 1.0f),
            .blocksMovement = true,
            .supportsTerrain = true,
            .movementSlowdown = 0.98f,
            .wheelGrip = 0.90f,
            .wheelSink = 0.04f,
            .digResistance = 0.38f,
            .rifleResistance = 0.22f,
            .blastResistance = 0.28f,
        };

    case MaterialId::Gravel:
        return {
            .id = material,
            .name = "Gravel",
            .color = MakeColor(0.58f, 0.55f, 0.48f, 1.0f),
            .blocksMovement = true,
            .supportsTerrain = true,
            .movementSlowdown = 0.95f,
            .wheelGrip = 0.76f,
            .wheelSink = 0.10f,
            .digResistance = 0.54f,
            .rifleResistance = 0.36f,
            .blastResistance = 0.34f,
        };

    case MaterialId::BasaltRock:
        return {
            .id = material,
            .name = "Basalt Rock",
            .color = MakeColor(0.22f, 0.24f, 0.28f, 1.0f),
            .blocksMovement = true,
            .supportsTerrain = true,
            .movementSlowdown = 1.0f,
            .wheelGrip = 1.02f,
            .wheelSink = 0.01f,
            .digResistance = 1.00f,
            .rifleResistance = 0.82f,
            .blastResistance = 0.72f,
        };

    case MaterialId::Air:
    default:
        return {};
    }
}

[[nodiscard]] inline auto BlocksMovement(const MaterialId material) -> bool
{
    return GetMaterialProperties(material).blocksMovement;
}

[[nodiscard]] inline auto IsLooseMaterial(const MaterialId material) -> bool
{
    const MaterialProperties properties = GetMaterialProperties(material);
    return properties.isLoose || properties.isLiquid;
}
}
