#include "app/application.hpp"

#include "core/log.hpp"
#include "game/debug_hud.hpp"
#include "platform/filesystem.hpp"
#include "platform/input_state.hpp"
#include "platform/sdl_platform.hpp"
#include "steam/steam_transport.hpp"
#include "world/material_properties.hpp"

#include <steam/isteamnetworkingsockets.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

namespace df
{
namespace
{
constexpr float kMinimumRenderDistanceMeters = 100.0f;
constexpr float kMaximumRenderDistanceMeters = 5000.0f;

auto FormatFloat(const float value, const int decimals = 1) -> std::string
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

auto ParseUint64OrZero(const std::string_view text) -> std::uint64_t
{
    std::uint64_t value = 0u;
    const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || ptr != text.data() + text.size())
    {
        return 0u;
    }
    return value;
}

auto LocalWeaponCycleDecayRate(const game::ToolType tool) -> float
{
    return game::GetWeaponDefinition(tool).weaponCycleDecayRate;
}

auto ClampRenderDistanceMeters(const float meters) -> float
{
    return Clamp(meters, kMinimumRenderDistanceMeters, kMaximumRenderDistanceMeters);
}

auto FindActorSnapshot(const net::ActorSnapshotFrame& frame, const game::PlayerId playerId) -> const net::ActorSnapshot*
{
    for (const net::ActorSnapshot& actor : frame.players)
    {
        if (actor.id == playerId)
        {
            return &actor;
        }
    }

    return nullptr;
}

void AppendHolosightReticle(
    std::vector<render::ColorVertex2D>& triangles,
    const float screenWidth,
    const float screenHeight)
{
    const float centerX = screenWidth * 0.5f;
    const float centerY = screenHeight * 0.5f;
    const Vec4 dotColor = MakeColor(0.94f, 0.22f, 0.18f, 0.92f);
    game::AppendRect(triangles, centerX - 2.0f, centerY - 2.0f, 4.0f, 4.0f, dotColor, screenWidth, screenHeight);
    game::AppendRect(triangles, centerX - 8.0f, centerY - 0.5f, 5.0f, 1.0f, dotColor, screenWidth, screenHeight);
    game::AppendRect(triangles, centerX + 3.0f, centerY - 0.5f, 5.0f, 1.0f, dotColor, screenWidth, screenHeight);
    game::AppendRect(triangles, centerX - 0.5f, centerY - 8.0f, 1.0f, 5.0f, dotColor, screenWidth, screenHeight);
    game::AppendRect(triangles, centerX - 0.5f, centerY + 3.0f, 1.0f, 5.0f, dotColor, screenWidth, screenHeight);
}

auto BuildEmptyRenderData(const int viewportWidth, const int viewportHeight) -> render::FrameRenderData
{
    render::FrameRenderData data{};
    data.clearColor = MakeColor(0.08f, 0.10f, 0.14f, 1.0f);
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(std::max(viewportHeight, 1));
    data.worldToClip =
        PerspectiveMatrix(DegreesToRadians(70.0f), aspect, 0.1f, 200.0f) *
        LookAtMatrix(Vec3{0.0f, 4.0f, -8.0f}, Vec3{0.0f, 0.5f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    return data;
}

auto PointInRect(const float x, const float y, const float rectX, const float rectY, const float rectWidth, const float rectHeight) -> bool
{
    return x >= rectX && x <= rectX + rectWidth && y >= rectY && y <= rectY + rectHeight;
}

auto HoveredMenuRow(
    const platform::InputState& input,
    const float panelX,
    const float rowStartY,
    const float rowWidth,
    const float rowHeight,
    const int rowCount) -> int
{
    if (!PointInRect(input.mouseX, input.mouseY, panelX + 18.0f, rowStartY - 4.0f, rowWidth, rowHeight * static_cast<float>(rowCount)))
    {
        return -1;
    }

    const int rowIndex = static_cast<int>((input.mouseY - (rowStartY - 4.0f)) / rowHeight);
    return rowIndex >= 0 && rowIndex < rowCount ? rowIndex : -1;
}

void AppendScopeOverlay(
    std::vector<render::ColorVertex2D>& triangles,
    const render::FrameRenderData::ScopedView& scopedView,
    const float screenWidth,
    const float screenHeight)
{
    if (!scopedView.enabled || scopedView.viewportWidth <= 0 || scopedView.viewportHeight <= 0)
    {
        return;
    }

    const float x = static_cast<float>(scopedView.viewportX);
    const float y = static_cast<float>(scopedView.viewportY);
    const float width = static_cast<float>(scopedView.viewportWidth);
    const float height = static_cast<float>(scopedView.viewportHeight);
    const float centerX = x + width * 0.5f;
    const float centerY = y + height * 0.5f;
    const float lineThickness = std::clamp(std::min(width, height) * 0.012f, 1.5f, 3.0f);
    const float segmentLength = std::clamp(std::min(width, height) * 0.18f, 10.0f, 18.0f);
    const float gap = std::clamp(std::min(width, height) * 0.065f, 5.0f, 9.0f);
    const Vec4 reticleColor = MakeColor(0.96f, 0.14f, 0.14f, 0.84f);
    game::AppendRect(triangles, centerX - lineThickness * 0.5f, centerY - gap - segmentLength, lineThickness, segmentLength, reticleColor, screenWidth, screenHeight);
    game::AppendRect(triangles, centerX - lineThickness * 0.5f, centerY + gap, lineThickness, segmentLength, reticleColor, screenWidth, screenHeight);
    game::AppendRect(triangles, centerX - gap - segmentLength, centerY - lineThickness * 0.5f, segmentLength, lineThickness, reticleColor, screenWidth, screenHeight);
    game::AppendRect(triangles, centerX + gap, centerY - lineThickness * 0.5f, segmentLength, lineThickness, reticleColor, screenWidth, screenHeight);
    game::AppendRect(triangles, centerX - lineThickness * 0.5f, centerY - lineThickness * 0.5f, lineThickness, lineThickness, reticleColor, screenWidth, screenHeight);
}
}

Application::Application() = default;
Application::~Application() = default;

int Application::Run()
{
    if (!steam_.Initialize())
    {
        LogError("Steam initialization failed before SDL startup: ", steam_.FailureMessage());
        return 1;
    }

    platform::WindowConfig windowConfig{};
    windowConfig.title = config::kApplicationName;
    windowConfig.width = config::kDefaultWindowWidth;
    windowConfig.height = config::kDefaultWindowHeight;
    platform_ = platform::SdlPlatform::Create(windowConfig);
    platform_->SetRelativeMouseMode(false);
    if (!audio_.Initialize())
    {
        LogWarning("SDL audio initialization failed; continuing without sound output.");
    }

    renderer_.Initialize(platform_->Window());
    renderer_.SetFrameProfiler(&frameProfiler_);
    userDataPath_ = platform::GetUserDataPath("Don Reagan", config::kApplicationName);
    pendingWorldSettings_ = {};
    playerName_ = steam_.PersonaName().empty() ? "Frontier Player" : steam_.PersonaName();
    statusText_ = "MAIN MENU";

    using Clock = std::chrono::steady_clock;
    auto previousFrameTime = Clock::now();
    bool running = true;

    while (running)
    {
        frameProfiler_.BeginFrame();
        const auto frameStartTime = Clock::now();
        const double frameDeltaSeconds = std::min(std::chrono::duration<double>(frameStartTime - previousFrameTime).count(), 0.25);
        previousFrameTime = frameStartTime;
        frameDeltaSeconds_ = static_cast<float>(frameDeltaSeconds);
        smoothedFps_ = Lerp(smoothedFps_, 1.0f / std::max(frameDeltaSeconds_, 0.0001f), 0.08f);

        {
            const ScopedProfileSection scope(&frameProfiler_, "Steam Callbacks");
            steam_.PumpCallbacks();
            HandleSteamEvents();
        }

        platform::FrameEvents frameEvents{};
        {
            const ScopedProfileSection scope(&frameProfiler_, "Pump Events");
            frameEvents = platform_->PumpEvents();
        }
        if (frameEvents.quitRequested)
        {
            running = false;
        }

        if (frameEvents.framebufferResized)
        {
            renderer_.RequestResize();
        }

        {
            const ScopedProfileSection scope(&frameProfiler_, "Frame Input");
            HandleFrameInput(platform_->Input(), running);
        }

        {
            const ScopedProfileSection scope(&frameProfiler_, "Fixed Tick");
            fixedStepClock_.Consume(frameDeltaSeconds, [this](const double dt)
            {
                TickSimulation(dt);
            });
        }

        {
            const ScopedProfileSection scope(&frameProfiler_, "Audio Update");
            if (hostSessionActive_)
            {
                for (const game::SessionRuntime::AudioCue& cue : hostSession_.ConsumeAudioCues())
                {
                    audio_.QueueCue({
                        .baseFrequency = cue.baseFrequency,
                        .durationSeconds = cue.durationSeconds,
                        .amplitude = cue.amplitude,
                        .noise = cue.noise,
                        .sweep = cue.sweep,
                    });
                }

                const bool engineActive = hostSession_.MutableRuntime().TruckDriverId() != game::kInvalidPlayerId ||
                    hostSession_.MutableRuntime().Truck().SpeedMetersPerSecond() > 0.6f;
                audio_.SetEngineState(
                    hostSession_.MutableRuntime().Truck().EngineLoad(),
                    world::GetMaterialProperties(hostSession_.MutableRuntime().Truck().ContactMaterial()).wheelSink,
                    engineActive);
            }
            else
            {
                audio_.SetEngineState(0.0f, 0.0f, false);
            }
        }

        if (platform_->IsMinimized())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            frameProfiler_.EndFrame();
            continue;
        }

        const auto [drawableWidth, drawableHeight] = platform_->DrawableSize();
        if (drawableWidth <= 0 || drawableHeight <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            frameProfiler_.EndFrame();
            continue;
        }

        const ScreenState renderSessionState =
            screenState_ == ScreenState::Browser && browserReturnState_.has_value()
                ? *browserReturnState_
                : screenState_;
        render::FrameRenderData renderData = BuildEmptyRenderData(drawableWidth, drawableHeight);
        if (hostSessionActive_ && (renderSessionState == ScreenState::OfflineSession || renderSessionState == ScreenState::ListenSession))
        {
            renderData = game::BuildRuntimeRenderData(
                hostSession_.MutableRuntime(),
                localPlayerId_,
                nullptr,
                renderOptions_,
                drawableWidth,
                drawableHeight);
        }
        else if (clientSessionActive_ && renderSessionState == ScreenState::ClientSession)
        {
            renderData = game::BuildClientRenderData(
                clientSession_,
                renderOptions_,
                drawableWidth,
                drawableHeight,
                static_cast<float>(fixedStepClock_.InterpolationAlpha()));
        }

        BuildOverlay(renderData, platform_->Input(), running);
        renderer_.Draw(renderData);

        const int targetFrameRate = std::clamp(targetFrameRate_, 30, 240);
        const auto minimumFrameDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(targetFrameRate)));
        const auto frameElapsed = Clock::now() - frameStartTime;
        if (frameElapsed < minimumFrameDuration)
        {
            std::this_thread::sleep_for(minimumFrameDuration - frameElapsed);
        }

        frameProfiler_.EndFrame();
    }

    DisconnectToMenu();
    renderer_.Shutdown();
    audio_.Shutdown();
    platform_.reset();
    steam_.Shutdown();
    return 0;
}

void Application::HandleFrameInput(const platform::InputState& input, bool& running)
{
    if (screenState_ != ScreenState::Browser &&
        (screenState_ == ScreenState::OfflineSession || screenState_ == ScreenState::ListenSession || screenState_ == ScreenState::ClientSession) &&
        input.KeyPressed(SDL_SCANCODE_ESCAPE))
    {
        if (!sessionMenuOpen_ && screenState_ == ScreenState::OfflineSession && hostSessionActive_)
        {
            pendingWorldSettings_ = hostSession_.MutableRuntime().World().GenerationSettings();
        }

        sessionMenuOpen_ = !sessionMenuOpen_;
        sessionMenuSelection_ = 0;
        sessionMenuDragItem_ = -1;
    }

    const bool browserOverSession =
        screenState_ == ScreenState::Browser &&
        browserReturnState_.has_value() &&
        *browserReturnState_ != ScreenState::MainMenu &&
        *browserReturnState_ != ScreenState::HostSetup &&
        *browserReturnState_ != ScreenState::Browser;
    const ScreenState activeSessionState = browserOverSession ? *browserReturnState_ : screenState_;

    switch (screenState_)
    {
    case ScreenState::MainMenu:
        HandleMainMenuInput(input, running);
        platform_->SetRelativeMouseMode(false);
        ClearLocalCommandState();
        return;
    case ScreenState::HostSetup:
        HandleHostSetupInput(input);
        platform_->SetRelativeMouseMode(false);
        ClearLocalCommandState();
        return;
    case ScreenState::Browser:
        HandleBrowserInput(input);
        if (!browserOverSession || activeSessionState == ScreenState::OfflineSession)
        {
            platform_->SetRelativeMouseMode(false);
            ClearLocalCommandState();
            return;
        }
        break;
    default:
        break;
    }

    if (sessionMenuOpen_)
    {
        HandleSessionOverlayInput(input);
    }

    const bool gameplayInputEnabled =
        screenState_ != ScreenState::Browser &&
        !sessionMenuOpen_ &&
        (activeSessionState == ScreenState::ListenSession || activeSessionState == ScreenState::ClientSession || activeSessionState == ScreenState::OfflineSession);
    platform_->SetRelativeMouseMode(gameplayInputEnabled);
    BuildLocalCommand(input, gameplayInputEnabled);
}

void Application::TickSimulation(const double dt)
{
    const bool browserOverSession =
        screenState_ == ScreenState::Browser &&
        browserReturnState_.has_value() &&
        *browserReturnState_ != ScreenState::MainMenu &&
        *browserReturnState_ != ScreenState::HostSetup &&
        *browserReturnState_ != ScreenState::Browser;
    const ScreenState activeSessionState = browserOverSession ? *browserReturnState_ : screenState_;

    if (hostSessionActive_ && (activeSessionState == ScreenState::OfflineSession || activeSessionState == ScreenState::ListenSession))
    {
        static_cast<void>(hostSession_.MutableRuntime().SubmitCommand(localPlayerId_, ConsumeLocalCommandForTick()));
        if (!(activeSessionState == ScreenState::OfflineSession && sessionMenuOpen_))
        {
            hostSession_.Tick(static_cast<float>(dt));
        }
        return;
    }

    if (clientSessionActive_ && activeSessionState == ScreenState::ClientSession)
    {
        clientSession_.Tick(static_cast<float>(dt), ConsumeLocalCommandForTick());
        if (clientSession_.Disconnected())
        {
            const std::string disconnectText = clientSession_.LastDisconnectText();
            DisconnectToMenu(disconnectText.empty() ? "DISCONNECTED" : disconnectText);
            return;
        }
        if (clientSession_.IsReady() && !clientSession_.SessionName().empty())
        {
            statusText_ = clientSession_.SessionName();
        }
        else if (clientSession_.HasServerConnection())
        {
            statusText_ = "SYNCING WORLD";
        }
        else
        {
            statusText_ = "CONNECTING TO HOST";
        }
        return;
    }

    ClearLocalCommandState();
}

void Application::HandleSteamEvents()
{
    for (const steam::SteamClientContext::Event& event : steam_.ConsumeEvents())
    {
        switch (event.type)
        {
        case steam::SteamClientContext::Event::Type::LobbyCreated:
            statusText_ = "LISTEN LOBBY CREATED";
            (void)steam_.SetCurrentLobbyData(steam::SteamClientContext::kLobbyWorldWidthKey, std::to_string(pendingWorldSettings_.worldWidth));
            (void)steam_.SetCurrentLobbyData(steam::SteamClientContext::kLobbyWorldHeightKey, std::to_string(pendingWorldSettings_.worldHeight));
            (void)steam_.SetCurrentLobbyData(steam::SteamClientContext::kLobbyWorldDepthKey, std::to_string(pendingWorldSettings_.worldDepth));
            (void)steam_.SetCurrentLobbyData(steam::SteamClientContext::kLobbyHostSteamIdKey, std::to_string(steam_.LocalSteamId()));
            (void)steam_.SetCurrentLobbyData(steam::SteamClientContext::kLobbyHostReadyKey, "1");
            (void)steam_.SetCurrentLobbyData(steam::SteamClientContext::kLobbyVirtualPortKey, "0");
            break;
        case steam::SteamClientContext::Event::Type::LobbyJoined:
            if (pendingListenJoin_.has_value() && pendingListenJoin_->lobbyId == event.lobbyId)
            {
                CompletePendingListenJoin();
            }
            else
            {
                statusText_ = "LOBBY JOINED";
            }
            break;
        case steam::SteamClientContext::Event::Type::LobbyJoinRequested:
            QueueListenJoin(event.lobbyId, 0u, "Steam Invite", "JOINING STEAM INVITE");
            break;
        case steam::SteamClientContext::Event::Type::BrowserUpdated:
            statusText_ = "BROWSER REFRESHED";
            if (browserSelection_ >= static_cast<int>(steam_.BrowserEntries().size()))
            {
                browserSelection_ = std::max(0, static_cast<int>(steam_.BrowserEntries().size()) - 1);
            }
            break;
        case steam::SteamClientContext::Event::Type::Error:
            if (pendingListenJoin_.has_value())
            {
                pendingListenJoin_.reset();
            }
            statusText_ = event.text;
            break;
        default:
            break;
        }
    }
}

void Application::BuildLocalCommand(const platform::InputState& input, const bool gameplayInputEnabled)
{
    localCommand_ = {};
    localCommand_.selectedTool = selectedTool_;

    if (!gameplayInputEnabled)
    {
        ClearLocalCommandState();
        return;
    }

    if (input.KeyPressed(SDL_SCANCODE_1))
    {
        selectedTool_ = game::ToolType::Rifle;
    }
    if (input.KeyPressed(SDL_SCANCODE_2))
    {
        selectedTool_ = game::ToolType::Grenade;
    }
    if (input.KeyPressed(SDL_SCANCODE_3))
    {
        selectedTool_ = game::ToolType::Dig;
    }
    if (input.KeyPressed(SDL_SCANCODE_4))
    {
        selectedTool_ = game::ToolType::Smg;
    }
    if (input.KeyPressed(SDL_SCANCODE_F2))
    {
        renderOptions_.showActiveChunks = !renderOptions_.showActiveChunks;
    }
    if (input.KeyPressed(SDL_SCANCODE_F3))
    {
        renderOptions_.showWireframe = !renderOptions_.showWireframe;
    }
    if (input.KeyPressed(SDL_SCANCODE_C))
    {
        renderOptions_.thirdPerson = !renderOptions_.thirdPerson;
    }
    const game::WeaponDefinition selectedWeapon = game::GetWeaponDefinition(selectedTool_);
    if (selectedWeapon.usesScope &&
        (input.KeyDown(SDL_SCANCODE_LCTRL) || input.KeyDown(SDL_SCANCODE_RCTRL)) &&
        std::abs(input.mouseWheelY) > 0.0f)
    {
        renderOptions_.zoomMagnification = Clamp(
            renderOptions_.zoomMagnification + input.mouseWheelY * 0.25f,
            selectedWeapon.minimumZoomMagnification,
            selectedWeapon.maximumZoomMagnification);
    }

    localCommand_.selectedTool = selectedTool_;
    localCommand_.control.moveForward = input.KeyDown(SDL_SCANCODE_W);
    localCommand_.control.moveBackward = input.KeyDown(SDL_SCANCODE_S);
    localCommand_.control.moveLeft = input.KeyDown(SDL_SCANCODE_A);
    localCommand_.control.moveRight = input.KeyDown(SDL_SCANCODE_D);
    localCommand_.control.sprint = input.KeyDown(SDL_SCANCODE_LSHIFT) || input.KeyDown(SDL_SCANCODE_RSHIFT);
    localCommand_.primaryDown = input.MouseDown(SDL_BUTTON_LEFT);
    localCommand_.secondaryDown = input.MouseDown(SDL_BUTTON_RIGHT) && selectedWeapon.supportsAds;
    renderOptions_.aimDownSights = localCommand_.secondaryDown;
    renderOptions_.localTool = selectedTool_;

    pendingLookYawDelta_ += input.mouseDeltaX;
    pendingLookPitchDelta_ += input.mouseDeltaY;
    pendingJumpPressed_ = pendingJumpPressed_ || input.KeyPressed(SDL_SCANCODE_SPACE);
    pendingPrimaryPressed_ = pendingPrimaryPressed_ || input.MousePressed(SDL_BUTTON_LEFT);
    pendingQuickGrenadePressed_ = pendingQuickGrenadePressed_ || input.KeyPressed(SDL_SCANCODE_G);
    pendingInteractPressed_ = pendingInteractPressed_ || input.KeyPressed(SDL_SCANCODE_E);
    pendingReloadPressed_ = pendingReloadPressed_ || input.KeyPressed(SDL_SCANCODE_R);
    localWeaponCycle_ = std::max(0.0f, localWeaponCycle_ - frameDeltaSeconds_ * LocalWeaponCycleDecayRate(selectedTool_));
    const bool repeatingToolAction =
        localCommand_.primaryDown &&
        localWeaponCycle_ <= 0.02f &&
        (selectedTool_ == game::ToolType::Dig || (game::IsFirearmTool(selectedTool_) && selectedWeapon.automatic));
    if (pendingPrimaryPressed_ || pendingQuickGrenadePressed_ || repeatingToolAction)
    {
        localWeaponCycle_ = 1.0f;
    }
    renderOptions_.localWeaponCycle = localWeaponCycle_;
}

auto Application::ConsumeLocalCommandForTick() -> game::PlayerCommandFrame
{
    game::PlayerCommandFrame command = localCommand_;
    command.sequence = ++localCommandSequence_;
    command.control.lookYawDelta = pendingLookYawDelta_;
    command.control.lookPitchDelta = pendingLookPitchDelta_;
    command.control.jumpPressed = pendingJumpPressed_;
    command.primaryPressed = pendingPrimaryPressed_;
    command.quickGrenadePressed = pendingQuickGrenadePressed_;
    command.interactPressed = pendingInteractPressed_;
    command.reloadPressed = pendingReloadPressed_;

    pendingLookYawDelta_ = 0.0f;
    pendingLookPitchDelta_ = 0.0f;
    pendingJumpPressed_ = false;
    pendingPrimaryPressed_ = false;
    pendingQuickGrenadePressed_ = false;
    pendingInteractPressed_ = false;
    pendingReloadPressed_ = false;
    return command;
}

void Application::ClearLocalCommandState()
{
    localCommand_ = {};
    pendingLookYawDelta_ = 0.0f;
    pendingLookPitchDelta_ = 0.0f;
    pendingJumpPressed_ = false;
    pendingPrimaryPressed_ = false;
    pendingQuickGrenadePressed_ = false;
    pendingInteractPressed_ = false;
    pendingReloadPressed_ = false;
    renderOptions_.aimDownSights = false;
    renderOptions_.localTool = selectedTool_;
    localWeaponCycle_ = std::max(0.0f, localWeaponCycle_ - frameDeltaSeconds_ * LocalWeaponCycleDecayRate(selectedTool_));
    renderOptions_.localWeaponCycle = localWeaponCycle_;
}

void Application::ResetActiveSessions()
{
    if (clientSessionActive_)
    {
        clientSession_.Shutdown();
        clientSessionActive_ = false;
    }
    if (hostSessionActive_)
    {
        hostSession_.Shutdown();
        hostSessionActive_ = false;
    }
    pendingListenJoin_.reset();
}

void Application::QueueListenJoin(
    const std::uint64_t lobbyId,
    const std::uint64_t hostSteamIdFallback,
    const std::string_view sessionName,
    const std::string_view statusText)
{
    if (lobbyId == 0u)
    {
        statusText_ = "LISTEN JOIN FAILED";
        return;
    }

    ResetActiveSessions();
    steam_.LeaveLobby();
    pendingListenJoin_ = PendingListenJoin{
        .lobbyId = lobbyId,
        .hostSteamIdFallback = hostSteamIdFallback,
        .sessionName = std::string(sessionName),
    };
    screenState_ = ScreenState::MainMenu;
    browserReturnState_.reset();
    sessionMenuOpen_ = false;
    sessionMenuSelection_ = 0;
    sessionMenuDragItem_ = -1;
    LogInfo(
        "Application queued listen join lobby_id=", lobbyId,
        " host_fallback=", hostSteamIdFallback,
        " session='", sessionName, "'");
    steam_.JoinLobby(lobbyId);
    statusText_ = std::string(statusText);
}

bool Application::StartClientSessionWithTransport(std::unique_ptr<steam::SteamSocketsTransport> transport, const std::string_view statusText)
{
    if (transport == nullptr)
    {
        return false;
    }

    clientSession_.Initialize({playerName_}, std::move(transport));
    clientSessionActive_ = true;
    hostSessionActive_ = false;
    sessionMenuOpen_ = false;
    sessionMenuSelection_ = 0;
    sessionMenuDragItem_ = -1;
    screenState_ = ScreenState::ClientSession;
    browserReturnState_.reset();
    renderOptions_.zoomMagnification = game::GetWeaponDefinition(game::ToolType::Rifle).defaultZoomMagnification;
    localWeaponCycle_ = 0.0f;
    ClearLocalCommandState();
    statusText_ = std::string(statusText);
    return true;
}

void Application::CompletePendingListenJoin()
{
    if (!pendingListenJoin_.has_value())
    {
        return;
    }

    const PendingListenJoin pending = *pendingListenJoin_;
    std::uint64_t hostSteamId = ParseUint64OrZero(steam_.CurrentLobbyData(steam::SteamClientContext::kLobbyHostSteamIdKey));
    if (hostSteamId == 0u)
    {
        hostSteamId = steam_.CurrentLobbyOwnerSteamId();
    }
    if (hostSteamId == 0u)
    {
        hostSteamId = pending.hostSteamIdFallback;
    }

    const bool hostReady = steam_.CurrentLobbyData(steam::SteamClientContext::kLobbyHostReadyKey) != "0";
    if (hostSteamId == 0u || !hostReady)
    {
        LogWarning(
            "Listen join could not resolve a valid host from lobby_id=", pending.lobbyId,
            " host_ready=", hostReady ? "yes" : "no");
        pendingListenJoin_.reset();
        steam_.LeaveLobby();
        statusText_ = "LISTEN JOIN FAILED";
        return;
    }

    auto transport = std::make_unique<steam::SteamSocketsTransport>(SteamNetworkingSockets(), false);
    if (!transport->ConnectP2P(hostSteamId, 0))
    {
        LogError("Listen join failed to connect to host steam_id=", hostSteamId, " lobby_id=", pending.lobbyId);
        pendingListenJoin_.reset();
        steam_.LeaveLobby();
        statusText_ = "LISTEN CONNECT FAILED";
        return;
    }

    pendingListenJoin_.reset();
    (void)StartClientSessionWithTransport(std::move(transport), "CONNECTING TO LISTEN HOST");
}

void Application::StartOfflineSession()
{
    ResetActiveSessions();
    steam_.LeaveLobby();

    net::SessionHost::Config config{};
    config.runtime.mode = game::SessionMode::Offline;
    config.runtime.sessionName = "Offline Frontier";
    config.runtime.savePath = userDataPath_ / "offline_world.bin";
    config.runtime.generationSettings = pendingWorldSettings_;
    config.localHostPlayerId = localPlayerId_;
    config.localHostPlayerName = playerName_;
    try
    {
        hostSession_.Initialize(config, nullptr);
    }
    catch (const std::exception& error)
    {
        LogError("Offline session startup failed: ", error.what());
        statusText_ = std::string("WORLD LOAD FAILED: ") + error.what();
        return;
    }
    hostSessionActive_ = true;
    clientSessionActive_ = false;
    sessionMenuOpen_ = false;
    sessionMenuSelection_ = 0;
    sessionMenuDragItem_ = -1;
    screenState_ = ScreenState::OfflineSession;
    pendingWorldSettings_ = hostSession_.MutableRuntime().World().GenerationSettings();
    renderOptions_.zoomMagnification = game::GetWeaponDefinition(game::ToolType::Rifle).defaultZoomMagnification;
    localWeaponCycle_ = 0.0f;
    ClearLocalCommandState();
    statusText_ = "OFFLINE SESSION";
}

void Application::StartListenSession()
{
    ResetActiveSessions();
    steam_.LeaveLobby();

    auto transport = std::make_unique<steam::SteamSocketsTransport>(SteamNetworkingSockets(), false);
    if (!transport->StartListenP2P(0))
    {
        statusText_ = "FAILED TO START LISTEN SOCKET";
        return;
    }

    net::SessionHost::Config config{};
    config.runtime.mode = game::SessionMode::ListenHost;
    config.runtime.sessionName = "Self Hosted Frontier";
    config.runtime.savePath = userDataPath_ / "listen_world.bin";
    config.runtime.generationSettings = pendingWorldSettings_;
    config.localHostPlayerId = localPlayerId_;
    config.localHostPlayerName = playerName_;
    try
    {
        hostSession_.Initialize(config, std::move(transport));
    }
    catch (const std::exception& error)
    {
        LogError("Listen session startup failed: ", error.what());
        statusText_ = std::string("WORLD LOAD FAILED: ") + error.what();
        return;
    }
    hostSessionActive_ = true;
    clientSessionActive_ = false;
    sessionMenuOpen_ = false;
    sessionMenuSelection_ = 0;
    sessionMenuDragItem_ = -1;
    screenState_ = ScreenState::ListenSession;
    pendingWorldSettings_ = hostSession_.MutableRuntime().World().GenerationSettings();
    renderOptions_.zoomMagnification = game::GetWeaponDefinition(game::ToolType::Rifle).defaultZoomMagnification;
    localWeaponCycle_ = 0.0f;
    ClearLocalCommandState();
    steam_.CreateLobby(config.runtime.sessionName, "SELF HOSTED WORLD", config.runtime.maxPlayers);
    statusText_ = "CREATING LISTEN LOBBY";
}

void Application::JoinBrowserEntry(const std::size_t index)
{
    const auto& entries = steam_.BrowserEntries();
    if (index >= entries.size())
    {
        return;
    }

    const net::SessionBrowserEntry& entry = entries[index];
    if (!entry.joinable)
    {
        statusText_ = "JOIN FAILED";
        return;
    }

    if (entry.type == net::BrowserEntryType::ListenHost)
    {
        QueueListenJoin(entry.lobbyId, entry.ownerSteamId, entry.name, "JOINING LISTEN LOBBY");
        return;
    }

    ResetActiveSessions();
    steam_.LeaveLobby();
    auto transport = std::make_unique<steam::SteamSocketsTransport>(SteamNetworkingSockets(), false);
    if (!transport->ConnectIp(entry.address, entry.port))
    {
        statusText_ = "JOIN FAILED";
        return;
    }

    (void)StartClientSessionWithTransport(std::move(transport), "CONNECTING");
}

void Application::DisconnectToMenu(const std::string_view statusText)
{
    ResetActiveSessions();
    sessionMenuOpen_ = false;
    sessionMenuSelection_ = 0;
    sessionMenuDragItem_ = -1;
    steam_.LeaveLobby();
    browserReturnState_.reset();
    platform_->SetRelativeMouseMode(false);
    renderOptions_.zoomMagnification = game::GetWeaponDefinition(game::ToolType::Rifle).defaultZoomMagnification;
    localWeaponCycle_ = 0.0f;
    ClearLocalCommandState();
    screenState_ = ScreenState::MainMenu;
    statusText_ = std::string(statusText);
}

void Application::ApplyPendingWorldSettings()
{
    if (!hostSessionActive_ || screenState_ != ScreenState::OfflineSession)
    {
        return;
    }

    hostSession_.MutableRuntime().SetGenerationSettings(pendingWorldSettings_);
    pendingWorldSettings_ = hostSession_.MutableRuntime().World().GenerationSettings();
    sessionMenuOpen_ = false;
    sessionMenuSelection_ = 0;
    sessionMenuDragItem_ = -1;
    statusText_ = "WORLD REBUILT";
}

void Application::BuildOverlay(render::FrameRenderData& renderData, const platform::InputState&, bool&)
{
    const auto [width, height] = platform_->DrawableSize();
    const float screenWidth = static_cast<float>(width);
    const float screenHeight = static_cast<float>(height);

    game::AppendRect(renderData.overlayTriangles, 16.0f, 16.0f, 540.0f, 210.0f, MakeColor(0.05f, 0.06f, 0.08f, 0.72f), screenWidth, screenHeight);
    game::AppendText(renderData.overlayTriangles, 28.0f, 28.0f, 2.0f, std::string("FPS ") + FormatFloat(smoothedFps_), MakeColor(0.96f, 0.98f, 1.0f, 1.0f), screenWidth, screenHeight);
    game::AppendText(renderData.overlayTriangles, 28.0f, 46.0f, 2.0f, std::string("FRAME ") + FormatFloat(frameDeltaSeconds_ * 1000.0f) + " MS", MakeColor(0.83f, 0.90f, 0.97f, 1.0f), screenWidth, screenHeight);
    game::AppendText(renderData.overlayTriangles, 28.0f, 64.0f, 2.0f, std::string("PLAYER ") + playerName_, MakeColor(0.92f, 0.95f, 0.99f, 1.0f), screenWidth, screenHeight);
    game::AppendText(renderData.overlayTriangles, 28.0f, 82.0f, 2.0f, statusText_, MakeColor(1.0f, 0.88f, 0.44f, 1.0f), screenWidth, screenHeight);

    const std::vector<net::PeerInfo> peers =
        hostSessionActive_ ? hostSession_.PeerInfos() :
        (clientSessionActive_ ? clientSession_.PeerInfos() : std::vector<net::PeerInfo>{});
    if (!peers.empty())
    {
        const net::PeerInfo& peer = peers.front();
        game::AppendText(
            renderData.overlayTriangles,
            28.0f,
            100.0f,
            2.0f,
            std::string("NET RTT ") + std::to_string(peer.stats.pingMilliseconds) + " MS  LOSS " + FormatFloat(peer.stats.lossPercent, 1) + "%",
            MakeColor(0.74f, 0.92f, 1.0f, 1.0f),
            screenWidth,
            screenHeight);
    }
    const game::WeaponDefinition selectedWeapon = game::GetWeaponDefinition(selectedTool_);
    if ((screenState_ == ScreenState::OfflineSession || screenState_ == ScreenState::ListenSession || screenState_ == ScreenState::ClientSession) &&
        selectedWeapon.usesScope)
    {
        game::AppendText(
            renderData.overlayTriangles,
            28.0f,
            118.0f,
            2.0f,
            std::string(renderOptions_.aimDownSights ? "ADS " : "SCOPE ") + FormatFloat(renderOptions_.zoomMagnification, 1) + "X",
            renderOptions_.aimDownSights ? MakeColor(0.98f, 0.95f, 0.66f, 1.0f) : MakeColor(0.80f, 0.86f, 0.92f, 1.0f),
            screenWidth,
            screenHeight);
    }
    if (screenState_ == ScreenState::OfflineSession || screenState_ == ScreenState::ListenSession || screenState_ == ScreenState::ClientSession)
    {
        game::ToolType hudTool = selectedTool_;
        int ammoInMagazine = -1;
        int reserveAmmo = -1;
        bool reloading = false;
        float reloadRemaining = 0.0f;
        float reloadTotal = 0.0f;

        if (hostSessionActive_)
        {
            if (const game::SessionRuntime::PlayerState* const player = hostSession_.Runtime().FindPlayer(localPlayerId_))
            {
                hudTool = player->tool;
                const auto& inventory = player->weaponInventories[game::ToToolIndex(player->tool)];
                ammoInMagazine = inventory.ammoInMagazine;
                reserveAmmo = inventory.reserveAmmo;
                reloading = player->reloading;
                reloadRemaining = player->reloadTimer;
                reloadTotal = player->reloadDuration;
            }
        }
        else if (clientSessionActive_)
        {
            const net::ActorSnapshotFrame hudFrame =
                clientSession_.BuildRenderActorFrame(static_cast<float>(fixedStepClock_.InterpolationAlpha()));
            if (const net::ActorSnapshot* const actor = FindActorSnapshot(hudFrame, localPlayerId_))
            {
                hudTool = actor->tool;
                ammoInMagazine = actor->ammoInMagazine;
                reserveAmmo = actor->reserveAmmo;
                reloading = actor->reloading;
                reloadRemaining = actor->reloadSecondsRemaining;
                reloadTotal = actor->reloadSecondsTotal;
            }
        }

        const game::WeaponDefinition hudWeapon = game::GetWeaponDefinition(hudTool);
        game::AppendText(
            renderData.overlayTriangles,
            28.0f,
            136.0f,
            2.0f,
            std::string("TOOL ") + std::string(hudWeapon.name) + "  DRAW " + FormatFloat(renderOptions_.terrainDrawDistanceMeters, 0) + "M",
            MakeColor(0.84f, 0.90f, 0.96f, 1.0f),
            screenWidth,
            screenHeight);

        if (hudWeapon.usesMagazine && ammoInMagazine >= 0)
        {
            const float panelWidth = 250.0f;
            const float panelHeight = reloading ? 94.0f : 74.0f;
            const float panelX = screenWidth - panelWidth - 22.0f;
            const float panelY = screenHeight - panelHeight - 22.0f;
            game::AppendRect(renderData.overlayTriangles, panelX, panelY, panelWidth, panelHeight, MakeColor(0.04f, 0.05f, 0.07f, 0.80f), screenWidth, screenHeight);
            game::AppendText(
                renderData.overlayTriangles,
                panelX + 16.0f,
                panelY + 14.0f,
                1.9f,
                std::string(hudWeapon.name),
                MakeColor(0.94f, 0.97f, 1.0f, 1.0f),
                screenWidth,
                screenHeight);
            game::AppendText(
                renderData.overlayTriangles,
                panelX + 16.0f,
                panelY + 36.0f,
                2.2f,
                std::string("AMMO ") + std::to_string(ammoInMagazine) + " / " + std::to_string(reserveAmmo),
                ammoInMagazine > 0 ? MakeColor(1.0f, 0.95f, 0.72f, 1.0f) : MakeColor(0.98f, 0.42f, 0.32f, 1.0f),
                screenWidth,
                screenHeight);
            game::AppendText(
                renderData.overlayTriangles,
                panelX + 16.0f,
                panelY + 56.0f,
                1.7f,
                reloading ? "RELOADING" : "R RELOAD",
                MakeColor(0.78f, 0.85f, 0.92f, 1.0f),
                screenWidth,
                screenHeight);
            if (reloading && reloadTotal > 0.0f)
            {
                const float progress = Clamp(1.0f - reloadRemaining / reloadTotal, 0.0f, 1.0f);
                game::AppendRect(renderData.overlayTriangles, panelX + 16.0f, panelY + 72.0f, panelWidth - 32.0f, 8.0f, MakeColor(0.16f, 0.19f, 0.24f, 0.95f), screenWidth, screenHeight);
                game::AppendRect(renderData.overlayTriangles, panelX + 16.0f, panelY + 72.0f, (panelWidth - 32.0f) * progress, 8.0f, MakeColor(0.78f, 0.82f, 0.32f, 0.95f), screenWidth, screenHeight);
            }
        }
    }

    if (screenState_ == ScreenState::MainMenu)
    {
        BuildMainMenu(renderData.overlayTriangles);
    }
    else if (screenState_ == ScreenState::HostSetup)
    {
        BuildHostSetupMenu(renderData.overlayTriangles);
    }
    else if (screenState_ == ScreenState::Browser)
    {
        BuildBrowserMenu(renderData.overlayTriangles);
    }
    else if (sessionMenuOpen_)
    {
        BuildSessionOverlay(renderData.overlayTriangles);
    }
    else if (screenState_ == ScreenState::OfflineSession || screenState_ == ScreenState::ListenSession || screenState_ == ScreenState::ClientSession)
    {
        if (renderData.scopedView.enabled)
        {
            AppendScopeOverlay(renderData.overlayTriangles, renderData.scopedView, screenWidth, screenHeight);
        }
        else if (selectedTool_ == game::ToolType::Smg && renderOptions_.aimDownSights)
        {
            AppendHolosightReticle(renderData.overlayTriangles, screenWidth, screenHeight);
        }
        else
        {
            game::AppendCrosshair(renderData.overlayTriangles, screenWidth, screenHeight, MakeColor(0.98f, 0.98f, 0.99f, 1.0f));
        }
    }
}

void Application::BuildMainMenu(std::vector<render::ColorVertex2D>& overlayTriangles) const
{
    const auto [width, height] = platform_->DrawableSize();
    const float screenWidth = static_cast<float>(width);
    const float screenHeight = static_cast<float>(height);
    const float panelX = screenWidth * 0.5f - 280.0f;
    const float panelY = screenHeight * 0.5f - 120.0f;

    game::AppendRect(overlayTriangles, panelX, panelY, 560.0f, 260.0f, MakeColor(0.08f, 0.09f, 0.12f, 0.92f), screenWidth, screenHeight);
    game::AppendText(overlayTriangles, panelX + 24.0f, panelY + 24.0f, 2.4f, "DEFORM FRONTIER", MakeColor(0.98f, 0.98f, 1.0f, 1.0f), screenWidth, screenHeight);

    const std::array<std::string, 4> items = {
        "CONTINUE OFFLINE",
        "HOST SELF HOSTED WORLD",
        "JOIN WORLD",
        "QUIT",
    };
    float rowY = panelY + 78.0f;
    for (int index = 0; index < static_cast<int>(items.size()); ++index)
    {
        if (index == mainMenuSelection_)
        {
            game::AppendRect(overlayTriangles, panelX + 18.0f, rowY - 4.0f, 524.0f, 24.0f, MakeColor(0.20f, 0.28f, 0.36f, 0.90f), screenWidth, screenHeight);
        }
        game::AppendText(
            overlayTriangles,
            panelX + 30.0f,
            rowY,
            2.0f,
            items[static_cast<std::size_t>(index)],
            index == mainMenuSelection_ ? MakeColor(1.0f, 0.95f, 0.66f, 1.0f) : MakeColor(0.92f, 0.95f, 0.99f, 1.0f),
            screenWidth,
            screenHeight);
        rowY += 28.0f;
    }
}

void Application::BuildHostSetupMenu(std::vector<render::ColorVertex2D>& overlayTriangles) const
{
    const auto [width, height] = platform_->DrawableSize();
    const float screenWidth = static_cast<float>(width);
    const float screenHeight = static_cast<float>(height);
    const float panelX = screenWidth * 0.5f - 320.0f;
    const float panelY = screenHeight * 0.5f - 170.0f;

    game::AppendRect(overlayTriangles, panelX, panelY, 640.0f, 340.0f, MakeColor(0.08f, 0.09f, 0.12f, 0.92f), screenWidth, screenHeight);
    game::AppendText(overlayTriangles, panelX + 24.0f, panelY + 24.0f, 2.2f, "HOST WORLD SETUP", MakeColor(0.98f, 0.98f, 1.0f, 1.0f), screenWidth, screenHeight);

    const std::array<std::string, 9> rows = {
        std::string("WORLD WIDTH ") + std::to_string(pendingWorldSettings_.worldWidth),
        std::string("WORLD HEIGHT ") + std::to_string(pendingWorldSettings_.worldHeight),
        std::string("WORLD DEPTH ") + std::to_string(pendingWorldSettings_.worldDepth),
        std::string("ACTIVE CHUNK ") + std::to_string(pendingWorldSettings_.activeChunkSize),
        std::string("CELL SCALE ") + FormatFloat(pendingWorldSettings_.cellSize, 2),
        std::string("SEED ") + std::to_string(pendingWorldSettings_.seed),
        std::string("RELIEF ") + FormatFloat(pendingWorldSettings_.terrainRelief, 2),
        std::string("WATER LEVEL ") + FormatFloat(pendingWorldSettings_.waterLevel, 2),
        "START HOSTING",
    };

    float rowY = panelY + 72.0f;
    for (int index = 0; index < static_cast<int>(rows.size()); ++index)
    {
        if (index == hostSetupSelection_)
        {
            game::AppendRect(overlayTriangles, panelX + 18.0f, rowY - 4.0f, 604.0f, 24.0f, MakeColor(0.20f, 0.28f, 0.36f, 0.90f), screenWidth, screenHeight);
        }
        game::AppendText(
            overlayTriangles,
            panelX + 30.0f,
            rowY,
            2.0f,
            rows[static_cast<std::size_t>(index)],
            index == hostSetupSelection_ ? MakeColor(1.0f, 0.95f, 0.66f, 1.0f) : MakeColor(0.92f, 0.95f, 0.99f, 1.0f),
            screenWidth,
            screenHeight);
        rowY += 28.0f;
    }
}

void Application::BuildBrowserMenu(std::vector<render::ColorVertex2D>& overlayTriangles) const
{
    const auto [width, height] = platform_->DrawableSize();
    const float screenWidth = static_cast<float>(width);
    const float screenHeight = static_cast<float>(height);
    const float panelX = screenWidth * 0.5f - 360.0f;
    const float panelY = screenHeight * 0.5f - 180.0f;

    game::AppendRect(overlayTriangles, panelX, panelY, 720.0f, 360.0f, MakeColor(0.08f, 0.09f, 0.12f, 0.92f), screenWidth, screenHeight);
    game::AppendText(overlayTriangles, panelX + 24.0f, panelY + 24.0f, 2.2f, "WORLD BROWSER", MakeColor(0.98f, 0.98f, 1.0f, 1.0f), screenWidth, screenHeight);

    float rowY = panelY + 68.0f;
    const auto& entries = steam_.BrowserEntries();
    if (entries.empty())
    {
        game::AppendText(overlayTriangles, panelX + 30.0f, rowY, 1.8f, "NO WORLDS FOUND. PRESS R TO REFRESH OR ESC TO GO BACK.", MakeColor(0.84f, 0.88f, 0.93f, 1.0f), screenWidth, screenHeight);
        return;
    }

    for (std::size_t index = 0; index < entries.size() && index < 9u; ++index)
    {
        if (static_cast<int>(index) == browserSelection_)
        {
            game::AppendRect(overlayTriangles, panelX + 18.0f, rowY - 4.0f, 684.0f, 28.0f, MakeColor(0.20f, 0.28f, 0.36f, 0.90f), screenWidth, screenHeight);
        }
        const std::string label =
            (entries[index].dedicated ? "[DEDICATED] " : "[LISTEN] ") +
            entries[index].name +
            "  " + std::to_string(entries[index].currentPlayers) + "/" + std::to_string(entries[index].maxPlayers);
        game::AppendText(
            overlayTriangles,
            panelX + 30.0f,
            rowY,
            1.8f,
            label,
            static_cast<int>(index) == browserSelection_ ? MakeColor(1.0f, 0.95f, 0.66f, 1.0f) : MakeColor(0.92f, 0.95f, 0.99f, 1.0f),
            screenWidth,
            screenHeight);
        rowY += 32.0f;
    }
}

void Application::BuildSessionOverlay(std::vector<render::ColorVertex2D>& overlayTriangles) const
{
    const auto [width, height] = platform_->DrawableSize();
    const float screenWidth = static_cast<float>(width);
    const float screenHeight = static_cast<float>(height);

    game::AppendRect(overlayTriangles, 0.0f, 0.0f, screenWidth, screenHeight, MakeColor(0.01f, 0.02f, 0.03f, 0.45f), screenWidth, screenHeight);

    if (screenState_ == ScreenState::OfflineSession)
    {
        const float panelWidth = 660.0f;
        const float panelHeight = 380.0f;
        const float panelX = screenWidth * 0.5f - panelWidth * 0.5f;
        const float panelY = screenHeight * 0.5f - panelHeight * 0.5f;

        game::AppendRect(overlayTriangles, panelX, panelY, panelWidth, panelHeight, MakeColor(0.08f, 0.09f, 0.12f, 0.92f), screenWidth, screenHeight);
        game::AppendRect(overlayTriangles, panelX + 14.0f, panelY + 14.0f, panelWidth - 28.0f, 30.0f, MakeColor(0.12f, 0.15f, 0.20f, 0.95f), screenWidth, screenHeight);
        game::AppendText(overlayTriangles, panelX + 28.0f, panelY + 24.0f, 2.0f, "PAUSE MENU", MakeColor(0.98f, 0.98f, 1.0f, 1.0f), screenWidth, screenHeight);

        const std::array<std::string, 12> rows = {
            "RESUME",
            std::string("WORLD WIDTH ") + std::to_string(pendingWorldSettings_.worldWidth) + " CELLS",
            std::string("WORLD HEIGHT ") + std::to_string(pendingWorldSettings_.worldHeight) + " CELLS",
            std::string("WORLD DEPTH ") + std::to_string(pendingWorldSettings_.worldDepth) + " CELLS",
            std::string("ACTIVE CHUNK ") + std::to_string(pendingWorldSettings_.activeChunkSize) + " CELLS",
            std::string("CELL SCALE ") + FormatFloat(pendingWorldSettings_.cellSize, 2) + " M",
            std::string("SEED ") + std::to_string(pendingWorldSettings_.seed),
            std::string("RELIEF ") + FormatFloat(pendingWorldSettings_.terrainRelief, 2),
            std::string("WATER LEVEL ") + FormatFloat(pendingWorldSettings_.waterLevel, 2),
            std::string("DRAW DIST ") + FormatFloat(renderOptions_.terrainDrawDistanceMeters, 0) + " M",
            std::string("TARGET FPS ") + std::to_string(targetFrameRate_),
            "APPLY REBUILD",
        };

        float rowY = panelY + 58.0f;
        for (int index = 0; index < static_cast<int>(rows.size()); ++index)
        {
            if (index == sessionMenuSelection_)
            {
                game::AppendRect(overlayTriangles, panelX + 18.0f, rowY - 4.0f, panelWidth - 36.0f, 24.0f, MakeColor(0.20f, 0.28f, 0.36f, 0.90f), screenWidth, screenHeight);
            }
            game::AppendText(
                overlayTriangles,
                panelX + 30.0f,
                rowY,
                2.0f,
                rows[static_cast<std::size_t>(index)],
                index == sessionMenuSelection_ ? MakeColor(1.0f, 0.95f, 0.66f, 1.0f) : MakeColor(0.92f, 0.95f, 0.99f, 1.0f),
                screenWidth,
                screenHeight);
            rowY += 24.0f;
        }

        game::AppendText(
            overlayTriangles,
            panelX + 28.0f,
            panelY + panelHeight - 44.0f,
            1.7f,
            "UP DOWN SELECT  LEFT RIGHT ADJUST  SHIFT COARSE  DRAG LMB TO TUNE",
            MakeColor(0.84f, 0.88f, 0.93f, 1.0f),
            screenWidth,
            screenHeight);
        game::AppendText(
            overlayTriangles,
            panelX + 28.0f,
            panelY + panelHeight - 24.0f,
            1.7f,
            "CLICK RESUME/APPLY  ENTER APPLY  ESC CLOSE",
            MakeColor(0.84f, 0.88f, 0.93f, 1.0f),
            screenWidth,
            screenHeight);
        return;
    }

    const float panelX = screenWidth * 0.5f - 260.0f;
    const float panelY = screenHeight * 0.5f - 126.0f;
    game::AppendRect(overlayTriangles, panelX, panelY, 520.0f, 252.0f, MakeColor(0.08f, 0.09f, 0.12f, 0.92f), screenWidth, screenHeight);
    game::AppendText(overlayTriangles, panelX + 24.0f, panelY + 24.0f, 2.2f, "SESSION MENU", MakeColor(0.98f, 0.98f, 1.0f, 1.0f), screenWidth, screenHeight);

    const std::array<std::string, 6> rows = {
        "RESUME",
        std::string("DRAW DIST ") + FormatFloat(renderOptions_.terrainDrawDistanceMeters, 0) + " M",
        "INVITE OR BROWSE",
        "SAVE WORLD",
        "RESTART WORLD",
        "DISCONNECT",
    };
    float rowY = panelY + 72.0f;
    for (int index = 0; index < static_cast<int>(rows.size()); ++index)
    {
        if (index == sessionMenuSelection_)
        {
            game::AppendRect(overlayTriangles, panelX + 18.0f, rowY - 4.0f, 484.0f, 24.0f, MakeColor(0.20f, 0.28f, 0.36f, 0.90f), screenWidth, screenHeight);
        }
        game::AppendText(
            overlayTriangles,
            panelX + 30.0f,
            rowY,
            2.0f,
            rows[static_cast<std::size_t>(index)],
            index == sessionMenuSelection_ ? MakeColor(1.0f, 0.95f, 0.66f, 1.0f) : MakeColor(0.92f, 0.95f, 0.99f, 1.0f),
            screenWidth,
            screenHeight);
        rowY += 28.0f;
    }

    game::AppendText(
        overlayTriangles,
        panelX + 24.0f,
        panelY + 220.0f,
        1.6f,
        "LEFT RIGHT OR DRAG DRAW DISTANCE",
        MakeColor(0.82f, 0.87f, 0.93f, 1.0f),
        screenWidth,
        screenHeight);
}

void Application::HandleMainMenuInput(const platform::InputState& input, bool& running)
{
    const auto [width, height] = platform_->DrawableSize();
    const float panelX = static_cast<float>(width) * 0.5f - 280.0f;
    const float panelY = static_cast<float>(height) * 0.5f - 120.0f;
    const int hoveredRow = HoveredMenuRow(input, panelX, panelY + 78.0f, 524.0f, 28.0f, 4);
    if (hoveredRow >= 0)
    {
        mainMenuSelection_ = hoveredRow;
    }

    if (input.KeyPressed(SDL_SCANCODE_UP))
    {
        mainMenuSelection_ = (mainMenuSelection_ + 3) % 4;
    }
    if (input.KeyPressed(SDL_SCANCODE_DOWN))
    {
        mainMenuSelection_ = (mainMenuSelection_ + 1) % 4;
    }

    const bool activate = input.KeyPressed(SDL_SCANCODE_RETURN) || (hoveredRow >= 0 && input.MousePressed(SDL_BUTTON_LEFT));
    if (!activate)
    {
        return;
    }

    switch (mainMenuSelection_)
    {
    case 0:
        StartOfflineSession();
        break;
    case 1:
        screenState_ = ScreenState::HostSetup;
        statusText_ = "CONFIGURE HOST";
        break;
    case 2:
        steam_.RefreshBrowser();
        browserReturnState_.reset();
        screenState_ = ScreenState::Browser;
        statusText_ = "REFRESHING BROWSER";
        break;
    case 3:
        running = false;
        break;
    default:
        break;
    }
}

void Application::HandleHostSetupInput(const platform::InputState& input)
{
    if (input.KeyPressed(SDL_SCANCODE_ESCAPE))
    {
        screenState_ = ScreenState::MainMenu;
        return;
    }

    const auto [width, height] = platform_->DrawableSize();
    const float panelX = static_cast<float>(width) * 0.5f - 320.0f;
    const float panelY = static_cast<float>(height) * 0.5f - 170.0f;
    const int hoveredRow = HoveredMenuRow(input, panelX, panelY + 72.0f, 604.0f, 28.0f, 9);
    if (hoveredRow >= 0)
    {
        hostSetupSelection_ = hoveredRow;
    }

    if (input.KeyPressed(SDL_SCANCODE_UP))
    {
        hostSetupSelection_ = (hostSetupSelection_ + 8) % 9;
    }
    if (input.KeyPressed(SDL_SCANCODE_DOWN))
    {
        hostSetupSelection_ = (hostSetupSelection_ + 1) % 9;
    }

    const auto adjust = [&](const float scalar)
    {
        switch (hostSetupSelection_)
        {
        case 0: pendingWorldSettings_.worldWidth += static_cast<int>(scalar) * 4; break;
        case 1: pendingWorldSettings_.worldHeight += static_cast<int>(scalar) * 2; break;
        case 2: pendingWorldSettings_.worldDepth += static_cast<int>(scalar) * 4; break;
        case 3: pendingWorldSettings_.activeChunkSize += static_cast<int>(scalar); break;
        case 4: pendingWorldSettings_.cellSize += scalar * 0.01f; break;
        case 5:
            if (scalar > 0.0f) { pendingWorldSettings_.seed += static_cast<std::uint32_t>(scalar); }
            else { pendingWorldSettings_.seed = pendingWorldSettings_.seed > 0u ? pendingWorldSettings_.seed - 1u : 0u; }
            break;
        case 6: pendingWorldSettings_.terrainRelief += scalar * 0.02f; break;
        case 7: pendingWorldSettings_.waterLevel += scalar * 0.01f; break;
        default: break;
        }
        pendingWorldSettings_ = world::DemoWorld::ClampGenerationSettings(pendingWorldSettings_);
    };

    if (input.KeyPressed(SDL_SCANCODE_LEFT))
    {
        adjust(-1.0f);
    }
    if (input.KeyPressed(SDL_SCANCODE_RIGHT))
    {
        adjust(1.0f);
    }

    if ((input.KeyPressed(SDL_SCANCODE_RETURN) || (hoveredRow >= 0 && input.MousePressed(SDL_BUTTON_LEFT))) && hostSetupSelection_ == 8)
    {
        StartListenSession();
    }
}

void Application::HandleBrowserInput(const platform::InputState& input)
{
    if (input.KeyPressed(SDL_SCANCODE_ESCAPE))
    {
        screenState_ = browserReturnState_.value_or(ScreenState::MainMenu);
        browserReturnState_.reset();
        return;
    }
    if (input.KeyPressed(SDL_SCANCODE_R))
    {
        steam_.RefreshBrowser();
    }

    const int browserSize = static_cast<int>(steam_.BrowserEntries().size());
    if (browserSize > 0)
    {
        const auto [width, height] = platform_->DrawableSize();
        const float panelX = static_cast<float>(width) * 0.5f - 360.0f;
        const float panelY = static_cast<float>(height) * 0.5f - 180.0f;
        const int hoveredRow = HoveredMenuRow(input, panelX, panelY + 68.0f, 684.0f, 32.0f, std::min(browserSize, 9));
        if (hoveredRow >= 0)
        {
            browserSelection_ = hoveredRow;
            if (input.MousePressed(SDL_BUTTON_LEFT))
            {
                JoinBrowserEntry(static_cast<std::size_t>(browserSelection_));
                return;
            }
        }
    }

    if (browserSize > 0)
    {
        if (input.KeyPressed(SDL_SCANCODE_UP))
        {
            browserSelection_ = (browserSelection_ + browserSize - 1) % browserSize;
        }
        if (input.KeyPressed(SDL_SCANCODE_DOWN))
        {
            browserSelection_ = (browserSelection_ + 1) % browserSize;
        }
        if (input.KeyPressed(SDL_SCANCODE_RETURN))
        {
            JoinBrowserEntry(static_cast<std::size_t>(browserSelection_));
        }
    }
}

void Application::HandleSessionOverlayInput(const platform::InputState& input)
{
    if (screenState_ == ScreenState::OfflineSession)
    {
        const auto applySelectionDelta = [&](const OfflinePauseItem item, const float scalar, const bool coarseAdjust)
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
            const float cellScaleStep = coarseAdjust ? 0.10f : 0.01f;
            const std::uint32_t seedStep = coarseAdjust ? 100u : 1u;
            const float reliefStep = coarseAdjust ? 0.10f : 0.02f;
            const float waterStep = coarseAdjust ? 0.05f : 0.01f;
            const int renderDistanceStep = coarseAdjust ? 250 : 25;
            const int fpsStep = coarseAdjust ? 10 : 2;

            switch (item)
            {
            case OfflinePauseItem::WorldWidth:
                pendingWorldSettings_.worldWidth += quantized(worldHorizontalStep);
                break;
            case OfflinePauseItem::WorldHeight:
                pendingWorldSettings_.worldHeight += quantized(worldVerticalStep);
                break;
            case OfflinePauseItem::WorldDepth:
                pendingWorldSettings_.worldDepth += quantized(worldHorizontalStep);
                break;
            case OfflinePauseItem::ActiveChunkSize:
                pendingWorldSettings_.activeChunkSize += quantized(chunkStep);
                break;
            case OfflinePauseItem::CellScale:
                pendingWorldSettings_.cellSize += scalar * cellScaleStep;
                break;
            case OfflinePauseItem::Seed:
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
            case OfflinePauseItem::Relief:
                pendingWorldSettings_.terrainRelief += scalar * reliefStep;
                break;
            case OfflinePauseItem::WaterLevel:
                pendingWorldSettings_.waterLevel += scalar * waterStep;
                break;
            case OfflinePauseItem::RenderDistance:
                renderOptions_.terrainDrawDistanceMeters = ClampRenderDistanceMeters(renderOptions_.terrainDrawDistanceMeters + static_cast<float>(quantized(renderDistanceStep)));
                break;
            case OfflinePauseItem::TargetFps:
                targetFrameRate_ = std::clamp(targetFrameRate_ + quantized(fpsStep), 30, 240);
                break;
            default:
                break;
            }

            pendingWorldSettings_ = world::DemoWorld::ClampGenerationSettings(pendingWorldSettings_);
        };

        if (input.KeyPressed(SDL_SCANCODE_UP))
        {
            sessionMenuSelection_ = (sessionMenuSelection_ + OfflinePauseItemCount() - 1) % OfflinePauseItemCount();
        }
        if (input.KeyPressed(SDL_SCANCODE_DOWN))
        {
            sessionMenuSelection_ = (sessionMenuSelection_ + 1) % OfflinePauseItemCount();
        }

        const bool coarseAdjust = input.KeyDown(SDL_SCANCODE_LSHIFT) || input.KeyDown(SDL_SCANCODE_RSHIFT);
        if (input.KeyPressed(SDL_SCANCODE_LEFT) || input.KeyPressed(SDL_SCANCODE_RIGHT))
        {
            applySelectionDelta(static_cast<OfflinePauseItem>(sessionMenuSelection_), input.KeyPressed(SDL_SCANCODE_RIGHT) ? 1.0f : -1.0f, coarseAdjust);
        }

        const auto [width, height] = platform_->DrawableSize();
        const float panelX = static_cast<float>(width) * 0.5f - 330.0f;
        const float panelY = static_cast<float>(height) * 0.5f - 190.0f;
        const int hoveredRow = HoveredMenuRow(input, panelX, panelY + 58.0f, 624.0f, 24.0f, OfflinePauseItemCount());
        if (hoveredRow >= 0)
        {
            sessionMenuSelection_ = hoveredRow;
        }

        if (input.MousePressed(SDL_BUTTON_LEFT))
        {
            sessionMenuDragItem_ = -1;
            if (hoveredRow >= 0)
            {
                sessionMenuSelection_ = hoveredRow;
                const OfflinePauseItem item = static_cast<OfflinePauseItem>(hoveredRow);
                if (item == OfflinePauseItem::Resume)
                {
                    sessionMenuOpen_ = false;
                    return;
                }
                if (item == OfflinePauseItem::ApplyAndRebuild)
                {
                    ApplyPendingWorldSettings();
                    return;
                }
                sessionMenuDragItem_ = hoveredRow;
            }
        }

        if (!input.MouseDown(SDL_BUTTON_LEFT))
        {
            sessionMenuDragItem_ = -1;
        }
        else if (sessionMenuDragItem_ >= 0 && std::abs(input.mouseDeltaX) > 0.0f)
        {
            applySelectionDelta(static_cast<OfflinePauseItem>(sessionMenuDragItem_), input.mouseDeltaX * 0.12f, coarseAdjust);
        }

        if (!input.KeyPressed(SDL_SCANCODE_RETURN))
        {
            return;
        }

        switch (static_cast<OfflinePauseItem>(sessionMenuSelection_))
        {
        case OfflinePauseItem::Resume:
            sessionMenuOpen_ = false;
            break;
        case OfflinePauseItem::ApplyAndRebuild:
            ApplyPendingWorldSettings();
            break;
        default:
            break;
        }
        return;
    }

    const auto [width, height] = platform_->DrawableSize();
    const float panelX = static_cast<float>(width) * 0.5f - 260.0f;
    const float panelY = static_cast<float>(height) * 0.5f - 126.0f;
    const int hoveredRow = HoveredMenuRow(input, panelX, panelY + 72.0f, 484.0f, 28.0f, 6);
    if (hoveredRow >= 0)
    {
        sessionMenuSelection_ = hoveredRow;
    }

    const bool coarseAdjust = input.KeyDown(SDL_SCANCODE_LSHIFT) || input.KeyDown(SDL_SCANCODE_RSHIFT);
    const auto adjustRenderDistance = [&](const float scalar)
    {
        if (std::abs(scalar) <= 1.0e-4f)
        {
            return;
        }

        const int direction = scalar >= 0.0f ? 1 : -1;
        const int magnitude = std::max(1, static_cast<int>(std::round(std::abs(scalar))));
        const int step = coarseAdjust ? 250 : 25;
        renderOptions_.terrainDrawDistanceMeters = ClampRenderDistanceMeters(
            renderOptions_.terrainDrawDistanceMeters + static_cast<float>(direction * magnitude * step));
    };

    if (input.KeyPressed(SDL_SCANCODE_UP))
    {
        sessionMenuSelection_ = (sessionMenuSelection_ + 5) % 6;
    }
    if (input.KeyPressed(SDL_SCANCODE_DOWN))
    {
        sessionMenuSelection_ = (sessionMenuSelection_ + 1) % 6;
    }
    if ((input.KeyPressed(SDL_SCANCODE_LEFT) || input.KeyPressed(SDL_SCANCODE_RIGHT)) &&
        static_cast<SessionMenuItem>(sessionMenuSelection_) == SessionMenuItem::RenderDistance)
    {
        adjustRenderDistance(input.KeyPressed(SDL_SCANCODE_RIGHT) ? 1.0f : -1.0f);
    }

    if (input.MousePressed(SDL_BUTTON_LEFT))
    {
        sessionMenuDragItem_ = -1;
        if (hoveredRow >= 0)
        {
            sessionMenuSelection_ = hoveredRow;
            if (static_cast<SessionMenuItem>(hoveredRow) == SessionMenuItem::RenderDistance)
            {
                sessionMenuDragItem_ = hoveredRow;
                return;
            }
        }
    }

    if (!input.MouseDown(SDL_BUTTON_LEFT))
    {
        sessionMenuDragItem_ = -1;
    }
    else if (sessionMenuDragItem_ == static_cast<int>(SessionMenuItem::RenderDistance) && std::abs(input.mouseDeltaX) > 0.0f)
    {
        adjustRenderDistance(input.mouseDeltaX * 0.12f);
    }

    if (!input.KeyPressed(SDL_SCANCODE_RETURN) && !(hoveredRow >= 0 && input.MousePressed(SDL_BUTTON_LEFT)))
    {
        return;
    }

    switch (static_cast<SessionMenuItem>(sessionMenuSelection_))
    {
    case SessionMenuItem::Resume:
        sessionMenuOpen_ = false;
        break;
    case SessionMenuItem::RenderDistance:
        break;
    case SessionMenuItem::InviteOrBrowse:
        sessionMenuOpen_ = false;
        if (hostSessionActive_ && screenState_ == ScreenState::ListenSession && steam_.OpenInviteOverlay())
        {
            statusText_ = "INVITE OVERLAY";
        }
        else
        {
            browserReturnState_ = screenState_;
            steam_.RefreshBrowser();
            screenState_ = ScreenState::Browser;
            statusText_ = "REFRESHING BROWSER";
        }
        break;
    case SessionMenuItem::SaveWorld:
        if (hostSessionActive_)
        {
            statusText_ = hostSession_.MutableRuntime().SaveNow() ? "WORLD SAVED" : "SAVE FAILED";
        }
        break;
    case SessionMenuItem::RestartWorld:
        if (hostSessionActive_ && screenState_ != ScreenState::ClientSession)
        {
            hostSession_.MutableRuntime().RestartWorld();
            statusText_ = "WORLD RESTARTED";
        }
        break;
    case SessionMenuItem::Disconnect:
        DisconnectToMenu();
        break;
    default:
        break;
    }
}
}
