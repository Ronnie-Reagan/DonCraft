#include "game/model_primitives.hpp"

#include <array>
#include <cmath>

namespace df::game::model
{
namespace
{
const Vec3 kLightDirection = Normalize(Vec3{0.28f, 0.88f, 0.36f});

auto LitColor(const Vec4& baseColor, const Vec3& a, const Vec3& b, const Vec3& c) -> Vec4
{
    const Vec3 normal = Normalize(Cross(b - a, c - a));
    if (LengthSquared(normal) <= 1.0e-6f)
    {
        return baseColor;
    }

    const float diffuse = std::max(0.0f, Dot(normal, kLightDirection));
    const float sky = Clamp(normal.y * 0.5f + 0.5f, 0.0f, 1.0f);
    const float light = 0.34f + 0.44f * diffuse + 0.22f * sky;
    return {
        Clamp(baseColor.x * light, 0.0f, 1.0f),
        Clamp(baseColor.y * light, 0.0f, 1.0f),
        Clamp(baseColor.z * light, 0.0f, 1.0f),
        baseColor.w,
    };
}
}

auto RotateAroundAxis(const Vec3& value, const Vec3& axis, const float radians) -> Vec3
{
    const Vec3 normalizedAxis = Normalize(axis);
    if (LengthSquared(normalizedAxis) <= 1.0e-6f)
    {
        return value;
    }

    const float sinAngle = std::sin(radians);
    const float cosAngle = std::cos(radians);
    return value * cosAngle +
        Cross(normalizedAxis, value) * sinAngle +
        normalizedAxis * (Dot(normalizedAxis, value) * (1.0f - cosAngle));
}

auto MakeBasis(const Vec3& origin, const Vec3& forward, const Vec3& upHint) -> Basis3
{
    const Vec3 basisForward = Normalize(forward);
    Vec3 basisRight = Normalize(Cross(basisForward, upHint));
    if (LengthSquared(basisRight) <= 1.0e-6f)
    {
        basisRight = Normalize(Cross(basisForward, Vec3{0.0f, 0.0f, 1.0f}));
        if (LengthSquared(basisRight) <= 1.0e-6f)
        {
            basisRight = {1.0f, 0.0f, 0.0f};
        }
    }

    const Vec3 basisUp = Normalize(Cross(basisRight, basisForward));
    return {
        origin,
        basisRight,
        basisUp,
        basisForward,
    };
}

auto TransformPoint(const Basis3& basis, const Vec3& localPoint) -> Vec3
{
    return basis.origin +
        basis.right * localPoint.x +
        basis.up * localPoint.y +
        basis.forward * localPoint.z;
}

auto TransformVector(const Basis3& basis, const Vec3& localVector) -> Vec3
{
    return basis.right * localVector.x +
        basis.up * localVector.y +
        basis.forward * localVector.z;
}

void AppendTriangle(std::vector<render::ColorVertex3D>& triangles, const Vec3& a, const Vec3& b, const Vec3& c, const Vec4& color)
{
    if (LengthSquared(Cross(b - a, c - a)) <= 1.0e-8f)
    {
        return;
    }

    const Vec4 litColor = LitColor(color, a, b, c);
    triangles.push_back({a, litColor});
    triangles.push_back({b, litColor});
    triangles.push_back({c, litColor});
}

void AppendQuad(std::vector<render::ColorVertex3D>& triangles, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec4& color)
{
    AppendTriangle(triangles, a, b, c, color);
    AppendTriangle(triangles, a, c, d, color);
}

void AppendBox(std::vector<render::ColorVertex3D>& triangles, const Basis3& basis, const Vec3& halfExtents, const Vec4& color)
{
    const std::array<Vec3, 8> localCorners = {
        Vec3{-halfExtents.x, -halfExtents.y, -halfExtents.z},
        Vec3{ halfExtents.x, -halfExtents.y, -halfExtents.z},
        Vec3{ halfExtents.x,  halfExtents.y, -halfExtents.z},
        Vec3{-halfExtents.x,  halfExtents.y, -halfExtents.z},
        Vec3{-halfExtents.x, -halfExtents.y,  halfExtents.z},
        Vec3{ halfExtents.x, -halfExtents.y,  halfExtents.z},
        Vec3{ halfExtents.x,  halfExtents.y,  halfExtents.z},
        Vec3{-halfExtents.x,  halfExtents.y,  halfExtents.z},
    };

    std::array<Vec3, 8> worldCorners{};
    for (std::size_t index = 0; index < localCorners.size(); ++index)
    {
        worldCorners[index] = TransformPoint(basis, localCorners[index]);
    }

    AppendQuad(triangles, worldCorners[5], worldCorners[6], worldCorners[7], worldCorners[4], color);
    AppendQuad(triangles, worldCorners[1], worldCorners[0], worldCorners[3], worldCorners[2], color);
    AppendQuad(triangles, worldCorners[0], worldCorners[4], worldCorners[7], worldCorners[3], color);
    AppendQuad(triangles, worldCorners[1], worldCorners[2], worldCorners[6], worldCorners[5], color);
    AppendQuad(triangles, worldCorners[3], worldCorners[7], worldCorners[6], worldCorners[2], color);
    AppendQuad(triangles, worldCorners[0], worldCorners[1], worldCorners[5], worldCorners[4], color);
}

void AppendCylinder(
    std::vector<render::ColorVertex3D>& triangles,
    const Basis3& basis,
    const float halfLength,
    const float radius,
    const int segments,
    const Vec4& color,
    const bool capStart,
    const bool capEnd)
{
    const int clampedSegments = std::max(3, segments);
    const Vec3 startCenter = TransformPoint(basis, Vec3{-halfLength, 0.0f, 0.0f});
    const Vec3 endCenter = TransformPoint(basis, Vec3{halfLength, 0.0f, 0.0f});

    for (int segment = 0; segment < clampedSegments; ++segment)
    {
        const float angle0 = (static_cast<float>(segment) / static_cast<float>(clampedSegments)) * kPi * 2.0f;
        const float angle1 = (static_cast<float>(segment + 1) / static_cast<float>(clampedSegments)) * kPi * 2.0f;
        const Vec3 ringOffset0 = basis.up * (std::cos(angle0) * radius) + basis.forward * (std::sin(angle0) * radius);
        const Vec3 ringOffset1 = basis.up * (std::cos(angle1) * radius) + basis.forward * (std::sin(angle1) * radius);

        const Vec3 a = startCenter + ringOffset0;
        const Vec3 b = endCenter + ringOffset0;
        const Vec3 c = endCenter + ringOffset1;
        const Vec3 d = startCenter + ringOffset1;
        AppendQuad(triangles, a, b, c, d, color);

        if (capStart)
        {
            AppendTriangle(triangles, startCenter, d, a, color);
        }
        if (capEnd)
        {
            AppendTriangle(triangles, endCenter, b, c, color);
        }
    }
}

void AppendOctahedron(std::vector<render::ColorVertex3D>& triangles, const Basis3& basis, const Vec3& radii, const Vec4& color)
{
    const Vec3 px = TransformPoint(basis, Vec3{ radii.x, 0.0f, 0.0f});
    const Vec3 nx = TransformPoint(basis, Vec3{-radii.x, 0.0f, 0.0f});
    const Vec3 py = TransformPoint(basis, Vec3{0.0f,  radii.y, 0.0f});
    const Vec3 ny = TransformPoint(basis, Vec3{0.0f, -radii.y, 0.0f});
    const Vec3 pz = TransformPoint(basis, Vec3{0.0f, 0.0f,  radii.z});
    const Vec3 nz = TransformPoint(basis, Vec3{0.0f, 0.0f, -radii.z});

    AppendTriangle(triangles, py, pz, px, color);
    AppendTriangle(triangles, py, nx, pz, color);
    AppendTriangle(triangles, py, nz, nx, color);
    AppendTriangle(triangles, py, px, nz, color);
    AppendTriangle(triangles, ny, px, pz, color);
    AppendTriangle(triangles, ny, pz, nx, color);
    AppendTriangle(triangles, ny, nx, nz, color);
    AppendTriangle(triangles, ny, nz, px, color);
}
}
