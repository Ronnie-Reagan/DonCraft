#include "game/game_mode.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "game/control_state.hpp"
#include "game/debug_hud.hpp"
#include "game/model_primitives.hpp"
#include "platform/input_state.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace df::game
{
namespace
{
constexpr std::size_t kMaxSortedTranslucentVertices = 24000u;
constexpr float kGrenadeRadius = 0.16f;
constexpr float kBulletRadius = 0.035f;
constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};

auto FormatFloat(const float value, const int decimals = 1) -> std::string
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

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

auto WrapAngle(const float radians) -> float
{
    return std::remainder(radians, kPi * 2.0f);
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

auto BuildControlState(const platform::InputState& input) -> ControlState
{
    ControlState control{};
    control.moveForward = input.KeyDown(SDL_SCANCODE_W);
    control.moveBackward = input.KeyDown(SDL_SCANCODE_S);
    control.moveLeft = input.KeyDown(SDL_SCANCODE_A);
    control.moveRight = input.KeyDown(SDL_SCANCODE_D);
    control.sprint = input.KeyDown(SDL_SCANCODE_LSHIFT) || input.KeyDown(SDL_SCANCODE_RSHIFT);
    control.jumpPressed = input.KeyPressed(SDL_SCANCODE_SPACE);
    control.lookYawDelta = input.mouseDeltaX;
    control.lookPitchDelta = input.mouseDeltaY;
    return control;
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

void IntegrateOrientation(Vec3& forward, Vec3& up, const Vec3& angularVelocity, const float dt)
{
    const float angularSpeed = Length(angularVelocity);
    if (angularSpeed <= 1.0e-5f)
    {
        return;
    }

    const Vec3 axis = angularVelocity / angularSpeed;
    forward = model::RotateAroundAxis(forward, axis, angularSpeed * dt);
    up = model::RotateAroundAxis(up, axis, angularSpeed * dt);

    forward = Normalize(forward);
    if (LengthSquared(forward) <= 1.0e-6f)
    {
        forward = Vec3{0.0f, 0.0f, 1.0f};
    }

    up = up - forward * Dot(up, forward);
    if (LengthSquared(up) <= 1.0e-6f)
    {
        up = kWorldUp - forward * Dot(kWorldUp, forward);
        if (LengthSquared(up) <= 1.0e-6f)
        {
            up = RightFromForward(forward);
        }
    }
    up = Normalize(up);
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

    model::Basis3 sightBasis = basis;
    sightBasis.origin = model::TransformPoint(basis, Vec3{0.0f, 0.07f, 0.20f});
    model::AppendBox(triangles, sightBasis, Vec3{0.020f, 0.030f, 0.060f}, metalColor);

    model::Basis3 barrelBasis = MakeAxisBasis(model::TransformPoint(basis, Vec3{0.0f, -0.01f, 0.72f}), basis.forward, basis.up);
    model::AppendCylinder(triangles, barrelBasis, 0.24f, 0.014f, 7, metalColor, false, true);

    model::Basis3 boltBasis = basis;
    boltBasis.origin = model::TransformPoint(basis, Vec3{0.0f, 0.01f, 0.14f - cycle * 0.12f});
    model::AppendBox(triangles, boltBasis, Vec3{0.030f, 0.024f, 0.10f}, metalColor);
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
    const Vec3& aimPosition,
    const Vec3& forward,
    const float walkCycle,
    const float moveSpeed,
    const float weaponCycle) -> model::Basis3
{
    model::Basis3 basis = model::MakeBasis(aimPosition, forward, kWorldUp);
    const float swayWeight = Clamp(moveSpeed / 7.5f, 0.0f, 1.0f);
    basis.origin += basis.right * (std::sin(walkCycle) * 0.024f * swayWeight + 0.30f);
    basis.origin -= basis.up * (0.21f - std::abs(std::sin(walkCycle * 0.5f)) * 0.018f * swayWeight);
    basis.origin += basis.forward * 0.52f;
    basis.origin -= basis.forward * (weaponCycle * weaponCycle) * 0.08f;
    return basis;
}

auto BuildShovelBasis(
    const Vec3& aimPosition,
    const Vec3& forward,
    const float walkCycle,
    const float moveSpeed,
    const float weaponCycle) -> model::Basis3
{
    model::Basis3 basis = BuildHeldItemBasis(aimPosition, forward, walkCycle, moveSpeed, weaponCycle);
    const float swingPhase = Clamp(weaponCycle, 0.0f, 1.0f);
    const float swingPitch = DegreesToRadians(Lerp(34.0f, -42.0f, swingPhase));
    const float swingYaw = DegreesToRadians(Lerp(-8.0f, 12.0f, swingPhase));
    basis.forward = Normalize(model::RotateAroundAxis(basis.forward, basis.right, swingPitch));
    basis.forward = Normalize(model::RotateAroundAxis(basis.forward, basis.up, swingYaw));
    basis.up = Normalize(Cross(basis.right, basis.forward));
    basis.origin += basis.forward * (0.08f + swingPhase * 0.10f);
    basis.origin -= basis.up * (swingPhase * 0.14f);
    return basis;
}

auto BuildShovelBladeCenter(const model::Basis3& shovelBasis) -> Vec3
{
    return model::TransformPoint(shovelBasis, Vec3{0.0f, -0.08f, 0.88f});
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

    model::Basis3 leftFootBasis = model::MakeBasis(leftFoot, flatForward, kWorldUp);
    model::AppendBox(triangles, leftFootBasis, Vec3{0.12f, 0.08f, 0.18f}, leftGrounded ? limbColor : MakeColor(0.24f, 0.22f, 0.20f, 1.0f));

    model::Basis3 rightFootBasis = model::MakeBasis(rightFoot, flatForward, kWorldUp);
    model::AppendBox(triangles, rightFootBasis, Vec3{0.12f, 0.08f, 0.18f}, rightGrounded ? limbColor : MakeColor(0.24f, 0.22f, 0.20f, 1.0f));

    model::Basis3 leftEyeBasis = bodyBasis;
    leftEyeBasis.origin = model::TransformPoint(bodyBasis, Vec3{-0.08f, 0.55f, 0.25f});
    model::AppendBox(triangles, leftEyeBasis, Vec3{0.035f, 0.035f, 0.020f}, eyeColor);

    model::Basis3 rightEyeBasis = bodyBasis;
    rightEyeBasis.origin = model::TransformPoint(bodyBasis, Vec3{0.08f, 0.55f, 0.25f});
    model::AppendBox(triangles, rightEyeBasis, Vec3{0.035f, 0.035f, 0.020f}, eyeColor);
}
}

void GameMode::Initialize(const std::filesystem::path& userDataDirectory)
{
    savePath_ = userDataDirectory / "demo_world.bin";
    if (!world_.Load(savePath_))
    {
        world_.Reset();
    }

    pendingWorldSettings_ = world_.GenerationSettings();
    player_.Spawn(world_);
    truck_.Reset(world_);
}

auto GameMode::ConsumeAudioCues() -> std::vector<AudioCue>
{
    std::vector<AudioCue> drained;
    drained.swap(audioCues_);
    return drained;
}

void GameMode::OpenPauseMenu()
{
    paused_ = true;
    pauseMenuSelection_ = 0;
    pendingWorldSettings_ = world_.GenerationSettings();
}

void GameMode::ApplyPendingWorldSettings()
{
    const ScopedProfileSection scope(profiler_, "World Apply Settings");
    world_.SetGenerationSettings(pendingWorldSettings_);
    ResetWorldAndActors();
    paused_ = false;
}

void GameMode::ResetWorldAndActors()
{
    const ScopedProfileSection scope(profiler_, "World Reset");
    world_.Reset();
    player_.Spawn(world_);
    truck_.Reset(world_);
    drivingTruck_ = false;
    grenades_.clear();
    beams_.clear();
    bullets_.clear();
    rifleCooldown_ = 0.0f;
    digCooldown_ = 0.0f;
    weaponCycle_ = 0.0f;
    footstepCooldown_ = 0.0f;
    crosshairMaterial_ = world::MaterialId::Air;
}

void GameMode::QueueAudioCue(const AudioCue& cue)
{
    audioCues_.push_back(cue);
}

void GameMode::UpdatePauseMenu(const platform::InputState& input)
{
    const auto applySelectionDelta = [&](const PauseMenuItem item, const float scalar, const bool coarseAdjust)
    {
        if (std::abs(scalar) <= 1.0e-4f)
        {
            return;
        }

        const auto quantized = [&](const int unit) -> int
        {
            const int direction = scalar >= 0.0f ? 1 : -1;
            const int magnitude = std::max(1, static_cast<int>(std::round(std::abs(scalar))));
            return direction * magnitude * unit;
        };

        const int worldHorizontalStep = coarseAdjust ? 16 : 4;
        const int worldVerticalStep = coarseAdjust ? 8 : 2;
        const int chunkStep = coarseAdjust ? 8 : 1;
        const std::uint32_t seedStep = coarseAdjust ? 100u : 1u;
        const float reliefStep = coarseAdjust ? 0.10f : 0.02f;
        const float waterStep = coarseAdjust ? 0.05f : 0.01f;
        const int fpsStep = coarseAdjust ? 10 : 2;

        switch (item)
        {
        case PauseMenuItem::WorldWidth:
            pendingWorldSettings_.worldWidth += quantized(worldHorizontalStep);
            break;
        case PauseMenuItem::WorldHeight:
            pendingWorldSettings_.worldHeight += quantized(worldVerticalStep);
            break;
        case PauseMenuItem::WorldDepth:
            pendingWorldSettings_.worldDepth += quantized(worldHorizontalStep);
            break;
        case PauseMenuItem::ActiveChunkSize:
            pendingWorldSettings_.activeChunkSize += quantized(chunkStep);
            break;
        case PauseMenuItem::Seed:
            if (scalar > 0.0f)
            {
                pendingWorldSettings_.seed += static_cast<std::uint32_t>(quantized(static_cast<int>(seedStep)));
            }
            else
            {
                const std::uint32_t amount = static_cast<std::uint32_t>(std::abs(quantized(static_cast<int>(seedStep))));
                pendingWorldSettings_.seed = pendingWorldSettings_.seed > amount ? pendingWorldSettings_.seed - amount : 0u;
            }
            break;
        case PauseMenuItem::Relief:
            pendingWorldSettings_.terrainRelief += scalar * reliefStep;
            break;
        case PauseMenuItem::WaterLevel:
            pendingWorldSettings_.waterLevel += scalar * waterStep;
            break;
        case PauseMenuItem::TargetFps:
            targetFrameRate_ = std::clamp(targetFrameRate_ + quantized(fpsStep), 30, 240);
            break;
        default:
            break;
        }

        pendingWorldSettings_ = world::DemoWorld::ClampGenerationSettings(pendingWorldSettings_);
    };

    if (input.KeyPressed(SDL_SCANCODE_UP))
    {
        pauseMenuSelection_ = (pauseMenuSelection_ + PauseMenuItemCount() - 1) % PauseMenuItemCount();
    }
    if (input.KeyPressed(SDL_SCANCODE_DOWN))
    {
        pauseMenuSelection_ = (pauseMenuSelection_ + 1) % PauseMenuItemCount();
    }

    const bool moveLeft = input.KeyPressed(SDL_SCANCODE_LEFT);
    const bool moveRight = input.KeyPressed(SDL_SCANCODE_RIGHT);
    if (moveLeft || moveRight)
    {
        const bool coarseAdjust = input.KeyDown(SDL_SCANCODE_LSHIFT) || input.KeyDown(SDL_SCANCODE_RSHIFT);
        applySelectionDelta(static_cast<PauseMenuItem>(pauseMenuSelection_), moveRight ? 1.0f : -1.0f, coarseAdjust);
    }

    constexpr float menuWidth = 660.0f;
    constexpr float menuHeight = 356.0f;
    const float menuX = (static_cast<float>(lastViewportWidth_) - menuWidth) * 0.5f;
    const float menuY = (static_cast<float>(lastViewportHeight_) - menuHeight) * 0.5f;
    const float rowStartY = menuY + 58.0f;
    const float rowHeight = 24.0f;
    const bool coarseAdjust = input.KeyDown(SDL_SCANCODE_LSHIFT) || input.KeyDown(SDL_SCANCODE_RSHIFT);

    if (input.MousePressed(SDL_BUTTON_LEFT))
    {
        pauseDragItem_ = -1;
        const float localX = input.mouseX - menuX;
        const float localY = input.mouseY - rowStartY;
        if (localX >= 18.0f && localX <= menuWidth - 18.0f && localY >= -4.0f)
        {
            const int rowIndex = static_cast<int>(localY / rowHeight);
            if (rowIndex >= 0 && rowIndex < PauseMenuItemCount())
            {
                pauseMenuSelection_ = rowIndex;
                const PauseMenuItem item = static_cast<PauseMenuItem>(rowIndex);
                if (item == PauseMenuItem::Resume)
                {
                    paused_ = false;
                    return;
                }
                if (item == PauseMenuItem::ApplyAndRebuild)
                {
                    ApplyPendingWorldSettings();
                    return;
                }
                pauseDragItem_ = rowIndex;
            }
        }
    }

    if (!input.MouseDown(SDL_BUTTON_LEFT))
    {
        pauseDragItem_ = -1;
    }
    else if (pauseDragItem_ >= 0 && std::abs(input.mouseDeltaX) > 0.0f)
    {
        applySelectionDelta(static_cast<PauseMenuItem>(pauseDragItem_), input.mouseDeltaX * 0.12f, coarseAdjust);
    }

    if (!input.KeyPressed(SDL_SCANCODE_RETURN))
    {
        return;
    }

    switch (static_cast<PauseMenuItem>(pauseMenuSelection_))
    {
    case PauseMenuItem::Resume:
        paused_ = false;
        break;
    case PauseMenuItem::ApplyAndRebuild:
        ApplyPendingWorldSettings();
        break;
    default:
        break;
    }
}

void GameMode::BeginFrame(const float frameDt)
{
    frameDeltaSeconds_ = frameDt;
    smoothedFps_ = Lerp(smoothedFps_, 1.0f / std::max(frameDt, 0.0001f), 0.08f);
}

void GameMode::Tick(const platform::InputState& input, const float fixedDt)
{
    const ScopedProfileSection tickScope(profiler_, "Game Tick");
    const ControlState control = BuildControlState(input);

    if (input.KeyPressed(SDL_SCANCODE_ESCAPE))
    {
        if (paused_)
        {
            paused_ = false;
            return;
        }
        else
        {
            OpenPauseMenu();
            return;
        }
    }

    if (paused_)
    {
        UpdatePauseMenu(input);
        return;
    }

    rifleCooldown_ = std::max(0.0f, rifleCooldown_ - fixedDt);
    digCooldown_ = std::max(0.0f, digCooldown_ - fixedDt);
    weaponCycle_ = std::max(0.0f, weaponCycle_ - fixedDt * 6.5f);
    footstepCooldown_ = std::max(0.0f, footstepCooldown_ - fixedDt);

    if (input.KeyPressed(SDL_SCANCODE_1))
    {
        tool_ = ToolType::Rifle;
    }
    if (input.KeyPressed(SDL_SCANCODE_2))
    {
        tool_ = ToolType::Grenade;
    }
    if (input.KeyPressed(SDL_SCANCODE_3))
    {
        tool_ = ToolType::Dig;
    }

    if (input.KeyPressed(SDL_SCANCODE_F2))
    {
        showActiveChunks_ = !showActiveChunks_;
    }
    if (input.KeyPressed(SDL_SCANCODE_F3))
    {
        showWireframe_ = !showWireframe_;
    }
    if (input.KeyPressed(SDL_SCANCODE_F4))
    {
        showProfiler_ = !showProfiler_;
    }
    if (input.KeyPressed(SDL_SCANCODE_C) && !drivingTruck_)
    {
        thirdPersonView_ = !thirdPersonView_;
    }
    if (input.KeyPressed(SDL_SCANCODE_R))
    {
        pendingWorldSettings_ = world_.GenerationSettings();
        ResetWorldAndActors();
    }
    if (input.KeyPressed(SDL_SCANCODE_F5))
    {
        const ScopedProfileSection scope(profiler_, "World Save");
        if (!world_.Save(savePath_))
        {
            LogWarning("Failed to save the demo world to ", savePath_.string());
        }
    }
    if (input.KeyPressed(SDL_SCANCODE_F9))
    {
        const ScopedProfileSection scope(profiler_, "World Load");
        if (!world_.Load(savePath_))
        {
            LogWarning("Failed to load the demo world from ", savePath_.string());
        }
        pendingWorldSettings_ = world_.GenerationSettings();
        player_.Spawn(world_);
        truck_.Reset(world_);
        drivingTruck_ = false;
        grenades_.clear();
        beams_.clear();
        bullets_.clear();
        rifleCooldown_ = 0.0f;
        digCooldown_ = 0.0f;
        weaponCycle_ = 0.0f;
        footstepCooldown_ = 0.0f;
        crosshairMaterial_ = world::MaterialId::Air;
    }
    if (input.KeyPressed(SDL_SCANCODE_P))
    {
        DumpPerfCounters();
    }

    if (input.KeyPressed(SDL_SCANCODE_E))
    {
        if (drivingTruck_)
        {
            drivingTruck_ = false;
            const Vec3 forward = truck_.ForwardVector();
            player_.PlaceAt(truck_.ExitPosition(), std::atan2(forward.z, forward.x), DegreesToRadians(-10.0f));
            QueueAudioCue({180.0f, 0.10f, 0.16f, 0.30f, -70.0f});
        }
        else if (truck_.CanEnter(player_.Position()))
        {
            drivingTruck_ = true;
            QueueAudioCue({140.0f, 0.14f, 0.18f, 0.18f, 90.0f});
        }
    }

    if (!drivingTruck_)
    {
        {
            const ScopedProfileSection scope(profiler_, "Player Tick");
            player_.Tick(control, world_, fixedDt);
        }

        const bool movingOnFoot =
            player_.OnGround() &&
            (input.KeyDown(SDL_SCANCODE_W) || input.KeyDown(SDL_SCANCODE_A) || input.KeyDown(SDL_SCANCODE_S) || input.KeyDown(SDL_SCANCODE_D));
        if (movingOnFoot && footstepCooldown_ <= 0.0f)
        {
            const world::MaterialId footMaterial = player_.MaterialUnderFeet(world_);
            switch (footMaterial)
            {
            case world::MaterialId::ShallowWater:
                QueueAudioCue({84.0f, 0.16f, 0.14f, 0.74f, -8.0f});
                footstepCooldown_ = 0.28f;
                break;
            case world::MaterialId::WetMud:
                QueueAudioCue({72.0f, 0.14f, 0.13f, 0.68f, -12.0f});
                footstepCooldown_ = 0.30f;
                break;
            case world::MaterialId::DrySand:
                QueueAudioCue({102.0f, 0.10f, 0.10f, 0.42f, -10.0f});
                footstepCooldown_ = 0.26f;
                break;
            case world::MaterialId::Grass:
                QueueAudioCue({88.0f, 0.11f, 0.09f, 0.24f, -4.0f});
                footstepCooldown_ = 0.25f;
                break;
            case world::MaterialId::Gravel:
                QueueAudioCue({138.0f, 0.09f, 0.10f, 0.34f, -18.0f});
                footstepCooldown_ = 0.26f;
                break;
            default:
                QueueAudioCue({118.0f, 0.08f, 0.09f, 0.16f, -6.0f});
                footstepCooldown_ = 0.24f;
                break;
            }
        }
    }
    {
        const ScopedProfileSection scope(profiler_, "Truck Tick");
        truck_.Tick(control, world_, fixedDt, drivingTruck_);
    }
    {
        const ScopedProfileSection scope(profiler_, "World Tick");
        world_.Tick(fixedDt);
    }
    {
        const ScopedProfileSection scope(profiler_, "Projectile Tick");
        UpdateBullets(fixedDt);
        UpdateGrenades(fixedDt);
        UpdateBeams(fixedDt);
    }

    if (!drivingTruck_ && input.KeyPressed(SDL_SCANCODE_G))
    {
        SpawnGrenade();
    }

    if (drivingTruck_)
    {
        const world::RaycastHit sightHit = world_.Raycast({CurrentAimPosition(), CurrentForwardVector()}, 72.0f);
        crosshairMaterial_ = sightHit.hit ? sightHit.material : world::MaterialId::Air;
        return;
    }

    if (tool_ == ToolType::Grenade)
    {
        if (input.MousePressed(SDL_BUTTON_LEFT))
        {
            SpawnGrenade();
        }
    }
    else if (tool_ == ToolType::Rifle)
    {
        if (input.MouseDown(SDL_BUTTON_LEFT) && rifleCooldown_ <= 0.0f)
        {
            FireRifle();
            rifleCooldown_ = 0.14f;
        }
    }
    else if (tool_ == ToolType::Dig)
    {
        if (input.MouseDown(SDL_BUTTON_LEFT) && digCooldown_ <= 0.0f)
        {
            UseDigTool();
            digCooldown_ = 0.07f;
        }
    }

    const world::RaycastHit sightHit = world_.Raycast({CurrentAimPosition(), CurrentForwardVector()}, 48.0f);
    crosshairMaterial_ = sightHit.hit ? sightHit.material : world::MaterialId::Air;
}

auto GameMode::BuildRenderData(const int viewportWidth, const int viewportHeight) -> render::FrameRenderData
{
    const ScopedProfileSection buildScope(profiler_, "Game Build Render");
    render::FrameRenderData data{};
    lastViewportWidth_ = viewportWidth;
    lastViewportHeight_ = viewportHeight;
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(std::max(viewportHeight, 1));
    const Vec3 cameraPosition = CurrentCameraPosition();
    data.clearColor = MakeColor(0.62f, 0.76f, 0.90f, 1.0f);
    data.cameraPosition = cameraPosition;
    data.worldToClip =
        PerspectiveMatrix(DegreesToRadians(70.0f), aspect, 0.1f, 200.0f) *
        LookAtMatrix(cameraPosition, CurrentViewTarget(), kWorldUp);

    {
        const ScopedProfileSection scope(profiler_, "World Gather");
        world_.GatherRenderGeometrySmoothed(data.terrainTriangles, data.translucentTerrainTriangles, data.debugLines, showWireframe_, showActiveChunks_);
    }
    data.terrainMeshVersion = world_.TerrainMeshVersion();
    if (!data.translucentTerrainTriangles.empty() && data.translucentTerrainTriangles.size() <= kMaxSortedTranslucentVertices)
    {
        const ScopedProfileSection scope(profiler_, "Water Sort");
        data.translucentTerrainTriangleStorage.assign(data.translucentTerrainTriangles.begin(), data.translucentTerrainTriangles.end());
        SortTrianglesBackToFront(data.translucentTerrainTriangleStorage, cameraPosition);
        data.translucentTerrainTriangles = data.translucentTerrainTriangleStorage;
    }

    truck_.AppendModelTriangles(data.dynamicTriangles, data.dynamicTranslucentTriangles);
    if (showWireframe_)
    {
        truck_.AppendDebugLines(data.debugLines);
    }

    for (const Beam& beam : beams_)
    {
        AppendBeam(data.debugLines, beam);
        AppendTracerModel(data.dynamicTriangles, beam.start, beam.end, beam.color, beam.ttl);
    }

    for (const Bullet& bullet : bullets_)
    {
        AppendTracerModel(data.dynamicTriangles, bullet.previousPosition, bullet.position, MakeColor(1.0f, 0.84f, 0.44f, 1.0f), 0.10f);
    }

    for (const Grenade& grenade : grenades_)
    {
        AppendGrenadeMesh(data.dynamicTriangles, grenade.position, grenade.orientationForward, grenade.orientationUp);
        if (showWireframe_)
        {
            AppendWorldMarker(data.debugLines, grenade.position, 0.18f, MakeColor(1.0f, 0.85f, 0.25f, 1.0f));
        }
    }

    if (!drivingTruck_ && thirdPersonView_)
    {
        AppendBlobCharacterMesh(
            data.dynamicTriangles,
            player_.Position(),
            player_.FlatForwardVector(),
            player_.LeftFootPosition(),
            player_.RightFootPosition(),
            player_.LeftFootGrounded(),
            player_.RightFootGrounded());
    }

    if (!drivingTruck_ && !thirdPersonView_)
    {
        model::Basis3 weaponBasis = BuildHeldItemBasis(
            CurrentAimPosition(),
            CurrentForwardVector(),
            player_.WalkCycleRadians(),
            player_.HorizontalSpeedMetersPerSecond(),
            weaponCycle_);

        switch (tool_)
        {
        case ToolType::Rifle:
            AppendRifleMesh(data.dynamicTriangles, weaponBasis, weaponCycle_);
            break;
        case ToolType::Grenade:
            AppendGrenadeMesh(data.dynamicTriangles, weaponBasis.origin, weaponBasis.forward, weaponBasis.up);
            break;
        case ToolType::Dig:
            AppendShovelMesh(
                data.dynamicTriangles,
                BuildShovelBasis(
                    CurrentAimPosition(),
                    CurrentForwardVector(),
                    player_.WalkCycleRadians(),
                    player_.HorizontalSpeedMetersPerSecond(),
                    weaponCycle_));
            break;
        default:
            break;
        }
    }

    if (!data.dynamicTranslucentTriangles.empty() && data.dynamicTranslucentTriangles.size() <= kMaxSortedTranslucentVertices)
    {
        const ScopedProfileSection scope(profiler_, "Dynamic Translucent Sort");
        SortTrianglesBackToFront(data.dynamicTranslucentTriangles, cameraPosition);
    }

    {
        const ScopedProfileSection scope(profiler_, "HUD Build");
        BuildHud(data.overlayTriangles, viewportWidth, viewportHeight);
    }
    return data;
}

void GameMode::FireRifle()
{
    const Vec3 origin = CurrentAimPosition();
    const Vec3 direction = CurrentForwardVector();
    weaponCycle_ = 1.0f;

    Bullet bullet{};
    bullet.position = origin;
    bullet.previousPosition = origin;
    bullet.velocity = direction * 112.0f;
    bullet.ttl = 8.0f;
    bullets_.push_back(bullet);

    QueueAudioCue({280.0f, 0.04f, 0.10f, 0.08f, 18.0f});
}

void GameMode::UseDigTool()
{
    weaponCycle_ = 1.0f;

    const Vec3 aimPosition = CurrentAimPosition();
    const Vec3 forward = CurrentForwardVector();
    const float walkCycle = player_.WalkCycleRadians();
    const float moveSpeed = player_.HorizontalSpeedMetersPerSecond();

    bool contactFound = false;
    Vec3 contactPoint{};
    world::MaterialId contactMaterial = world::MaterialId::Air;
    for (int swingIndex = 0; swingIndex <= 6 && !contactFound; ++swingIndex)
    {
        const float swingPhase = static_cast<float>(swingIndex) / 6.0f;
        const model::Basis3 shovelBasis = BuildShovelBasis(aimPosition, forward, walkCycle, moveSpeed, swingPhase);
        const Vec3 bladeCenter = BuildShovelBladeCenter(shovelBasis);
        for (const float lateral : {-0.10f, 0.0f, 0.10f})
        {
            for (const float depth : {-0.06f, 0.02f, 0.10f})
            {
                const Vec3 samplePoint =
                    bladeCenter +
                    shovelBasis.right * lateral +
                    shovelBasis.forward * depth -
                    shovelBasis.up * 0.03f;
                const world::MaterialId material = world_.MaterialAtWorldPosition(samplePoint);
                if (!world::BlocksMovement(material))
                {
                    continue;
                }

                contactFound = true;
                contactPoint = samplePoint;
                contactMaterial = material;
                break;
            }
            if (contactFound)
            {
                break;
            }
        }
    }

    if (!contactFound)
    {
        return;
    }

    world_.ApplyDig(contactPoint, 1.6f, 0.90f);
    switch (contactMaterial)
    {
    case world::MaterialId::WetMud:
        QueueAudioCue({82.0f, 0.15f, 0.14f, 0.62f, -10.0f});
        break;
    case world::MaterialId::ShallowWater:
        QueueAudioCue({94.0f, 0.15f, 0.14f, 0.78f, -8.0f});
        break;
    case world::MaterialId::Grass:
        QueueAudioCue({98.0f, 0.10f, 0.10f, 0.24f, -10.0f});
        break;
    default:
        QueueAudioCue({124.0f, 0.09f, 0.12f, 0.28f, -14.0f});
        break;
    }
}

void GameMode::SpawnGrenade()
{
    Grenade grenade{};
    const Vec3 forward = CurrentForwardVector();
    const Vec3 right = RightFromForward(forward);
    grenade.position = CurrentAimPosition() + forward * 0.78f + right * 0.18f - kWorldUp * 0.10f;
    grenade.velocity = forward * 13.0f + Vec3{0.0f, 3.5f, 0.0f};
    grenade.orientationForward = forward;
    grenade.orientationUp = kWorldUp;
    grenade.angularVelocity = right * 16.0f + kWorldUp * 4.0f;
    weaponCycle_ = std::max(weaponCycle_, 0.65f);
    grenades_.push_back(grenade);
}

void GameMode::UpdateGrenades(const float dt)
{
    for (auto iter = grenades_.begin(); iter != grenades_.end();)
    {
        iter->fuse -= dt;

        const float substep = dt / 4.0f;
        for (int step = 0; step < 4; ++step)
        {
            iter->velocity.y -= 18.0f * substep;
            IntegrateOrientation(iter->orientationForward, iter->orientationUp, iter->angularVelocity, substep);

            const Vec3 moveDelta = iter->velocity * substep;
            const float travelDistance = Length(moveDelta);
            if (travelDistance <= 1.0e-5f)
            {
                iter->angularVelocity *= 0.94f;
                continue;
            }

            const Vec3 moveDirection = moveDelta / travelDistance;
            const world::RaycastHit hit = world_.Raycast({iter->position, moveDirection}, travelDistance + kGrenadeRadius + world_.CellSize() * 0.35f);
            if (hit.hit && hit.distance <= travelDistance + kGrenadeRadius * 0.9f)
            {
                iter->position = hit.position + hit.normal * (kGrenadeRadius + 0.02f);

                const float normalSpeed = Dot(iter->velocity, hit.normal);
                Vec3 tangentVelocity = iter->velocity - hit.normal * normalSpeed;
                if (normalSpeed < 0.0f)
                {
                    const float bounce = std::abs(hit.normal.y) > 0.55f ? 0.30f : 0.18f;
                    const float tangentialRetention = std::abs(hit.normal.y) > 0.55f ? 0.74f : 0.56f;
                    tangentVelocity *= tangentialRetention;
                    iter->velocity = tangentVelocity - hit.normal * normalSpeed * bounce;
                }

                if (LengthSquared(tangentVelocity) > 1.0e-5f)
                {
                    const Vec3 rollAxis = Normalize(Cross(hit.normal, tangentVelocity));
                    const float targetRollSpeed = Length(tangentVelocity) / kGrenadeRadius;
                    iter->angularVelocity = Lerp(iter->angularVelocity, rollAxis * targetRollSpeed, Clamp(substep * 18.0f, 0.0f, 1.0f));
                }
                else
                {
                    iter->angularVelocity *= 0.58f;
                }

                if (std::abs(hit.normal.y) > 0.55f && std::abs(iter->velocity.y) < 0.8f)
                {
                    iter->velocity.y = 0.0f;
                    iter->velocity.x *= 0.84f;
                    iter->velocity.z *= 0.84f;
                }
            }
            else
            {
                const Vec3 nextPosition = iter->position + moveDelta;
                if (world_.OverlapsBlocking(nextPosition, Vec3{kGrenadeRadius, kGrenadeRadius, kGrenadeRadius}))
                {
                    iter->velocity *= 0.62f;
                    iter->angularVelocity *= 0.72f;
                }
                else
                {
                    iter->position = nextPosition;
                }
            }

            iter->angularVelocity *= 0.985f;
        }

        if (iter->fuse <= 0.0f)
        {
            const world::MaterialId detonationMaterial = world_.MaterialAtWorldPosition(iter->position);
            world_.ApplyExplosion(iter->position, 4.8f, 0.95f);
            switch (detonationMaterial)
            {
            case world::MaterialId::ShallowWater:
                QueueAudioCue({82.0f, 0.55f, 0.30f, 0.78f, -44.0f});
                break;
            case world::MaterialId::WetMud:
                QueueAudioCue({76.0f, 0.48f, 0.28f, 0.66f, -38.0f});
                break;
            default:
                QueueAudioCue({92.0f, 0.42f, 0.32f, 0.40f, -70.0f});
                break;
            }
            iter = grenades_.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}

void GameMode::UpdateBeams(const float dt)
{
    for (auto iter = beams_.begin(); iter != beams_.end();)
    {
        iter->ttl -= dt;
        if (iter->ttl <= 0.0f)
        {
            iter = beams_.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}

void GameMode::UpdateBullets(const float dt)
{
    const Vec3 worldMin = world_.WorldMin() - Vec3{world_.CellSize() * 4.0f, world_.CellSize() * 4.0f, world_.CellSize() * 4.0f};
    const Vec3 worldMax = world_.WorldMax() + Vec3{world_.CellSize() * 4.0f, world_.CellSize() * 4.0f, world_.CellSize() * 4.0f};
    for (auto iter = bullets_.begin(); iter != bullets_.end();)
    {
        iter->ttl -= dt;
        iter->previousPosition = iter->position;

        bool impacted = false;
        const float substep = dt / 6.0f;
        for (int step = 0; step < 6 && !impacted; ++step)
        {
            const Vec3 stepStart = iter->position;
            iter->velocity.y -= 12.5f * substep;
            const Vec3 stepDelta = iter->velocity * substep;
            const float travelDistance = Length(stepDelta);
            if (travelDistance <= 1.0e-5f)
            {
                continue;
            }

            const Vec3 direction = stepDelta / travelDistance;
            const world::RaycastHit hit = world_.Raycast({stepStart, direction}, travelDistance + world_.CellSize() * 0.25f);
            if (hit.hit && hit.distance <= travelDistance + kBulletRadius)
            {
                iter->position = hit.position;
                world_.ApplyRifleImpact(hit.position, direction, hit.material);

                Beam impactBeam{};
                impactBeam.start = stepStart;
                impactBeam.end = hit.position;
                impactBeam.color = MakeColor(1.0f, 0.82f, 0.40f, 1.0f);
                impactBeam.ttl = 0.08f;
                beams_.push_back(impactBeam);

                switch (hit.material)
                {
                case world::MaterialId::BrittleConcrete:
                case world::MaterialId::BasaltRock:
                    QueueAudioCue({420.0f, 0.07f, 0.14f, 0.08f, -90.0f});
                    break;
                case world::MaterialId::ShallowWater:
                    QueueAudioCue({110.0f, 0.09f, 0.10f, 0.72f, -8.0f});
                    break;
                case world::MaterialId::WetMud:
                    QueueAudioCue({92.0f, 0.10f, 0.12f, 0.60f, -10.0f});
                    break;
                default:
                    QueueAudioCue({188.0f, 0.06f, 0.10f, 0.18f, -20.0f});
                    break;
                }

                impacted = true;
                break;
            }

            iter->position += stepDelta;
        }

        const bool outOfBounds =
            iter->position.x < worldMin.x || iter->position.y < worldMin.y || iter->position.z < worldMin.z ||
            iter->position.x > worldMax.x || iter->position.y > worldMax.y || iter->position.z > worldMax.z;
        if (impacted || iter->ttl <= 0.0f || outOfBounds)
        {
            iter = bullets_.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}

void GameMode::BuildHud(std::vector<render::ColorVertex2D>& overlayTriangles, const int viewportWidth, const int viewportHeight) const
{
    const float screenWidth = static_cast<float>(viewportWidth);
    const float screenHeight = static_cast<float>(viewportHeight);
    const world::WorldGenerationSettings settings = world_.GenerationSettings();
    const Vec3 worldExtentMeters = world_.WorldMax() - world_.WorldMin();
    const float footerWidth = std::max(320.0f, std::min(screenWidth - 32.0f, 1260.0f));

    AppendRect(overlayTriangles, 16.0f, 16.0f, 520.0f, 286.0f, MakeColor(0.05f, 0.06f, 0.08f, 0.72f), screenWidth, screenHeight);
    AppendRect(overlayTriangles, 16.0f, screenHeight - 58.0f, footerWidth, 42.0f, MakeColor(0.05f, 0.06f, 0.08f, 0.65f), screenWidth, screenHeight);

    AppendText(overlayTriangles, 28.0f, 28.0f, 2.0f, std::string("FPS ") + FormatFloat(smoothedFps_), MakeColor(0.96f, 0.98f, 1.0f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 46.0f, 2.0f, std::string("FRAME ") + FormatFloat(frameDeltaSeconds_ * 1000.0f) + " MS", MakeColor(0.83f, 0.90f, 0.97f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 64.0f, 2.0f, std::string("TOOL ") + std::string(ToolName()), MakeColor(1.0f, 0.88f, 0.44f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 82.0f, 2.0f, std::string("ACTIVE CHUNKS ") + std::to_string(world_.ActiveChunkCount()), MakeColor(0.40f, 0.96f, 1.0f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 100.0f, 2.0f, std::string("DIRTY CHUNKS ") + std::to_string(world_.DirtyChunkStateCount()) + "  TRACKED " + std::to_string(world_.TrackedChunkStateCount()), MakeColor(0.74f, 0.92f, 1.0f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 118.0f, 2.0f, std::string("MATERIAL ") + ToUpperAscii(world::ToString(crosshairMaterial_)), MakeColor(0.82f, 1.0f, 0.84f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 136.0f, 2.0f, std::string("WORLD ") + std::to_string(settings.worldWidth) + "X" + std::to_string(settings.worldHeight) + "X" + std::to_string(settings.worldDepth) + "  CHUNK " + std::to_string(settings.activeChunkSize), MakeColor(0.90f, 0.83f, 1.0f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 154.0f, 2.0f, std::string("EXTENT ") + FormatFloat(worldExtentMeters.x, 1) + " X " + FormatFloat(worldExtentMeters.y, 1) + " X " + FormatFloat(worldExtentMeters.z, 1) + " M", MakeColor(0.82f, 0.95f, 1.0f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 172.0f, 2.0f, std::string("UNIT SCALE 1.00 M  SEED ") + std::to_string(settings.seed), MakeColor(1.0f, 0.82f, 0.72f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 190.0f, 2.0f, std::string("TRUCK ") + FormatFloat(truck_.SpeedMetersPerSecond()) + " MPS  " + ToUpperAscii(world::ToString(truck_.ContactMaterial())), MakeColor(0.97f, 0.82f, 0.58f, 1.0f), screenWidth, screenHeight);
    std::string simLine = std::string("SIM ") + std::string(world_.ActiveMpmBackendName());
    if (world_.ActiveMpmBackendName() == "CPU Reference")
    {
        simLine += "  THREADS " + std::to_string(world_.MpmWorkerCount());
    }
    AppendText(overlayTriangles, 28.0f, 208.0f, 2.0f, simLine, MakeColor(0.74f, 0.90f, 0.72f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 226.0f, 2.0f, std::string("MESH ") + FormatFloat(static_cast<float>(world_.LastTerrainRebuildMilliseconds())) + " MS  VERTS " + std::to_string(world_.OpaqueTerrainVertexCount()) + "/" + std::to_string(world_.TranslucentTerrainVertexCount()), MakeColor(0.78f, 0.86f, 0.98f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 244.0f, 2.0f, std::string("CAMERA ") + (thirdPersonView_ ? "3P" : "1P") + "  WALK " + FormatFloat(player_.HorizontalSpeedMetersPerSecond()) + " MPS", MakeColor(0.88f, 0.90f, 0.74f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, 262.0f, 2.0f, std::string("CAP ") + std::to_string(targetFrameRate_) + " FPS  BULLETS " + std::to_string(bullets_.size()), MakeColor(0.86f, 0.88f, 0.96f, 1.0f), screenWidth, screenHeight);
    AppendText(overlayTriangles, 28.0f, screenHeight - 46.0f, 2.0f, "1 RIFLE  2 GRENADE  3 DIG   G THROW   E TRUCK   C CAMERA   F4 PROF  F5 SAVE  F9 LOAD  R RESET  ESC PAUSE  F2 CHUNKS  F3 WIRE  P PERF", MakeColor(0.92f, 0.94f, 0.97f, 1.0f), screenWidth, screenHeight);

    if (showProfiler_)
    {
        BuildProfilerHud(overlayTriangles, viewportWidth, viewportHeight);
    }

    if (!paused_)
    {
        AppendCrosshair(overlayTriangles, screenWidth, screenHeight, MakeColor(0.98f, 0.98f, 0.99f, 1.0f));
    }
    else
    {
        BuildPauseMenu(overlayTriangles, viewportWidth, viewportHeight);
    }
}

void GameMode::BuildProfilerHud(std::vector<render::ColorVertex2D>& overlayTriangles, const int viewportWidth, const int viewportHeight) const
{
    if (profilerSnapshot_.stageCount == 0u)
    {
        return;
    }

    const float screenWidth = static_cast<float>(viewportWidth);
    const float screenHeight = static_cast<float>(viewportHeight);
    const std::size_t rowCount = std::min<std::size_t>(config::kProfilerHudStageCount, profilerSnapshot_.stageCount);
    const float panelWidth = 780.0f;
    const float panelHeight = 74.0f + static_cast<float>(rowCount) * 18.0f;
    const float panelX = screenWidth - panelWidth - 16.0f;
    const float panelY = 16.0f;

    AppendRect(overlayTriangles, panelX, panelY, panelWidth, panelHeight, MakeColor(0.05f, 0.06f, 0.08f, 0.76f), screenWidth, screenHeight);
    AppendRect(overlayTriangles, panelX + 12.0f, panelY + 12.0f, panelWidth - 24.0f, 26.0f, MakeColor(0.11f, 0.14f, 0.18f, 0.94f), screenWidth, screenHeight);

    AppendText(overlayTriangles, panelX + 24.0f, panelY + 20.0f, 2.0f, "PROFILER", MakeColor(0.98f, 0.98f, 1.0f, 1.0f), screenWidth, screenHeight);
    AppendText(
        overlayTriangles,
        panelX + 24.0f,
        panelY + 46.0f,
        1.8f,
        std::string("FRAME ") + FormatFloat(static_cast<float>(profilerSnapshot_.frameLastMilliseconds), 1) +
            " / " + FormatFloat(static_cast<float>(profilerSnapshot_.frameAverageMilliseconds), 1) +
            " / " + FormatFloat(static_cast<float>(profilerSnapshot_.frameMaxMilliseconds), 1) +
            " MS  HOT SELF " + ToUpperAscii(profilerSnapshot_.hottestStageName),
        MakeColor(0.83f, 0.91f, 0.99f, 1.0f),
        screenWidth,
        screenHeight);

    float rowY = panelY + 66.0f;
    for (std::size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
    {
        const FrameProfiler::StageSnapshot& stage = profilerSnapshot_.stages[profilerSnapshot_.sortOrder[rowIndex]];
        if (stage.lastMilliseconds <= 0.01 && stage.averageMilliseconds <= 0.01)
        {
            continue;
        }

        const std::string line =
            ToUpperAscii(stage.name) +
            "  S " + FormatFloat(static_cast<float>(stage.lastSelfMilliseconds), 1) +
            "  T " + FormatFloat(static_cast<float>(stage.lastMilliseconds), 1) +
            "  AVG " + FormatFloat(static_cast<float>(stage.averageSelfMilliseconds), 1) +
            " / " + FormatFloat(static_cast<float>(stage.averageMilliseconds), 1) +
            " MS X" + std::to_string(stage.lastCalls);
        AppendText(
            overlayTriangles,
            panelX + 24.0f,
            rowY,
            1.7f,
            line,
            rowIndex == 0 ? MakeColor(1.0f, 0.88f, 0.50f, 1.0f) : MakeColor(0.91f, 0.94f, 0.98f, 1.0f),
            screenWidth,
            screenHeight);
        rowY += 18.0f;
    }
}

void GameMode::BuildPauseMenu(std::vector<render::ColorVertex2D>& overlayTriangles, const int viewportWidth, const int viewportHeight) const
{
    const float screenWidth = static_cast<float>(viewportWidth);
    const float screenHeight = static_cast<float>(viewportHeight);
    const float menuWidth = 660.0f;
    const float menuHeight = 356.0f;
    const float menuX = (screenWidth - menuWidth) * 0.5f;
    const float menuY = (screenHeight - menuHeight) * 0.5f;

    AppendRect(overlayTriangles, 0.0f, 0.0f, screenWidth, screenHeight, MakeColor(0.01f, 0.02f, 0.03f, 0.45f), screenWidth, screenHeight);
    AppendRect(overlayTriangles, menuX, menuY, menuWidth, menuHeight, MakeColor(0.08f, 0.09f, 0.12f, 0.92f), screenWidth, screenHeight);
    AppendRect(overlayTriangles, menuX + 14.0f, menuY + 14.0f, menuWidth - 28.0f, 30.0f, MakeColor(0.12f, 0.15f, 0.20f, 0.95f), screenWidth, screenHeight);
    AppendText(overlayTriangles, menuX + 28.0f, menuY + 24.0f, 2.0f, "PAUSE MENU", MakeColor(0.98f, 0.98f, 1.0f, 1.0f), screenWidth, screenHeight);

    const std::array<std::string, 10> menuRows = {
        "RESUME",
        std::string("WORLD WIDTH ") + std::to_string(pendingWorldSettings_.worldWidth) + " CELLS",
        std::string("WORLD HEIGHT ") + std::to_string(pendingWorldSettings_.worldHeight) + " CELLS",
        std::string("WORLD DEPTH ") + std::to_string(pendingWorldSettings_.worldDepth) + " CELLS",
        std::string("ACTIVE CHUNK ") + std::to_string(pendingWorldSettings_.activeChunkSize) + " CELLS",
        std::string("SEED ") + std::to_string(pendingWorldSettings_.seed),
        std::string("RELIEF ") + FormatFloat(pendingWorldSettings_.terrainRelief, 2),
        std::string("WATER LEVEL ") + FormatFloat(pendingWorldSettings_.waterLevel, 2),
        std::string("TARGET FPS ") + std::to_string(targetFrameRate_),
        "APPLY REBUILD",
    };

    float rowY = menuY + 58.0f;
    for (int index = 0; index < static_cast<int>(menuRows.size()); ++index)
    {
        const bool selected = index == pauseMenuSelection_;
        if (selected)
        {
            AppendRect(overlayTriangles, menuX + 18.0f, rowY - 4.0f, menuWidth - 36.0f, 24.0f, MakeColor(0.20f, 0.28f, 0.36f, 0.90f), screenWidth, screenHeight);
        }

        AppendText(
            overlayTriangles,
            menuX + 30.0f,
            rowY,
            2.0f,
            menuRows[static_cast<std::size_t>(index)],
            selected ? MakeColor(1.0f, 0.95f, 0.66f, 1.0f) : MakeColor(0.92f, 0.95f, 0.99f, 1.0f),
            screenWidth,
            screenHeight);
        rowY += 24.0f;
    }

    AppendText(
        overlayTriangles,
        menuX + 28.0f,
        menuY + menuHeight - 44.0f,
        1.7f,
        "UP DOWN SELECT  LEFT RIGHT ADJUST  SHIFT COARSE  DRAG LMB TO TUNE",
        MakeColor(0.84f, 0.88f, 0.93f, 1.0f),
        screenWidth,
        screenHeight);
    AppendText(
        overlayTriangles,
        menuX + 28.0f,
        menuY + menuHeight - 24.0f,
        1.7f,
        "CLICK RESUME/APPLY  ENTER APPLY  ESC CLOSE",
        MakeColor(0.84f, 0.88f, 0.93f, 1.0f),
        screenWidth,
        screenHeight);
}

void GameMode::AppendBeam(std::vector<render::ColorVertex3D>& lines, const Beam& beam) const
{
    lines.push_back({beam.start, beam.color});
    lines.push_back({beam.end, beam.color});
}

void GameMode::AppendWorldMarker(std::vector<render::ColorVertex3D>& lines, const Vec3& center, const float radius, const Vec4& color) const
{
    lines.push_back({center + Vec3{-radius, 0.0f, 0.0f}, color});
    lines.push_back({center + Vec3{ radius, 0.0f, 0.0f}, color});
    lines.push_back({center + Vec3{0.0f, -radius, 0.0f}, color});
    lines.push_back({center + Vec3{0.0f,  radius, 0.0f}, color});
    lines.push_back({center + Vec3{0.0f, 0.0f, -radius}, color});
    lines.push_back({center + Vec3{0.0f, 0.0f,  radius}, color});
}

void GameMode::DumpPerfCounters() const
{
    LogInfo(
        "Perf counters | fps=", smoothedFps_,
        " target_fps=", targetFrameRate_,
        " frame_ms=", frameDeltaSeconds_ * 1000.0f,
        " active_chunks=", world_.ActiveChunkCount(),
        " grenades=", grenades_.size(),
        " bullets=", bullets_.size(),
        " beams=", beams_.size(),
        " tool=", ToolName(),
        " paused=", paused_,
        " driving_truck=", drivingTruck_,
        " truck_speed=", truck_.SpeedMetersPerSecond(),
        " world=", world_.WorldWidthCells(), "x", world_.WorldHeightCells(), "x", world_.WorldDepthCells(),
        " chunk_size=", world_.ActiveChunkSizeCells(),
        " cell_size=", world_.GenerationSettings().cellSize,
        " seed=", world_.GenerationSettings().seed,
        " sim_backend=", world_.ActiveMpmBackendName(),
        " sim_threads=", world_.MpmWorkerCount(),
        " mesh_ms=", world_.LastTerrainRebuildMilliseconds());

    if (profilerSnapshot_.stageCount == 0u)
    {
        return;
    }

    LogInfo(
        "Profiler | frame=", profilerSnapshot_.frameIndex,
        " frame_last_ms=", profilerSnapshot_.frameLastMilliseconds,
        " frame_avg_ms=", profilerSnapshot_.frameAverageMilliseconds,
        " frame_max_ms=", profilerSnapshot_.frameMaxMilliseconds,
        " hottest_self=", profilerSnapshot_.hottestStageName,
        " hottest_self_ms=", profilerSnapshot_.hottestStageMilliseconds,
        " hottest_total=", profilerSnapshot_.hottestInclusiveStageName,
        " hottest_total_ms=", profilerSnapshot_.hottestInclusiveStageMilliseconds);

    const std::size_t stageCount = std::min<std::size_t>(config::kProfilerHudStageCount, profilerSnapshot_.stageCount);
    for (std::size_t orderIndex = 0; orderIndex < stageCount; ++orderIndex)
    {
        const FrameProfiler::StageSnapshot& stage = profilerSnapshot_.stages[profilerSnapshot_.sortOrder[orderIndex]];
        LogInfo(
            "Profiler stage | rank=", (orderIndex + 1u),
            " name=", stage.name,
            " self_ms=", stage.lastSelfMilliseconds,
            " total_ms=", stage.lastMilliseconds,
            " avg_self_ms=", stage.averageSelfMilliseconds,
            " avg_total_ms=", stage.averageMilliseconds,
            " max_self_ms=", stage.maxSelfMilliseconds,
            " max_total_ms=", stage.maxMilliseconds,
            " calls=", stage.lastCalls);
    }
}

auto GameMode::CurrentAimPosition() const -> Vec3
{
    return drivingTruck_ ? truck_.CameraPosition() : player_.CameraPosition();
}

auto GameMode::CurrentCameraPosition() const -> Vec3
{
    if (drivingTruck_)
    {
        return truck_.CameraPosition();
    }

    if (!thirdPersonView_)
    {
        return player_.CameraPosition();
    }

    const Vec3 anchor = player_.CameraPosition() + Vec3{0.0f, 0.18f, 0.0f};
    const Vec3 flatForward = player_.FlatForwardVector();
    const Vec3 right = player_.RightVector();
    const Vec3 desired = anchor - flatForward * 4.2f + right * 0.70f + Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 offset = desired - anchor;
    const float distance = Length(offset);
    if (distance <= 0.01f)
    {
        return desired;
    }

    const Vec3 direction = offset / distance;
    const world::RaycastHit hit = world_.Raycast({anchor, direction}, distance);
    if (hit.hit)
    {
        return anchor + direction * std::max(0.30f, hit.distance - 0.24f);
    }

    return desired;
}

auto GameMode::CurrentViewTarget() const -> Vec3
{
    return CurrentAimPosition() + CurrentForwardVector() * 12.0f;
}

auto GameMode::CurrentForwardVector() const -> Vec3
{
    return drivingTruck_ ? truck_.ForwardVector() : player_.ForwardVector();
}

auto GameMode::ToolName() const -> std::string_view
{
    switch (tool_)
    {
    case ToolType::Rifle:
        return "RIFLE";
    case ToolType::Grenade:
        return "GRENADE";
    case ToolType::Dig:
        return "DIG";
    default:
        return "UNKNOWN";
    }
}
}
