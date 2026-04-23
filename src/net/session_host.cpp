#include "net/session_host.hpp"

#include "core/log.hpp"

#include <exception>

namespace df::net
{
namespace
{
constexpr float kClientHelloTimeoutSeconds = 8.0f;
constexpr std::size_t kReliableFlushByteBudgetPerPeerPerTick = 12u * 1024u;
constexpr std::size_t kMaxReliableQueueBytesPerPeer = 16u * 1024u * 1024u;
constexpr std::size_t kMaxAudioCuesPerActorFrame = 8u;

auto WorldSettingsMatchForDelta(
    const world::WorldGenerationSettings& lhs,
    const world::WorldGenerationSettings& rhs) -> bool
{
    return lhs.worldWidth == rhs.worldWidth &&
           lhs.worldHeight == rhs.worldHeight &&
           lhs.worldDepth == rhs.worldDepth &&
           lhs.activeChunkSize == rhs.activeChunkSize &&
           lhs.seed == rhs.seed &&
           lhs.cellSize == rhs.cellSize &&
           lhs.terrainRelief == rhs.terrainRelief &&
           lhs.waterLevel == rhs.waterLevel;
}
}

void SessionHost::Initialize(const Config& config, std::unique_ptr<ITransport> transport)
{
    config_ = config;
    runtime_.Initialize(config_.runtime);
    transport_ = std::move(transport);
    peers_.clear();
    nextRemotePlayerId_ = std::max<game::PlayerId>(2u, config_.localHostPlayerId + 1u);
    worldSnapshotCache_ = {};
    worldSnapshotVersion_ = 0;
    cachedWorldDeltaBaselineVersion_ = 0;
    cachedWorldDeltaTargetVersion_ = 0;
    cachedWorldDeltas_.clear();
    localAudioCues_.clear();
    tickAudioCues_.clear();

    if (config_.localHostPlayerId != game::kInvalidPlayerId)
    {
        (void)runtime_.AddPlayer(config_.localHostPlayerId, config_.localHostPlayerName);
    }

    RefreshWorldSnapshotCache();

    LogInfo(
        "Session host initialized mode=", static_cast<int>(config_.runtime.mode),
        " session='", config_.runtime.sessionName,
        "' max_players=", config_.runtime.maxPlayers);
}

void SessionHost::Shutdown()
{
    LogInfo("Session host shutting down session='", config_.runtime.sessionName, "'");
    runtime_.Shutdown();
    transport_.reset();
    peers_.clear();
    worldSnapshotCache_ = {};
    worldSnapshotVersion_ = 0;
    cachedWorldDeltaBaselineVersion_ = 0;
    cachedWorldDeltaTargetVersion_ = 0;
    cachedWorldDeltas_.clear();
    localAudioCues_.clear();
    tickAudioCues_.clear();
}

void SessionHost::Tick(const float dt)
{
    if (transport_ != nullptr)
    {
        transport_->Pump();
        ProcessPeerEvents();
        ProcessPackets();
        UpdatePendingHandshakes(dt);
    }

    runtime_.Tick(dt);
    tickAudioCues_ = runtime_.ConsumeAudioCues();
    localAudioCues_.insert(localAudioCues_.end(), tickAudioCues_.begin(), tickAudioCues_.end());

    if (transport_ != nullptr)
    {
        RefreshWorldSnapshotCache();
        for (auto& [peerId, peer] : peers_)
        {
            if (!transport_->IsConnected(peerId) || !peer.welcomed)
            {
                continue;
            }

            SendWorldDelta(peerId, peer, worldSnapshotCache_);
            FlushReliableQueue(peerId, peer);
        }

        BroadcastActorFrame(BuildActorFrame());
    }
}

void SessionHost::RefreshWorldSnapshotCache()
{
    const std::uint64_t currentVersion = runtime_.World().TerrainContentVersion();
    if (worldSnapshotVersion_ == currentVersion && !worldSnapshotCache_.cells.empty())
    {
        return;
    }

    world::DenseWorldSnapshot previousSnapshot = std::move(worldSnapshotCache_);
    const std::uint64_t previousVersion = worldSnapshotVersion_;

    worldSnapshotCache_ = runtime_.World().CaptureSnapshot();
    worldSnapshotVersion_ = currentVersion;
    cachedWorldDeltas_.clear();
    cachedWorldDeltaBaselineVersion_ = 0;
    cachedWorldDeltaTargetVersion_ = 0;

    if (!previousSnapshot.cells.empty() &&
        WorldSettingsMatchForDelta(previousSnapshot.settings, worldSnapshotCache_.settings))
    {
        cachedWorldDeltas_ = BuildChunkDeltas(previousSnapshot, worldSnapshotCache_);
        cachedWorldDeltaBaselineVersion_ = previousVersion;
        cachedWorldDeltaTargetVersion_ = currentVersion;
    }
}

auto SessionHost::BuildActorFrame() const -> ActorSnapshotFrame
{
    ActorSnapshotFrame frame{};
    frame.serverTick = runtime_.TickIndex();

    for (const auto& entry : runtime_.Players())
    {
        const game::SessionRuntime::PlayerState& player = entry.second;
        ActorSnapshot actor{};
        actor.id = player.id;
        actor.position = player.controller.Position();
        actor.cameraPosition = player.controller.CameraPosition();
        actor.forward = player.controller.ForwardVector();
        actor.flatForward = player.controller.FlatForwardVector();
        actor.leftFootPosition = player.controller.LeftFootPosition();
        actor.rightFootPosition = player.controller.RightFootPosition();
        actor.leftFootGrounded = player.controller.LeftFootGrounded();
        actor.rightFootGrounded = player.controller.RightFootGrounded();
        actor.onGround = player.controller.OnGround();
        actor.drivingTruck = player.drivingTruck;
        actor.tool = player.tool;
        actor.crosshairMaterial = player.crosshairMaterial;
        actor.horizontalSpeed = player.controller.HorizontalSpeedMetersPerSecond();
        actor.walkCycleRadians = player.controller.WalkCycleRadians();
        actor.yawRadians = player.controller.YawRadians();
        actor.pitchRadians = player.controller.PitchRadians();
        actor.weaponCycle = player.weaponCycle;
        const auto& inventory = player.weaponInventories[game::ToToolIndex(player.tool)];
        actor.ammoInMagazine = inventory.ammoInMagazine;
        actor.reserveAmmo = inventory.reserveAmmo;
        actor.reloading = player.reloading;
        actor.reloadSecondsRemaining = player.reloadTimer;
        actor.reloadSecondsTotal = player.reloadDuration;
        actor.lastAppliedCommandSequence = player.lastAppliedCommandSequence;
        frame.players.push_back(std::move(actor));
    }

    frame.truck.position = runtime_.Truck().Position();
    frame.truck.forward = runtime_.Truck().ForwardVector();
    frame.truck.speedMetersPerSecond = runtime_.Truck().SpeedMetersPerSecond();
    frame.truck.engineLoad = runtime_.Truck().EngineLoad();
    frame.truck.averageSink = runtime_.Truck().AverageSink();
    frame.truck.contactMaterial = runtime_.Truck().ContactMaterial();
    frame.truck.occupied = runtime_.TruckDriverId() != game::kInvalidPlayerId;
    frame.truck.driverId = runtime_.TruckDriverId();

    frame.grenades.reserve(runtime_.Grenades().size());
    for (const game::SessionRuntime::Grenade& grenade : runtime_.Grenades())
    {
        frame.grenades.push_back({
            grenade.position,
            grenade.orientationForward,
            grenade.orientationUp,
        });
    }

    frame.bullets.reserve(runtime_.Bullets().size());
    for (const game::SessionRuntime::Bullet& bullet : runtime_.Bullets())
    {
        frame.bullets.push_back({
            bullet.previousPosition,
            bullet.position,
        });
    }

    frame.beams.reserve(runtime_.Beams().size());
    for (const game::SessionRuntime::Beam& beam : runtime_.Beams())
    {
        frame.beams.push_back({
            beam.start,
            beam.end,
            beam.color,
            beam.ttl,
        });
    }

    const std::size_t audioCueCount = std::min(tickAudioCues_.size(), kMaxAudioCuesPerActorFrame);
    frame.audioCues.reserve(audioCueCount);
    for (std::size_t index = 0; index < audioCueCount; ++index)
    {
        const game::SessionRuntime::AudioCue& cue = tickAudioCues_[index];
        frame.audioCues.push_back({
            .position = cue.position,
            .baseFrequency = cue.baseFrequency,
            .durationSeconds = cue.durationSeconds,
            .amplitude = cue.amplitude,
            .noise = cue.noise,
            .sweep = cue.sweep,
        });
    }

    return frame;
}

auto SessionHost::ConsumeAudioCues() -> std::vector<game::SessionRuntime::AudioCue>
{
    std::vector<game::SessionRuntime::AudioCue> drained;
    drained.swap(localAudioCues_);
    return drained;
}

auto SessionHost::QueueReliablePayload(const PeerId peerId, RemotePeerState& peer, std::vector<std::byte> payload) -> bool
{
    if (payload.empty())
    {
        return true;
    }

    if (peer.reliableQueueBytes + payload.size() > kMaxReliableQueueBytesPerPeer)
    {
        LogWarning(
            "Session host closing lagging peer=", peerId,
            " because the reliable backlog exceeded the cap bytes=", peer.reliableQueueBytes + payload.size(),
            " queued_messages=", peer.reliableQueue.size());
        if (transport_ != nullptr)
        {
            transport_->Close(peerId, 1, "reliable backlog overflow");
        }
        return false;
    }

    peer.reliableQueueBytes += payload.size();
    peer.reliableQueue.push_back(std::move(payload));
    return true;
}

void SessionHost::FlushReliableQueue(const PeerId peerId, RemotePeerState& peer)
{
    std::size_t sentBytes = 0u;
    while (!peer.reliableQueue.empty() && sentBytes < kReliableFlushByteBudgetPerPeerPerTick)
    {
        const std::size_t messageBytes = peer.reliableQueue.front().size();
        if (sentBytes > 0u && sentBytes + messageBytes > kReliableFlushByteBudgetPerPeerPerTick)
        {
            break;
        }

        const bool sent = transport_->Send({
            .peerId = peerId,
            .payload = peer.reliableQueue.front(),
            .reliable = true,
            .channel = TransportChannel::Reliable,
        });
        if (!sent)
        {
            if (!peer.reliableQueueBlocked)
            {
                LogWarning(
                    "Session host reliable queue blocked for peer=", peerId,
                    " pending_messages=", peer.reliableQueue.size());
                peer.reliableQueueBlocked = true;
            }
            break;
        }

        peer.reliableQueueBytes -= messageBytes;
        peer.reliableQueue.pop_front();
        peer.reliableQueueBlocked = false;
        sentBytes += messageBytes;
    }
}

void SessionHost::SendWelcome(const PeerId peerId, RemotePeerState& peer, const game::PlayerId playerId)
{
    static_cast<void>(peerId);
    Welcome welcome{};
    welcome.playerId = playerId;
    welcome.sessionMode = config_.runtime.mode;
    welcome.serverTick = runtime_.TickIndex();
    welcome.sessionName = config_.runtime.sessionName;
    welcome.maxPlayers = config_.runtime.maxPlayers;

    (void)QueueReliablePayload(peerId, peer, EncodeMessage(MessageType::Welcome, EncodeWelcome(welcome)));
}

void SessionHost::SendWorldSnapshot(const PeerId peerId, RemotePeerState& peer, const world::DenseWorldSnapshot& snapshot)
{
    const std::uint64_t serverTick = runtime_.TickIndex();
    const std::vector<WorldSnapshotMessage> messages = BuildWorldSnapshotMessages(serverTick, snapshot);
    LogInfo(
        "Session host queued world snapshot for peer=", peerId,
        " tick=", serverTick,
        " segments=", messages.size(),
        " world_cells=", snapshot.cells.size());
    for (const WorldSnapshotMessage& message : messages)
    {
        if (!QueueReliablePayload(peerId, peer, EncodeMessage(MessageType::WorldSnapshot, EncodeWorldSnapshotMessage(message))))
        {
            break;
        }
    }
}

void SessionHost::SendInitialState(const PeerId peerId, RemotePeerState& peer)
{
    RefreshWorldSnapshotCache();
    peer.baselineWorld = worldSnapshotCache_;
    peer.baselineWorldVersion = worldSnapshotVersion_;
    LogInfo(
        "Session host sending initial state to peer=", peerId,
        " player_id=", peer.playerId,
        " tick=", runtime_.TickIndex(),
        " world_cells=", peer.baselineWorld.cells.size());
    peer.reliableQueue.clear();
    peer.reliableQueueBytes = 0u;
    peer.reliableQueueBlocked = false;
    SendWelcome(peerId, peer, peer.playerId);
    SendWorldSnapshot(peerId, peer, peer.baselineWorld);
}

void SessionHost::SendWorldDelta(const PeerId peerId, RemotePeerState& peer, const world::DenseWorldSnapshot& current)
{
    if (!peer.reliableQueue.empty())
    {
        return;
    }

    if (peer.baselineWorldVersion == worldSnapshotVersion_)
    {
        return;
    }

    if (peer.baselineWorld.cells.empty() ||
        !WorldSettingsMatchForDelta(peer.baselineWorld.settings, current.settings))
    {
        peer.baselineWorld = current;
        peer.baselineWorldVersion = worldSnapshotVersion_;
        SendWorldSnapshot(peerId, peer, current);
        return;
    }

    std::vector<ChunkDelta> fallbackDeltas;
    const std::vector<ChunkDelta>* deltas = nullptr;
    if (peer.baselineWorldVersion == cachedWorldDeltaBaselineVersion_ &&
        worldSnapshotVersion_ == cachedWorldDeltaTargetVersion_)
    {
        deltas = &cachedWorldDeltas_;
    }
    else
    {
        fallbackDeltas = BuildChunkDeltas(peer.baselineWorld, current);
        deltas = &fallbackDeltas;
    }

    if (deltas->empty())
    {
        peer.baselineWorldVersion = worldSnapshotVersion_;
        return;
    }

    const std::vector<ChunkDeltaBatchMessage> batches = BuildChunkDeltaBatches(*deltas, runtime_.TickIndex());
    for (const ChunkDeltaBatchMessage& batch : batches)
    {
        if (!QueueReliablePayload(peerId, peer, EncodeMessage(MessageType::ChunkDeltaBatch, EncodeChunkDeltaBatchMessage(batch))))
        {
            break;
        }
    }
    if (ApplyChunkDeltas(peer.baselineWorld, *deltas))
    {
        peer.baselineWorldVersion = worldSnapshotVersion_;
    }
    else
    {
        peer.baselineWorld = current;
        peer.baselineWorldVersion = worldSnapshotVersion_;
    }
}

void SessionHost::BroadcastActorFrame(const ActorSnapshotFrame& frame)
{
    const std::vector<std::byte> payload = EncodeMessage(MessageType::ActorSnapshotFrame, EncodeActorSnapshotFrame(frame));
    for (const auto& [peerId, peer] : peers_)
    {
        if (!transport_->IsConnected(peerId) || !peer.welcomed)
        {
            continue;
        }

        (void)transport_->Send({
            .peerId = peerId,
            .payload = payload,
            .reliable = false,
            .channel = TransportChannel::State,
        });
    }
}

void SessionHost::ProcessPeerEvents()
{
    for (const PeerEvent& event : transport_->ConsumePeerEvents())
    {
        if (event.status == ConnectionStatus::Connecting || event.status == ConnectionStatus::Connected)
        {
            RemotePeerState& peer = peers_[event.peerId];
            peer.peerId = event.peerId;
            if (event.status == ConnectionStatus::Connected)
            {
                peer.handshakeElapsedSeconds = 0.0f;
            }
            LogInfo(
                "Session host peer event peer=", event.peerId,
                " status=", static_cast<int>(event.status),
                " origin=", static_cast<int>(event.origin),
                " detail='", event.debugText, "'");
            continue;
        }

        if (event.status == ConnectionStatus::Closed)
        {
            const auto iter = peers_.find(event.peerId);
            if (iter != peers_.end())
            {
                LogInfo(
                    "Session host removing peer=", event.peerId,
                    " player_id=", iter->second.playerId,
                    " welcomed=", iter->second.welcomed ? "yes" : "no",
                    " detail='", event.debugText, "'");
                runtime_.RemovePlayer(iter->second.playerId);
                peers_.erase(iter);
            }
            else
            {
                LogInfo("Session host observed closed peer=", event.peerId, " detail='", event.debugText, "'");
            }
        }
    }
}

void SessionHost::ProcessPackets()
{
    for (const TransportPacket& packet : transport_->Receive())
    {
        DecodedMessage message{};
        try
        {
            message = DecodeMessage(packet.payload);
        }
        catch (const std::exception& error)
        {
            LogError("Session host failed to decode packet from peer=", packet.peerId, ": ", error.what());
            transport_->Close(packet.peerId, 1, "invalid session packet");
            continue;
        }

        try
        {
            if (message.type == MessageType::ClientHello)
            {
                const ClientHello hello = DecodeClientHello(message.payload);
                if (hello.protocolVersion != kSessionProtocolVersion)
                {
                    LogError(
                        "Session host rejected client hello from peer=", packet.peerId,
                        " protocol_version=", hello.protocolVersion);
                    transport_->Close(packet.peerId, 1, "session version mismatch");
                    peers_.erase(packet.peerId);
                    continue;
                }

                RemotePeerState& peer = peers_[packet.peerId];
                peer.peerId = packet.peerId;
                peer.handshakeElapsedSeconds = 0.0f;
                if (!peer.welcomed)
                {
                    peer.playerId = nextRemotePlayerId_++;
                    if (!runtime_.AddPlayer(peer.playerId, hello.playerName))
                    {
                        LogWarning("Session host rejected peer=", packet.peerId, " because the session is full or unavailable.");
                        transport_->Close(packet.peerId, 1, "session unavailable");
                        peers_.erase(packet.peerId);
                        continue;
                    }
                    peer.welcomed = true;
                    LogInfo(
                        "Session host admitted peer=", packet.peerId,
                        " as player_id=", peer.playerId,
                        " name='", hello.playerName, "'");
                }
                else
                {
                    LogInfo("Session host received duplicate hello from peer=", packet.peerId, " player_id=", peer.playerId);
                }

                SendInitialState(packet.peerId, peer);
                continue;
            }

            const auto peerIter = peers_.find(packet.peerId);
            if (peerIter == peers_.end() || !peerIter->second.welcomed)
            {
                LogWarning(
                    "Session host closing peer=", packet.peerId,
                    " for unexpected pre-handshake message type=", static_cast<int>(message.type));
                transport_->Close(packet.peerId, 1, "expected client hello");
                peers_.erase(packet.peerId);
                continue;
            }

            if (message.type == MessageType::ClientStateFrame)
            {
                static_cast<void>(DecodeClientStateFrame(message.payload));
            }
            else if (message.type == MessageType::CommandFrame)
            {
                static_cast<void>(runtime_.SubmitCommand(peerIter->second.playerId, DecodeCommandFrame(message.payload)));
            }
            else if (message.type == MessageType::CommandBundle)
            {
                for (const game::PlayerCommandFrame& command : DecodeCommandBundle(message.payload))
                {
                    static_cast<void>(runtime_.SubmitCommand(peerIter->second.playerId, command));
                }
            }
            else
            {
                LogWarning("Session host ignored unexpected message type=", static_cast<int>(message.type), " from peer=", packet.peerId);
            }
        }
        catch (const std::exception& error)
        {
            LogError(
                "Session host failed to process message type=", static_cast<int>(message.type),
                " from peer=", packet.peerId, ": ", error.what());
            transport_->Close(packet.peerId, 1, "invalid session payload");
            peers_.erase(packet.peerId);
        }
    }
}

void SessionHost::UpdatePendingHandshakes(const float dt)
{
    for (auto iter = peers_.begin(); iter != peers_.end();)
    {
        RemotePeerState& peer = iter->second;
        if (peer.welcomed || !transport_->IsConnected(iter->first))
        {
            ++iter;
            continue;
        }

        peer.handshakeElapsedSeconds += dt;
        if (peer.handshakeElapsedSeconds < kClientHelloTimeoutSeconds)
        {
            ++iter;
            continue;
        }

        LogWarning("Session host closing peer=", iter->first, " after client hello timeout.");
        transport_->Close(iter->first, 1, "client hello timed out");
        iter = peers_.erase(iter);
    }
}
}
