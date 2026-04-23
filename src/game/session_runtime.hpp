#pragma once

#include "core/math.hpp"
#include "game/construction.hpp"
#include "game/player_controller.hpp"
#include "game/session_types.hpp"
#include "game/truck_controller.hpp"
#include "game/weapon_definitions.hpp"
#include "world/demo_world.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace df::game
{
class SessionRuntime
{
public:
    struct AudioCue
    {
        Vec3 position{};
        float baseFrequency = 220.0f;
        float durationSeconds = 0.1f;
        float amplitude = 0.18f;
        float noise = 0.0f;
        float sweep = 0.0f;
    };

    struct Grenade
    {
        PlayerId ownerId = kInvalidPlayerId;
        Vec3 position{};
        Vec3 velocity{};
        Vec3 orientationForward{0.0f, 0.0f, 1.0f};
        Vec3 orientationUp{0.0f, 1.0f, 0.0f};
        Vec3 angularVelocity{};
        float fuse = 2.25f;
    };

    struct Beam
    {
        Vec3 start{};
        Vec3 end{};
        Vec4 color{};
        float ttl = 0.0f;
    };

    struct Bullet
    {
        PlayerId ownerId = kInvalidPlayerId;
        Vec3 position{};
        Vec3 previousPosition{};
        Vec3 velocity{};
        int damage = 0;
        float ttl = 8.0f;
    };

    struct PlayerState
    {
        struct WeaponInventory
        {
            int ammoInMagazine = 0;
            int reserveAmmo = 0;
        };

        PlayerId id = kInvalidPlayerId;
        std::string name;
        PlayerController controller;
        PlayerCommandFrame command{};
        bool hasReceivedCommand = false;
        ToolType tool = ToolType::Rifle;
        bool drivingTruck = false;
        std::array<WeaponInventory, kToolTypeCount> weaponInventories{};
        float fireCooldown = 0.0f;
        float digCooldown = 0.0f;
        float buildCooldown = 0.0f;
        float reloadTimer = 0.0f;
        float reloadDuration = 0.0f;
        float weaponCycle = 0.0f;
        float footstepCooldown = 0.0f;
        bool reloading = false;
        std::uint8_t buildRotationQuarterTurns = 0u;
        world::MaterialId crosshairMaterial = world::MaterialId::Air;
        std::uint32_t lastAppliedCommandSequence = 0;
        std::uint32_t lastJumpPressCount = 0;
        std::uint32_t lastPrimaryPressCount = 0;
        std::uint32_t lastQuickGrenadePressCount = 0;
        std::uint32_t lastInteractPressCount = 0;
        std::uint32_t lastReloadPressCount = 0;
        float lastCumulativeLookYawDelta = 0.0f;
        float lastCumulativeLookPitchDelta = 0.0f;
        int health = 100;
    };

    struct Config
    {
        SessionMode mode = SessionMode::Offline;
        std::filesystem::path savePath;
        std::string sessionName = "DonCraft World";
        world::WorldGenerationSettings generationSettings{};
        bool loadExistingWorld = true;
        bool autosaveEnabled = true;
        double autosaveIntervalSeconds = 20.0;
        int maxPlayers = 8;
    };

    void Initialize(const Config& config);
    void Shutdown();

    [[nodiscard]] auto Configuration() const -> const Config&
    {
        return config_;
    }

    [[nodiscard]] bool AddPlayer(PlayerId id, std::string_view name);
    void RemovePlayer(PlayerId id);
    [[nodiscard]] bool SubmitCommand(PlayerId id, const PlayerCommandFrame& command);
    void Tick(float dt);

    [[nodiscard]] bool SaveNow() const;
    [[nodiscard]] bool LoadFromDisk();
    void RestartWorld();
    void SetGenerationSettings(const world::WorldGenerationSettings& settings);

    [[nodiscard]] auto World() const -> const world::DemoWorld&
    {
        return world_;
    }

    [[nodiscard]] auto MutableWorld() -> world::DemoWorld&
    {
        return world_;
    }

    [[nodiscard]] auto Truck() const -> const TruckController&
    {
        return truck_;
    }

    [[nodiscard]] auto Players() const -> const std::unordered_map<PlayerId, PlayerState>&
    {
        return players_;
    }

    [[nodiscard]] auto FindPlayer(PlayerId id) const -> const PlayerState*;
    [[nodiscard]] auto FindPlayer(PlayerId id) -> PlayerState*;
    [[nodiscard]] auto TruckDriverId() const -> PlayerId
    {
        return truckDriverId_;
    }

    [[nodiscard]] auto Grenades() const -> const std::vector<Grenade>&
    {
        return grenades_;
    }

    [[nodiscard]] auto Beams() const -> const std::vector<Beam>&
    {
        return beams_;
    }

    [[nodiscard]] auto Bullets() const -> const std::vector<Bullet>&
    {
        return bullets_;
    }

    [[nodiscard]] auto TickIndex() const -> std::uint64_t
    {
        return tickIndex_;
    }

    [[nodiscard]] auto ConsumeAudioCues() -> std::vector<AudioCue>;

private:
    void ResetActors();
    void QueueAudioCue(const AudioCue& cue);
    void TickPlayers(float dt);
    void TickTruck(float dt);
    void UpdateProjectiles(float dt);
    void UpdatePlayerCrosshair(PlayerState& player);
    void FireWeapon(PlayerState& player);
    void UseDigTool(PlayerState& player);
    void PlaceConstruction(PlayerState& player);
    void SpawnGrenade(const PlayerState& player);
    void UpdateGrenades(float dt);
    void UpdateBeams(float dt);
    void UpdateBullets(float dt);
    void QueueBulletImpactAudio(const Vec3& position, world::MaterialId material, float impactSpeed);
    void StartReload(PlayerState& player);
    void CompleteReload(PlayerState& player);
    void ApplyDamage(PlayerState& target, int damage, PlayerId instigator, const Vec3& impactPosition);
    void RespawnPlayer(PlayerState& player);

    [[nodiscard]] auto CurrentAimPosition(const PlayerState& player) const -> Vec3;
    [[nodiscard]] auto CurrentForwardVector(const PlayerState& player) const -> Vec3;

    Config config_{};
    world::DemoWorld world_;
    TruckController truck_;
    std::unordered_map<PlayerId, PlayerState> players_;
    PlayerId truckDriverId_ = kInvalidPlayerId;
    std::vector<AudioCue> audioCues_;
    std::vector<Grenade> grenades_;
    std::vector<Beam> beams_;
    std::vector<Bullet> bullets_;
    double autosaveTimerSeconds_ = 0.0;
    std::uint64_t tickIndex_ = 0;
};
}
