#include "net/session_client.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace df::net
{
namespace
{
constexpr float kPredictionCorrectionSmoothingRate = 14.0f;
constexpr float kMaximumSmoothedPredictionCorrection = 2.5f;
constexpr double kRenderInterpolationDelayTicks = 8.0;

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

auto WorldSettingsEqual(const world::WorldGenerationSettings& lhs, const world::WorldGenerationSettings& rhs) -> bool
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

auto ExpectedWorldCellCount(const world::WorldGenerationSettings& settings) -> std::size_t
{
    return static_cast<std::size_t>(std::max(settings.worldWidth, 0)) *
           static_cast<std::size_t>(std::max(settings.worldHeight, 0)) *
           static_cast<std::size_t>(std::max(settings.worldDepth, 0));
}

auto BuildCellEditsFromChunkDeltas(const std::span<const ChunkDelta> deltas) -> std::vector<world::DemoWorld::CellMaterialEdit>
{
    std::size_t editCount = 0;
    for (const ChunkDelta& delta : deltas)
    {
        editCount += delta.entries.size();
    }

    std::vector<world::DemoWorld::CellMaterialEdit> edits;
    edits.reserve(editCount);
    for (const ChunkDelta& delta : deltas)
    {
        const int baseX = delta.chunk.x * static_cast<int>(world::kChunkSize);
        const int baseY = delta.chunk.y * static_cast<int>(world::kChunkSize);
        const int baseZ = delta.chunk.z * static_cast<int>(world::kChunkSize);

        for (const ChunkDeltaEntry& entry : delta.entries)
        {
            const int localX = entry.cellIndex % static_cast<int>(world::kChunkSize);
            const int localY = (entry.cellIndex / static_cast<int>(world::kChunkSize)) % static_cast<int>(world::kChunkSize);
            const int localZ = entry.cellIndex / static_cast<int>(world::kChunkSize * world::kChunkSize);
            edits.push_back({
                .x = baseX + localX,
                .y = baseY + localY,
                .z = baseZ + localZ,
                .material = entry.material,
            });
        }
    }

    return edits;
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
    result.weaponCycle = Lerp(from.weaponCycle, to.weaponCycle, t);
    result.leftFootGrounded = t < 0.5f ? from.leftFootGrounded : to.leftFootGrounded;
    result.rightFootGrounded = t < 0.5f ? from.rightFootGrounded : to.rightFootGrounded;
    result.onGround = t < 0.5f ? from.onGround : to.onGround;
    result.drivingTruck = t < 0.5f ? from.drivingTruck : to.drivingTruck;
    result.tool = t < 0.5f ? from.tool : to.tool;
    result.crosshairMaterial = t < 0.5f ? from.crosshairMaterial : to.crosshairMaterial;
    result.ammoInMagazine = t < 0.5f ? from.ammoInMagazine : to.ammoInMagazine;
    result.reserveAmmo = t < 0.5f ? from.reserveAmmo : to.reserveAmmo;
    result.reloading = t < 0.5f ? from.reloading : to.reloading;
    result.reloadSecondsRemaining = Lerp(from.reloadSecondsRemaining, to.reloadSecondsRemaining, t);
    result.reloadSecondsTotal = std::max(from.reloadSecondsTotal, to.reloadSecondsTotal);
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
    connectElapsedSeconds_ = 0.0f;
    handshakeElapsedSeconds_ = 0.0f;
    helloResendSeconds_ = 0.0f;
    disconnected_ = false;
    lastDisconnectText_.clear();
    ResetSessionState();
}

void SessionClient::ResetSessionState()
{
    serverPeerId_ = kInvalidPeerId;
    helloSent_ = false;
    worldReady_ = false;
    actorSnapshotReady_ = false;
    localPlayerId_ = game::kInvalidPlayerId;
    sessionMode_ = game::SessionMode::Client;
    sessionName_.clear();
    worldBaseline_ = {};
    pendingWorldSnapshot_ = {};
    actorFrame_ = {};
    actorHistory_ = {};
    actorHistoryCount_ = 0;
    lastAudioCueServerTick_ = 0;
    pendingAudioCues_.clear();
    pendingCommands_.clear();
    predictedPlayerValid_ = false;
    renderedPredictedPlayerValid_ = false;
    predictedRenderOffset_ = {};
}

void SessionClient::Shutdown()
{
    transport_.reset();
    connectElapsedSeconds_ = 0.0f;
    handshakeElapsedSeconds_ = 0.0f;
    helloResendSeconds_ = 0.0f;
    disconnected_ = false;
    lastDisconnectText_.clear();
    ResetSessionState();
}

void SessionClient::Tick(const float dt, const game::PlayerCommandFrame& localCommand)
{
    if (transport_ == nullptr)
    {
        return;
    }

    transport_->Pump();
    ProcessPeerEvents();
    if (disconnected_)
    {
        return;
    }

    if (serverPeerId_ == kInvalidPeerId)
    {
        connectElapsedSeconds_ += dt;
        if (connectElapsedSeconds_ >= std::max(config_.connectTimeoutSeconds, 0.1f))
        {
            FailConnection("connection timed out");
            return;
        }
    }
    else if (!IsReady())
    {
        handshakeElapsedSeconds_ += dt;
        helloResendSeconds_ += dt;
        if (localPlayerId_ == game::kInvalidPlayerId &&
            helloResendSeconds_ >= std::max(config_.helloResendIntervalSeconds, 0.1f))
        {
            SendClientHello();
            helloResendSeconds_ = 0.0f;
        }
        if (handshakeElapsedSeconds_ >= std::max(config_.handshakeTimeoutSeconds, 0.1f))
        {
            FailConnection("session handshake timed out");
            return;
        }
    }

    if (serverPeerId_ != kInvalidPeerId && helloSent_)
    {
        AdvancePredictedLocalPlayer(localCommand.control, dt);
        StorePendingCommand(localCommand, dt);

        const std::vector<game::PlayerCommandFrame> commandBundle = BuildCommandBundle(localCommand);
        const std::vector<std::byte> payload = commandBundle.size() > 1u
            ? EncodeMessage(MessageType::CommandBundle, EncodeCommandBundle(commandBundle))
            : EncodeMessage(MessageType::CommandFrame, EncodeCommandFrame(localCommand));
        const bool sent = transport_->Send({
            .peerId = serverPeerId_,
            .payload = payload,
            .reliable = false,
            .channel = TransportChannel::State,
        });
        if (!sent)
        {
            LogWarning("Session client failed to send command frame sequence=", localCommand.sequence);
        }
    }

    ProcessPackets();
    world_.TickRenderState(dt);
    UpdatePredictionSmoothing(dt);
}

void SessionClient::SendClientHello()
{
    if (transport_ == nullptr || serverPeerId_ == kInvalidPeerId)
    {
        return;
    }

    const ClientHello hello{.playerName = config_.playerName};
    const std::vector<std::byte> payload = EncodeMessage(MessageType::ClientHello, EncodeClientHello(hello));
    const bool sent = transport_->Send({
        .peerId = serverPeerId_,
        .payload = payload,
        .reliable = true,
        .channel = TransportChannel::Reliable,
    });
    if (sent)
    {
        helloSent_ = true;
        LogInfo("Session client sent hello to peer=", serverPeerId_, " player='", config_.playerName, "'");
    }
    else
    {
        LogWarning("Session client failed to send hello to peer=", serverPeerId_);
    }
}

void SessionClient::FailConnection(const std::string_view reason)
{
    if (disconnected_)
    {
        return;
    }

    LogWarning("Session client disconnecting: ", reason);
    if (transport_ != nullptr && serverPeerId_ != kInvalidPeerId)
    {
        const std::string reasonText(reason);
        transport_->Close(serverPeerId_, 0, reasonText.c_str());
    }

    lastDisconnectText_ = std::string(reason);
    connectElapsedSeconds_ = 0.0f;
    handshakeElapsedSeconds_ = 0.0f;
    helloResendSeconds_ = 0.0f;
    ResetSessionState();
    disconnected_ = true;
}

void SessionClient::ProcessPeerEvents()
{
    for (const PeerEvent& event : transport_->ConsumePeerEvents())
    {
        if (event.status == ConnectionStatus::Connected && serverPeerId_ == kInvalidPeerId)
        {
            serverPeerId_ = event.peerId;
            connectElapsedSeconds_ = 0.0f;
            handshakeElapsedSeconds_ = 0.0f;
            helloResendSeconds_ = 0.0f;
            disconnected_ = false;
            lastDisconnectText_.clear();
            LogInfo("Session client connected to peer=", serverPeerId_, " detail='", event.debugText, "'");
            SendClientHello();
        }
        else if (event.status == ConnectionStatus::Closed && event.peerId == serverPeerId_)
        {
            FailConnection(event.debugText.empty() ? "connection closed" : event.debugText);
        }
    }
}

void SessionClient::ProcessPackets()
{
    for (const TransportPacket& packet : transport_->Receive())
    {
        if (serverPeerId_ != kInvalidPeerId && packet.peerId != serverPeerId_)
        {
            LogWarning("Session client ignored packet from unexpected peer=", packet.peerId, " expected=", serverPeerId_);
            continue;
        }

        DecodedMessage message{};
        try
        {
            message = DecodeMessage(packet.payload);
        }
        catch (const std::exception& error)
        {
            LogError("Session client failed to decode packet from peer=", packet.peerId, ": ", error.what());
            FailConnection("invalid session packet");
            return;
        }

        try
        {
            switch (message.type)
            {
            case MessageType::Welcome:
            {
                const Welcome welcome = DecodeWelcome(message.payload);
                if (welcome.protocolVersion != kSessionProtocolVersion)
                {
                    LogError("Session client rejected welcome with protocol_version=", welcome.protocolVersion);
                    FailConnection("session version mismatch");
                    return;
                }

                localPlayerId_ = welcome.playerId;
                sessionMode_ = welcome.sessionMode;
                sessionName_ = welcome.sessionName;
                actorSnapshotReady_ = FindActor(localPlayerId_) != nullptr;
                disconnected_ = false;
                handshakeElapsedSeconds_ = 0.0f;
                helloResendSeconds_ = 0.0f;
                LogInfo(
                    "Session client received welcome player_id=", localPlayerId_,
                    " mode=", static_cast<int>(sessionMode_),
                    " session='", sessionName_, "'");
                if (worldReady_ && actorSnapshotReady_)
                {
                    RebuildPredictedLocalPlayer();
                }
                break;
            }

            case MessageType::WorldSnapshot:
            {
                const WorldSnapshotMessage snapshot = DecodeWorldSnapshotMessage(message.payload);
                const std::size_t expectedCellCount = ExpectedWorldCellCount(snapshot.settings);
                if (snapshot.totalCellCount != expectedCellCount)
                {
                    throw std::runtime_error("World snapshot segment size did not match the advertised world dimensions.");
                }

                if (snapshot.cellOffset == 0u)
                {
                    pendingWorldSnapshot_ = {};
                    pendingWorldSnapshot_.active = true;
                    pendingWorldSnapshot_.serverTick = snapshot.serverTick;
                    pendingWorldSnapshot_.snapshot.settings = snapshot.settings;
                    pendingWorldSnapshot_.snapshot.cells.assign(snapshot.totalCellCount, static_cast<std::uint8_t>(world::MaterialId::Air));
                }

                if (!pendingWorldSnapshot_.active)
                {
                    throw std::runtime_error("Received a world snapshot continuation without an active transfer.");
                }

                if (pendingWorldSnapshot_.serverTick != snapshot.serverTick ||
                    !WorldSettingsEqual(pendingWorldSnapshot_.snapshot.settings, snapshot.settings) ||
                    pendingWorldSnapshot_.snapshot.cells.size() != snapshot.totalCellCount)
                {
                    throw std::runtime_error("World snapshot segments disagreed on transfer metadata.");
                }

                if (snapshot.cellOffset != pendingWorldSnapshot_.nextCellOffset)
                {
                    throw std::runtime_error("World snapshot segments arrived out of order.");
                }

                const std::vector<std::uint8_t> decodedCells = DecodeWorldSnapshotCells(snapshot);
                if (!decodedCells.empty())
                {
                    std::copy(
                        decodedCells.begin(),
                        decodedCells.end(),
                        pendingWorldSnapshot_.snapshot.cells.begin() + static_cast<std::ptrdiff_t>(snapshot.cellOffset));
                }
                pendingWorldSnapshot_.nextCellOffset += snapshot.decodedCellCount;
                handshakeElapsedSeconds_ = 0.0f;

                if (pendingWorldSnapshot_.nextCellOffset == snapshot.totalCellCount)
                {
                    worldBaseline_ = pendingWorldSnapshot_.snapshot;
                    worldReady_ = world_.ApplySnapshot(worldBaseline_);
                    pendingWorldSnapshot_ = {};
                    if (worldReady_)
                    {
                        handshakeElapsedSeconds_ = 0.0f;
                        if (actorSnapshotReady_)
                        {
                            RebuildPredictedLocalPlayer();
                        }
                        LogInfo(
                            "Session client applied full world snapshot tick=", snapshot.serverTick,
                            " cells=", worldBaseline_.cells.size());
                    }
                }
                break;
            }

            case MessageType::ChunkDeltaBatch:
            {
                ChunkDeltaBatchMessage deltaMessage = DecodeChunkDeltaBatchMessage(message.payload);
                if (ApplyChunkDeltas(worldBaseline_, deltaMessage.deltas))
                {
                    const std::vector<world::DemoWorld::CellMaterialEdit> edits = BuildCellEditsFromChunkDeltas(deltaMessage.deltas);
                    worldReady_ = world_.ApplyCellEdits(edits);
                    if (!worldReady_)
                    {
                        worldReady_ = world_.ApplySnapshot(worldBaseline_);
                    }
                    if (worldReady_ && actorSnapshotReady_)
                    {
                        RebuildPredictedLocalPlayer();
                    }
                    handshakeElapsedSeconds_ = 0.0f;
                }
                break;
            }

            case MessageType::ActorSnapshotFrame:
            {
                ActorSnapshotFrame incomingFrame = DecodeActorSnapshotFrame(message.payload);
                if (incomingFrame.serverTick > lastAudioCueServerTick_ && !incomingFrame.audioCues.empty())
                {
                    pendingAudioCues_.insert(pendingAudioCues_.end(), incomingFrame.audioCues.begin(), incomingFrame.audioCues.end());
                    lastAudioCueServerTick_ = incomingFrame.serverTick;
                }
                else if (incomingFrame.serverTick > lastAudioCueServerTick_)
                {
                    lastAudioCueServerTick_ = incomingFrame.serverTick;
                }
                actorFrame_ = std::move(incomingFrame);
                PushActorFrame(actorFrame_);
                actorSnapshotReady_ = localPlayerId_ != game::kInvalidPlayerId && FindActor(localPlayerId_) != nullptr;
                handshakeElapsedSeconds_ = 0.0f;
                if (worldReady_ && actorSnapshotReady_)
                {
                    RebuildPredictedLocalPlayer();
                }
                break;
            }

            default:
                LogWarning("Session client ignored unexpected message type=", static_cast<int>(message.type));
                break;
            }
        }
        catch (const std::exception& error)
        {
            LogError("Session client failed to process message type=", static_cast<int>(message.type), ": ", error.what());
            FailConnection("invalid session payload");
            return;
        }
    }
}

void SessionClient::StorePendingCommand(const game::PlayerCommandFrame& command, const float dt)
{
    if (localPlayerId_ == game::kInvalidPlayerId)
    {
        return;
    }

    if (!pendingCommands_.empty() && !SequenceGreaterThan(command.sequence, pendingCommands_.back().sequence))
    {
        return;
    }

    pendingCommands_.push_back({
        .frame = command,
        .dt = dt,
        .sequence = command.sequence,
        .valid = true,
    });

    while (pendingCommands_.size() > kPendingCommandHistorySize)
    {
        pendingCommands_.pop_front();
    }
}

auto SessionClient::BuildCommandBundle(const game::PlayerCommandFrame& currentCommand) const -> std::vector<game::PlayerCommandFrame>
{
    std::vector<game::PlayerCommandFrame> frames;
    const std::size_t pendingCount = pendingCommands_.size();
    if (pendingCount == 0u)
    {
        frames.push_back(currentCommand);
        return frames;
    }

    const std::size_t bundleCount = std::min(pendingCount, kMaxCommandBundleFrames);
    frames.reserve(bundleCount);
    const std::size_t firstPending = pendingCount - bundleCount;
    for (std::size_t index = firstPending; index < pendingCount; ++index)
    {
        if (pendingCommands_[index].valid)
        {
            frames.push_back(pendingCommands_[index].frame);
        }
    }

    if (frames.empty() || frames.back().sequence != currentCommand.sequence)
    {
        if (frames.size() == kMaxCommandBundleFrames)
        {
            frames.erase(frames.begin());
        }
        frames.push_back(currentCommand);
    }
    return frames;
}

void SessionClient::AdvancePredictedLocalPlayer(const game::ControlState& control, const float dt)
{
    if (!predictedPlayerValid_)
    {
        return;
    }

    predictedPlayer_.Tick(control, world_, dt);
}

void SessionClient::ClearAcknowledgedPendingCommands(const std::uint32_t lastAppliedSequence)
{
    while (!pendingCommands_.empty() &&
           pendingCommands_.front().valid &&
           SequenceLessOrEqual(pendingCommands_.front().sequence, lastAppliedSequence))
    {
        pendingCommands_.pop_front();
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
    if (!worldReady_ || !actorSnapshotReady_)
    {
        predictedPlayerValid_ = false;
        renderedPredictedPlayerValid_ = false;
        predictedRenderOffset_ = {};
        return;
    }

    const ActorSnapshot* const localActor = FindActor(localPlayerId_);
    if (localActor == nullptr)
    {
        actorSnapshotReady_ = false;
        predictedPlayerValid_ = false;
        renderedPredictedPlayerValid_ = false;
        predictedRenderOffset_ = {};
        return;
    }

    const bool hadRenderedPrediction = renderedPredictedPlayerValid_;
    const Vec3 previousRenderedPosition = hadRenderedPrediction ? renderedPredictedPlayer_.Position() : Vec3{};

    ClearAcknowledgedPendingCommands(localActor->lastAppliedCommandSequence);

    predictedPlayer_.PlaceAt(localActor->position, localActor->yawRadians, localActor->pitchRadians);

    for (const PendingCommand& pending : pendingCommands_)
    {
        if (!pending.valid || !SequenceGreaterThan(pending.sequence, localActor->lastAppliedCommandSequence))
        {
            continue;
        }

        predictedPlayer_.Tick(pending.frame.control, world_, pending.dt);
    }

    predictedPlayerValid_ = true;
    if (hadRenderedPrediction)
    {
        const Vec3 correction = previousRenderedPosition - predictedPlayer_.Position();
        if (LengthSquared(correction) <= kMaximumSmoothedPredictionCorrection * kMaximumSmoothedPredictionCorrection)
        {
            predictedRenderOffset_ = correction;
        }
        else
        {
            predictedRenderOffset_ = {};
        }
    }
    else
    {
        predictedRenderOffset_ = {};
    }

    RefreshRenderedPredictedPlayer();
}

auto SessionClient::ConsumeAudioCues() -> std::vector<AudioCueSnapshot>
{
    std::vector<AudioCueSnapshot> drained;
    drained.swap(pendingAudioCues_);
    return drained;
}

void SessionClient::RefreshRenderedPredictedPlayer()
{
    if (!predictedPlayerValid_)
    {
        renderedPredictedPlayerValid_ = false;
        return;
    }

    renderedPredictedPlayer_ = predictedPlayer_;
    if (LengthSquared(predictedRenderOffset_) > 1.0e-8f)
    {
        renderedPredictedPlayer_.Translate(predictedRenderOffset_);
    }
    renderedPredictedPlayerValid_ = true;
}

void SessionClient::UpdatePredictionSmoothing(const float dt)
{
    if (!predictedPlayerValid_)
    {
        renderedPredictedPlayerValid_ = false;
        predictedRenderOffset_ = {};
        return;
    }

    if (LengthSquared(predictedRenderOffset_) > 1.0e-8f)
    {
        predictedRenderOffset_ = Lerp(predictedRenderOffset_, Vec3{}, Clamp(dt * kPredictionCorrectionSmoothingRate, 0.0f, 1.0f));
        if (LengthSquared(predictedRenderOffset_) <= 1.0e-6f)
        {
            predictedRenderOffset_ = {};
        }
    }

    RefreshRenderedPredictedPlayer();
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
    const double earliestTick = static_cast<double>(actorHistory_[0u].serverTick);
    double targetTick = static_cast<double>(latest.serverTick) - kRenderInterpolationDelayTicks + static_cast<double>(clampedAlpha);
    targetTick = std::clamp(targetTick, earliestTick, static_cast<double>(latest.serverTick));

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
