#include "game/session_runtime.hpp"
#include "core/bit_packer.hpp"
#include "core/log.hpp"
#include "net/chunk_delta.hpp"
#include "net/session_client.hpp"
#include "net/session_host.hpp"
#include "net/session_protocol.hpp"
#include "net/transport.hpp"
#include "sim/reference_mpm.hpp"
#include "sim/vulkan_mpm_2d.hpp"
#include "world/demo_world.hpp"
#include "world/material_field.hpp"

#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
bool Expect(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        df::LogError("Self-check failure: ", message);
        return false;
    }

    return true;
}

auto FindActorById(const df::net::ActorSnapshotFrame& frame, const df::game::PlayerId id) -> const df::net::ActorSnapshot*
{
    for (const df::net::ActorSnapshot& actor : frame.players)
    {
        if (actor.id == id)
        {
            return &actor;
        }
    }

    return nullptr;
}

class LoopbackTransport final : public df::net::ITransport
{
public:
    struct DelayedPacket
    {
        std::uint64_t deliverAtHostPump = 0;
        df::net::TransportPacket packet;
    };

    struct SharedState
    {
        bool connected = false;
        bool wasConnected = false;
        bool closedEventSent = false;
        int serverToClientStateDelayTicks = 0;
        std::uint64_t hostPumpCount = 0;
        std::vector<df::net::TransportPacket> toServer;
        std::vector<df::net::TransportPacket> toClient;
        std::vector<DelayedPacket> delayedToClient;
        std::vector<df::net::PeerEvent> serverEvents;
        std::vector<df::net::PeerEvent> clientEvents;
    };

    LoopbackTransport(std::shared_ptr<SharedState> shared, const bool server)
        : shared_(std::move(shared))
        , server_(server)
    {
    }

    void Connect()
    {
        if (shared_->connected)
        {
            return;
        }

        shared_->connected = true;
        shared_->wasConnected = true;
        shared_->closedEventSent = false;
        shared_->toServer.clear();
        shared_->toClient.clear();
        shared_->delayedToClient.clear();
        shared_->serverEvents.push_back({1u, df::net::ConnectionOrigin::Incoming, df::net::ConnectionStatus::Connected, "loopback connected"});
        shared_->clientEvents.push_back({1u, df::net::ConnectionOrigin::Outgoing, df::net::ConnectionStatus::Connected, "loopback connected"});
    }

    [[nodiscard]] bool IsServer() const override
    {
        return server_;
    }

    [[nodiscard]] bool IsListening() const override
    {
        return server_;
    }

    [[nodiscard]] bool IsConnected(const df::net::PeerId peerId) const override
    {
        return peerId == 1u && shared_->connected;
    }

    void Pump() override
    {
        if (!server_)
        {
            return;
        }

        ++shared_->hostPumpCount;
        auto delayedIter = shared_->delayedToClient.begin();
        while (delayedIter != shared_->delayedToClient.end())
        {
            if (delayedIter->deliverAtHostPump > shared_->hostPumpCount)
            {
                ++delayedIter;
                continue;
            }

            shared_->toClient.push_back(std::move(delayedIter->packet));
            delayedIter = shared_->delayedToClient.erase(delayedIter);
        }
    }

    [[nodiscard]] bool Send(const df::net::TransportPacket& packet) override
    {
        if (!shared_->connected || packet.peerId != 1u)
        {
            return false;
        }

        if (server_)
        {
            if (packet.channel == df::net::TransportChannel::State && shared_->serverToClientStateDelayTicks > 0)
            {
                shared_->delayedToClient.push_back({
                    shared_->hostPumpCount + static_cast<std::uint64_t>(shared_->serverToClientStateDelayTicks),
                    packet,
                });
            }
            else
            {
                shared_->toClient.push_back(packet);
            }
        }
        else
        {
            shared_->toServer.push_back(packet);
        }

        return true;
    }

    [[nodiscard]] std::vector<df::net::TransportPacket> Receive() override
    {
        std::vector<df::net::TransportPacket> drained;
        auto& queue = server_ ? shared_->toServer : shared_->toClient;
        drained.swap(queue);
        return drained;
    }

    [[nodiscard]] std::vector<df::net::PeerEvent> ConsumePeerEvents() override
    {
        std::vector<df::net::PeerEvent> drained;
        auto& queue = server_ ? shared_->serverEvents : shared_->clientEvents;
        drained.swap(queue);
        return drained;
    }

    [[nodiscard]] std::vector<df::net::PeerInfo> SnapshotPeers() const override
    {
        if (!shared_->wasConnected)
        {
            return {};
        }

        return {{
            .peerId = 1u,
            .status = shared_->connected ? df::net::ConnectionStatus::Connected : df::net::ConnectionStatus::Closed,
            .stats = {
                .pingMilliseconds = 1,
                .lossPercent = 0.0f,
                .connectionQuality = 1.0f,
            },
        }};
    }

    void Close(const df::net::PeerId peerId, int, const char* debugText) override
    {
        if (peerId != 1u || !shared_->wasConnected || shared_->closedEventSent)
        {
            return;
        }

        shared_->connected = false;
        shared_->closedEventSent = true;
        shared_->toServer.clear();
        shared_->toClient.clear();
        shared_->delayedToClient.clear();
        const std::string reason = debugText != nullptr ? debugText : "loopback closed";
        shared_->serverEvents.push_back({1u, df::net::ConnectionOrigin::Incoming, df::net::ConnectionStatus::Closed, reason});
        shared_->clientEvents.push_back({1u, df::net::ConnectionOrigin::Outgoing, df::net::ConnectionStatus::Closed, reason});
    }

private:
    std::shared_ptr<SharedState> shared_;
    bool server_ = false;
};

auto PumpSessionPair(
    df::net::SessionHost& host,
    df::net::SessionClient& client,
    const df::game::PlayerCommandFrame& command,
    const int steps = 1,
    const float dt = 1.0f / 60.0f) -> void
{
    for (int step = 0; step < steps; ++step)
    {
        client.Tick(dt, command);
        host.Tick(dt);
        client.Tick(dt, command);
    }
}
}

int main()
{
    bool allPassed = true;

    try
    {
        const auto FlushWorldMesh = [&](df::world::DemoWorld& world, const std::size_t maxPasses = 64u) -> bool
        {
            std::span<const df::render::ColorVertex3D> opaqueTriangles;
            std::span<const df::render::ColorVertex3D> translucentTriangles;
            std::vector<df::render::ColorVertex3D> debugLines;
            for (std::size_t pass = 0; pass < maxPasses; ++pass)
            {
                world.Tick(1.0f / 30.0f);
                world.GatherRenderGeometrySmoothed(opaqueTriangles, translucentTriangles, debugLines, false, false);
                if (world.DirtyChunkStateCount() == 0u)
                {
                    return true;
                }
            }

            df::LogError(
                "FlushWorldMesh did not converge. dirty_chunks=", world.DirtyChunkStateCount(),
                " tracked_chunks=", world.TrackedChunkStateCount(),
                " terrain_version=", world.TerrainMeshVersion(),
                " opaque_verts=", world.OpaqueTerrainVertexCount(),
                " translucent_verts=", world.TranslucentTerrainVertexCount());
            return world.DirtyChunkStateCount() == 0u;
        };

        {
            df::ByteWriter writer;
            writer.WritePod<std::uint32_t>(12345u);
            writer.WriteBool(true);
            writer.WriteString("wet_mud");

            df::ByteReader reader(writer.Span());
            allPassed &= Expect(reader.ReadPod<std::uint32_t>() == 12345u, "Serialization roundtrip lost the uint32.");
            allPassed &= Expect(reader.ReadBool(), "Serialization roundtrip lost the bool.");
            allPassed &= Expect(reader.ReadString() == "wet_mud", "Serialization roundtrip lost the string.");
            allPassed &= Expect(reader.Empty(), "Serialization roundtrip left trailing bytes.");
        }

        {
            df::net::ChunkDelta sourceDelta{};
            sourceDelta.chunk.x = 2;
            sourceDelta.chunk.y = -1;
            sourceDelta.chunk.z = 5;
            sourceDelta.entries.push_back({12u, df::world::MaterialId::DrySand});
            sourceDelta.entries.push_back({511u, df::world::MaterialId::BrittleConcrete});

            const auto encoded = df::net::EncodeChunkDelta(sourceDelta);
            const df::net::ChunkDelta decoded = df::net::DecodeChunkDelta(encoded);
            allPassed &= Expect(decoded == sourceDelta, "Chunk delta packing roundtrip changed the payload.");
        }

        {
            df::net::SessionBrowserEntry sourceEntry{};
            sourceEntry.type = df::net::BrowserEntryType::DedicatedServer;
            sourceEntry.lobbyId = 111u;
            sourceEntry.ownerSteamId = 222u;
            sourceEntry.name = "Dedicated Frontier";
            sourceEntry.summary = "Persistent world";
            sourceEntry.address = "127.0.0.1";
            sourceEntry.port = 27035u;
            sourceEntry.currentPlayers = 2;
            sourceEntry.maxPlayers = 8;
            sourceEntry.joinable = true;
            sourceEntry.dedicated = true;

            const auto encoded = df::net::EncodeBrowserEntry(sourceEntry);
            const auto decoded = df::net::DecodeBrowserEntry(encoded);
            allPassed &= Expect(decoded.type == sourceEntry.type, "Browser entry roundtrip lost the entry type.");
            allPassed &= Expect(decoded.ownerSteamId == sourceEntry.ownerSteamId, "Browser entry roundtrip lost the owner Steam ID.");
            allPassed &= Expect(decoded.name == sourceEntry.name, "Browser entry roundtrip lost the server name.");
            allPassed &= Expect(decoded.address == sourceEntry.address, "Browser entry roundtrip lost the server address.");
            allPassed &= Expect(decoded.port == sourceEntry.port, "Browser entry roundtrip lost the server port.");
            allPassed &= Expect(decoded.dedicated == sourceEntry.dedicated, "Browser entry roundtrip lost the dedicated flag.");
        }

        {
            df::world::MaterialField field;
            field.SetCell(0, 0, 0, df::world::MaterialId::DrySand);
            field.SetCell(40, 1, 1, df::world::MaterialId::WetMud);
            field.SetCell(-5, 2, 70, df::world::MaterialId::BrittleConcrete);

            const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "Don_Craft_material_field.bin";
            allPassed &= Expect(field.Save(tempFile), "Material field save failed.");

            df::world::MaterialField loadedField;
            allPassed &= Expect(loadedField.Load(tempFile), "Material field load failed.");
            allPassed &= Expect(loadedField.GetCell(0, 0, 0) == df::world::MaterialId::DrySand, "Loaded field lost the origin material.");
            allPassed &= Expect(loadedField.GetCell(40, 1, 1) == df::world::MaterialId::WetMud, "Loaded field lost the positive chunk material.");
            allPassed &= Expect(loadedField.GetCell(-5, 2, 70) == df::world::MaterialId::BrittleConcrete, "Loaded field lost the negative chunk material.");

            std::error_code removeError;
            std::filesystem::remove(tempFile, removeError);
        }

        {
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 24;
            settings.worldHeight = 18;
            settings.worldDepth = 24;
            settings.activeChunkSize = 8;
            settings.seed = 2026u;
            world.SetGenerationSettings(settings);
            world.Reset();
            world.EditCell(3, 4, 5, df::world::MaterialId::BrittleConcrete);

            const df::world::DenseWorldSnapshot snapshot = world.CaptureSnapshot();
            const df::net::WorldSnapshotMessage message{
                .serverTick = 77u,
                .world = snapshot,
            };
            const auto encoded = df::net::EncodeWorldSnapshotMessage(message);
            const auto decoded = df::net::DecodeWorldSnapshotMessage(encoded);

            allPassed &= Expect(decoded.serverTick == message.serverTick, "World snapshot message roundtrip lost the server tick.");
            allPassed &= Expect(decoded.world.settings.worldWidth == snapshot.settings.worldWidth, "World snapshot message roundtrip lost the world width.");
            allPassed &= Expect(decoded.world.cells == snapshot.cells, "World snapshot message roundtrip changed cell payload.");

            df::world::DemoWorld replica;
            allPassed &= Expect(replica.ApplySnapshot(decoded.world), "Applying a decoded world snapshot failed.");
            allPassed &= Expect(replica.MaterialAtCell(3, 4, 5) == df::world::MaterialId::BrittleConcrete, "Applied world snapshot lost the edited cell.");
        }

        {
            df::world::DemoWorld baselineWorld;
            df::world::WorldGenerationSettings settings = baselineWorld.GenerationSettings();
            settings.worldWidth = 24;
            settings.worldHeight = 18;
            settings.worldDepth = 24;
            settings.activeChunkSize = 8;
            baselineWorld.SetGenerationSettings(settings);
            baselineWorld.Reset();

            df::world::DemoWorld editedWorld;
            editedWorld.SetGenerationSettings(settings);
            editedWorld.Reset();
            editedWorld.EditCell(2, 3, 4, df::world::MaterialId::BrittleConcrete);
            editedWorld.EditCell(7, 6, 5, df::world::MaterialId::DrySand);

            df::world::DenseWorldSnapshot baselineSnapshot = baselineWorld.CaptureSnapshot();
            const df::world::DenseWorldSnapshot editedSnapshot = editedWorld.CaptureSnapshot();
            const auto deltas = df::net::BuildChunkDeltas(baselineSnapshot, editedSnapshot);

            allPassed &= Expect(!deltas.empty(), "Chunk delta generation produced no changes for edited terrain.");
            allPassed &= Expect(df::net::ApplyChunkDeltas(baselineSnapshot, deltas), "Applying generated chunk deltas failed.");
            allPassed &= Expect(baselineSnapshot.cells == editedSnapshot.cells, "Applying generated chunk deltas did not reproduce the edited world.");
        }

        {
            const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "Don_Craft_session_runtime.bin";

            df::game::SessionRuntime runtime;
            df::game::SessionRuntime::Config config{};
            config.mode = df::game::SessionMode::Offline;
            config.savePath = tempFile;
            config.loadExistingWorld = false;
            config.autosaveEnabled = false;
            config.generationSettings.worldWidth = 24;
            config.generationSettings.worldHeight = 18;
            config.generationSettings.worldDepth = 24;
            config.generationSettings.activeChunkSize = 8;
            runtime.Initialize(config);
            runtime.MutableWorld().EditCell(4, 5, 6, df::world::MaterialId::BrittleConcrete);
            allPassed &= Expect(runtime.SaveNow(), "Session runtime save failed.");
            runtime.Shutdown();

            df::game::SessionRuntime loadedRuntime;
            config.loadExistingWorld = true;
            loadedRuntime.Initialize(config);
            allPassed &= Expect(loadedRuntime.World().MaterialAtCell(4, 5, 6) == df::world::MaterialId::BrittleConcrete, "Session runtime reload lost the saved terrain edit.");
            loadedRuntime.Shutdown();

            std::error_code removeError;
            std::filesystem::remove(tempFile, removeError);
        }

        {
            const auto shared = std::make_shared<LoopbackTransport::SharedState>();
            auto serverTransport = std::make_unique<LoopbackTransport>(shared, true);
            auto clientTransport = std::make_unique<LoopbackTransport>(shared, false);
            LoopbackTransport* const clientTransportRaw = clientTransport.get();
            clientTransport->Connect();

            df::net::SessionHost host;
            df::net::SessionHost::Config hostConfig{};
            hostConfig.runtime.mode = df::game::SessionMode::ListenHost;
            hostConfig.runtime.autosaveEnabled = false;
            hostConfig.runtime.loadExistingWorld = false;
            hostConfig.runtime.sessionName = "Loopback Listen";
            hostConfig.runtime.generationSettings.worldWidth = 24;
            hostConfig.runtime.generationSettings.worldHeight = 18;
            hostConfig.runtime.generationSettings.worldDepth = 24;
            hostConfig.runtime.generationSettings.activeChunkSize = 8;
            hostConfig.localHostPlayerId = 1u;
            hostConfig.localHostPlayerName = "Host";
            host.Initialize(hostConfig, std::move(serverTransport));

            df::net::SessionClient client;
            client.Initialize({.playerName = "Client"}, std::move(clientTransport));

            df::game::PlayerCommandFrame idleCommand{};
            PumpSessionPair(host, client, idleCommand, 12);
            allPassed &= Expect(client.IsReady(), "Listen-host loopback client never became ready.");
            allPassed &= Expect(client.ActorFrame().players.size() >= 2u, "Listen-host loopback actor frame did not include host and client players.");

            const df::game::SessionRuntime::PlayerState* const remotePlayer = host.Runtime().FindPlayer(2u);
            allPassed &= Expect(remotePlayer != nullptr, "Listen-host loopback never assigned a remote player ID.");
            const df::Vec3 startPosition = remotePlayer != nullptr ? remotePlayer->controller.Position() : df::Vec3{};

            df::game::PlayerCommandFrame moveCommand{};
            moveCommand.control.moveForward = true;
            for (std::uint32_t sequence = 1u; sequence <= 24u; ++sequence)
            {
                moveCommand.sequence = sequence;
                PumpSessionPair(host, client, moveCommand, 1);
            }

            const df::game::SessionRuntime::PlayerState* const movedPlayer = host.Runtime().FindPlayer(2u);
            allPassed &= Expect(movedPlayer != nullptr, "Listen-host loopback lost the remote player after movement replication.");
            allPassed &= Expect(movedPlayer != nullptr && df::LengthSquared(movedPlayer->controller.Position() - startPosition) > 0.001f, "Listen-host loopback did not advance the authoritative remote player.");

            df::game::PlayerCommandFrame hostMoveCommand{};
            hostMoveCommand.sequence = 500u;
            hostMoveCommand.control.moveRight = true;
            df::game::PlayerCommandFrame syncCommand{};
            syncCommand.sequence = 100u;
            for (int step = 0; step < 6; ++step)
            {
                host.MutableRuntime().SubmitCommand(1u, hostMoveCommand);
                syncCommand.sequence += 1u;
                PumpSessionPair(host, client, syncCommand, 1);
            }
            const df::net::ActorSnapshot* const latestRemoteActor = FindActorById(client.ActorFrame(), 1u);
            const df::net::ActorSnapshotFrame interpolatedFrame = client.BuildRenderActorFrame(0.0f);
            const df::net::ActorSnapshot* const interpolatedRemoteActor = FindActorById(interpolatedFrame, 1u);
            allPassed &= Expect(latestRemoteActor != nullptr && interpolatedRemoteActor != nullptr, "Listen-host interpolation test could not find the remote host actor.");
            allPassed &= Expect(
                latestRemoteActor != nullptr &&
                interpolatedRemoteActor != nullptr &&
                df::LengthSquared(latestRemoteActor->position - interpolatedRemoteActor->position) > 0.0001f,
                "Listen-host remote interpolation did not produce a lagged render snapshot.");
            host.MutableRuntime().SubmitCommand(1u, {});

            shared->serverToClientStateDelayTicks = 4;
            df::game::PlayerCommandFrame delayedMoveCommand{};
            delayedMoveCommand.control.moveForward = true;
            for (std::uint32_t sequence = 200u; sequence < 210u; ++sequence)
            {
                delayedMoveCommand.sequence = sequence;
                PumpSessionPair(host, client, delayedMoveCommand, 1);
            }

            const df::net::ActorSnapshot* const authoritativeLocalActor = FindActorById(client.ActorFrame(), client.LocalPlayerId());
            const df::game::PlayerController* const predictedLocalDuringDelay = client.PredictedLocalPlayer();
            allPassed &= Expect(authoritativeLocalActor != nullptr, "Listen-host reconciliation test could not find the authoritative local actor.");
            allPassed &= Expect(predictedLocalDuringDelay != nullptr, "Listen-host reconciliation test lost the predicted local player.");
            allPassed &= Expect(
                authoritativeLocalActor != nullptr &&
                predictedLocalDuringDelay != nullptr &&
                df::LengthSquared(predictedLocalDuringDelay->Position() - authoritativeLocalActor->position) > 0.001f,
                "Listen-host reconciliation test did not retain unacknowledged local movement while actor snapshots were delayed.");

            shared->serverToClientStateDelayTicks = 0;
            df::game::PlayerCommandFrame flushCommand{};
            for (std::uint32_t sequence = 210u; sequence < 226u; ++sequence)
            {
                flushCommand.sequence = sequence;
                PumpSessionPair(host, client, flushCommand, 1);
            }

            const df::net::ActorSnapshot* const correctedLocalActor = FindActorById(client.ActorFrame(), client.LocalPlayerId());
            const df::game::PlayerController* const predictedLocalAfterFlush = client.PredictedLocalPlayer();
            allPassed &= Expect(correctedLocalActor != nullptr, "Listen-host reconciliation flush lost the authoritative local actor.");
            allPassed &= Expect(predictedLocalAfterFlush != nullptr, "Listen-host reconciliation flush lost the predicted local player.");
            allPassed &= Expect(
                correctedLocalActor != nullptr &&
                predictedLocalAfterFlush != nullptr &&
                df::LengthSquared(predictedLocalAfterFlush->Position() - correctedLocalActor->position) < 0.01f,
                "Listen-host reconciliation did not converge after the delayed authoritative snapshots were delivered.");

            host.MutableRuntime().MutableWorld().EditCell(2, 3, 4, df::world::MaterialId::BrittleConcrete);
            PumpSessionPair(host, client, idleCommand, 6);
            allPassed &= Expect(client.World().MaterialAtCell(2, 3, 4) == df::world::MaterialId::BrittleConcrete, "Listen-host loopback client did not receive terrain edits.");

            clientTransportRaw->Close(1u, 0, "loopback closed");
            PumpSessionPair(host, client, idleCommand, 2);
            allPassed &= Expect(client.Disconnected(), "Listen-host loopback client did not observe disconnect.");
            allPassed &= Expect(host.Runtime().Players().size() == 1u, "Listen-host loopback host did not remove the disconnected remote player.");

            clientTransportRaw->Connect();
            for (std::uint32_t sequence = 300u; sequence < 314u; ++sequence)
            {
                idleCommand.sequence = sequence;
                PumpSessionPair(host, client, idleCommand, 1);
            }
            allPassed &= Expect(client.IsReady(), "Listen-host loopback client did not recover after reconnect.");
            allPassed &= Expect(!client.Disconnected(), "Listen-host loopback client stayed disconnected after reconnect.");
            allPassed &= Expect(host.Runtime().Players().size() == 2u, "Listen-host loopback host did not restore the reconnecting remote player.");

            host.MutableRuntime().MutableWorld().EditCell(4, 4, 4, df::world::MaterialId::DrySand);
            PumpSessionPair(host, client, idleCommand, 6);
            allPassed &= Expect(client.World().MaterialAtCell(4, 4, 4) == df::world::MaterialId::DrySand, "Listen-host reconnect path did not resume terrain replication.");

            client.Shutdown();
            host.Shutdown();
        }

        {
            const auto shared = std::make_shared<LoopbackTransport::SharedState>();
            auto serverTransport = std::make_unique<LoopbackTransport>(shared, true);
            auto clientTransport = std::make_unique<LoopbackTransport>(shared, false);
            clientTransport->Connect();

            df::net::SessionHost host;
            df::net::SessionHost::Config hostConfig{};
            hostConfig.runtime.mode = df::game::SessionMode::DedicatedServer;
            hostConfig.runtime.autosaveEnabled = false;
            hostConfig.runtime.loadExistingWorld = false;
            hostConfig.runtime.sessionName = "Loopback Dedicated";
            hostConfig.runtime.generationSettings.worldWidth = 24;
            hostConfig.runtime.generationSettings.worldHeight = 18;
            hostConfig.runtime.generationSettings.worldDepth = 24;
            hostConfig.runtime.generationSettings.activeChunkSize = 8;
            host.Initialize(hostConfig, std::move(serverTransport));

            df::net::SessionClient client;
            client.Initialize({.playerName = "Client"}, std::move(clientTransport));

            df::game::PlayerCommandFrame idleCommand{};
            PumpSessionPair(host, client, idleCommand, 12);
            allPassed &= Expect(client.IsReady(), "Dedicated loopback client never became ready.");
            allPassed &= Expect(client.ActorFrame().players.size() == 1u, "Dedicated loopback actor frame did not contain exactly the connected player.");
            allPassed &= Expect(client.SessionMode() == df::game::SessionMode::DedicatedServer, "Dedicated loopback welcome used the wrong session mode.");

            host.MutableRuntime().MutableWorld().EditCell(6, 4, 5, df::world::MaterialId::DrySand);
            PumpSessionPair(host, client, idleCommand, 6);
            allPassed &= Expect(client.World().MaterialAtCell(6, 4, 5) == df::world::MaterialId::DrySand, "Dedicated loopback client did not receive terrain deltas.");

            client.Shutdown();
            host.Shutdown();
        }

        {
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 64;
            settings.worldHeight = 28;
            settings.worldDepth = 72;
            settings.activeChunkSize = 9;
            settings.seed = 424242u;
            settings.cellSize = 0.40f;
            settings.terrainRelief = 1.35f;
            settings.waterLevel = 0.42f;
            world.SetGenerationSettings(settings);
            world.Reset();
            world.EditCell(4, 9, 4, df::world::MaterialId::BrittleConcrete);

            const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "Don_Craft_demo_world.bin";
            allPassed &= Expect(world.Save(tempFile), "Demo world save failed.");

            df::world::DemoWorld loadedWorld;
            allPassed &= Expect(loadedWorld.Load(tempFile), "Demo world load failed.");
            allPassed &= Expect(loadedWorld.GenerationSettings().worldWidth == settings.worldWidth, "Demo world load lost the world width setting.");
            allPassed &= Expect(loadedWorld.GenerationSettings().worldHeight == settings.worldHeight, "Demo world load lost the world height setting.");
            allPassed &= Expect(loadedWorld.GenerationSettings().worldDepth == settings.worldDepth, "Demo world load lost the world depth setting.");
            allPassed &= Expect(loadedWorld.GenerationSettings().activeChunkSize == settings.activeChunkSize, "Demo world load lost the active chunk size setting.");
            allPassed &= Expect(std::abs(loadedWorld.GenerationSettings().cellSize - settings.cellSize) < 0.0001f, "Demo world load lost the cell scale setting.");
            allPassed &= Expect(loadedWorld.GenerationSettings().seed == settings.seed, "Demo world load lost the generation seed.");
            allPassed &= Expect(loadedWorld.MaterialAtCell(4, 9, 4) == df::world::MaterialId::BrittleConcrete, "Demo world load lost the edited cell.");

            std::error_code removeError;
            std::filesystem::remove(tempFile, removeError);
        }

        {
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 192;
            settings.worldHeight = 96;
            settings.worldDepth = 192;
            settings.activeChunkSize = 1;
            world.SetGenerationSettings(settings);
            world.Reset();

            allPassed &= Expect(world.ActiveChunkSizeCells() >= 8, "Demo world accepted an unsafe active chunk size below the large-world stability floor.");
        }

        {
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 48;
            settings.worldHeight = 20;
            settings.worldDepth = 48;
            settings.activeChunkSize = 8;
            world.SetGenerationSettings(settings);
            world.Reset();

            allPassed &= Expect(FlushWorldMesh(world), "Initial mesh rebuild did not converge for the baseline world.");
            const std::size_t baselineTrackedChunks = world.TrackedChunkStateCount();

            world.EditCell(1, settings.worldHeight - 2, 1, df::world::MaterialId::BrittleConcrete);
            allPassed &= Expect(FlushWorldMesh(world), "Mesh rebuild did not converge after inserting an isolated high-altitude chunk.");
            allPassed &= Expect(world.TrackedChunkStateCount() > baselineTrackedChunks, "Adding an isolated material chunk did not create a tracked runtime chunk.");

            world.EditCell(1, settings.worldHeight - 2, 1, df::world::MaterialId::Air);
            world.Tick(3.0f);
            allPassed &= Expect(FlushWorldMesh(world), "Mesh rebuild did not converge after removing an isolated high-altitude chunk.");
            allPassed &= Expect(world.TrackedChunkStateCount() == baselineTrackedChunks, "Retired empty chunk runtime state leaked after its mesh and activity lifetime completed.");
        }

        {
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 48;
            settings.worldHeight = 20;
            settings.worldDepth = 48;
            settings.activeChunkSize = 8;
            world.SetGenerationSettings(settings);
            world.Reset();

            allPassed &= Expect(FlushWorldMesh(world), "Baseline world mesh rebuild did not converge before edit leak validation.");
            const std::size_t trackedBeforeBoundaryEdit = world.TrackedChunkStateCount();

            world.EditCell(29, 11, 24, df::world::MaterialId::Air);
            allPassed &= Expect(FlushWorldMesh(world), "Mesh rebuild did not converge after removing a wall-top cell.");
            allPassed &= Expect(world.TrackedChunkStateCount() <= trackedBeforeBoundaryEdit + 1u, "Boundary edits spawned too many empty neighbor chunk states.");
        }

        {
            df::sim::ReferenceMpm2D referenceMpm(8, 8, 0.5f, 1.0f / 60.0f);
            df::sim::ReferenceMpm2D::Particle referenceParticle{};
            referenceParticle.position = {1.5f, 2.5f};
            referenceParticle.velocity = {0.0f, 0.0f};
            referenceParticle.mass = 2.0f;
            referenceMpm.AddParticle(referenceParticle);

            const float totalParticleMass = referenceMpm.TotalParticleMass();
            referenceMpm.Step();

            allPassed &= Expect(std::abs(referenceMpm.TotalGridMass() - totalParticleMass) < 0.0001f, "Reference MPM lost mass during particle-to-grid transfer.");
            allPassed &= Expect(!referenceMpm.Particles().empty(), "Reference MPM lost its particles.");
            allPassed &= Expect(referenceMpm.Particles().front().velocity.y < 0.0f, "Reference MPM did not integrate gravity.");

            df::sim::VulkanMpm2D vulkanMpm(8, 8, 0.5f, 1.0f / 60.0f);
            df::sim::VulkanMpm2D::Particle vulkanParticle{};
            vulkanParticle.position = {1.5f, 2.5f};
            vulkanParticle.velocity = {0.0f, 0.0f};
            vulkanParticle.mass = 2.0f;
            vulkanMpm.AddParticle(vulkanParticle);

            if (vulkanMpm.IsAvailable())
            {
                vulkanMpm.Step();

                allPassed &= Expect(std::abs(vulkanMpm.TotalGridMass() - totalParticleMass) < 0.0001f, "Vulkan MPM lost mass during particle-to-grid transfer.");
                allPassed &= Expect(!vulkanMpm.Particles().empty(), "Vulkan MPM lost its particles.");
                allPassed &= Expect(vulkanMpm.Particles().front().velocity.y < 0.0f, "Vulkan MPM did not integrate gravity.");
                allPassed &= Expect(std::abs(vulkanMpm.Particles().front().position.x - referenceMpm.Particles().front().position.x) < 0.0001f, "Vulkan MPM diverged from the reference particle X position.");
                allPassed &= Expect(std::abs(vulkanMpm.Particles().front().position.y - referenceMpm.Particles().front().position.y) < 0.0001f, "Vulkan MPM diverged from the reference particle Y position.");
                allPassed &= Expect(std::abs(vulkanMpm.Particles().front().velocity.x - referenceMpm.Particles().front().velocity.x) < 0.0001f, "Vulkan MPM diverged from the reference particle X velocity.");
                allPassed &= Expect(std::abs(vulkanMpm.Particles().front().velocity.y - referenceMpm.Particles().front().velocity.y) < 0.0001f, "Vulkan MPM diverged from the reference particle Y velocity.");
            }
            else
            {
                df::LogWarning("Vulkan MPM self-check skipped: ", vulkanMpm.FailureMessage());
            }
        }
    }
    catch (const std::exception& error)
    {
        df::LogError("Self-check threw an exception: ", error.what());
        allPassed = false;
    }

    if (allPassed)
    {
        df::LogInfo("All DonCraft self-checks passed.");
        return 0;
    }

    return 1;
}
