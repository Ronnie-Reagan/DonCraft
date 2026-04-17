#include "game/session_runtime.hpp"

#include "core/log.hpp"
#include "game/model_primitives.hpp"
#include "world/material_properties.hpp"

#include <algorithm>
#include <cmath>

namespace df::game
{
namespace
{
constexpr float kGrenadeRadius = 0.16f;
constexpr float kBulletRadius = 0.035f;
constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
constexpr float kRifleWeaponCycleDecayRate = 6.5f;
constexpr float kDigWeaponCycleDecayRate = 2.1f;
constexpr float kDigRepeatCooldownSeconds = 0.42f;

auto WeaponCycleDecayRate(const ToolType tool) -> float
{
    return tool == ToolType::Dig ? kDigWeaponCycleDecayRate : kRifleWeaponCycleDecayRate;
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

auto BuildHeldItemBasis(
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
    basis.origin -= basis.forward * (weaponCycle * weaponCycle) * 0.08f;
    if (clampedAdsBlend > 0.0f)
    {
        const Vec3 sightPoint = model::TransformPoint(basis, Vec3{0.0f, 0.07f, 0.20f});
        const Vec3 desiredSightPoint = aimPosition + basis.forward * 0.40f;
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
    model::Basis3 basis = BuildHeldItemBasis(aimPosition, forward, walkCycle, moveSpeed, weaponCycle);
    const float swingPhase = Clamp(weaponCycle, 0.0f, 1.0f);
    const float swingPitch = DegreesToRadians(Lerp(30.0f, -56.0f, swingPhase));
    const float swingYaw = DegreesToRadians(Lerp(-8.0f, 12.0f, swingPhase));
    basis.forward = Normalize(model::RotateAroundAxis(basis.forward, basis.right, swingPitch));
    basis.forward = Normalize(model::RotateAroundAxis(basis.forward, basis.up, swingYaw));
    basis.up = Normalize(Cross(basis.right, basis.forward));
    basis.origin += basis.forward * (0.14f + swingPhase * 0.18f);
    basis.origin -= basis.up * (0.01f + swingPhase * 0.18f);
    return basis;
}

auto BuildShovelBladeCenter(const model::Basis3& shovelBasis) -> Vec3
{
    return model::TransformPoint(shovelBasis, Vec3{0.0f, -0.10f, 1.00f});
}

auto BuildPlayerHeldItemBasis(const SessionRuntime::PlayerState& player) -> model::Basis3
{
    const float adsBlend =
        player.tool == ToolType::Rifle && player.command.secondaryDown
            ? 1.0f
            : 0.0f;
    return BuildHeldItemBasis(
        player.controller.CameraPosition(),
        player.controller.ForwardVector(),
        player.controller.WalkCycleRadians(),
        player.controller.HorizontalSpeedMetersPerSecond(),
        player.weaponCycle,
        adsBlend);
}

auto BuildRifleMuzzlePosition(const SessionRuntime::PlayerState& player) -> Vec3
{
    const model::Basis3 heldItemBasis = BuildPlayerHeldItemBasis(player);
    return model::TransformPoint(heldItemBasis, Vec3{0.0f, -0.01f, 0.96f});
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
}

void SessionRuntime::Initialize(const Config& config)
{
    config_ = config;
    world_.SetGenerationSettings(config_.generationSettings);
    if (!config_.savePath.empty() && config_.loadExistingWorld && world_.Load(config_.savePath))
    {
        config_.generationSettings = world_.GenerationSettings();
    }
    else
    {
        world_.Reset();
    }

    autosaveTimerSeconds_ = config_.autosaveIntervalSeconds;
    tickIndex_ = 0;
    ResetActors();
}

void SessionRuntime::Shutdown()
{
    if (config_.autosaveEnabled && !config_.savePath.empty())
    {
        (void)SaveNow();
    }
}

bool SessionRuntime::AddPlayer(const PlayerId id, const std::string_view name)
{
    if (id == kInvalidPlayerId || players_.contains(id) || static_cast<int>(players_.size()) >= config_.maxPlayers)
    {
        return false;
    }

    PlayerState state{};
    state.id = id;
    state.name = std::string(name);
    state.controller.Spawn(world_);
    players_.emplace(id, std::move(state));
    return true;
}

void SessionRuntime::RemovePlayer(const PlayerId id)
{
    auto iter = players_.find(id);
    if (iter == players_.end())
    {
        return;
    }

    if (iter->second.drivingTruck || truckDriverId_ == id)
    {
        truckDriverId_ = kInvalidPlayerId;
    }

    players_.erase(iter);
}

void SessionRuntime::SubmitCommand(const PlayerId id, const PlayerCommandFrame& command)
{
    auto* const player = FindPlayer(id);
    if (player == nullptr)
    {
        return;
    }

    player->command = command;
    player->tool = command.selectedTool;
}

void SessionRuntime::Tick(const float dt)
{
    TickPlayers(dt);
    TickTruck(dt);
    world_.Tick(dt);
    UpdateProjectiles(dt);

    if (config_.autosaveEnabled && !config_.savePath.empty())
    {
        autosaveTimerSeconds_ -= dt;
        if (autosaveTimerSeconds_ <= 0.0)
        {
            (void)SaveNow();
            autosaveTimerSeconds_ = std::max(5.0, config_.autosaveIntervalSeconds);
        }
    }

    ++tickIndex_;
}

bool SessionRuntime::SaveNow() const
{
    if (config_.savePath.empty())
    {
        return false;
    }

    return world_.Save(config_.savePath);
}

bool SessionRuntime::LoadFromDisk()
{
    if (config_.savePath.empty())
    {
        return false;
    }
    if (!world_.Load(config_.savePath))
    {
        return false;
    }

    config_.generationSettings = world_.GenerationSettings();
    ResetActors();
    return true;
}

void SessionRuntime::RestartWorld()
{
    world_.SetGenerationSettings(config_.generationSettings);
    world_.Reset();
    ResetActors();
}

void SessionRuntime::SetGenerationSettings(const world::WorldGenerationSettings& settings)
{
    config_.generationSettings = world::DemoWorld::ClampGenerationSettings(settings);
    world_.SetGenerationSettings(config_.generationSettings);
    world_.Reset();
    ResetActors();
}

auto SessionRuntime::FindPlayer(const PlayerId id) const -> const PlayerState*
{
    const auto iter = players_.find(id);
    return iter != players_.end() ? &iter->second : nullptr;
}

auto SessionRuntime::FindPlayer(const PlayerId id) -> PlayerState*
{
    const auto iter = players_.find(id);
    return iter != players_.end() ? &iter->second : nullptr;
}

auto SessionRuntime::ConsumeAudioCues() -> std::vector<AudioCue>
{
    std::vector<AudioCue> drained;
    drained.swap(audioCues_);
    return drained;
}

void SessionRuntime::ResetActors()
{
    truck_.Reset(world_);
    truckDriverId_ = kInvalidPlayerId;
    grenades_.clear();
    beams_.clear();
    bullets_.clear();
    audioCues_.clear();

    for (auto& [id, player] : players_)
    {
        player.controller.Spawn(world_);
        player.tool = ToolType::Rifle;
        player.drivingTruck = false;
        player.rifleCooldown = 0.0f;
        player.digCooldown = 0.0f;
        player.weaponCycle = 0.0f;
        player.footstepCooldown = 0.0f;
        player.crosshairMaterial = world::MaterialId::Air;
        player.lastAppliedCommandSequence = 0;
        player.command = {};
    }
}

void SessionRuntime::QueueAudioCue(const AudioCue& cue)
{
    audioCues_.push_back(cue);
}

void SessionRuntime::TickPlayers(const float dt)
{
    for (auto& [id, player] : players_)
    {
        player.rifleCooldown = std::max(0.0f, player.rifleCooldown - dt);
        player.digCooldown = std::max(0.0f, player.digCooldown - dt);
        player.weaponCycle = std::max(0.0f, player.weaponCycle - dt * WeaponCycleDecayRate(player.tool));
        player.footstepCooldown = std::max(0.0f, player.footstepCooldown - dt);

        if (player.command.interactPressed)
        {
            if (player.drivingTruck && truckDriverId_ == id)
            {
                player.drivingTruck = false;
                truckDriverId_ = kInvalidPlayerId;
                const Vec3 forward = truck_.ForwardVector();
                player.controller.PlaceAt(truck_.ExitPosition(), std::atan2(forward.z, forward.x), DegreesToRadians(-10.0f));
                QueueAudioCue({180.0f, 0.10f, 0.16f, 0.30f, -70.0f});
            }
            else if (truckDriverId_ == kInvalidPlayerId && truck_.CanEnter(player.controller.Position()))
            {
                player.drivingTruck = true;
                truckDriverId_ = id;
                QueueAudioCue({140.0f, 0.14f, 0.18f, 0.18f, 90.0f});
            }
        }

        if (!player.drivingTruck)
        {
            player.controller.Tick(player.command.control, world_, dt);

            const bool movingOnFoot =
                player.controller.OnGround() &&
                (player.command.control.moveForward || player.command.control.moveBackward || player.command.control.moveLeft || player.command.control.moveRight);
            if (movingOnFoot && player.footstepCooldown <= 0.0f)
            {
                const world::MaterialId footMaterial = player.controller.MaterialUnderFeet(world_);
                switch (footMaterial)
                {
                case world::MaterialId::ShallowWater:
                    QueueAudioCue({84.0f, 0.16f, 0.14f, 0.74f, -8.0f});
                    player.footstepCooldown = 0.28f;
                    break;
                case world::MaterialId::WetMud:
                    QueueAudioCue({72.0f, 0.14f, 0.13f, 0.68f, -12.0f});
                    player.footstepCooldown = 0.30f;
                    break;
                case world::MaterialId::DrySand:
                    QueueAudioCue({102.0f, 0.10f, 0.10f, 0.42f, -10.0f});
                    player.footstepCooldown = 0.26f;
                    break;
                case world::MaterialId::Grass:
                    QueueAudioCue({88.0f, 0.11f, 0.09f, 0.24f, -4.0f});
                    player.footstepCooldown = 0.25f;
                    break;
                case world::MaterialId::Gravel:
                    QueueAudioCue({138.0f, 0.09f, 0.10f, 0.34f, -18.0f});
                    player.footstepCooldown = 0.26f;
                    break;
                default:
                    QueueAudioCue({118.0f, 0.08f, 0.09f, 0.16f, -6.0f});
                    player.footstepCooldown = 0.24f;
                    break;
                }
            }
        }

        if (!player.drivingTruck && player.command.quickGrenadePressed)
        {
            SpawnGrenade(player);
        }

        if (!player.drivingTruck)
        {
            switch (player.tool)
            {
            case ToolType::Grenade:
                if (player.command.primaryPressed)
                {
                    SpawnGrenade(player);
                }
                break;
            case ToolType::Rifle:
                if (player.command.primaryDown && player.rifleCooldown <= 0.0f)
                {
                    FireRifle(player);
                    player.rifleCooldown = 0.14f;
                }
                break;
            case ToolType::Dig:
                if (player.command.primaryDown && player.digCooldown <= 0.0f)
                {
                    UseDigTool(player);
                    player.digCooldown = kDigRepeatCooldownSeconds;
                }
                break;
            default:
                break;
            }
        }

        UpdatePlayerCrosshair(player);
        player.lastAppliedCommandSequence = player.command.sequence;
    }
}

void SessionRuntime::TickTruck(const float dt)
{
    ControlState driverControl{};
    if (const PlayerState* const driver = FindPlayer(truckDriverId_))
    {
        driverControl = driver->command.control;
    }

    truck_.Tick(driverControl, world_, dt, truckDriverId_ != kInvalidPlayerId);
}

void SessionRuntime::UpdateProjectiles(const float dt)
{
    UpdateBullets(dt);
    UpdateGrenades(dt);
    UpdateBeams(dt);
}

void SessionRuntime::UpdatePlayerCrosshair(PlayerState& player)
{
    const world::RaycastHit sightHit = world_.Raycast(
        {CurrentAimPosition(player), CurrentForwardVector(player)},
        player.drivingTruck ? 72.0f : 48.0f);
    player.crosshairMaterial = sightHit.hit ? sightHit.material : world::MaterialId::Air;
}

void SessionRuntime::FireRifle(PlayerState& player)
{
    const Vec3 sightOrigin = CurrentAimPosition(player);
    const Vec3 sightDirection = CurrentForwardVector(player);
    const world::RaycastHit sightHit = world_.Raycast({sightOrigin, sightDirection}, 140.0f);
    const Vec3 origin = BuildRifleMuzzlePosition(player);
    Vec3 direction = Normalize((sightHit.hit ? sightHit.position : (sightOrigin + sightDirection * 140.0f)) - origin);
    if (LengthSquared(direction) <= 1.0e-6f)
    {
        direction = sightDirection;
    }
    player.weaponCycle = 1.0f;

    Bullet bullet{};
    bullet.ownerId = player.id;
    bullet.position = origin;
    bullet.previousPosition = origin;
    bullet.velocity = direction * 112.0f;
    bullet.ttl = 8.0f;
    bullets_.push_back(bullet);

    QueueAudioCue({74.0f, 0.11f, 0.26f, 0.18f, -36.0f});
    QueueAudioCue({238.0f, 0.06f, 0.18f, 0.14f, 18.0f});
    QueueAudioCue({820.0f, 0.03f, 0.08f, 0.30f, -140.0f});
}

void SessionRuntime::UseDigTool(PlayerState& player)
{
    player.weaponCycle = 1.0f;

    const Vec3 aimPosition = CurrentAimPosition(player);
    const Vec3 forward = CurrentForwardVector(player);
    const float walkCycle = player.controller.WalkCycleRadians();
    const float moveSpeed = player.controller.HorizontalSpeedMetersPerSecond();

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
            for (const float depth : {0.06f, 0.18f, 0.30f})
            {
                const Vec3 samplePoint =
                    bladeCenter +
                    shovelBasis.right * lateral +
                    shovelBasis.forward * depth -
                    shovelBasis.up * 0.06f;
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

void SessionRuntime::SpawnGrenade(const PlayerState& player)
{
    Grenade grenade{};
    grenade.ownerId = player.id;
    const Vec3 forward = CurrentForwardVector(player);
    const Vec3 right = RightFromForward(forward);
    const model::Basis3 heldItemBasis = BuildHeldItemBasis(
        CurrentAimPosition(player),
        forward,
        player.controller.WalkCycleRadians(),
        player.controller.HorizontalSpeedMetersPerSecond(),
        player.weaponCycle);
    grenade.position = heldItemBasis.origin + heldItemBasis.forward * 0.12f;
    grenade.velocity = forward * 13.0f + Vec3{0.0f, 3.5f, 0.0f};
    grenade.orientationForward = forward;
    grenade.orientationUp = kWorldUp;
    grenade.angularVelocity = right * 16.0f + kWorldUp * 4.0f;
    grenades_.push_back(grenade);
}

void SessionRuntime::UpdateGrenades(const float dt)
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

void SessionRuntime::UpdateBeams(const float dt)
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

void SessionRuntime::UpdateBullets(const float dt)
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

                QueueBulletImpactAudio(hit.material, Length(iter->velocity));

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

void SessionRuntime::QueueBulletImpactAudio(const world::MaterialId material, const float impactSpeed)
{
    const world::MaterialProperties properties = world::GetMaterialProperties(material);
    const float impactEnergy = Clamp(impactSpeed / 112.0f, 0.72f, 1.18f);
    const float hardness = Clamp(properties.rifleResistance, 0.0f, 1.0f);

    if (properties.isLiquid || material == world::MaterialId::ShallowWater)
    {
        QueueAudioCue({136.0f, 0.035f, 0.16f * impactEnergy, 0.90f, -26.0f});
        QueueAudioCue({96.0f, 0.125f, 0.12f * impactEnergy, 0.76f, -18.0f});
        QueueAudioCue({58.0f, 0.220f, 0.07f * impactEnergy, 0.36f, -8.0f});
        return;
    }

    if (material == world::MaterialId::WetMud)
    {
        QueueAudioCue({122.0f, 0.040f, 0.14f * impactEnergy, 0.76f, -30.0f});
        QueueAudioCue({86.0f, 0.145f, 0.11f * impactEnergy, 0.62f, -16.0f});
        QueueAudioCue({52.0f, 0.200f, 0.06f * impactEnergy, 0.22f, -10.0f});
        return;
    }

    if (material == world::MaterialId::Grass)
    {
        QueueAudioCue({286.0f, 0.026f, 0.10f * impactEnergy, 0.74f, -150.0f});
        QueueAudioCue({148.0f, 0.082f, 0.08f * impactEnergy, 0.34f, -36.0f});
        QueueAudioCue({92.0f, 0.130f, 0.05f * impactEnergy, 0.18f, -16.0f});
        return;
    }

    if (material == world::MaterialId::DrySand || material == world::MaterialId::Gravel || properties.isLoose)
    {
        QueueAudioCue({332.0f, 0.028f, 0.12f * impactEnergy, 0.82f, -170.0f});
        QueueAudioCue({162.0f, 0.095f, 0.10f * impactEnergy, 0.40f, -44.0f});
        QueueAudioCue({84.0f, 0.155f, 0.06f * impactEnergy, 0.26f, -18.0f});
        return;
    }

    if (hardness >= 0.58f)
    {
        QueueAudioCue({980.0f, 0.018f, 0.18f * impactEnergy, 0.86f, -520.0f});
        QueueAudioCue({Lerp(300.0f, 520.0f, hardness), 0.075f, Lerp(0.10f, 0.16f, hardness) * impactEnergy, 0.14f, -110.0f});
        QueueAudioCue({Lerp(132.0f, 196.0f, hardness), 0.135f, 0.06f * impactEnergy, 0.05f, -22.0f});
        return;
    }

    QueueAudioCue({304.0f, 0.030f, 0.11f * impactEnergy, 0.72f, -140.0f});
    QueueAudioCue({174.0f, 0.088f, 0.10f * impactEnergy, 0.28f, -32.0f});
    QueueAudioCue({98.0f, 0.125f, 0.05f * impactEnergy, 0.12f, -12.0f});
}

auto SessionRuntime::CurrentAimPosition(const PlayerState& player) const -> Vec3
{
    return player.drivingTruck ? truck_.CameraPosition() : player.controller.CameraPosition();
}

auto SessionRuntime::CurrentForwardVector(const PlayerState& player) const -> Vec3
{
    return player.drivingTruck ? truck_.ForwardVector() : player.controller.ForwardVector();
}
}
