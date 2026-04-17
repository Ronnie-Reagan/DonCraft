#pragma once

#include "core/profiler.hpp"
#include "game/player_controller.hpp"
#include "game/truck_controller.hpp"
#include "render/frame_data.hpp"
#include "world/demo_world.hpp"
#include "world/material_properties.hpp"

#include <filesystem>
#include <string_view>
#include <vector>

namespace df::platform
{
struct InputState;
}

namespace df::game
{
class GameMode
{
public:
    struct AudioCue
    {
        float baseFrequency = 220.0f;
        float durationSeconds = 0.1f;
        float amplitude = 0.18f;
        float noise = 0.0f;
        float sweep = 0.0f;
    };

    void Initialize(const std::filesystem::path& userDataDirectory);
    void BeginFrame(float frameDt);
    void SetFrameProfiler(FrameProfiler* profiler)
    {
        profiler_ = profiler;
        world_.SetFrameProfiler(profiler);
    }
    void SetProfilerSnapshot(const FrameProfiler::Snapshot& snapshot)
    {
        profilerSnapshot_ = snapshot;
    }
    void Tick(const platform::InputState& input, float fixedDt);
    [[nodiscard]] auto BuildRenderData(int viewportWidth, int viewportHeight) -> render::FrameRenderData;
    [[nodiscard]] auto WantsRelativeMouseMode() const -> bool
    {
        return !paused_;
    }
    [[nodiscard]] auto ConsumeAudioCues() -> std::vector<AudioCue>;
    [[nodiscard]] auto TruckEngineLoad() const -> float
    {
        return truck_.EngineLoad();
    }
    [[nodiscard]] auto TruckAudioRoughness() const -> float
    {
        return world::GetMaterialProperties(truck_.ContactMaterial()).wheelSink;
    }
    [[nodiscard]] auto TruckEngineActive() const -> bool
    {
        return drivingTruck_ || truck_.SpeedMetersPerSecond() > 0.6f;
    }
    [[nodiscard]] auto TargetFrameRate() const -> int
    {
        return targetFrameRate_;
    }

private:
    enum class ToolType
    {
        Rifle,
        Grenade,
        Dig,
    };

    struct Grenade
    {
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
        Vec3 position{};
        Vec3 previousPosition{};
        Vec3 velocity{};
        float ttl = 8.0f;
    };

    enum class PauseMenuItem
    {
        Resume = 0,
        WorldWidth,
        WorldHeight,
        WorldDepth,
        ActiveChunkSize,
        CellScale,
        Seed,
        Relief,
        WaterLevel,
        TargetFps,
        ApplyAndRebuild,
    };

    void OpenPauseMenu();
    void UpdatePauseMenu(const platform::InputState& input);
    void ApplyPendingWorldSettings();
    void ResetWorldAndActors();
    void QueueAudioCue(const AudioCue& cue);
    void FireRifle();
    void UseDigTool();
    void SpawnGrenade();
    void UpdateGrenades(float dt);
    void UpdateBeams(float dt);
    void UpdateBullets(float dt);
    void BuildHud(std::vector<render::ColorVertex2D>& overlayTriangles, int viewportWidth, int viewportHeight) const;
    void BuildPauseMenu(std::vector<render::ColorVertex2D>& overlayTriangles, int viewportWidth, int viewportHeight) const;
    void BuildProfilerHud(std::vector<render::ColorVertex2D>& overlayTriangles, int viewportWidth, int viewportHeight) const;
    void AppendBeam(std::vector<render::ColorVertex3D>& lines, const Beam& beam) const;
    void AppendWorldMarker(std::vector<render::ColorVertex3D>& lines, const Vec3& center, float radius, const Vec4& color) const;
    void DumpPerfCounters() const;

    [[nodiscard]] auto ToolName() const -> std::string_view;
    [[nodiscard]] auto CurrentAimPosition() const -> Vec3;
    [[nodiscard]] auto CurrentCameraPosition() const -> Vec3;
    [[nodiscard]] auto CurrentViewTarget() const -> Vec3;
    [[nodiscard]] auto CurrentForwardVector() const -> Vec3;
    [[nodiscard]] auto PauseMenuItemCount() const -> int
    {
        return static_cast<int>(PauseMenuItem::ApplyAndRebuild) + 1;
    }

    world::DemoWorld world_;
    PlayerController player_;
    std::filesystem::path savePath_;
    ToolType tool_ = ToolType::Rifle;
    bool showWireframe_ = false;
    bool showActiveChunks_ = true;
    bool showProfiler_ = true;
    bool paused_ = false;
    bool thirdPersonView_ = false;
    bool drivingTruck_ = false;
    int pauseMenuSelection_ = 0;
    int pauseDragItem_ = -1;
    world::WorldGenerationSettings pendingWorldSettings_{};
    float rifleCooldown_ = 0.0f;
    float digCooldown_ = 0.0f;
    float weaponCycle_ = 0.0f;
    float frameDeltaSeconds_ = 1.0f / 60.0f;
    float smoothedFps_ = 60.0f;
    int targetFrameRate_ = 72;
    int lastViewportWidth_ = 1600;
    int lastViewportHeight_ = 900;
    world::MaterialId crosshairMaterial_ = world::MaterialId::Air;
    TruckController truck_;
    float footstepCooldown_ = 0.0f;
    std::vector<AudioCue> audioCues_;
    std::vector<Grenade> grenades_;
    std::vector<Beam> beams_;
    std::vector<Bullet> bullets_;
    FrameProfiler* profiler_ = nullptr;
    FrameProfiler::Snapshot profilerSnapshot_{};
};
}
