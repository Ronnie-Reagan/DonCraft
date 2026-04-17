#pragma once

#include "game/session_runtime.hpp"
#include "net/transport.hpp"

#include <memory>
#include <unordered_map>

namespace df::net
{
class SessionHost
{
public:
    struct Config
    {
        game::SessionRuntime::Config runtime;
        game::PlayerId localHostPlayerId = game::kInvalidPlayerId;
        std::string localHostPlayerName;
    };

    void Initialize(const Config& config, std::unique_ptr<ITransport> transport);
    void Shutdown();
    void Tick(float dt);

    [[nodiscard]] auto Runtime() const -> const game::SessionRuntime&
    {
        return runtime_;
    }

    [[nodiscard]] auto MutableRuntime() -> game::SessionRuntime&
    {
        return runtime_;
    }

    [[nodiscard]] auto ConsumeAudioCues() -> std::vector<game::SessionRuntime::AudioCue>
    {
        return runtime_.ConsumeAudioCues();
    }

    [[nodiscard]] auto PeerInfos() const -> std::vector<PeerInfo>
    {
        return transport_ != nullptr ? transport_->SnapshotPeers() : std::vector<PeerInfo>{};
    }

private:
    struct RemotePeerState
    {
        PeerId peerId = kInvalidPeerId;
        game::PlayerId playerId = game::kInvalidPlayerId;
        bool welcomed = false;
        world::DenseWorldSnapshot baselineWorld{};
    };

    [[nodiscard]] auto BuildActorFrame() const -> ActorSnapshotFrame;
    void SendWelcome(PeerId peerId, game::PlayerId playerId);
    void SendWorldSnapshot(PeerId peerId, const world::DenseWorldSnapshot& snapshot);
    void SendWorldDelta(PeerId peerId, RemotePeerState& peer, const world::DenseWorldSnapshot& current);
    void BroadcastActorFrame(const ActorSnapshotFrame& frame);
    void ProcessPeerEvents();
    void ProcessPackets();

    Config config_{};
    game::SessionRuntime runtime_{};
    std::unique_ptr<ITransport> transport_;
    std::unordered_map<PeerId, RemotePeerState> peers_;
    game::PlayerId nextRemotePlayerId_ = 2u;
};
}
