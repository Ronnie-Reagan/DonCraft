#include "game/session_render.hpp"

#include "game/model_primitives.hpp"
#include "game/weapon_definitions.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace df::game
{
namespace
{
constexpr std::size_t kMaxSortedTranslucentVertices = 24000u;
constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
constexpr float kShovelViewForwardOffset = 0.14f;
constexpr float kShovelViewForwardTravel = 0.18f;
constexpr float kShovelViewDownTravel = 0.18f;

struct RifleScopeMeshTuning
{
    Vec3 bodyCenterLocalPoint{};
    Vec3 mountCenterLocalPoint{};
    Vec3 mountHalfExtents{};
    Vec2 ringInnerHalfSize{};
    float rearRingLocalZ = 0.0f;
    float frontRingLocalZ = 0.0f;
    float bodyRadius = 0.0f;
    float ringThickness = 0.0f;
};

constexpr RifleScopeMeshTuning kRifleScopeMesh{
    Vec3{0.0f, 0.064f, 0.252f},
    Vec3{0.0f, 0.018f, 0.22f},
    Vec3{0.020f, 0.040f, 0.070f},
    Vec2{0.032f, 0.026f},
    0.150f,
    0.354f,
    0.045f,
    0.007f,
};

auto TriangleSortDistanceSquared(const render::ColorVertex3D& a, const render::ColorVertex3D& b, const render::ColorVertex3D& c, const Vec3& cameraPosition) -> float
{
    const Vec3 centroid = (a.position + b.position + c.position) / 3.0f;
    return LengthSquared(centroid - cameraPosition);
}

void SortTrianglesBackToFront(std::vector<render::ColorVertex3D>& triangles, const Vec3& cameraPosition)
{
    if (triangles.size() < 6)
    {
        return;
    }

    std::vector<std::size_t> order(triangles.size() / 3u);
    for (std::size_t index = 0; index < order.size(); ++index)
    {
        order[index] = index;
    }

    std::sort(order.begin(), order.end(), [&](const std::size_t left, const std::size_t right)
    {
        const std::size_t leftBase = left * 3u;
        const std::size_t rightBase = right * 3u;
        return TriangleSortDistanceSquared(
                   triangles[leftBase + 0u],
                   triangles[leftBase + 1u],
                   triangles[leftBase + 2u],
                   cameraPosition) >
               TriangleSortDistanceSquared(
                   triangles[rightBase + 0u],
                   triangles[rightBase + 1u],
                   triangles[rightBase + 2u],
                   cameraPosition);
    });

    std::vector<render::ColorVertex3D> sorted;
    sorted.reserve(triangles.size());
    for (const std::size_t triangleIndex : order)
    {
        const std::size_t base = triangleIndex * 3u;
        sorted.push_back(triangles[base + 0u]);
        sorted.push_back(triangles[base + 1u]);
        sorted.push_back(triangles[base + 2u]);
    }
    triangles.swap(sorted);
}

auto FlatForwardFromDirection(const Vec3& forward) -> Vec3
{
    const Vec3 flatForward = Normalize(Vec3{forward.x, 0.0f, forward.z});
    return LengthSquared(flatForward) > 1.0e-6f ? flatForward : Vec3{1.0f, 0.0f, 0.0f};
}

auto RightFromForward(const Vec3& forward) -> Vec3
{
    return Normalize(Cross(FlatForwardFromDirection(forward), kWorldUp));
}

auto WeaponSightLocalPoint(const ToolType tool) -> Vec3
{
    return GetWeaponViewTuning(tool).ads.sightLocalPoint;
}

auto WeaponAdsEyeRelief(const ToolType tool) -> float
{
    return GetWeaponViewTuning(tool).ads.eyeReliefMeters;
}

auto MakeAxisBasis(const Vec3& origin, const Vec3& axis, const Vec3& upHint) -> model::Basis3
{
    Vec3 right = Normalize(axis);
    if (LengthSquared(right) <= 1.0e-6f)
    {
        right = Vec3{1.0f, 0.0f, 0.0f};
    }

    Vec3 up = upHint - right * Dot(upHint, right);
    if (LengthSquared(up) <= 1.0e-6f)
    {
        up = kWorldUp - right * Dot(kWorldUp, right);
        if (LengthSquared(up) <= 1.0e-6f)
        {
            up = Vec3{0.0f, 0.0f, 1.0f};
        }
    }
    up = Normalize(up);

    Vec3 forward = Normalize(Cross(right, up));
    if (LengthSquared(forward) <= 1.0e-6f)
    {
        forward = Vec3{0.0f, 0.0f, 1.0f};
    }
    up = Normalize(Cross(forward, right));
    return {origin, right, up, forward};
}

void AppendTracerModel(
    std::vector<render::ColorVertex3D>& triangles,
    const Vec3& start,
    const Vec3& end,
    const Vec4& color,
    const float ttl)
{
    const Vec3 direction = end - start;
    const float distance = Length(direction);
    if (distance <= 0.01f)
    {
        return;
    }

    const Vec3 forward = direction / distance;
    const float tracerLength = std::min(distance, 2.2f);
    const Vec3 tracerStart = end - forward * tracerLength;
    const Vec3 center = (tracerStart + end) * 0.5f;
    const float glow = Clamp(ttl / 0.10f, 0.25f, 1.0f);
    const Vec4 tracerColor = MakeColor(color.x * glow, color.y * glow, color.z * glow, 1.0f);

    const model::Basis3 tracerBasis = MakeAxisBasis(center, forward, kWorldUp);
    model::AppendCylinder(triangles, tracerBasis, tracerLength * 0.5f, 0.018f, 6, tracerColor, false, true);

    model::Basis3 tipBasis = tracerBasis;
    tipBasis.origin = end;
    model::AppendOctahedron(triangles, tipBasis, Vec3{0.06f, 0.028f, 0.028f}, tracerColor);
}

void AppendGrenadeMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const Vec3& position,
    const Vec3& forward,
    const Vec3& up)
{
    const Vec4 bodyColor = MakeColor(0.30f, 0.38f, 0.24f, 1.0f);
    const Vec4 capColor = MakeColor(0.22f, 0.24f, 0.20f, 1.0f);
    const Vec4 metalColor = MakeColor(0.62f, 0.64f, 0.58f, 1.0f);
    const model::Basis3 axisBasis = MakeAxisBasis(position, forward, up);

    model::AppendCylinder(triangles, axisBasis, 0.11f, 0.075f, 8, bodyColor, true, true);

    model::Basis3 capBasis = axisBasis;
    capBasis.origin = model::TransformPoint(axisBasis, Vec3{0.12f, 0.0f, 0.0f});
    model::AppendBox(triangles, capBasis, Vec3{0.025f, 0.055f, 0.055f}, capColor);

    model::Basis3 handleBasis = axisBasis;
    handleBasis.origin = model::TransformPoint(axisBasis, Vec3{0.10f, 0.10f, 0.0f});
    model::AppendBox(triangles, handleBasis, Vec3{0.020f, 0.085f, 0.010f}, metalColor);

    model::Basis3 leverBasis = axisBasis;
    leverBasis.origin = model::TransformPoint(axisBasis, Vec3{0.06f, 0.12f, 0.0f});
    model::AppendBox(triangles, leverBasis, Vec3{0.055f, 0.020f, 0.012f}, metalColor);

    model::Basis3 pinBasis = axisBasis;
    pinBasis.origin = model::TransformPoint(axisBasis, Vec3{0.16f, 0.09f, 0.0f});
    model::AppendOctahedron(triangles, pinBasis, Vec3{0.020f, 0.020f, 0.020f}, metalColor);
}

void AppendRifleScopeRingSegments(
    std::vector<render::ColorVertex3D>& triangles,
    const model::Basis3& basis,
    const float localZ,
    const Vec4& color)
{
    constexpr float kOuterHalfWidth = 0.056f;
    constexpr float kOuterHalfHeight = 0.046f;

    model::Basis3 topBasis = basis;
    topBasis.origin = model::TransformPoint(
        basis,
        Vec3{
            0.0f,
            kRifleScopeMesh.bodyCenterLocalPoint.y + kRifleScopeMesh.ringInnerHalfSize.y + kRifleScopeMesh.ringThickness,
            localZ,
        });
    model::AppendBox(triangles, topBasis, Vec3{kOuterHalfWidth, kRifleScopeMesh.ringThickness, kRifleScopeMesh.ringThickness}, color);

    model::Basis3 bottomBasis = basis;
    bottomBasis.origin = model::TransformPoint(
        basis,
        Vec3{
            0.0f,
            kRifleScopeMesh.bodyCenterLocalPoint.y - kRifleScopeMesh.ringInnerHalfSize.y - kRifleScopeMesh.ringThickness,
            localZ,
        });
    model::AppendBox(triangles, bottomBasis, Vec3{kOuterHalfWidth, kRifleScopeMesh.ringThickness, kRifleScopeMesh.ringThickness}, color);

    model::Basis3 leftBasis = basis;
    leftBasis.origin = model::TransformPoint(
        basis,
        Vec3{
            -kRifleScopeMesh.ringInnerHalfSize.x - kRifleScopeMesh.ringThickness,
            kRifleScopeMesh.bodyCenterLocalPoint.y,
            localZ,
        });
    model::AppendBox(triangles, leftBasis, Vec3{kRifleScopeMesh.ringThickness, kOuterHalfHeight, kRifleScopeMesh.ringThickness}, color);

    model::Basis3 rightBasis = basis;
    rightBasis.origin = model::TransformPoint(
        basis,
        Vec3{
            kRifleScopeMesh.ringInnerHalfSize.x + kRifleScopeMesh.ringThickness,
            kRifleScopeMesh.bodyCenterLocalPoint.y,
            localZ,
        });
    model::AppendBox(triangles, rightBasis, Vec3{kRifleScopeMesh.ringThickness, kOuterHalfHeight, kRifleScopeMesh.ringThickness}, color);
}

void AppendRifleMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const model::Basis3& basis,
    const float cycle)
{
    const Vec4 receiverColor = MakeColor(0.16f, 0.18f, 0.20f, 1.0f);
    const Vec4 accentColor = MakeColor(0.46f, 0.28f, 0.18f, 1.0f);
    const Vec4 metalColor = MakeColor(0.56f, 0.60f, 0.64f, 1.0f);

    model::Basis3 stockBasis = basis;
    stockBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.06f, -0.16f});
    model::AppendBox(triangles, stockBasis, Vec3{0.045f, 0.055f, 0.21f}, accentColor);

    model::Basis3 receiverBasis = basis;
    receiverBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.02f, 0.16f});
    model::AppendBox(triangles, receiverBasis, Vec3{0.055f, 0.060f, 0.22f}, receiverColor);

    model::Basis3 handguardBasis = basis;
    handguardBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.03f, 0.50f});
    model::AppendBox(triangles, handguardBasis, Vec3{0.045f, 0.045f, 0.18f}, accentColor);

    model::Basis3 gripBasis = basis;
    gripBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.16f, 0.07f});
    model::AppendBox(triangles, gripBasis, Vec3{0.035f, 0.12f, 0.035f}, receiverColor);

    model::Basis3 magazineBasis = basis;
    magazineBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.15f, 0.24f});
    model::AppendBox(triangles, magazineBasis, Vec3{0.040f, 0.11f, 0.050f}, metalColor);

    model::Basis3 scopeBodyBasis = MakeAxisBasis(
        model::TransformPoint(basis, kRifleScopeMesh.bodyCenterLocalPoint),
        basis.forward,
        basis.up);
    model::AppendCylinder(triangles, scopeBodyBasis, 0.14f, kRifleScopeMesh.bodyRadius, 10, receiverColor, false, false);

    model::Basis3 scopeMountBasis = basis;
    scopeMountBasis.origin = model::TransformPoint(basis, kRifleScopeMesh.mountCenterLocalPoint);
    model::AppendBox(triangles, scopeMountBasis, kRifleScopeMesh.mountHalfExtents, metalColor);
    AppendRifleScopeRingSegments(triangles, basis, kRifleScopeMesh.rearRingLocalZ, metalColor);
    AppendRifleScopeRingSegments(triangles, basis, kRifleScopeMesh.frontRingLocalZ, metalColor);

    model::Basis3 barrelBasis = MakeAxisBasis(model::TransformPoint(basis, Vec3{0.0f, -0.01f, 0.72f}), basis.forward, basis.up);
    model::AppendCylinder(triangles, barrelBasis, 0.24f, 0.014f, 7, metalColor, false, true);

    model::Basis3 boltBasis = basis;
    boltBasis.origin = model::TransformPoint(basis, Vec3{0.0f, 0.01f, 0.14f - cycle * 0.12f});
    model::AppendBox(triangles, boltBasis, Vec3{0.030f, 0.024f, 0.10f}, metalColor);
}

void AppendRifleScopeOccluderMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const model::Basis3& basis)
{
    const Vec4 metalColor = MakeColor(0.56f, 0.60f, 0.64f, 1.0f);
    AppendRifleScopeRingSegments(triangles, basis, kRifleScopeMesh.rearRingLocalZ, metalColor);
    AppendRifleScopeRingSegments(triangles, basis, kRifleScopeMesh.frontRingLocalZ, metalColor);
}

void AppendSmgMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const model::Basis3& basis,
    const float cycle)
{
    const Vec4 receiverColor = MakeColor(0.16f, 0.18f, 0.20f, 1.0f);
    const Vec4 polymerColor = MakeColor(0.18f, 0.20f, 0.22f, 1.0f);
    const Vec4 metalColor = MakeColor(0.58f, 0.62f, 0.66f, 1.0f);
    const Vec4 accentColor = MakeColor(0.26f, 0.30f, 0.24f, 1.0f);

    model::Basis3 stockTubeBasis = MakeAxisBasis(model::TransformPoint(basis, Vec3{0.0f, 0.00f, -0.18f}), basis.forward, basis.up);
    model::AppendCylinder(triangles, stockTubeBasis, 0.18f, 0.020f, 8, metalColor, true, true);

    model::Basis3 buttBasis = basis;
    buttBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.01f, -0.32f});
    model::AppendBox(triangles, buttBasis, Vec3{0.055f, 0.055f, 0.050f}, polymerColor);

    model::Basis3 receiverBasis = MakeAxisBasis(model::TransformPoint(basis, Vec3{0.0f, -0.01f, 0.10f}), basis.forward, basis.up);
    model::AppendCylinder(triangles, receiverBasis, 0.26f, 0.046f, 14, receiverColor, true, true);

    model::Basis3 handguardBasis = MakeAxisBasis(model::TransformPoint(basis, Vec3{0.0f, -0.02f, 0.40f}), basis.forward, basis.up);
    model::AppendCylinder(triangles, handguardBasis, 0.16f, 0.048f, 14, accentColor, true, true);

    model::Basis3 barrelBasis = MakeAxisBasis(model::TransformPoint(basis, Vec3{0.0f, -0.01f, 0.63f}), basis.forward, basis.up);
    model::AppendCylinder(triangles, barrelBasis, 0.18f, 0.013f, 8, metalColor, false, true);

    model::Basis3 shroudBasis = MakeAxisBasis(model::TransformPoint(basis, Vec3{0.0f, -0.01f, 0.55f}), basis.forward, basis.up);
    model::AppendCylinder(triangles, shroudBasis, 0.08f, 0.024f, 10, receiverColor, false, true);

    model::Basis3 magwellBasis = basis;
    magwellBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.10f, 0.18f});
    model::AppendBox(triangles, magwellBasis, Vec3{0.034f, 0.070f, 0.050f}, receiverColor);

    model::Basis3 magazineBasis = basis;
    magazineBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.23f, 0.22f});
    model::AppendBox(triangles, magazineBasis, Vec3{0.030f, 0.150f, 0.042f}, metalColor);

    model::Basis3 gripBasis = basis;
    gripBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.16f, 0.06f});
    model::AppendBox(triangles, gripBasis, Vec3{0.034f, 0.120f, 0.036f}, polymerColor);

    model::Basis3 foregripBasis = basis;
    foregripBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.14f, 0.47f});
    model::AppendBox(triangles, foregripBasis, Vec3{0.030f, 0.085f, 0.028f}, polymerColor);

    model::Basis3 chargingHandleBasis = basis;
    chargingHandleBasis.origin = model::TransformPoint(basis, Vec3{0.0f, 0.02f, 0.02f - cycle * 0.05f});
    model::AppendBox(triangles, chargingHandleBasis, Vec3{0.016f, 0.016f, 0.055f}, metalColor);

    model::Basis3 railBasis = basis;
    railBasis.origin = model::TransformPoint(basis, Vec3{0.0f, 0.050f, 0.15f});
    model::AppendBox(triangles, railBasis, Vec3{0.032f, 0.010f, 0.160f}, metalColor);

    model::Basis3 holoBaseBasis = basis;
    holoBaseBasis.origin = model::TransformPoint(basis, Vec3{0.0f, 0.060f, 0.13f});
    model::AppendBox(triangles, holoBaseBasis, Vec3{0.028f, 0.022f, 0.050f}, metalColor);

    model::Basis3 holoLeftBasis = basis;
    holoLeftBasis.origin = model::TransformPoint(basis, Vec3{-0.034f, 0.088f, 0.15f});
    model::AppendBox(triangles, holoLeftBasis, Vec3{0.006f, 0.034f, 0.045f}, metalColor);

    model::Basis3 holoRightBasis = basis;
    holoRightBasis.origin = model::TransformPoint(basis, Vec3{0.034f, 0.088f, 0.15f});
    model::AppendBox(triangles, holoRightBasis, Vec3{0.006f, 0.034f, 0.045f}, metalColor);

    model::Basis3 holoTopBasis = basis;
    holoTopBasis.origin = model::TransformPoint(basis, Vec3{0.0f, 0.118f, 0.15f});
    model::AppendBox(triangles, holoTopBasis, Vec3{0.034f, 0.006f, 0.045f}, metalColor);

}

void AppendShovelMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const model::Basis3& basis)
{
    const Vec4 woodColor = MakeColor(0.48f, 0.31f, 0.19f, 1.0f);
    const Vec4 steelColor = MakeColor(0.52f, 0.58f, 0.62f, 1.0f);

    model::Basis3 handleBasis = MakeAxisBasis(model::TransformPoint(basis, Vec3{0.0f, -0.03f, 0.30f}), basis.forward, basis.up);
    model::AppendCylinder(triangles, handleBasis, 0.42f, 0.018f, 7, woodColor, true, true);

    model::Basis3 pommelBasis = basis;
    pommelBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.03f, -0.15f});
    model::AppendBox(triangles, pommelBasis, Vec3{0.045f, 0.030f, 0.040f}, woodColor);

    model::Basis3 bladeStemBasis = basis;
    bladeStemBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.05f, 0.70f});
    model::AppendBox(triangles, bladeStemBasis, Vec3{0.020f, 0.030f, 0.060f}, steelColor);

    model::Basis3 bladeBasis = basis;
    bladeBasis.origin = model::TransformPoint(basis, Vec3{0.0f, -0.08f, 0.88f});
    model::AppendBox(triangles, bladeBasis, Vec3{0.11f, 0.020f, 0.12f}, steelColor);
}

auto BuildHeldItemBasis(
    const ToolType tool,
    const Vec3& aimPosition,
    const Vec3& forward,
    const float walkCycle,
    const float moveSpeed,
    const float weaponCycle,
    const float adsBlend = 0.0f) -> model::Basis3
{
    model::Basis3 basis = model::MakeBasis(aimPosition, forward, kWorldUp);
    const float swayWeight = Clamp(moveSpeed / 7.5f, 0.0f, 1.0f);
    const float clampedAdsBlend = Clamp(adsBlend, 0.0f, 1.0f);
    basis.origin += basis.right * Lerp(std::sin(walkCycle) * 0.024f * swayWeight + 0.30f, 0.015f, clampedAdsBlend);
    basis.origin -= basis.up * Lerp(0.21f - std::abs(std::sin(walkCycle * 0.5f)) * 0.018f * swayWeight, 0.055f, clampedAdsBlend);
    basis.origin += basis.forward * Lerp(0.52f, 0.66f, clampedAdsBlend);
    const WeaponDefinition definition = GetWeaponDefinition(tool);
    basis.origin -= basis.forward * (weaponCycle * weaponCycle) * definition.viewModelKickDistance;
    basis.origin += basis.up * weaponCycle * definition.viewModelKickRise;
    if (clampedAdsBlend > 0.0f && definition.supportsAds)
    {
        const Vec3 sightPoint = model::TransformPoint(basis, WeaponSightLocalPoint(tool));
        const Vec3 desiredSightPoint = aimPosition + basis.forward * WeaponAdsEyeRelief(tool);
        basis.origin += (desiredSightPoint - sightPoint) * clampedAdsBlend;
    }
    return basis;
}

auto BuildShovelBasis(
    const Vec3& aimPosition,
    const Vec3& forward,
    const float walkCycle,
    const float moveSpeed,
    const float weaponCycle) -> model::Basis3
{
    model::Basis3 basis = BuildHeldItemBasis(ToolType::Dig, aimPosition, forward, walkCycle, moveSpeed, weaponCycle);
    const float swingPhase = Clamp(weaponCycle, 0.0f, 1.0f);
    const float swingPitch = DegreesToRadians(Lerp(30.0f, -56.0f, swingPhase));
    const float swingYaw = DegreesToRadians(Lerp(-8.0f, 12.0f, swingPhase));
    basis.forward = Normalize(model::RotateAroundAxis(basis.forward, basis.right, swingPitch));
    basis.forward = Normalize(model::RotateAroundAxis(basis.forward, basis.up, swingYaw));
    basis.up = Normalize(Cross(basis.right, basis.forward));
    basis.origin += basis.forward * (kShovelViewForwardOffset + swingPhase * kShovelViewForwardTravel);
    basis.origin -= basis.up * (0.01f + swingPhase * kShovelViewDownTravel);
    return basis;
}

void AppendBlobCharacterMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const Vec3& position,
    const Vec3& facingForward,
    const Vec3& leftFoot,
    const Vec3& rightFoot,
    bool leftGrounded,
    bool rightGrounded);

auto BuildCharacterHeldToolBasis(
    const Vec3& position,
    const Vec3& lookForward,
    const ToolType tool,
    const float walkCycle,
    const float moveSpeed,
    const float weaponCycle) -> model::Basis3
{
    const Vec3 resolvedForward =
        LengthSquared(lookForward) > 1.0e-6f
            ? Normalize(lookForward)
            : Vec3{0.0f, 0.0f, 1.0f};
    const Vec3 aimAnchor = position + Vec3{0.0f, 0.78f, 0.0f};

    model::Basis3 basis =
        tool == ToolType::Dig
            ? BuildShovelBasis(aimAnchor, resolvedForward, walkCycle, moveSpeed, weaponCycle)
            : BuildHeldItemBasis(tool, aimAnchor, resolvedForward, walkCycle, moveSpeed, weaponCycle);
    basis.origin -= basis.forward * 0.50f;
    basis.origin -= basis.up * 0.40f;
    basis.origin -= basis.right * 0.22f;
    return basis;
}

void AppendCharacterHeldToolMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const Vec3& position,
    const Vec3& lookForward,
    const float walkCycle,
    const float moveSpeed,
    const ToolType tool,
    const float weaponCycle)
{
    const float clampedWeaponCycle = Clamp(weaponCycle, 0.0f, 1.0f);
    const model::Basis3 basis = BuildCharacterHeldToolBasis(position, lookForward, tool, walkCycle, moveSpeed, clampedWeaponCycle);

    switch (tool)
    {
    case ToolType::Rifle:
        AppendRifleMesh(triangles, basis, clampedWeaponCycle);
        break;
    case ToolType::Smg:
        AppendSmgMesh(triangles, basis, clampedWeaponCycle);
        break;
    case ToolType::Grenade:
        AppendGrenadeMesh(triangles, basis.origin, basis.forward, basis.up);
        break;
    case ToolType::Dig:
        AppendShovelMesh(triangles, basis);
        break;
    default:
        break;
    }
}

void AppendCharacterModel(
    std::vector<render::ColorVertex3D>& triangles,
    const Vec3& position,
    const Vec3& bodyForward,
    const Vec3& lookForward,
    const Vec3& leftFoot,
    const Vec3& rightFoot,
    const bool leftGrounded,
    const bool rightGrounded,
    const float walkCycle,
    const float moveSpeed,
    const ToolType tool,
    const float weaponCycle,
    const bool drivingTruck)
{
    AppendBlobCharacterMesh(
        triangles,
        position,
        bodyForward,
        leftFoot,
        rightFoot,
        leftGrounded,
        rightGrounded);
    if (!drivingTruck)
    {
        AppendCharacterHeldToolMesh(triangles, position, lookForward, walkCycle, moveSpeed, tool, weaponCycle);
    }
}

void AppendBlobCharacterMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const Vec3& position,
    const Vec3& facingForward,
    const Vec3& leftFoot,
    const Vec3& rightFoot,
    const bool leftGrounded,
    const bool rightGrounded)
{
    const Vec4 bodyColor = MakeColor(0.80f, 0.67f, 0.44f, 1.0f);
    const Vec4 limbColor = MakeColor(0.18f, 0.16f, 0.14f, 1.0f);
    const Vec4 eyeColor = MakeColor(0.03f, 0.03f, 0.04f, 1.0f);

    const Vec3 flatForward = FlatForwardFromDirection(facingForward);
    model::Basis3 bodyBasis = model::MakeBasis(position + Vec3{0.0f, -0.02f, 0.0f}, flatForward, kWorldUp);
    const float bodyGroundY = std::min(leftFoot.y, rightFoot.y);
    const float bodyClearance = position.y - bodyGroundY;
    const float squash = Clamp(1.03f - bodyClearance * 0.03f, 0.94f, 1.04f);
    bodyBasis.origin.y += Clamp((leftFoot.y + rightFoot.y) * 0.5f - (position.y - 0.92f), -0.06f, 0.08f);

    model::AppendOctahedron(triangles, bodyBasis, Vec3{0.36f, 0.74f * squash, 0.30f}, bodyColor);

    model::Basis3 headBasis = bodyBasis;
    headBasis.origin = model::TransformPoint(bodyBasis, Vec3{0.0f, 0.58f, 0.05f});
    model::AppendOctahedron(triangles, headBasis, Vec3{0.24f, 0.28f, 0.22f}, bodyColor);

    const Vec3 leftHip = model::TransformPoint(bodyBasis, Vec3{-0.16f, -0.46f, 0.02f});
    const Vec3 rightHip = model::TransformPoint(bodyBasis, Vec3{0.16f, -0.46f, 0.02f});

    const model::Basis3 leftLegBasis = MakeAxisBasis((leftHip + leftFoot) * 0.5f, leftFoot - leftHip, bodyBasis.forward);
    model::AppendCylinder(triangles, leftLegBasis, Length(leftFoot - leftHip) * 0.5f, 0.055f, 6, limbColor, true, true);

    const model::Basis3 rightLegBasis = MakeAxisBasis((rightHip + rightFoot) * 0.5f, rightFoot - rightHip, bodyBasis.forward);
    model::AppendCylinder(triangles, rightLegBasis, Length(rightFoot - rightHip) * 0.5f, 0.055f, 6, limbColor, true, true);

    model::Basis3 leftEyeBasis = bodyBasis;
    leftEyeBasis.origin = model::TransformPoint(bodyBasis, Vec3{-0.08f, 0.55f, 0.25f});
    model::AppendBox(triangles, leftEyeBasis, Vec3{0.035f, 0.035f, 0.020f}, eyeColor);

    model::Basis3 rightEyeBasis = bodyBasis;
    rightEyeBasis.origin = model::TransformPoint(bodyBasis, Vec3{0.08f, 0.55f, 0.25f});
    model::AppendBox(triangles, rightEyeBasis, Vec3{0.035f, 0.035f, 0.020f}, eyeColor);

    model::Basis3 leftFootBasis = model::MakeBasis(leftFoot, flatForward, kWorldUp);
    model::AppendBox(triangles, leftFootBasis, Vec3{0.12f, 0.08f, 0.18f}, leftGrounded ? limbColor : MakeColor(0.24f, 0.22f, 0.20f, 1.0f));

    model::Basis3 rightFootBasis = model::MakeBasis(rightFoot, flatForward, kWorldUp);
    model::AppendBox(triangles, rightFootBasis, Vec3{0.12f, 0.08f, 0.18f}, rightGrounded ? limbColor : MakeColor(0.24f, 0.22f, 0.20f, 1.0f));
}

void AppendTruckApproxMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const Vec3& position,
    const Vec3& forward,
    const float sink)
{
    const Vec4 chassisColor = MakeColor(0.81f, 0.25f, 0.14f, 1.0f);
    const Vec4 trimColor = MakeColor(0.16f, 0.18f, 0.20f, 1.0f);
    const Vec4 cabinColor = MakeColor(0.72f, 0.20f, 0.14f, 1.0f);
    const Vec4 wheelColor = MakeColor(0.10f, 0.11f, 0.13f, 1.0f);
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const model::Basis3 bodyBasis = model::MakeBasis(position - Vec3{0.0f, sink * 0.12f, 0.0f}, forward, up);

    model::Basis3 lowerBasis = bodyBasis;
    lowerBasis.origin = model::TransformPoint(bodyBasis, Vec3{0.0f, -0.06f, -0.02f});
    model::AppendBox(triangles, lowerBasis, Vec3{0.86f, 0.22f, 1.44f}, chassisColor);

    model::Basis3 cabBasis = bodyBasis;
    cabBasis.origin = model::TransformPoint(bodyBasis, Vec3{0.0f, 0.32f, 0.36f});
    model::AppendBox(triangles, cabBasis, Vec3{0.76f, 0.34f, 0.58f}, cabinColor);

    model::Basis3 hoodBasis = bodyBasis;
    hoodBasis.origin = model::TransformPoint(bodyBasis, Vec3{0.0f, 0.16f, 1.02f});
    model::AppendBox(triangles, hoodBasis, Vec3{0.72f, 0.16f, 0.44f}, chassisColor);

    model::Basis3 bumperBasis = bodyBasis;
    bumperBasis.origin = model::TransformPoint(bodyBasis, Vec3{0.0f, -0.10f, 1.60f});
    model::AppendBox(triangles, bumperBasis, Vec3{0.82f, 0.10f, 0.12f}, trimColor);

    constexpr std::array<Vec3, 4> wheelOffsets = {
        Vec3{-0.72f, -0.44f,  1.08f},
        Vec3{ 0.72f, -0.44f,  1.08f},
        Vec3{-0.72f, -0.44f, -1.08f},
        Vec3{ 0.72f, -0.44f, -1.08f},
    };
    for (const Vec3& wheelOffset : wheelOffsets)
    {
        model::Basis3 wheelBasis = bodyBasis;
        wheelBasis.origin = model::TransformPoint(bodyBasis, wheelOffset);
        model::AppendCylinder(triangles, wheelBasis, 0.13f, 0.32f, 10, wheelColor, true, true);
    }
}

auto BuildPerspective(
    const Vec3& cameraPosition,
    const Vec3& viewTarget,
    const float fovDegrees,
    const float farPlaneMeters,
    const int viewportWidth,
    const int viewportHeight) -> Mat4
{
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(std::max(viewportHeight, 1));
    return PerspectiveMatrix(DegreesToRadians(fovDegrees), aspect, 0.1f, std::max(farPlaneMeters, 16.0f)) *
        LookAtMatrix(cameraPosition, viewTarget, kWorldUp);
}

auto BuildThirdPersonCameraPosition(
    const world::DemoWorld& world,
    const Vec3& cameraAnchor,
    const Vec3& flatForward) -> Vec3
{
    const Vec3 desired = cameraAnchor - flatForward * 4.4f + Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 offset = desired - cameraAnchor;
    const float distance = Length(offset);
    if (distance <= 0.01f)
    {
        return desired;
    }

    const Vec3 direction = offset / distance;
    const world::RaycastHit hit = world.Raycast({cameraAnchor, direction}, distance);
    if (hit.hit)
    {
        return cameraAnchor + direction * std::max(0.30f, hit.distance - 0.24f);
    }

    return desired;
}

auto BuildViewCameraPosition(
    const world::DemoWorld& world,
    const PlayerController* const player,
    const bool drivingTruck,
    const Vec3& truckCameraPosition,
    const SessionRenderOptions& options,
    const Vec3& fallbackCameraPosition = Vec3{0.0f, 4.0f, -8.0f},
    const Vec3& fallbackFlatForward = Vec3{0.0f, 0.0f, 1.0f},
    const bool hasFallback = false) -> Vec3
{
    if (drivingTruck)
    {
        return truckCameraPosition;
    }
    if (player != nullptr)
    {
        if (!options.thirdPerson)
        {
            return player->CameraPosition();
        }

        const Vec3 anchor = player->CameraPosition() + Vec3{0.0f, 0.18f, 0.0f};
        return BuildThirdPersonCameraPosition(world, anchor, player->FlatForwardVector());
    }

    if (!hasFallback)
    {
        return fallbackCameraPosition;
    }

    if (!options.thirdPerson)
    {
        return fallbackCameraPosition;
    }

    const Vec3 anchor = fallbackCameraPosition + Vec3{0.0f, 0.18f, 0.0f};
    return BuildThirdPersonCameraPosition(world, anchor, fallbackFlatForward);
}

auto BuildMainSessionFovDegrees() -> float
{
    return 70.0f;
}

auto BuildScopedSessionFovDegrees(
    const SessionRenderOptions& options,
    const int scopedViewportWidth,
    const int scopedViewportHeight,
    const int screenWidth,
    const int screenHeight) -> float
{
    const float baseFovRadians = DegreesToRadians(BuildMainSessionFovDegrees());
    const float viewportFraction = Clamp(
        std::min(
            static_cast<float>(scopedViewportWidth) / static_cast<float>(std::max(screenWidth, 1)),
            static_cast<float>(scopedViewportHeight) / static_cast<float>(std::max(screenHeight, 1))),
        0.08f,
        1.0f);
    const float scopedHalfFovRadians = std::atan(std::tan(baseFovRadians * 0.5f) * viewportFraction / std::max(options.zoomMagnification, 1.0f));
    return Clamp(scopedHalfFovRadians * 2.0f * (180.0f / kPi), 2.0f, BuildMainSessionFovDegrees());
}

auto ProjectWorldPointToScreen(
    const Mat4& worldToClip,
    const Vec3& point,
    const int viewportWidth,
    const int viewportHeight,
    float& screenX,
    float& screenY) -> bool
{
    const Vec3 clip = TransformPoint(worldToClip, point);
    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z))
    {
        return false;
    }

    screenX = (clip.x * 0.5f + 0.5f) * static_cast<float>(viewportWidth);
    screenY = (clip.y * 0.5f + 0.5f) * static_cast<float>(viewportHeight);
    return true;
}

auto ProjectScopeViewportRect(
    const Mat4& worldToClip,
    const model::Basis3& scopeBasis,
    const ScopeScreenTuning& screenTuning,
    const int viewportWidth,
    const int viewportHeight,
    float& minX,
    float& minY,
    float& maxX,
    float& maxY) -> bool
{
    const std::array<Vec3, 4> localCorners = {
        screenTuning.localCenterPoint + Vec3{-screenTuning.localHalfSize.x, -screenTuning.localHalfSize.y, 0.0f},
        screenTuning.localCenterPoint + Vec3{-screenTuning.localHalfSize.x,  screenTuning.localHalfSize.y, 0.0f},
        screenTuning.localCenterPoint + Vec3{ screenTuning.localHalfSize.x, -screenTuning.localHalfSize.y, 0.0f},
        screenTuning.localCenterPoint + Vec3{ screenTuning.localHalfSize.x,  screenTuning.localHalfSize.y, 0.0f},
    };

    bool anyProjected = false;
    minX = static_cast<float>(viewportWidth);
    minY = static_cast<float>(viewportHeight);
    maxX = 0.0f;
    maxY = 0.0f;
    for (const Vec3& localCorner : localCorners)
    {
        float screenX = 0.0f;
        float screenY = 0.0f;
        if (!ProjectWorldPointToScreen(
                worldToClip,
                model::TransformPoint(scopeBasis, localCorner),
                viewportWidth,
                viewportHeight,
                screenX,
                screenY))
        {
            continue;
        }

        anyProjected = true;
        minX = std::min(minX, screenX);
        minY = std::min(minY, screenY);
        maxX = std::max(maxX, screenX);
        maxY = std::max(maxY, screenY);
    }

    if (!anyProjected)
    {
        return false;
    }

    minX += screenTuning.pixelOffset.x;
    maxX += screenTuning.pixelOffset.x;
    minY += screenTuning.pixelOffset.y;
    maxY += screenTuning.pixelOffset.y;

    const float padding = std::max(screenTuning.paddingPixels, 0.0f);
    minX = std::max(0.0f, minX - padding);
    minY = std::max(0.0f, minY - padding);
    maxX = std::min(static_cast<float>(viewportWidth), maxX + padding);
    maxY = std::min(static_cast<float>(viewportHeight), maxY + padding);
    return maxX > minX && maxY > minY;
}

auto ShouldEnableScopedView(const SessionRenderOptions& options) -> bool
{
    const WeaponDefinition definition = GetWeaponDefinition(options.localTool);
    return options.aimDownSights && definition.supportsAds && definition.usesScope && !options.thirdPerson;
}

void ConfigureScopedView(
    render::FrameRenderData& data,
    const SessionRenderOptions& options,
    const model::Basis3* const scopeBasis,
    const int viewportWidth,
    const int viewportHeight)
{
    const WeaponViewTuning viewTuning = GetWeaponViewTuning(options.localTool);
    if (!ShouldEnableScopedView(options) || scopeBasis == nullptr || !viewTuning.scope.enabled)
    {
        data.scopedView = {};
        return;
    }

    const WeaponScopeTuning& scopeTuning = viewTuning.scope;
    data.scopedView.enabled = true;

    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    if (ProjectScopeViewportRect(data.worldToClip, *scopeBasis, scopeTuning.screen, viewportWidth, viewportHeight, minX, minY, maxX, maxY))
    {
        data.scopedView.viewportX = std::clamp(static_cast<int>(std::floor(minX)), 0, std::max(viewportWidth - 1, 0));
        data.scopedView.viewportY = std::clamp(static_cast<int>(std::floor(minY)), 0, std::max(viewportHeight - 1, 0));
        data.scopedView.viewportWidth = std::max(1, std::min(viewportWidth - data.scopedView.viewportX, static_cast<int>(std::ceil(maxX - minX))));
        data.scopedView.viewportHeight = std::max(1, std::min(viewportHeight - data.scopedView.viewportY, static_cast<int>(std::ceil(maxY - minY))));
    }
    else
    {
        const int insetSize = std::clamp(
            static_cast<int>(std::min(viewportWidth, viewportHeight) * scopeTuning.screen.fallbackScreenFraction),
            scopeTuning.screen.fallbackMinSizePixels,
            scopeTuning.screen.fallbackMaxSizePixels);
        const Vec3 scopeViewportAnchor = model::TransformPoint(*scopeBasis, scopeTuning.screen.localCenterPoint);

        float lensCenterX = static_cast<float>(viewportWidth) * 0.5f;
        float lensCenterY = static_cast<float>(viewportHeight) * 0.5f;
        (void)ProjectWorldPointToScreen(
            data.worldToClip,
            scopeViewportAnchor,
            viewportWidth,
            viewportHeight,
            lensCenterX,
            lensCenterY);
        lensCenterX += scopeTuning.screen.pixelOffset.x;
        lensCenterY += scopeTuning.screen.pixelOffset.y;

        data.scopedView.viewportWidth = insetSize;
        data.scopedView.viewportHeight = insetSize;
        data.scopedView.viewportX = std::clamp(static_cast<int>(std::lround(lensCenterX)) - insetSize / 2, 0, std::max(viewportWidth - insetSize, 0));
        data.scopedView.viewportY = std::clamp(static_cast<int>(std::lround(lensCenterY)) - insetSize / 2, 0, std::max(viewportHeight - insetSize, 0));
    }

    const Vec3 scopedCameraPosition = model::TransformPoint(*scopeBasis, scopeTuning.camera.localPosition);
    const Vec3 localFocalVector = scopeTuning.camera.localFocalPoint - scopeTuning.camera.localPosition;
    Vec3 focalDirection = model::TransformVector(*scopeBasis, localFocalVector);
    if (LengthSquared(focalDirection) <= 1.0e-6f)
    {
        focalDirection = scopeBasis->forward;
    }
    else
    {
        focalDirection = Normalize(focalDirection);
    }
    const float scopedTargetDistance = std::max(
        scopeTuning.camera.minimumTargetDistanceMeters,
        Length(localFocalVector));
    const Vec3 scopedViewTarget = scopedCameraPosition + focalDirection * scopedTargetDistance;
    data.scopedView.worldToClip = BuildPerspective(
        scopedCameraPosition,
        scopedViewTarget,
        BuildScopedSessionFovDegrees(
            options,
            data.scopedView.viewportWidth,
            data.scopedView.viewportHeight,
            viewportWidth,
            viewportHeight),
        options.terrainDrawDistanceMeters + scopeTuning.camera.farPlanePaddingMeters,
        data.scopedView.viewportWidth,
        data.scopedView.viewportHeight);
}

void AppendHeldToolMesh(
    std::vector<render::ColorVertex3D>& triangles,
    const Vec3& aimPosition,
    const Vec3& forward,
    const float walkCycle,
    const float moveSpeed,
    const ToolType tool,
    const float weaponCycle,
    const bool aimDownSights,
    const model::Basis3* const rifleBasisOverride = nullptr)
{
    if (tool == ToolType::Rifle)
    {
        const model::Basis3 weaponBasis =
            rifleBasisOverride != nullptr
                ? *rifleBasisOverride
                : BuildHeldItemBasis(tool, aimPosition, forward, walkCycle, moveSpeed, weaponCycle, aimDownSights ? 1.0f : 0.0f);
        AppendRifleMesh(triangles, weaponBasis, weaponCycle);
        return;
    }

    if (tool == ToolType::Smg)
    {
        const model::Basis3 weaponBasis = BuildHeldItemBasis(tool, aimPosition, forward, walkCycle, moveSpeed, weaponCycle, aimDownSights ? 1.0f : 0.0f);
        AppendSmgMesh(triangles, weaponBasis, weaponCycle);
        return;
    }

    if (tool == ToolType::Grenade)
    {
        const model::Basis3 weaponBasis = BuildHeldItemBasis(tool, aimPosition, forward, walkCycle, moveSpeed, weaponCycle);
        AppendGrenadeMesh(triangles, weaponBasis.origin, weaponBasis.forward, weaponBasis.up);
        return;
    }

    const model::Basis3 shovelBasis = BuildShovelBasis(aimPosition, forward, walkCycle, moveSpeed, weaponCycle);
    AppendShovelMesh(triangles, shovelBasis);
}
}

auto BuildRuntimeRenderData(
    SessionRuntime& runtime,
    const PlayerId localPlayerId,
    const PlayerController* const predictedLocalPlayer,
    const SessionRenderOptions& options,
    const int viewportWidth,
    const int viewportHeight) -> render::FrameRenderData
{
    render::FrameRenderData data{};
    data.clearColor = MakeColor(0.53f, 0.73f, 0.92f, 1.0f);

    const SessionRuntime::PlayerState* const localPlayer = runtime.FindPlayer(localPlayerId);
    const PlayerController* const cameraPlayer = predictedLocalPlayer != nullptr ? predictedLocalPlayer : (localPlayer != nullptr ? &localPlayer->controller : nullptr);
    const bool drivingTruck = localPlayer != nullptr && localPlayer->drivingTruck;
    const Vec3 cameraPosition = BuildViewCameraPosition(runtime.World(), cameraPlayer, drivingTruck, runtime.Truck().CameraPosition(), options);
    const Vec3 forward = drivingTruck ? runtime.Truck().ForwardVector() : (cameraPlayer != nullptr ? cameraPlayer->ForwardVector() : Vec3{0.0f, 0.0f, 1.0f});
    const Vec3 aimPosition = drivingTruck ? runtime.Truck().CameraPosition() : (cameraPlayer != nullptr ? cameraPlayer->CameraPosition() : cameraPosition);
    data.worldToClip = BuildPerspective(
        cameraPosition,
        aimPosition + forward * std::max(options.terrainDrawDistanceMeters, 24.0f),
        BuildMainSessionFovDegrees(),
        options.terrainDrawDistanceMeters + 64.0f,
        viewportWidth,
        viewportHeight);

    runtime.MutableWorld().GatherRenderGeometrySmoothedCulled(
        data.terrainTriangleStorage,
        data.translucentTerrainTriangleStorage,
        data.debugLines,
        data.worldToClip,
        cameraPosition,
        options.terrainDrawDistanceMeters,
        options.showWireframe,
        options.showActiveChunks);
    data.terrainTriangles = data.terrainTriangleStorage;
    data.translucentTerrainTriangles = data.translucentTerrainTriangleStorage;
    data.terrainMeshVersion = runtime.World().TerrainMeshVersion();
    if (!data.translucentTerrainTriangles.empty() && data.translucentTerrainTriangles.size() <= kMaxSortedTranslucentVertices)
    {
        SortTrianglesBackToFront(data.translucentTerrainTriangleStorage, cameraPosition);
        data.translucentTerrainTriangles = data.translucentTerrainTriangleStorage;
    }

    runtime.Truck().AppendModelTriangles(data.dynamicTriangles, data.dynamicTranslucentTriangles);
    if (options.showWireframe)
    {
        runtime.Truck().AppendDebugLines(data.debugLines);
    }

    for (const SessionRuntime::Beam& beam : runtime.Beams())
    {
        AppendTracerModel(data.effectTriangles, beam.start, beam.end, beam.color, beam.ttl);
    }

    for (const SessionRuntime::Bullet& bullet : runtime.Bullets())
    {
        AppendTracerModel(data.effectTriangles, bullet.previousPosition, bullet.position, MakeColor(1.0f, 0.84f, 0.44f, 1.0f), 0.10f);
    }

    for (const SessionRuntime::Grenade& grenade : runtime.Grenades())
    {
        AppendGrenadeMesh(data.dynamicTriangles, grenade.position, grenade.orientationForward, grenade.orientationUp);
    }

    for (const auto& [id, player] : runtime.Players())
    {
        if (!options.thirdPerson && id == localPlayerId && !player.drivingTruck)
        {
            continue;
        }

        AppendCharacterModel(
            data.dynamicTriangles,
            player.controller.Position(),
            player.controller.FlatForwardVector(),
            player.controller.ForwardVector(),
            player.controller.LeftFootPosition(),
            player.controller.RightFootPosition(),
            player.controller.LeftFootGrounded(),
            player.controller.RightFootGrounded(),
            player.controller.WalkCycleRadians(),
            player.controller.HorizontalSpeedMetersPerSecond(),
            player.tool,
            player.weaponCycle,
            player.drivingTruck);
    }

    if (!options.thirdPerson && localPlayer != nullptr && !localPlayer->drivingTruck)
    {
        const PlayerController* const viewPlayer = predictedLocalPlayer != nullptr ? predictedLocalPlayer : &localPlayer->controller;
        const WeaponDefinition localWeapon = GetWeaponDefinition(options.localTool);
        const bool localAds = options.aimDownSights && localWeapon.supportsAds;
        const bool rifleAds = localAds && localWeapon.usesScope;
        model::Basis3 rifleBasis{};
        const model::Basis3* rifleBasisOverride = nullptr;
        if (options.localTool == ToolType::Rifle)
        {
            rifleBasis = BuildHeldItemBasis(
                options.localTool,
                viewPlayer->CameraPosition(),
                viewPlayer->ForwardVector(),
                viewPlayer->WalkCycleRadians(),
                viewPlayer->HorizontalSpeedMetersPerSecond(),
                std::max(localPlayer->weaponCycle, options.localWeaponCycle),
                localAds ? 1.0f : 0.0f);
            rifleBasisOverride = &rifleBasis;
        }

        AppendHeldToolMesh(
            data.viewModelTriangles,
            viewPlayer->CameraPosition(),
            viewPlayer->ForwardVector(),
            viewPlayer->WalkCycleRadians(),
            viewPlayer->HorizontalSpeedMetersPerSecond(),
            options.localTool,
            std::max(localPlayer->weaponCycle, options.localWeaponCycle),
            localAds,
            rifleBasisOverride);
        if (rifleAds && rifleBasisOverride != nullptr)
        {
            AppendRifleScopeOccluderMesh(data.viewModelPostScopeTriangles, *rifleBasisOverride);
        }
        ConfigureScopedView(data, options, rifleAds ? rifleBasisOverride : nullptr, viewportWidth, viewportHeight);
    }
    else
    {
        ConfigureScopedView(data, options, nullptr, viewportWidth, viewportHeight);
    }

    if (!data.dynamicTranslucentTriangles.empty() && data.dynamicTranslucentTriangles.size() <= kMaxSortedTranslucentVertices)
    {
        SortTrianglesBackToFront(data.dynamicTranslucentTriangles, cameraPosition);
    }

    return data;
}

auto BuildClientRenderData(
    net::SessionClient& client,
    const SessionRenderOptions& options,
    const int viewportWidth,
    const int viewportHeight,
    const float interpolationAlpha) -> render::FrameRenderData
{
    render::FrameRenderData data{};
    data.clearColor = MakeColor(0.53f, 0.73f, 0.92f, 1.0f);

    const net::ActorSnapshotFrame& authoritativeFrame = client.ActorFrame();
    const net::ActorSnapshotFrame frame = client.BuildRenderActorFrame(interpolationAlpha);
    const net::ActorSnapshot* localActor = nullptr;
    for (const net::ActorSnapshot& actor : authoritativeFrame.players)
    {
        if (actor.id == client.LocalPlayerId())
        {
            localActor = &actor;
            break;
        }
    }

    const PlayerController* const predictedLocalPlayer = client.PredictedLocalPlayer();
    const bool drivingTruck = localActor != nullptr && localActor->drivingTruck;
    const Vec3 truckCameraPosition = frame.truck.position + Vec3{0.0f, 1.1f, 0.0f};
    const Vec3 cameraPosition = BuildViewCameraPosition(
        client.World(),
        predictedLocalPlayer,
        drivingTruck,
        truckCameraPosition,
        options,
        localActor != nullptr ? localActor->cameraPosition : Vec3{0.0f, 4.0f, -8.0f},
        localActor != nullptr ? localActor->flatForward : Vec3{0.0f, 0.0f, 1.0f},
        localActor != nullptr);
    const Vec3 forward = drivingTruck
        ? frame.truck.forward
        : (predictedLocalPlayer != nullptr ? predictedLocalPlayer->ForwardVector() : (localActor != nullptr ? localActor->forward : Vec3{0.0f, 0.0f, 1.0f}));
    const Vec3 aimPosition = drivingTruck
        ? truckCameraPosition
        : (predictedLocalPlayer != nullptr ? predictedLocalPlayer->CameraPosition() : (localActor != nullptr ? localActor->cameraPosition : cameraPosition));
    data.worldToClip = BuildPerspective(
        cameraPosition,
        aimPosition + forward * std::max(options.terrainDrawDistanceMeters, 24.0f),
        BuildMainSessionFovDegrees(),
        options.terrainDrawDistanceMeters + 64.0f,
        viewportWidth,
        viewportHeight);

    client.MutableWorld().GatherRenderGeometrySmoothedCulled(
        data.terrainTriangleStorage,
        data.translucentTerrainTriangleStorage,
        data.debugLines,
        data.worldToClip,
        cameraPosition,
        options.terrainDrawDistanceMeters,
        options.showWireframe,
        options.showActiveChunks);
    data.terrainTriangles = data.terrainTriangleStorage;
    data.translucentTerrainTriangles = data.translucentTerrainTriangleStorage;
    data.terrainMeshVersion = client.World().TerrainMeshVersion();
    if (!data.translucentTerrainTriangles.empty() && data.translucentTerrainTriangles.size() <= kMaxSortedTranslucentVertices)
    {
        SortTrianglesBackToFront(data.translucentTerrainTriangleStorage, cameraPosition);
        data.translucentTerrainTriangles = data.translucentTerrainTriangleStorage;
    }

    AppendTruckApproxMesh(data.dynamicTriangles, frame.truck.position, frame.truck.forward, frame.truck.averageSink);

    for (const net::BeamSnapshot& beam : frame.beams)
    {
        AppendTracerModel(data.effectTriangles, beam.start, beam.end, beam.color, beam.ttl);
    }

    for (const net::BulletSnapshot& bullet : frame.bullets)
    {
        AppendTracerModel(data.effectTriangles, bullet.previousPosition, bullet.position, MakeColor(1.0f, 0.84f, 0.44f, 1.0f), 0.10f);
    }

    for (const net::GrenadeSnapshot& grenade : frame.grenades)
    {
        AppendGrenadeMesh(data.dynamicTriangles, grenade.position, grenade.forward, grenade.up);
    }

    for (const net::ActorSnapshot& actor : frame.players)
    {
        if (!options.thirdPerson && actor.id == client.LocalPlayerId() && !actor.drivingTruck)
        {
            continue;
        }

        if (actor.id == client.LocalPlayerId() && predictedLocalPlayer != nullptr && !actor.drivingTruck)
        {
            AppendCharacterModel(
                data.dynamicTriangles,
                predictedLocalPlayer->Position(),
                predictedLocalPlayer->FlatForwardVector(),
                predictedLocalPlayer->ForwardVector(),
                predictedLocalPlayer->LeftFootPosition(),
                predictedLocalPlayer->RightFootPosition(),
                predictedLocalPlayer->LeftFootGrounded(),
                predictedLocalPlayer->RightFootGrounded(),
                predictedLocalPlayer->WalkCycleRadians(),
                predictedLocalPlayer->HorizontalSpeedMetersPerSecond(),
                options.localTool,
                std::max(actor.weaponCycle, options.localWeaponCycle),
                false);
            continue;
        }

        AppendCharacterModel(
            data.dynamicTriangles,
            actor.position,
            actor.flatForward,
            actor.forward,
            actor.leftFootPosition,
            actor.rightFootPosition,
            actor.leftFootGrounded,
            actor.rightFootGrounded,
            actor.walkCycleRadians,
            actor.horizontalSpeed,
            actor.tool,
            actor.weaponCycle,
            actor.drivingTruck);
    }

    if (!options.thirdPerson && !drivingTruck)
    {
        const Vec3 localAimPosition = predictedLocalPlayer != nullptr ? predictedLocalPlayer->CameraPosition() : aimPosition;
        const Vec3 localForward = predictedLocalPlayer != nullptr ? predictedLocalPlayer->ForwardVector() : forward;
        const float localWalkCycle = predictedLocalPlayer != nullptr ? predictedLocalPlayer->WalkCycleRadians() : (localActor != nullptr ? localActor->walkCycleRadians : 0.0f);
        const float localMoveSpeed = predictedLocalPlayer != nullptr ? predictedLocalPlayer->HorizontalSpeedMetersPerSecond() : (localActor != nullptr ? localActor->horizontalSpeed : 0.0f);
        const WeaponDefinition localWeapon = GetWeaponDefinition(options.localTool);
        const bool localAds = options.aimDownSights && localWeapon.supportsAds;
        const bool rifleAds = localAds && localWeapon.usesScope;
        model::Basis3 rifleBasis{};
        const model::Basis3* rifleBasisOverride = nullptr;
        if (options.localTool == ToolType::Rifle)
        {
            rifleBasis = BuildHeldItemBasis(
                options.localTool,
                localAimPosition,
                localForward,
                localWalkCycle,
                localMoveSpeed,
                options.localWeaponCycle,
                localAds ? 1.0f : 0.0f);
            rifleBasisOverride = &rifleBasis;
        }

        AppendHeldToolMesh(
            data.viewModelTriangles,
            localAimPosition,
            localForward,
            localWalkCycle,
            localMoveSpeed,
            options.localTool,
            options.localWeaponCycle,
            localAds,
            rifleBasisOverride);
        if (rifleAds && rifleBasisOverride != nullptr)
        {
            AppendRifleScopeOccluderMesh(data.viewModelPostScopeTriangles, *rifleBasisOverride);
        }
        ConfigureScopedView(data, options, rifleAds ? rifleBasisOverride : nullptr, viewportWidth, viewportHeight);
    }
    else
    {
        ConfigureScopedView(data, options, nullptr, viewportWidth, viewportHeight);
    }

    if (!data.dynamicTranslucentTriangles.empty() && data.dynamicTranslucentTriangles.size() <= kMaxSortedTranslucentVertices)
    {
        SortTrianglesBackToFront(data.dynamicTranslucentTriangles, cameraPosition);
    }

    return data;
}
}
