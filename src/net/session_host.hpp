#pragma once

#include "game/session_runtime.hpp"
#include "net/session_protocol.hpp"
#include "net/transport.hpp"

#include <deque>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

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

    [[nodiscard]] auto ConsumeAudioCues() -> std::vector<game::SessionRuntime::AudioCue>;

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
        float handshakeElapsedSeconds = 0.0f;
        world::DenseWorldSnapshot baselineWorld{};
        std::uint64_t baselineWorldVersion = 0;
        std::deque<std::vector<std::byte>> reliableQueue{};
        std::size_t reliableQueueBytes = 0u;
        bool reliableQueueBlocked = false;
    };

    void RefreshWorldSnapshotCache();
    [[nodiscard]] auto BuildActorFrame() const -> ActorSnapshotFrame;
    [[nodiscard]] bool QueueReliablePayload(PeerId peerId, RemotePeerState& peer, std::vector<std::byte> payload);
    void FlushReliableQueue(PeerId peerId, RemotePeerState& peer);
    void SendWelcome(PeerId peerId, RemotePeerState& peer, game::PlayerId playerId);
    void SendWorldSnapshot(PeerId peerId, RemotePeerState& peer, const world::DenseWorldSnapshot& snapshot);
    void SendWorldDelta(PeerId peerId, RemotePeerState& peer, const world::DenseWorldSnapshot& current);
    void SendInitialState(PeerId peerId, RemotePeerState& peer);
    void BroadcastActorFrame(const ActorSnapshotFrame& frame);
    void ProcessPeerEvents();
    void ProcessPackets();
    void UpdatePendingHandshakes(float dt);

    Config config_{};
    game::SessionRuntime runtime_{};
    std::unique_ptr<ITransport> transport_;
    std::unordered_map<PeerId, RemotePeerState> peers_;
    game::PlayerId nextRemotePlayerId_ = 2u;
    world::DenseWorldSnapshot worldSnapshotCache_{};
    std::uint64_t worldSnapshotVersion_ = 0;
    std::uint64_t cachedWorldDeltaBaselineVersion_ = 0;
    std::uint64_t cachedWorldDeltaTargetVersion_ = 0;
    std::vector<ChunkDelta> cachedWorldDeltas_{};
    std::vector<game::SessionRuntime::AudioCue> localAudioCues_{};
    std::vector<game::SessionRuntime::AudioCue> tickAudioCues_{};
};
}
