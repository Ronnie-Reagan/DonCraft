#include "net/session_client.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace df::net
{
namespace
{
auto SequenceGreaterThan(const std::uint32_t lhs, const std::uint32_t rhs) -> bool
{
    return static_cast<std::int32_t>(lhs - rhs) > 0;
}

auto SequenceLessOrEqual(const std::uint32_t lhs, const std::uint32_t rhs) -> bool
{
    return static_cast<std::int32_t>(lhs - rhs) <= 0;
}

auto LerpDirection(const Vec3& from, const Vec3& to, const float t, const Vec3& fallback) -> Vec3
{
    const Vec3 blended = Normalize(Lerp(from, to, t));
    return LengthSquared(blended) > 1.0e-6f ? blended : fallback;
}

auto LerpAngleRadians(const float from, const float to, const float t) -> float
{
    return from + std::remainder(to - from, kPi * 2.0f) * t;
}

auto InterpolateActorSnapshot(const ActorSnapshot& from, const ActorSnapshot& to, const float t) -> ActorSnapshot
{
    ActorSnapshot result = t < 0.5f ? from : to;
    result.id = to.id;
    result.name = to.name;
    result.position = Lerp(from.position, to.position, t);
    result.cameraPosition = Lerp(from.cameraPosition, to.cameraPosition, t);
    result.forward = LerpDirection(from.forward, to.forward, t, to.forward);
    result.flatForward = LerpDirection(from.flatForward, to.flatForward, t, to.flatForward);
    result.leftFootPosition = Lerp(from.leftFootPosition, to.leftFootPosition, t);
    result.rightFootPosition = Lerp(from.rightFootPosition, to.rightFootPosition, t);
    result.horizontalSpeed = Lerp(from.horizontalSpeed, to.horizontalSpeed, t);
    result.walkCycleRadians = LerpAngleRadians(from.walkCycleRadians, to.walkCycleRadians, t);
    result.yawRadians = LerpAngleRadians(from.yawRadians, to.yawRadians, t);
    result.pitchRadians = LerpAngleRadians(from.pitchRadians, to.pitchRadians, t);
    result.leftFootGrounded = t < 0.5f ? from.leftFootGrounded : to.leftFootGrounded;
    result.rightFootGrounded = t < 0.5f ? from.rightFootGrounded : to.rightFootGrounded;
    result.onGround = t < 0.5f ? from.onGround : to.onGround;
    result.drivingTruck = t < 0.5f ? from.drivingTruck : to.drivingTruck;
    result.tool = t < 0.5f ? from.tool : to.tool;
    result.crosshairMaterial = t < 0.5f ? from.crosshairMaterial : to.crosshairMaterial;
    result.lastAppliedCommandSequence = to.lastAppliedCommandSequence;
    return result;
}

auto InterpolateTruckSnapshot(const TruckSnapshot& from, const TruckSnapshot& to, const float t) -> TruckSnapshot
{
    TruckSnapshot result = t < 0.5f ? from : to;
    result.position = Lerp(from.position, to.position, t);
    result.forward = LerpDirection(from.forward, to.forward, t, to.forward);
    result.speedMetersPerSecond = Lerp(from.speedMetersPerSecond, to.speedMetersPerSecond, t);
    result.engineLoad = Lerp(from.engineLoad, to.engineLoad, t);
    result.averageSink = Lerp(from.averageSink, to.averageSink, t);
    result.contactMaterial = t < 0.5f ? from.contactMaterial : to.contactMaterial;
    result.occupied = t < 0.5f ? from.occupied : to.occupied;
    result.driverId = t < 0.5f ? from.driverId : to.driverId;
    return result;
}
}

void SessionClient::Initialize(const Config& config, std::unique_ptr<ITransport> transport)
{
    config_ = config;
    transport_ = std::move(transport);
    serverPeerId_ = kInvalidPeerId;
    helloSent_ = false;
    worldReady_ = false;
    localPlayerId_ = game::kInvalidPlayerId;
    sessionMode_ = game::SessionMode::Client;
    sessionName_.clear();
    disconnected_ = false;
    lastDisconnectText_.clear();
    worldBaseline_ = {};
    actorFrame_ = {};
    actorHistory_ = {};
    actorHistoryCount_ = 0;
    pendingCommands_ = {};
    predictedPlayerValid_ = false;
}

void SessionClient::Shutdown()
{
    transport_.reset();
    disconnected_ = false;
    lastDisconnectText_.clear();
    actorFrame_ = {};
    actorHistory_ = {};
    actorHistoryCount_ = 0;
    pendingCommands_ = {};
    predictedPlayerValid_ = false;
}

void SessionClient::Tick(const float dt, const game::PlayerCommandFrame& localCommand)
{
    if (transport_ == nullptr)
    {
        return;
    }

    transport_->Pump();
    ProcessPeerEvents();

    if (serverPeerId_ != kInvalidPeerId && helloSent_)
    {
        const std::vector<std::byte> payload = EncodeMessage(MessageType::CommandFrame, EncodeCommandFrame(localCommand));
        (void)transport_->Send({
            .peerId = serverPeerId_,
            .payload = payload,
            .reliable = false,
            .channel = TransportChannel::State,
        });
        StorePendingCommand(localCommand, dt);
    }

    ProcessPackets();
}

void SessionClient::ProcessPeerEvents()
{
    for (const PeerEvent& event : transport_->ConsumePeerEvents())
    {
        if (event.status == ConnectionStatus::Connected && serverPeerId_ == kInvalidPeerId)
        {
            serverPeerId_ = event.peerId;
            disconnected_ = false;
            lastDisconnectText_.clear();
            const ClientHello hello{.playerName = config_.playerName};
            const std::vector<std::byte> payload = EncodeMessage(MessageType::ClientHello, EncodeClientHello(hello));
            (void)transport_->Send({
                .peerId = serverPeerId_,
                .payload = payload,
                .reliable = true,
                .channel = TransportChannel::Reliable,
            });
            helloSent_ = true;
        }
        else if (event.status == ConnectionStatus::Closed && event.peerId == serverPeerId_)
        {
            lastDisconnectText_ = event.debugText.empty() ? "CONNECTION CLOSED" : event.debugText;
            serverPeerId_ = kInvalidPeerId;
            helloSent_ = false;
            localPlayerId_ = game::kInvalidPlayerId;
            worldReady_ = false;
            actorFrame_ = {};
            actorHistory_ = {};
            actorHistoryCount_ = 0;
            pendingCommands_ = {};
            predictedPlayerValid_ = false;
            disconnected_ = true;
        }
    }
}

void SessionClient::ProcessPackets()
{
    for (const TransportPacket& packet : transport_->Receive())
    {
        const DecodedMessage message = DecodeMessage(packet.payload);
        switch (message.type)
        {
        case MessageType::Welcome:
        {
            const Welcome welcome = DecodeWelcome(message.payload);
            localPlayerId_ = welcome.playerId;
            sessionMode_ = welcome.sessionMode;
            sessionName_ = welcome.sessionName;
            disconnected_ = false;
            break;
        }

        case MessageType::WorldSnapshot:
        {
            const WorldSnapshotMessage snapshot = DecodeWorldSnapshotMessage(message.payload);
            worldBaseline_ = snapshot.world;
            worldReady_ = world_.ApplySnapshot(snapshot.world);
            if (worldReady_)
            {
                RebuildPredictedLocalPlayer();
            }
            break;
        }

        case MessageType::ChunkDeltaBatch:
        {
            ChunkDeltaBatchMessage deltaMessage = DecodeChunkDeltaBatchMessage(message.payload);
            if (ApplyChunkDeltas(worldBaseline_, deltaMessage.deltas))
            {
                worldReady_ = world_.ApplySnapshot(worldBaseline_);
                if (worldReady_)
                {
                    RebuildPredictedLocalPlayer();
                }
            }
            break;
        }

        case MessageType::ActorSnapshotFrame:
        {
            actorFrame_ = DecodeActorSnapshotFrame(message.payload);
            PushActorFrame(actorFrame_);
            RebuildPredictedLocalPlayer();
            break;
        }

        default:
            break;
        }
    }
}

void SessionClient::StorePendingCommand(const game::PlayerCommandFrame& command, const float dt)
{
    if (localPlayerId_ == game::kInvalidPlayerId)
    {
        return;
    }

    PendingCommand& pending = pendingCommands_[command.sequence % pendingCommands_.size()];
    pending.frame = command;
    pending.dt = dt;
    pending.sequence = command.sequence;
    pending.valid = true;
}

void SessionClient::ClearAcknowledgedPendingCommands(const std::uint32_t lastAppliedSequence)
{
    for (PendingCommand& pending : pendingCommands_)
    {
        if (pending.valid && SequenceLessOrEqual(pending.sequence, lastAppliedSequence))
        {
            pending = {};
        }
    }
}

void SessionClient::PushActorFrame(const ActorSnapshotFrame& frame)
{
    if (actorHistoryCount_ > 0u)
    {
        ActorSnapshotFrame& latest = actorHistory_[actorHistoryCount_ - 1u];
        if (frame.serverTick < latest.serverTick)
        {
            return;
        }

        if (frame.serverTick == latest.serverTick)
        {
            latest = frame;
            return;
        }
    }

    if (actorHistoryCount_ < actorHistory_.size())
    {
        actorHistory_[actorHistoryCount_++] = frame;
        return;
    }

    for (std::size_t index = 1u; index < actorHistory_.size(); ++index)
    {
        actorHistory_[index - 1u] = std::move(actorHistory_[index]);
    }
    actorHistory_.back() = frame;
}

void SessionClient::RebuildPredictedLocalPlayer()
{
    const ActorSnapshot* const localActor = FindActor(localPlayerId_);
    if (localActor == nullptr)
    {
        predictedPlayerValid_ = false;
        return;
    }

    ClearAcknowledgedPendingCommands(localActor->lastAppliedCommandSequence);

    predictedPlayer_.PlaceAt(localActor->position, localActor->yawRadians, localActor->pitchRadians);

    std::vector<const PendingCommand*> replayCommands;
    replayCommands.reserve(pendingCommands_.size());
    for (const PendingCommand& pending : pendingCommands_)
    {
        if (!pending.valid || !SequenceGreaterThan(pending.sequence, localActor->lastAppliedCommandSequence))
        {
            continue;
        }

        replayCommands.push_back(&pending);
    }

    std::sort(replayCommands.begin(), replayCommands.end(), [](const PendingCommand* const left, const PendingCommand* const right)
    {
        return left->sequence < right->sequence;
    });

    for (const PendingCommand* const pending : replayCommands)
    {
        predictedPlayer_.Tick(pending->frame.control, world_, pending->dt);
    }

    predictedPlayerValid_ = true;
}

auto SessionClient::FindActor(const game::PlayerId id) const -> const ActorSnapshot*
{
    return FindActor(id, actorFrame_);
}

auto SessionClient::FindActor(const game::PlayerId id, const ActorSnapshotFrame& frame) const -> const ActorSnapshot*
{
    for (const ActorSnapshot& actor : frame.players)
    {
        if (actor.id == id)
        {
            return &actor;
        }
    }

    return nullptr;
}

auto SessionClient::BuildRenderActorFrame(const float interpolationAlpha) const -> ActorSnapshotFrame
{
    if (actorHistoryCount_ == 0u)
    {
        return actorFrame_;
    }

    if (actorHistoryCount_ == 1u)
    {
        return actorHistory_[0u];
    }

    const ActorSnapshotFrame& latest = actorHistory_[actorHistoryCount_ - 1u];
    const float clampedAlpha = Clamp(interpolationAlpha, 0.0f, 1.0f);
    double targetTick = static_cast<double>(latest.serverTick);
    if (latest.serverTick >= 2u)
    {
        targetTick -= 2.0;
    }
    targetTick = std::min(targetTick + static_cast<double>(clampedAlpha), static_cast<double>(latest.serverTick));

    const ActorSnapshotFrame* from = &actorHistory_[0u];
    const ActorSnapshotFrame* to = from;
    if (targetTick > static_cast<double>(actorHistory_[0u].serverTick))
    {
        from = &latest;
        to = &latest;
        for (std::size_t index = 1u; index < actorHistoryCount_; ++index)
        {
            const ActorSnapshotFrame& candidate = actorHistory_[index];
            if (static_cast<double>(candidate.serverTick) >= targetTick)
            {
                from = &actorHistory_[index - 1u];
                to = &candidate;
                break;
            }

            from = &candidate;
            to = &candidate;
        }
    }

    float blend = 0.0f;
    if (from != to && to->serverTick > from->serverTick)
    {
        blend = static_cast<float>((targetTick - static_cast<double>(from->serverTick)) /
                                   static_cast<double>(to->serverTick - from->serverTick));
    }

    ActorSnapshotFrame renderFrame = actorFrame_;
    renderFrame.serverTick = static_cast<std::uint64_t>(targetTick);
    renderFrame.players.clear();
    renderFrame.players.reserve(std::max(from->players.size(), to->players.size()));

    for (const ActorSnapshot& actor : to->players)
    {
        if (const ActorSnapshot* const previous = FindActor(actor.id, *from))
        {
            renderFrame.players.push_back(InterpolateActorSnapshot(*previous, actor, blend));
        }
        else
        {
            renderFrame.players.push_back(actor);
        }
    }

    for (const ActorSnapshot& actor : from->players)
    {
        if (FindActor(actor.id, *to) == nullptr)
        {
            renderFrame.players.push_back(actor);
        }
    }

    renderFrame.truck = from == to ? from->truck : InterpolateTruckSnapshot(from->truck, to->truck, blend);
    return renderFrame;
}
}
