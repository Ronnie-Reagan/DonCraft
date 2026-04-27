#pragma once

#include "core/math.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace df::render
{
inline constexpr float kSurfaceShadingFlat = 0.0f;
inline constexpr float kSurfaceShadingTerrain = 1.0f;
inline constexpr float kSurfaceShadingWater = 2.0f;

[[nodiscard]] inline auto MakeSceneMaterial(
    const float materialId,
    const float param0 = 0.0f,
    const float param1 = 0.0f,
    const float shadingModel = kSurfaceShadingFlat) -> Vec4
{
    return {materialId, param0, param1, shadingModel};
}

struct ColorVertex3D
{
    Vec3 position{};
    Vec4 color{};
    Vec3 normal{};
    Vec4 material{};
};

struct ColorVertex2D
{
    Vec2 clipPosition{};
    Vec4 color{};
};

struct TerrainChunkDraw
{
    std::uint64_t key = 0;
    std::uint64_t meshVersion = 0;
    std::span<const ColorVertex3D> opaqueTriangles;
    std::span<const ColorVertex3D> translucentTriangles;
};

struct FrameRenderData
{
    struct ScopedView
    {
        bool enabled = false;
        Mat4 worldToClip{};
        int viewportX = 0;
        int viewportY = 0;
        int viewportWidth = 0;
        int viewportHeight = 0;
    };

    Mat4 worldToClip{};
    Vec3 cameraPosition{};
    Vec4 clearColor = MakeColor(0.08f, 0.1f, 0.14f, 1.0f);
    std::uint64_t terrainMeshVersion = 0;
    std::span<const ColorVertex3D> terrainTriangles;
    std::span<const ColorVertex3D> translucentTerrainTriangles;
    std::vector<TerrainChunkDraw> terrainChunks;
    std::vector<ColorVertex3D> terrainTriangleStorage;
    std::vector<ColorVertex3D> translucentTerrainTriangleStorage;
    std::vector<ColorVertex3D> dynamicTriangles;
    std::vector<ColorVertex3D> dynamicTranslucentTriangles;
    std::vector<ColorVertex3D> viewModelTriangles;
    std::vector<ColorVertex3D> viewModelPostScopeTriangles;
    std::vector<ColorVertex3D> effectTriangles;
    std::vector<ColorVertex3D> debugLines;
    std::vector<ColorVertex2D> overlayTriangles;
    ScopedView scopedView{};
};
}
