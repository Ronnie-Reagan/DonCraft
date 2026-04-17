#pragma once

#include "core/math.hpp"
#include "render/frame_data.hpp"

#include <vector>

namespace df::game::model
{
struct Basis3
{
    Vec3 origin{};
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 forward{0.0f, 0.0f, 1.0f};
};

[[nodiscard]] auto RotateAroundAxis(const Vec3& value, const Vec3& axis, float radians) -> Vec3;
[[nodiscard]] auto MakeBasis(const Vec3& origin, const Vec3& forward, const Vec3& upHint = Vec3{0.0f, 1.0f, 0.0f}) -> Basis3;
[[nodiscard]] auto TransformPoint(const Basis3& basis, const Vec3& localPoint) -> Vec3;
[[nodiscard]] auto TransformVector(const Basis3& basis, const Vec3& localVector) -> Vec3;

void AppendTriangle(std::vector<render::ColorVertex3D>& triangles, const Vec3& a, const Vec3& b, const Vec3& c, const Vec4& color);
void AppendQuad(std::vector<render::ColorVertex3D>& triangles, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec4& color);
void AppendBox(std::vector<render::ColorVertex3D>& triangles, const Basis3& basis, const Vec3& halfExtents, const Vec4& color);
void AppendCylinder(
    std::vector<render::ColorVertex3D>& triangles,
    const Basis3& basis,
    float halfLength,
    float radius,
    int segments,
    const Vec4& color,
    bool capStart = true,
    bool capEnd = true);
void AppendOctahedron(std::vector<render::ColorVertex3D>& triangles, const Basis3& basis, const Vec3& radii, const Vec4& color);
}
