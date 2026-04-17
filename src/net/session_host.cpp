#include "net/session_host.hpp"

#include "core/log.hpp"

namespace df::net
{
void SessionHost::Initialize(const Config& config, std::unique_ptr<ITransport> transport)
{
    config_ = config;
    runtime_.Initialize(config_.runtime);
    transport_ = std::move(transport);
    peers_.clear();
    nextRemotePlayerId_ = std::max<game::PlayerId>(2u, config_.localHostPlayerId + 1u);

    if (config_.localHostPlayerId != game::kInvalidPlayerId)
    {
        (void)runtime_.AddPlayer(config_.localHostPlayerId, config_.localHostPlayerName);
    }
}

void SessionHost::Shutdown()
{
    runtime_.Shutdown();
    transport_.reset();
    peers_.clear();
}

void SessionHost::Tick(const float dt)
{
    if (transport_ != nullptr)
    {
        transport_->Pump();
        ProcessPeerEvents();
        ProcessPackets();
    }

    runtime_.Tick(dt);

    if (transport_ != nullptr)
    {
        const world::DenseWorldSnapshot snapshot = runtime_.World().CaptureSnapshot();
        for (auto& [peerId, peer] : peers_)
        {
            if (!transport_->IsConnected(peerId) || !peer.welcomed)
            {
                continue;
            }

            SendWorldDelta(peerId, peer, snapshot);
        }

        BroadcastActorFrame(BuildActorFrame());
    }
}

auto SessionHost::BuildActorFrame() const -> ActorSnapshotFrame
{
    ActorSnapshotFrame frame{};
    frame.serverTick = runtime_.TickIndex();

    for (const auto& [id, player] : runtime_.Players())
    {
        ActorSnapshot actor{};
        actor.id = player.id;
        actor.name = player.name;
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

    return frame;
}

void SessionHost::SendWelcome(const PeerId peerId, const game::PlayerId playerId)
{
    Welcome welcome{};
    welcome.playerId = playerId;
    welcome.sessionMode = config_.runtime.mode;
    welcome.serverTick = runtime_.TickIndex();
    welcome.sessionName = config_.runtime.sessionName;
    welcome.maxPlayers = config_.runtime.maxPlayers;

    const std::vector<std::byte> payload = EncodeMessage(MessageType::Welcome, EncodeWelcome(welcome));
    (void)transport_->Send({
        .peerId = peerId,
        .payload = payload,
        .reliable = true,
        .channel = TransportChannel::Reliable,
    });
}

void SessionHost::SendWorldSnapshot(const PeerId peerId, const world::DenseWorldSnapshot& snapshot)
{
    WorldSnapshotMessage message{};
    message.serverTick = runtime_.TickIndex();
    message.world = snapshot;
    const std::vector<std::byte> payload = EncodeMessage(MessageType::WorldSnapshot, EncodeWorldSnapshotMessage(message));
    (void)transport_->Send({
        .peerId = peerId,
        .payload = payload,
        .reliable = true,
        .channel = TransportChannel::Reliable,
    });
}

void SessionHost::SendWorldDelta(const PeerId peerId, RemotePeerState& peer, const world::DenseWorldSnapshot& current)
{
    if (peer.baselineWorld.cells.empty() ||
        peer.baselineWorld.settings.worldWidth != current.settings.worldWidth ||
        peer.baselineWorld.settings.worldHeight != current.settings.worldHeight ||
        peer.baselineWorld.settings.worldDepth != current.settings.worldDepth)
    {
        peer.baselineWorld = current;
        SendWorldSnapshot(peerId, current);
        return;
    }

    const std::vector<ChunkDelta> deltas = BuildChunkDeltas(peer.baselineWorld, current);
    if (deltas.empty())
    {
        return;
    }

    ChunkDeltaBatchMessage message{};
    message.serverTick = runtime_.TickIndex();
    message.deltas = deltas;
    const std::vector<std::byte> payload = EncodeMessage(MessageType::ChunkDeltaBatch, EncodeChunkDeltaBatchMessage(message));
    (void)transport_->Send({
        .peerId = peerId,
        .payload = payload,
        .reliable = true,
        .channel = TransportChannel::Reliable,
    });
    peer.baselineWorld = current;
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
        if (event.status == ConnectionStatus::Closed)
        {
            const auto iter = peers_.find(event.peerId);
            if (iter != peers_.end())
            {
                runtime_.RemovePlayer(iter->second.playerId);
                peers_.erase(iter);
            }
        }
    }
}

void SessionHost::ProcessPackets()
{
    for (const TransportPacket& packet : transport_->Receive())
    {
        const DecodedMessage message = DecodeMessage(packet.payload);
        if (message.type == MessageType::ClientHello)
        {
            const ClientHello hello = DecodeClientHello(message.payload);
            RemotePeerState& peer = peers_[packet.peerId];
            peer.peerId = packet.peerId;
            if (!peer.welcomed)
            {
                peer.playerId = nextRemotePlayerId_++;
                if (!runtime_.AddPlayer(peer.playerId, hello.playerName))
                {
                    transport_->Close(packet.peerId, 1, "session unavailable");
                    peers_.erase(packet.peerId);
                    continue;
                }
                peer.welcomed = true;
                peer.baselineWorld = runtime_.World().CaptureSnapshot();
                SendWelcome(packet.peerId, peer.playerId);
                SendWorldSnapshot(packet.peerId, peer.baselineWorld);
            }
            continue;
        }

        const auto peerIter = peers_.find(packet.peerId);
        if (peerIter == peers_.end() || !peerIter->second.welcomed)
        {
            continue;
        }

        if (message.type == MessageType::CommandFrame)
        {
            runtime_.SubmitCommand(peerIter->second.playerId, DecodeCommandFrame(message.payload));
        }
    }
}
}
