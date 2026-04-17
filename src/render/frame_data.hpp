#pragma once

#include "core/math.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace df::render
{
struct ColorVertex3D
{
    Vec3 position{};
    Vec4 color{};
};

struct ColorVertex2D
{
    Vec2 clipPosition{};
    Vec4 color{};
};

struct FrameRenderData
{
    Mat4 worldToClip{};
    Vec4 clearColor = MakeColor(0.08f, 0.1f, 0.14f, 1.0f);
    std::uint64_t terrainMeshVersion = 0;
    std::span<const ColorVertex3D> terrainTriangles;
    std::span<const ColorVertex3D> translucentTerrainTriangles;
    std::vector<ColorVertex3D> terrainTriangleStorage;
    std::vector<ColorVertex3D> translucentTerrainTriangleStorage;
    std::vector<ColorVertex3D> dynamicTriangles;
    std::vector<ColorVertex3D> dynamicTranslucentTriangles;
    std::vector<ColorVertex3D> effectTriangles;
    std::vector<ColorVertex3D> debugLines;
    std::vector<ColorVertex2D> overlayTriangles;
};
}
