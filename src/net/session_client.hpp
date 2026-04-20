#pragma once

#include "game/player_controller.hpp"
#include "game/session_types.hpp"
#include "net/transport.hpp"
#include "world/demo_world.hpp"

#include <array>
#include <cstddef>
#include <memory>

namespace df::net
{
class SessionClient
{
public:
    struct Config
    {
        std::string playerName = "Frontier Player";
        float connectTimeoutSeconds = 8.0f;
        float handshakeTimeoutSeconds = 8.0f;
        float helloResendIntervalSeconds = 1.0f;
    };

    void Initialize(const Config& config, std::unique_ptr<ITransport> transport);
    void Shutdown();
    void Tick(float dt, const game::PlayerCommandFrame& localCommand);

    [[nodiscard]] bool IsReady() const
    {
        return localPlayerId_ != game::kInvalidPlayerId && worldReady_;
    }

    [[nodiscard]] auto SessionMode() const -> game::SessionMode
    {
        return sessionMode_;
    }

    [[nodiscard]] auto SessionName() const -> const std::string&
    {
        return sessionName_;
    }

    [[nodiscard]] auto LocalPlayerId() const -> game::PlayerId
    {
        return localPlayerId_;
    }

    [[nodiscard]] auto World() const -> const world::DemoWorld&
    {
        return world_;
    }

    [[nodiscard]] auto MutableWorld() -> world::DemoWorld&
    {
        return world_;
    }

    [[nodiscard]] auto ActorFrame() const -> const ActorSnapshotFrame&
    {
        return actorFrame_;
    }

    [[nodiscard]] auto BuildRenderActorFrame(float interpolationAlpha = 0.0f) const -> ActorSnapshotFrame;

    [[nodiscard]] auto PredictedLocalPlayer() const -> const game::PlayerController*
    {
        return predictedPlayerValid_ ? &predictedPlayer_ : nullptr;
    }

    [[nodiscard]] auto PeerInfos() const -> std::vector<PeerInfo>
    {
        return transport_ != nullptr ? transport_->SnapshotPeers() : std::vector<PeerInfo>{};
    }

    [[nodiscard]] bool HasServerConnection() const
    {
        return serverPeerId_ != kInvalidPeerId;
    }

    [[nodiscard]] bool Disconnected() const
    {
        return disconnected_;
    }

    [[nodiscard]] const std::string& LastDisconnectText() const
    {
        return lastDisconnectText_;
    }

private:
    struct PendingCommand
    {
        game::PlayerCommandFrame frame{};
        float dt = 1.0f / 60.0f;
        std::uint32_t sequence = 0;
        bool valid = false;
    };

    struct PendingWorldSnapshot
    {
        world::DenseWorldSnapshot snapshot{};
        std::uint64_t serverTick = 0;
        std::uint32_t nextCellOffset = 0;
        bool active = false;
    };

    static constexpr std::size_t kPendingCommandHistorySize = 64u;
    static constexpr std::size_t kSnapshotHistorySize = 4u;

    void ProcessPeerEvents();
    void ProcessPackets();
    void SendClientHello();
    void StorePendingCommand(const game::PlayerCommandFrame& command, float dt);
    void AdvancePredictedLocalPlayer(const game::ControlState& control, float dt);
    void ClearAcknowledgedPendingCommands(std::uint32_t lastAppliedSequence);
    void PushActorFrame(const ActorSnapshotFrame& frame);
    void RebuildPredictedLocalPlayer();
    void ResetSessionState();
    void FailConnection(std::string_view reason);
    [[nodiscard]] auto FindActor(game::PlayerId id) const -> const ActorSnapshot*;
    [[nodiscard]] auto FindActor(game::PlayerId id, const ActorSnapshotFrame& frame) const -> const ActorSnapshot*;

    Config config_{};
    std::unique_ptr<ITransport> transport_;
    PeerId serverPeerId_ = kInvalidPeerId;
    bool helloSent_ = false;
    bool worldReady_ = false;
    game::PlayerId localPlayerId_ = game::kInvalidPlayerId;
    game::SessionMode sessionMode_ = game::SessionMode::Client;
    std::string sessionName_;
    bool disconnected_ = false;
    std::string lastDisconnectText_;
    float connectElapsedSeconds_ = 0.0f;
    float handshakeElapsedSeconds_ = 0.0f;
    float helloResendSeconds_ = 0.0f;
    world::DemoWorld world_;
    world::DenseWorldSnapshot worldBaseline_{};
    PendingWorldSnapshot pendingWorldSnapshot_{};
    ActorSnapshotFrame actorFrame_{};
    std::array<ActorSnapshotFrame, kSnapshotHistorySize> actorHistory_{};
    std::size_t actorHistoryCount_ = 0;
    std::array<PendingCommand, kPendingCommandHistorySize> pendingCommands_{};
    game::PlayerController predictedPlayer_{};
    bool predictedPlayerValid_ = false;
};
}
