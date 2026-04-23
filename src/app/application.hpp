#pragma once

#include "core/config.hpp"
#include "core/fixed_step_clock.hpp"
#include "core/job_system.hpp"
#include "core/profiler.hpp"
#include "game/session_render.hpp"
#include "net/session_client.hpp"
#include "net/session_host.hpp"
#include "platform/audio_device.hpp"
#include "render/vulkan_renderer.hpp"
#include "steam/steam_client.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace df::platform
{
class SdlPlatform;
struct InputState;
}

namespace df::steam
{
class SteamSocketsTransport;
}

namespace df
{
class Application
{
public:
    Application();
    ~Application();

    int Run();

private:
    struct PendingListenJoin
    {
        std::uint64_t lobbyId = 0;
        std::uint64_t hostSteamIdFallback = 0;
        std::string sessionName;
    };

    enum class ScreenState
    {
        MainMenu,
        HostSetup,
        Browser,
        OfflineSession,
        ListenSession,
        ClientSession,
    };

    enum class SessionMenuItem
    {
        Resume = 0,
        RenderDistance,
        InviteOrBrowse,
        SaveWorld,
        RestartWorld,
        MainMenu,
        QuitGame,
    };

    enum class OfflinePauseItem
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
        RenderDistance,
        TargetFps,
        ApplyAndRebuild,
        MainMenu,
        QuitGame,
    };

    void HandleFrameInput(const platform::InputState& input, bool& running);
    void TickSimulation(double dt);
    void HandleSteamEvents();
    void BuildLocalCommand(const platform::InputState& input, bool gameplayInputEnabled);
    [[nodiscard]] auto ConsumeLocalCommandForTick(float dt) -> game::PlayerCommandFrame;
    void ClearLocalCommandState();
    void ResetActiveSessions();
    void QueueListenJoin(std::uint64_t lobbyId, std::uint64_t hostSteamIdFallback, std::string_view sessionName, std::string_view statusText);
    [[nodiscard]] bool StartClientSessionWithTransport(std::unique_ptr<steam::SteamSocketsTransport> transport, std::string_view statusText);
    void CompletePendingListenJoin();
    [[nodiscard]] auto WorldSavePath(int slot) const -> std::filesystem::path;
    [[nodiscard]] bool WorldSaveExists(int slot) const;
    [[nodiscard]] auto ResolveWorldSavePathForStart(int slot, bool createNewWorld, bool listenSession) const -> std::filesystem::path;
    [[nodiscard]] auto ResolveWorldSlotForStart(bool createNewWorld) -> int;
    void StartOfflineSession(bool createNewWorld);
    void StartListenSession(bool createNewWorld);
    void JoinBrowserEntry(std::size_t index);
    void DisconnectToMenu(std::string_view statusText = "MAIN MENU");
    void ApplyPendingWorldSettings();
    void ResetLocalInputEdgeCounters();
    void BuildOverlay(render::FrameRenderData& renderData, const platform::InputState& input, bool& running);
    void BuildMainMenu(std::vector<render::ColorVertex2D>& overlayTriangles) const;
    void BuildHostSetupMenu(std::vector<render::ColorVertex2D>& overlayTriangles) const;
    void BuildBrowserMenu(std::vector<render::ColorVertex2D>& overlayTriangles) const;
    void BuildSessionOverlay(std::vector<render::ColorVertex2D>& overlayTriangles) const;
    void HandleMainMenuInput(const platform::InputState& input, bool& running);
    void HandleHostSetupInput(const platform::InputState& input);
    void HandleBrowserInput(const platform::InputState& input);
    void HandleSessionOverlayInput(const platform::InputState& input, bool& running);
    [[nodiscard]] auto OfflinePauseItemCount() const -> int
    {
        return static_cast<int>(OfflinePauseItem::QuitGame) + 1;
    }

    JobSystem jobSystem_;
    FixedStepClock fixedStepClock_{config::kFixedTickSeconds, 0.25, config::kMaxFixedTicksPerFrame};
    steam::SteamClientContext steam_;
    std::unique_ptr<platform::SdlPlatform> platform_;
    platform::AudioDevice audio_;
    game::SessionRenderOptions renderOptions_{};
    game::PlayerCommandFrame localCommand_{};
    std::uint32_t localCommandSequence_ = 0;
    game::PlayerId localPlayerId_ = 1u;
    game::ToolType selectedTool_ = game::ToolType::Rifle;
    std::uint8_t buildRotationQuarterTurns_ = 0u;
    std::filesystem::path userDataPath_;
    ScreenState screenState_ = ScreenState::MainMenu;
    std::optional<ScreenState> browserReturnState_{};
    std::optional<PendingListenJoin> pendingListenJoin_{};
    bool sessionMenuOpen_ = false;
    int mainMenuSelection_ = 0;
    int hostSetupSelection_ = 0;
    int browserSelection_ = 0;
    int sessionMenuSelection_ = 0;
    int selectedWorldSlot_ = 1;
    int mainMenuDragItem_ = -1;
    int hostSetupDragItem_ = -1;
    int sessionMenuDragItem_ = -1;
    bool hostSetupCreateNewWorld_ = false;
    world::WorldGenerationSettings pendingWorldSettings_{};
    int targetFrameRate_ = 72;
    float frameDeltaSeconds_ = static_cast<float>(config::kFixedTickSeconds);
    float smoothedFps_ = 60.0f;
    float pendingLookYawDelta_ = 0.0f;
    float pendingLookPitchDelta_ = 0.0f;
    float cumulativeLookYawDelta_ = 0.0f;
    float cumulativeLookPitchDelta_ = 0.0f;
    float localWeaponCycle_ = 0.0f;
    bool pendingJumpPressed_ = false;
    bool pendingPrimaryPressed_ = false;
    bool pendingQuickGrenadePressed_ = false;
    bool pendingInteractPressed_ = false;
    bool pendingReloadPressed_ = false;
    std::uint32_t jumpPressCount_ = 0;
    std::uint32_t primaryPressCount_ = 0;
    std::uint32_t quickGrenadePressCount_ = 0;
    std::uint32_t interactPressCount_ = 0;
    std::uint32_t reloadPressCount_ = 0;
    std::string statusText_;
    std::string playerName_ = "Frontier Player";
    net::SessionHost hostSession_{};
    net::SessionClient clientSession_{};
    bool hostSessionActive_ = false;
    bool clientSessionActive_ = false;
    render::VulkanRenderer renderer_;
    FrameProfiler frameProfiler_{};
};
}
