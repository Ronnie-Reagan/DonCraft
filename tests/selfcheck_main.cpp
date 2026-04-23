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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
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
        int clientReliableSendFailuresRemaining = 0;
        int clientUnreliableSendFailuresRemaining = 0;
        int serverToClientStateDelayTicks = 0;
        std::uint32_t clientHelloAttempts = 0;
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
            try
            {
                const df::net::DecodedMessage message = df::net::DecodeMessage(packet.payload);
                if (message.type == df::net::MessageType::ClientHello)
                {
                    ++shared_->clientHelloAttempts;
                }
            }
            catch (const std::exception&)
            {
            }

            if (packet.reliable && shared_->clientReliableSendFailuresRemaining > 0)
            {
                --shared_->clientReliableSendFailuresRemaining;
                return false;
            }
            if (!packet.reliable && shared_->clientUnreliableSendFailuresRemaining > 0)
            {
                --shared_->clientUnreliableSendFailuresRemaining;
                return true;
            }
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

        const auto FlushWorldMeshWithoutSimulation = [&](df::world::DemoWorld& world, const std::size_t maxPasses = 64u) -> bool
        {
            std::span<const df::render::ColorVertex3D> opaqueTriangles;
            std::span<const df::render::ColorVertex3D> translucentTriangles;
            std::vector<df::render::ColorVertex3D> debugLines;
            for (std::size_t pass = 0; pass < maxPasses; ++pass)
            {
                world.GatherRenderGeometrySmoothed(opaqueTriangles, translucentTriangles, debugLines, false, false);
                if (world.DirtyChunkStateCount() == 0u)
                {
                    return true;
                }
            }

            df::LogError(
                "FlushWorldMeshWithoutSimulation did not converge. dirty_chunks=", world.DirtyChunkStateCount(),
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
            std::vector<df::game::PlayerCommandFrame> sourceCommands;
            df::game::PlayerCommandFrame firstCommand{};
            firstCommand.sequence = 7u;
            firstCommand.control.moveForward = true;
            firstCommand.control.lookYawDelta = 3.0f;
            firstCommand.cumulativeLookYawDelta = 17.0f;
            firstCommand.cumulativeLookPitchDelta = -4.0f;
            firstCommand.hasCumulativeLook = true;
            firstCommand.primaryPressCount = 1u;
            sourceCommands.push_back(firstCommand);

            df::game::PlayerCommandFrame secondCommand = firstCommand;
            secondCommand.sequence = 8u;
            secondCommand.control.lookYawDelta = 1.0f;
            secondCommand.cumulativeLookYawDelta = 18.0f;
            secondCommand.reloadPressCount = 1u;
            sourceCommands.push_back(secondCommand);

            const auto encodedBundle = df::net::EncodeCommandBundle(std::span<const df::game::PlayerCommandFrame>(
                sourceCommands.data(),
                sourceCommands.size()));
            const auto decodedBundle = df::net::DecodeCommandBundle(encodedBundle);
            allPassed &= Expect(decodedBundle.size() == sourceCommands.size(), "Command bundle roundtrip lost command frames.");
            allPassed &= Expect(decodedBundle.back().sequence == 8u, "Command bundle roundtrip lost the newest sequence.");
            allPassed &= Expect(std::abs(decodedBundle.front().cumulativeLookYawDelta - 17.0f) < 0.0001f, "Command bundle roundtrip lost cumulative mouse yaw.");
            allPassed &= Expect(decodedBundle.front().hasCumulativeLook, "Command bundle roundtrip lost cumulative mouse support metadata.");
            allPassed &= Expect(decodedBundle.back().reloadPressCount == 1u, "Command bundle roundtrip lost edge recovery counters.");
        }

        {
            df::net::ActorSnapshotFrame sourceFrame{};
            sourceFrame.serverTick = 42u;
            sourceFrame.players.push_back({
                .id = 7u,
                .position = {1.0f, 2.0f, 3.0f},
                .cameraPosition = {1.0f, 2.5f, 3.0f},
                .forward = {0.0f, 0.0f, 1.0f},
                .flatForward = {0.0f, 0.0f, 1.0f},
                .tool = df::game::ToolType::Rifle,
                .crosshairMaterial = df::world::MaterialId::BrittleConcrete,
                .ammoInMagazine = 9,
                .reserveAmmo = 27,
                .lastAppliedCommandSequence = 123u,
            });
            sourceFrame.truck.position = {4.0f, 0.5f, -2.0f};
            sourceFrame.truck.forward = {1.0f, 0.0f, 0.0f};
            sourceFrame.audioCues.push_back({
                .position = {6.0f, 1.0f, -3.0f},
                .baseFrequency = 180.0f,
                .durationSeconds = 0.08f,
                .amplitude = 0.22f,
                .noise = 0.35f,
                .sweep = -24.0f,
            });

            const auto encoded = df::net::EncodeActorSnapshotFrame(sourceFrame);
            const auto decoded = df::net::DecodeActorSnapshotFrame(encoded);
            allPassed &= Expect(decoded.serverTick == sourceFrame.serverTick, "Actor snapshot frame roundtrip lost the server tick.");
            allPassed &= Expect(decoded.players.size() == 1u, "Actor snapshot frame roundtrip lost the player list.");
            allPassed &= Expect(decoded.audioCues.size() == 1u, "Actor snapshot frame roundtrip lost the audio cue list.");
            allPassed &= Expect(
                decoded.audioCues.size() == 1u &&
                std::abs(decoded.audioCues.front().position.x - sourceFrame.audioCues.front().position.x) < 0.0001f &&
                std::abs(decoded.audioCues.front().position.y - sourceFrame.audioCues.front().position.y) < 0.0001f &&
                std::abs(decoded.audioCues.front().position.z - sourceFrame.audioCues.front().position.z) < 0.0001f,
                "Actor snapshot frame roundtrip changed the audio cue position.");
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
            const auto segments = df::net::BuildWorldSnapshotMessages(77u, snapshot, 257u);
            allPassed &= Expect(segments.size() > 1u, "World snapshot segmentation should have produced multiple messages for a bounded payload.");

            df::world::DenseWorldSnapshot decodedSnapshot{};
            std::uint64_t decodedServerTick = 0u;
            std::size_t nextCellOffset = 0u;
            for (const df::net::WorldSnapshotMessage& segment : segments)
            {
                const auto encoded = df::net::EncodeWorldSnapshotMessage(segment);
                const auto decoded = df::net::DecodeWorldSnapshotMessage(encoded);
                const auto decodedCells = df::net::DecodeWorldSnapshotCells(decoded);

                if (decoded.cellOffset == 0u)
                {
                    decodedSnapshot.settings = decoded.settings;
                    decodedSnapshot.cells.assign(decoded.totalCellCount, 0u);
                    decodedServerTick = decoded.serverTick;
                    nextCellOffset = 0u;
                }

                allPassed &= Expect(decoded.serverTick == 77u, "World snapshot message roundtrip lost the server tick.");
                allPassed &= Expect(static_cast<std::size_t>(decoded.cellOffset) == nextCellOffset, "World snapshot message roundtrip changed the segment offset.");
                allPassed &= Expect(decoded.cells.size() <= 257u, "World snapshot segment exceeded the requested payload budget.");
                allPassed &= Expect(decodedCells.size() == decoded.decodedCellCount, "World snapshot segment decoded to an unexpected number of cells.");

                std::copy(
                    decodedCells.begin(),
                    decodedCells.end(),
                    decodedSnapshot.cells.begin() + static_cast<std::ptrdiff_t>(decoded.cellOffset));
                nextCellOffset += decodedCells.size();
            }

            allPassed &= Expect(decodedServerTick == 77u, "World snapshot message segmentation lost the transfer tick.");
            allPassed &= Expect(decodedSnapshot.settings.worldWidth == snapshot.settings.worldWidth, "World snapshot message roundtrip lost the world width.");
            allPassed &= Expect(decodedSnapshot.cells == snapshot.cells, "World snapshot message roundtrip changed cell payload.");

            df::world::DemoWorld replica;
            allPassed &= Expect(replica.ApplySnapshot(decodedSnapshot), "Applying a decoded world snapshot failed.");
            allPassed &= Expect(replica.MaterialAtCell(3, 4, 5) == df::world::MaterialId::BrittleConcrete, "Applied world snapshot lost the edited cell.");
        }

        {
            df::world::DenseWorldSnapshot snapshot{};
            snapshot.settings.worldWidth = 64;
            snapshot.settings.worldHeight = 1;
            snapshot.settings.worldDepth = 1;
            snapshot.settings.activeChunkSize = 8;
            snapshot.cells.assign(64u, static_cast<std::uint8_t>(df::world::MaterialId::CompactedSoil));

            const auto segments = df::net::BuildWorldSnapshotMessages(91u, snapshot, 32u);
            allPassed &= Expect(!segments.empty(), "RLE snapshot test produced no world snapshot segments.");
            allPassed &= Expect(
                std::all_of(segments.begin(), segments.end(), [](const df::net::WorldSnapshotMessage& segment)
                {
                    return segment.encoding == df::net::WorldSnapshotMessage::Encoding::Rle &&
                           segment.cells.size() < segment.decodedCellCount;
                }),
                "World snapshot RLE compression did not engage for a long repeated run.");
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
            editedWorld.EditCell(10, 2, 12, df::world::MaterialId::WetMud);
            editedWorld.EditCell(18, 9, 4, df::world::MaterialId::Grass);
            editedWorld.EditCell(21, 12, 19, df::world::MaterialId::BasaltRock);

            df::world::DenseWorldSnapshot baselineSnapshot = baselineWorld.CaptureSnapshot();
            const df::world::DenseWorldSnapshot editedSnapshot = editedWorld.CaptureSnapshot();
            const auto deltas = df::net::BuildChunkDeltas(baselineSnapshot, editedSnapshot);
            const auto batches = df::net::BuildChunkDeltaBatches(deltas, 88u, 64u);

            allPassed &= Expect(!deltas.empty(), "Chunk delta generation produced no changes for edited terrain.");
            allPassed &= Expect(batches.size() > 1u, "Chunk delta batching should have split changes into multiple payloads.");
            for (const df::net::ChunkDeltaBatchMessage& batch : batches)
            {
                allPassed &= Expect(df::net::ApplyChunkDeltas(baselineSnapshot, batch.deltas), "Applying generated chunk delta batch failed.");
            }
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
            const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "Don_Craft_session_runtime_new_over_existing.bin";

            df::game::SessionRuntime savedRuntime;
            df::game::SessionRuntime::Config savedConfig{};
            savedConfig.mode = df::game::SessionMode::Offline;
            savedConfig.savePath = tempFile;
            savedConfig.loadExistingWorld = false;
            savedConfig.autosaveEnabled = false;
            savedConfig.generationSettings.worldWidth = 24;
            savedConfig.generationSettings.worldHeight = 18;
            savedConfig.generationSettings.worldDepth = 24;
            savedConfig.generationSettings.activeChunkSize = 8;
            savedConfig.generationSettings.seed = 111u;
            savedRuntime.Initialize(savedConfig);
            savedRuntime.MutableWorld().EditCell(4, 5, 6, df::world::MaterialId::BrittleConcrete);
            allPassed &= Expect(savedRuntime.SaveNow(), "Session runtime failed to create the existing-save fixture.");
            savedRuntime.Shutdown();

            df::game::SessionRuntime newRuntime;
            df::game::SessionRuntime::Config newConfig{};
            newConfig.mode = df::game::SessionMode::Offline;
            newConfig.savePath = tempFile;
            newConfig.loadExistingWorld = false;
            newConfig.autosaveEnabled = false;
            newConfig.generationSettings.worldWidth = 36;
            newConfig.generationSettings.worldHeight = 14;
            newConfig.generationSettings.worldDepth = 32;
            newConfig.generationSettings.activeChunkSize = 8;
            newConfig.generationSettings.seed = 222u;
            newRuntime.Initialize(newConfig);
            allPassed &= Expect(newRuntime.World().WorldWidthCells() == 36, "New-world startup loaded an existing save width instead of selected settings.");
            allPassed &= Expect(newRuntime.World().WorldHeightCells() == 14, "New-world startup loaded an existing save height instead of selected settings.");
            allPassed &= Expect(newRuntime.World().WorldDepthCells() == 32, "New-world startup loaded an existing save depth instead of selected settings.");
            allPassed &= Expect(newRuntime.World().GenerationSettings().seed == 222u, "New-world startup loaded an existing save seed instead of selected settings.");
            allPassed &= Expect(
                newRuntime.World().MaterialAtCell(4, 5, 6) != df::world::MaterialId::BrittleConcrete,
                "New-world startup preserved an edited cell from the existing save.");
            newRuntime.Shutdown();

            std::error_code removeError;
            std::filesystem::remove(tempFile, removeError);
        }

        {
            df::game::SessionRuntime runtime;
            df::game::SessionRuntime::Config config{};
            config.mode = df::game::SessionMode::Offline;
            config.autosaveEnabled = false;
            config.loadExistingWorld = false;
            config.maxPlayers = 4;
            runtime.Initialize(config);
            allPassed &= Expect(runtime.AddPlayer(1u, "SequenceTest"), "Session runtime sequence test failed to add a player.");

            df::game::PlayerCommandFrame forwardCommand{};
            forwardCommand.sequence = 10u;
            forwardCommand.selectedTool = df::game::ToolType::Rifle;
            forwardCommand.control.moveForward = true;
            const bool acceptedForward = runtime.SubmitCommand(1u, forwardCommand);

            df::game::PlayerCommandFrame staleCommand = forwardCommand;
            staleCommand.sequence = 9u;
            staleCommand.control.moveForward = false;
            staleCommand.control.moveBackward = true;
            const bool acceptedStale = runtime.SubmitCommand(1u, staleCommand);

            const df::game::SessionRuntime::PlayerState* const player = runtime.FindPlayer(1u);
            allPassed &= Expect(acceptedForward, "Session runtime rejected the first authoritative command.");
            allPassed &= Expect(!acceptedStale, "Session runtime accepted an out-of-order stale command.");
            allPassed &= Expect(player != nullptr && player->command.sequence == 10u, "Session runtime stale command handling rolled back the current command sequence.");
            allPassed &= Expect(player != nullptr && player->command.control.moveForward, "Session runtime stale command handling replaced the active control state.");
            const float yawBeforeLookRecovery = player != nullptr ? player->controller.YawRadians() : 0.0f;
            df::game::PlayerCommandFrame recoveredLookCommand{};
            recoveredLookCommand.sequence = 11u;
            recoveredLookCommand.control.lookYawDelta = 3.0f;
            recoveredLookCommand.cumulativeLookYawDelta = 11.0f;
            recoveredLookCommand.hasCumulativeLook = true;
            const bool acceptedRecoveredLook = runtime.SubmitCommand(1u, recoveredLookCommand);
            runtime.Tick(1.0f / 60.0f);
            const df::game::SessionRuntime::PlayerState* const playerAfterLookRecovery = runtime.FindPlayer(1u);
            const float recoveredYawDelta = playerAfterLookRecovery != nullptr
                ? playerAfterLookRecovery->controller.YawRadians() - yawBeforeLookRecovery
                : 0.0f;
            allPassed &= Expect(acceptedRecoveredLook, "Session runtime rejected the cumulative look recovery command.");
            allPassed &= Expect(
                std::abs(recoveredYawDelta - 11.0f * 0.0026f) < 0.0005f,
                "Session runtime did not recover mouse look from cumulative command deltas after a dropped input frame.");

            const float yawBeforeBundledLook = playerAfterLookRecovery != nullptr ? playerAfterLookRecovery->controller.YawRadians() : 0.0f;
            df::game::PlayerCommandFrame bundledLookCommand{};
            bundledLookCommand.sequence = 12u;
            bundledLookCommand.control.lookYawDelta = 9.0f;
            bundledLookCommand.cumulativeLookYawDelta = 20.0f;
            bundledLookCommand.hasCumulativeLook = true;
            const bool acceptedBundledLook = runtime.SubmitCommand(1u, bundledLookCommand);
            df::game::PlayerCommandFrame bundledLookFollowup = bundledLookCommand;
            bundledLookFollowup.sequence = 13u;
            bundledLookFollowup.control.lookYawDelta = 5.0f;
            bundledLookFollowup.cumulativeLookYawDelta = 25.0f;
            const bool acceptedBundledLookFollowup = runtime.SubmitCommand(1u, bundledLookFollowup);
            runtime.Tick(1.0f / 60.0f);
            const df::game::SessionRuntime::PlayerState* const playerAfterBundledLook = runtime.FindPlayer(1u);
            const float bundledLookYawDelta = playerAfterBundledLook != nullptr
                ? playerAfterBundledLook->controller.YawRadians() - yawBeforeBundledLook
                : 0.0f;
            allPassed &= Expect(acceptedBundledLook && acceptedBundledLookFollowup, "Session runtime rejected bundled cumulative look frames.");
            allPassed &= Expect(
                std::abs(bundledLookYawDelta - 14.0f * 0.0026f) < 0.0005f,
                "Session runtime did not aggregate cumulative mouse look when multiple command frames arrived before one tick.");

            if (df::game::SessionRuntime::PlayerState* const mutablePlayer = runtime.FindPlayer(1u))
            {
                mutablePlayer->tool = df::game::ToolType::Rifle;
                auto& rifleInventory = mutablePlayer->weaponInventories[df::game::ToToolIndex(df::game::ToolType::Rifle)];
                rifleInventory.ammoInMagazine = 0;
                rifleInventory.reserveAmmo = 5;
            }

            df::game::PlayerCommandFrame bundledEdgeCommand{};
            bundledEdgeCommand.sequence = 14u;
            bundledEdgeCommand.reloadPressCount = 1u;
            const bool acceptedBundledEdge = runtime.SubmitCommand(1u, bundledEdgeCommand);
            df::game::PlayerCommandFrame bundledFollowupCommand = bundledEdgeCommand;
            bundledFollowupCommand.sequence = 15u;
            const bool acceptedBundledFollowup = runtime.SubmitCommand(1u, bundledFollowupCommand);
            runtime.Tick(1.0f / 60.0f);
            const df::game::SessionRuntime::PlayerState* const playerAfterBundledEdge = runtime.FindPlayer(1u);
            allPassed &= Expect(acceptedBundledEdge && acceptedBundledFollowup, "Session runtime rejected bundled command edge frames.");
            allPassed &= Expect(
                playerAfterBundledEdge != nullptr && playerAfterBundledEdge->reloading,
                "Session runtime lost a one-shot input edge when multiple command frames arrived before one tick.");
            runtime.Shutdown();
        }

        {
            const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "Don_Craft_corrupt_world.bin";
            {
                std::ofstream output(tempFile, std::ios::binary);
                output << "not a valid world";
            }

            df::game::SessionRuntime runtime;
            df::game::SessionRuntime::Config config{};
            config.mode = df::game::SessionMode::Offline;
            config.savePath = tempFile;
            config.loadExistingWorld = true;
            config.autosaveEnabled = false;

            bool threw = false;
            try
            {
                runtime.Initialize(config);
            }
            catch (const std::exception&)
            {
                threw = true;
            }

            allPassed &= Expect(threw, "Session runtime did not fail fast on a corrupt existing save.");

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

            df::net::ActorSnapshot spoofedClientState{};
            spoofedClientState.id = client.LocalPlayerId();
            spoofedClientState.position = {999.0f, 888.0f, 777.0f};
            spoofedClientState.cameraPosition = spoofedClientState.position;
            spoofedClientState.forward = {0.0f, 0.0f, -1.0f};
            spoofedClientState.flatForward = {0.0f, 0.0f, -1.0f};
            spoofedClientState.tool = df::game::ToolType::Grenade;
            spoofedClientState.ammoInMagazine = 999;
            spoofedClientState.reserveAmmo = 999;
            spoofedClientState.reloading = true;
            const std::vector<std::byte> spoofedPayload = df::net::EncodeMessage(
                df::net::MessageType::ClientStateFrame,
                df::net::EncodeClientStateFrame(spoofedClientState));
            allPassed &= Expect(clientTransportRaw->Send({
                .peerId = 1u,
                .payload = spoofedPayload,
                .reliable = false,
                .channel = df::net::TransportChannel::State,
            }), "Listen-host spoof test failed to inject a forged client state frame.");
            host.Tick(1.0f / 60.0f);
            client.Tick(1.0f / 60.0f, idleCommand);
            const df::game::SessionRuntime::PlayerState* const authoritativeRemoteAfterSpoof = host.Runtime().FindPlayer(2u);
            allPassed &= Expect(
                authoritativeRemoteAfterSpoof != nullptr &&
                df::LengthSquared(authoritativeRemoteAfterSpoof->controller.Position() - spoofedClientState.position) > 1000.0f,
                "Listen-host accepted a forged client state position update.");
            allPassed &= Expect(
                authoritativeRemoteAfterSpoof != nullptr &&
                authoritativeRemoteAfterSpoof->tool != df::game::ToolType::Grenade,
                "Listen-host accepted a forged client inventory/tool update.");

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
                static_cast<void>(host.MutableRuntime().SubmitCommand(1u, hostMoveCommand));
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
            static_cast<void>(host.MutableRuntime().SubmitCommand(1u, {}));

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
            for (std::uint32_t sequence = 210u; sequence < 212u; ++sequence)
            {
                flushCommand.sequence = sequence;
                PumpSessionPair(host, client, flushCommand, 1);
            }

            const df::game::PlayerController* const renderedLocalDuringCorrection = client.RenderedLocalPlayer();
            const df::game::PlayerController* const predictedLocalDuringCorrection = client.PredictedLocalPlayer();
            allPassed &= Expect(renderedLocalDuringCorrection != nullptr, "Listen-host correction smoothing lost the rendered local player.");
            allPassed &= Expect(predictedLocalDuringCorrection != nullptr, "Listen-host correction smoothing lost the predicted local player.");
            allPassed &= Expect(
                renderedLocalDuringCorrection != nullptr &&
                predictedLocalDuringCorrection != nullptr &&
                df::LengthSquared(renderedLocalDuringCorrection->Position() - predictedLocalDuringCorrection->Position()) > 0.0001f,
                "Listen-host correction smoothing did not preserve a temporary visual offset after an authoritative correction.");

            for (std::uint32_t sequence = 212u; sequence < 226u; ++sequence)
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

            shared->serverToClientStateDelayTicks = 72;
            df::game::PlayerCommandFrame lagSpikeCommand{};
            lagSpikeCommand.control.moveLeft = true;
            for (std::uint32_t sequence = 400u; sequence < 520u; ++sequence)
            {
                lagSpikeCommand.sequence = sequence;
                PumpSessionPair(host, client, lagSpikeCommand, 1);
            }

            const df::game::PlayerController* const predictedLocalDuringLagSpike = client.PredictedLocalPlayer();
            const df::net::ActorSnapshot* const authoritativeLocalDuringLagSpike = FindActorById(client.ActorFrame(), client.LocalPlayerId());
            allPassed &= Expect(predictedLocalDuringLagSpike != nullptr, "Listen-host lag-spike test lost the predicted local player.");
            allPassed &= Expect(authoritativeLocalDuringLagSpike != nullptr, "Listen-host lag-spike test lost the authoritative local actor.");
            allPassed &= Expect(
                predictedLocalDuringLagSpike != nullptr &&
                authoritativeLocalDuringLagSpike != nullptr &&
                df::LengthSquared(predictedLocalDuringLagSpike->Position() - authoritativeLocalDuringLagSpike->position) > 0.001f,
                "Listen-host lag-spike test did not preserve a backlog of unacknowledged local commands.");

            shared->serverToClientStateDelayTicks = 0;
            df::game::PlayerCommandFrame lagSpikeFlushCommand{};
            for (std::uint32_t sequence = 520u; sequence < 700u; ++sequence)
            {
                lagSpikeFlushCommand.sequence = sequence;
                PumpSessionPair(host, client, lagSpikeFlushCommand, 1);
            }

            const df::game::PlayerController* const predictedLocalAfterLagSpike = client.PredictedLocalPlayer();
            const df::net::ActorSnapshot* const correctedLocalAfterLagSpike = FindActorById(client.ActorFrame(), client.LocalPlayerId());
            allPassed &= Expect(predictedLocalAfterLagSpike != nullptr, "Listen-host lag-spike flush lost the predicted local player.");
            allPassed &= Expect(correctedLocalAfterLagSpike != nullptr, "Listen-host lag-spike flush lost the authoritative local actor.");
            allPassed &= Expect(
                predictedLocalAfterLagSpike != nullptr &&
                correctedLocalAfterLagSpike != nullptr &&
                df::LengthSquared(predictedLocalAfterLagSpike->Position() - correctedLocalAfterLagSpike->position) < 0.02f,
                "Listen-host reconciliation did not converge after a command history backlog longer than the old 64-slot buffer.");

            allPassed &= Expect(
                FlushWorldMeshWithoutSimulation(client.MutableWorld()),
                "Listen-host client initial terrain mesh did not converge without local world simulation.");
            const std::uint64_t clientMeshVersionBeforeReplicatedEdit = client.World().TerrainMeshVersion();

            host.MutableRuntime().MutableWorld().EditCell(2, 3, 4, df::world::MaterialId::BrittleConcrete);
            PumpSessionPair(host, client, idleCommand, 6);
            allPassed &= Expect(client.World().MaterialAtCell(2, 3, 4) == df::world::MaterialId::BrittleConcrete, "Listen-host loopback client did not receive terrain edits.");
            {
                std::span<const df::render::ColorVertex3D> opaqueTriangles;
                std::span<const df::render::ColorVertex3D> translucentTriangles;
                std::vector<df::render::ColorVertex3D> debugLines;
                client.MutableWorld().GatherRenderGeometrySmoothed(opaqueTriangles, translucentTriangles, debugLines, false, false);
            }
            allPassed &= Expect(
                client.World().TerrainMeshVersion() > clientMeshVersionBeforeReplicatedEdit,
                "Listen-host loopback client did not rebuild the terrain mesh after a replicated edit.");
            allPassed &= Expect(
                client.World().DirtyChunkStateCount() == 0u,
                "Listen-host loopback client left replicated terrain edits dirty after render-only client ticks.");

            clientTransportRaw->Close(1u, 0, "loopback closed");
            PumpSessionPair(host, client, idleCommand, 2);
            allPassed &= Expect(client.Disconnected(), "Listen-host loopback client did not observe disconnect.");
            allPassed &= Expect(client.LastDisconnectText() == "loopback closed", "Listen-host disconnect reason was not preserved.");
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

            static_cast<void>(client.ConsumeAudioCues());
            df::game::PlayerCommandFrame fireCommand{};
            fireCommand.sequence = 350u;
            fireCommand.selectedTool = df::game::ToolType::Rifle;
            fireCommand.primaryPressed = true;
            fireCommand.primaryPressCount = 1u;
            PumpSessionPair(host, client, fireCommand, 3);
            const auto replicatedAudioCues = client.ConsumeAudioCues();
            allPassed &= Expect(!replicatedAudioCues.empty(), "Listen-host client did not receive replicated audio cues.");
            allPassed &= Expect(
                !replicatedAudioCues.empty() &&
                std::any_of(replicatedAudioCues.begin(), replicatedAudioCues.end(), [](const df::net::AudioCueSnapshot& cue)
                {
                    return std::isfinite(cue.position.x) &&
                           std::isfinite(cue.position.y) &&
                           std::isfinite(cue.position.z);
                }),
                "Listen-host client received an audio cue with an invalid position.");

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
            hostConfig.runtime.sessionName = "Input Recovery";
            hostConfig.runtime.generationSettings.worldWidth = 24;
            hostConfig.runtime.generationSettings.worldHeight = 18;
            hostConfig.runtime.generationSettings.worldDepth = 24;
            hostConfig.runtime.generationSettings.activeChunkSize = 8;
            host.Initialize(hostConfig, std::move(serverTransport));

            df::net::SessionClient client;
            client.Initialize({.playerName = "RecoverClient"}, std::move(clientTransport));

            df::game::PlayerCommandFrame idleCommand{};
            PumpSessionPair(host, client, idleCommand, 12);
            allPassed &= Expect(client.IsReady(), "Input recovery test client never became ready.");

            df::game::SessionRuntime::PlayerState* const remotePlayer = host.MutableRuntime().FindPlayer(client.LocalPlayerId());
            allPassed &= Expect(remotePlayer != nullptr, "Input recovery test could not find the authoritative remote player.");
            if (remotePlayer != nullptr)
            {
                remotePlayer->tool = df::game::ToolType::Rifle;
                auto& rifleInventory = remotePlayer->weaponInventories[df::game::ToToolIndex(df::game::ToolType::Rifle)];
                rifleInventory.ammoInMagazine = 0;
                rifleInventory.reserveAmmo = 5;
            }

            shared->clientUnreliableSendFailuresRemaining = 1;
            df::game::PlayerCommandFrame droppedReloadCommand{};
            droppedReloadCommand.sequence = 1u;
            droppedReloadCommand.selectedTool = df::game::ToolType::Rifle;
            droppedReloadCommand.reloadPressed = true;
            droppedReloadCommand.reloadPressCount = 1u;

            client.Tick(1.0f / 60.0f, droppedReloadCommand);
            host.Tick(1.0f / 60.0f);

            df::game::PlayerCommandFrame recoveredReloadCommand{};
            recoveredReloadCommand.sequence = 2u;
            recoveredReloadCommand.selectedTool = df::game::ToolType::Rifle;
            recoveredReloadCommand.reloadPressCount = 1u;

            client.Tick(1.0f / 60.0f, recoveredReloadCommand);
            host.Tick(1.0f / 60.0f);

            allPassed &= Expect(
                remotePlayer != nullptr && remotePlayer->reloading,
                "Session runtime did not recover a dropped edge-triggered reload input from the command counters.");

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
            const auto shared = std::make_shared<LoopbackTransport::SharedState>();
            shared->clientReliableSendFailuresRemaining = 1;
            auto serverTransport = std::make_unique<LoopbackTransport>(shared, true);
            auto clientTransport = std::make_unique<LoopbackTransport>(shared, false);
            clientTransport->Connect();

            df::net::SessionHost host;
            df::net::SessionHost::Config hostConfig{};
            hostConfig.runtime.mode = df::game::SessionMode::DedicatedServer;
            hostConfig.runtime.autosaveEnabled = false;
            hostConfig.runtime.loadExistingWorld = false;
            hostConfig.runtime.sessionName = "Hello Retry";
            hostConfig.runtime.generationSettings.worldWidth = 24;
            hostConfig.runtime.generationSettings.worldHeight = 18;
            hostConfig.runtime.generationSettings.worldDepth = 24;
            hostConfig.runtime.generationSettings.activeChunkSize = 8;
            host.Initialize(hostConfig, std::move(serverTransport));

            df::net::SessionClient client;
            client.Initialize({
                .playerName = "RetryClient",
                .connectTimeoutSeconds = 1.0f,
                .handshakeTimeoutSeconds = 1.0f,
                .helloResendIntervalSeconds = 0.01f,
            }, std::move(clientTransport));

            df::game::PlayerCommandFrame idleCommand{};
            PumpSessionPair(host, client, idleCommand, 20);
            allPassed &= Expect(shared->clientHelloAttempts >= 2u, "Session client did not retry the hello after the first reliable send failed.");
            allPassed &= Expect(client.IsReady(), "Session client did not recover from a failed initial hello send.");

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
            hostConfig.runtime.sessionName = "Readiness Gate";
            hostConfig.runtime.generationSettings.worldWidth = 96;
            hostConfig.runtime.generationSettings.worldHeight = 32;
            hostConfig.runtime.generationSettings.worldDepth = 96;
            hostConfig.runtime.generationSettings.activeChunkSize = 8;
            host.Initialize(hostConfig, std::move(serverTransport));

            df::net::SessionClient client;
            client.Initialize({
                .playerName = "GateClient",
                .connectTimeoutSeconds = 1.0f,
                .handshakeTimeoutSeconds = 1.0f,
                .helloResendIntervalSeconds = 0.01f,
            }, std::move(clientTransport));

            df::game::PlayerCommandFrame idleCommand{};
            PumpSessionPair(host, client, idleCommand, 2);
            allPassed &= Expect(!client.IsReady(), "Session client became ready before the initial world snapshot finished downloading.");
            allPassed &= Expect(client.PredictedLocalPlayer() == nullptr, "Session client enabled prediction before the authoritative world was ready.");
            allPassed &= Expect(client.RenderedLocalPlayer() == nullptr, "Session client exposed a renderable predicted player before the world snapshot completed.");

            PumpSessionPair(host, client, idleCommand, 48);
            allPassed &= Expect(client.IsReady(), "Session client did not become ready after the full world snapshot completed.");
            allPassed &= Expect(client.PredictedLocalPlayer() != nullptr, "Session client never rebuilt the predicted local player after the world became ready.");

            client.Shutdown();
            host.Shutdown();
        }

        {
            const auto shared = std::make_shared<LoopbackTransport::SharedState>();
            auto clientTransport = std::make_unique<LoopbackTransport>(shared, false);
            clientTransport->Connect();

            df::net::SessionClient client;
            client.Initialize({
                .playerName = "TimeoutClient",
                .connectTimeoutSeconds = 1.0f,
                .handshakeTimeoutSeconds = 0.14f,
                .helloResendIntervalSeconds = 0.01f,
            }, std::move(clientTransport));

            df::game::PlayerCommandFrame idleCommand{};
            for (int step = 0; step < 5; ++step)
            {
                client.Tick(1.0f / 60.0f, idleCommand);
            }

            for (int step = 0; step < 5; ++step)
            {
                client.Tick(1.0f / 60.0f, idleCommand);
            }

            allPassed &= Expect(client.Disconnected(), "Session client did not time out after a stalled handshake.");

            client.Shutdown();
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
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 48;
            settings.worldHeight = 20;
            settings.worldDepth = 48;
            settings.activeChunkSize = 8;
            world.SetGenerationSettings(settings);
            world.Reset();

            allPassed &= Expect(FlushWorldMesh(world), "Baseline world mesh rebuild did not converge before terrain stress validation.");
            const df::Vec3 worldMinimum = world.WorldMin();
            const float cellSize = world.CellSize();
            for (int iteration = 0; iteration < 48; ++iteration)
            {
                const float offset = static_cast<float>(iteration % 6) * cellSize * 1.35f;
                const df::Vec3 digCenter{
                    worldMinimum.x + cellSize * 18.0f + offset,
                    cellSize * (7.0f + static_cast<float>(iteration % 4)),
                    worldMinimum.z + cellSize * (18.0f + static_cast<float>((iteration * 3) % 11)),
                };
                world.ApplyDig(digCenter, 1.35f, 0.92f);
                world.ApplyExplosion(digCenter + df::Vec3{cellSize * 0.35f, cellSize * 0.5f, cellSize * 0.20f}, 2.2f, 0.55f);
                for (int tick = 0; tick < 5; ++tick)
                {
                    world.Tick(1.0f / 60.0f);
                }
                allPassed &= Expect(FlushWorldMesh(world), "Terrain stress validation left the mesh rebuild backlog dirty.");
            }
        }

        {
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 48;
            settings.worldHeight = 20;
            settings.worldDepth = 48;
            settings.activeChunkSize = 8;
            settings.seed = 9001u;
            world.SetGenerationSettings(settings);
            world.Reset();

            const float cellSize = world.CellSize();
            const df::Vec3 worldMinimum = world.WorldMin();
            const int waterOriginX = settings.worldWidth / 2 - 2;
            const int waterOriginZ = settings.worldDepth / 2 - 2;
            const int waterY = settings.worldHeight / 2 + 2;
            for (int z = 0; z < 5; ++z)
            {
                for (int x = 0; x < 5; ++x)
                {
                    world.EditCell(waterOriginX + x, waterY, waterOriginZ + z, df::world::MaterialId::ShallowWater);
                }
            }

            const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "Don_Craft_water_stress.bin";
            for (int iteration = 0; iteration < 48; ++iteration)
            {
                const int patternX = waterOriginX + (iteration % 5);
                const int patternZ = waterOriginZ + ((iteration * 3) % 5);
                const df::Vec3 impactCenter{
                    worldMinimum.x + (static_cast<float>(patternX) + 0.5f) * cellSize,
                    (static_cast<float>(waterY) + 0.5f) * cellSize,
                    worldMinimum.z + (static_cast<float>(patternZ) + 0.5f) * cellSize,
                };
                world.ApplyRifleImpact(impactCenter, df::Vec3{0.0f, -0.2f, 1.0f}, df::world::MaterialId::ShallowWater);
                world.ApplyExplosion(impactCenter + df::Vec3{0.0f, cellSize * 0.35f, 0.0f}, 2.6f, 0.72f);
                world.EditCell(patternX, waterY + 1, patternZ, df::world::MaterialId::ShallowWater);

                for (int tick = 0; tick < 10; ++tick)
                {
                    world.Tick(1.0f / 60.0f);
                }

                allPassed &= Expect(FlushWorldMesh(world), "Water/deformation stress left the mesh rebuild backlog dirty.");

                if (iteration % 4 == 0)
                {
                    allPassed &= Expect(world.Save(tempFile), "Water/deformation stress save failed.");
                    df::world::DemoWorld reloadedWorld;
                    allPassed &= Expect(reloadedWorld.Load(tempFile), "Water/deformation stress reload failed.");
                    allPassed &= Expect(
                        reloadedWorld.GenerationSettings().worldWidth == settings.worldWidth &&
                        reloadedWorld.GenerationSettings().worldDepth == settings.worldDepth,
                        "Water/deformation stress reload lost world dimensions.");
                }
            }

            std::error_code removeError;
            std::filesystem::remove(tempFile, removeError);
        }

        {
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 16;
            settings.worldHeight = 16;
            settings.worldDepth = 16;
            settings.activeChunkSize = 8;
            world.SetGenerationSettings(settings);
            world.Reset();

            const int waterX = settings.worldWidth / 2;
            const int waterZ = settings.worldDepth / 2;
            const int startY = settings.worldHeight - 2;
            for (int y = 1; y <= startY; ++y)
            {
                world.EditCell(waterX, y, waterZ, df::world::MaterialId::Air);
            }
            world.EditCell(waterX, startY, waterZ, df::world::MaterialId::ShallowWater);

            world.Tick(0.07f);

            bool fellFarEnough = false;
            for (int y = startY - 2; y >= std::max(0, startY - 6); --y)
            {
                if (world.MaterialAtCell(waterX, y, waterZ) == df::world::MaterialId::ShallowWater)
                {
                    fellFarEnough = true;
                    break;
                }
            }

            allPassed &= Expect(
                world.MaterialAtCell(waterX, startY, waterZ) == df::world::MaterialId::Air && fellFarEnough,
                "Water fall pass no longer clears a shaft quickly enough during a single update.");
        }

        {
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 16;
            settings.worldHeight = 16;
            settings.worldDepth = 16;
            settings.activeChunkSize = 8;
            world.SetGenerationSettings(settings);
            world.Reset();

            const int corridorX = settings.worldWidth / 2 - 3;
            const int corridorY = settings.worldHeight / 2;
            const int corridorZ = settings.worldDepth / 2;
            for (int offset = -2; offset <= 3; ++offset)
            {
                world.EditCell(corridorX + offset, corridorY - 1, corridorZ, df::world::MaterialId::BrittleConcrete);
                world.EditCell(corridorX + offset, corridorY - 1, corridorZ - 1, df::world::MaterialId::BrittleConcrete);
                world.EditCell(corridorX + offset, corridorY - 1, corridorZ + 1, df::world::MaterialId::BrittleConcrete);
                world.EditCell(corridorX + offset, corridorY, corridorZ, df::world::MaterialId::Air);
                world.EditCell(corridorX + offset, corridorY, corridorZ - 1, df::world::MaterialId::BrittleConcrete);
                world.EditCell(corridorX + offset, corridorY, corridorZ + 1, df::world::MaterialId::BrittleConcrete);
            }
            world.EditCell(corridorX - 2, corridorY, corridorZ, df::world::MaterialId::BrittleConcrete);
            world.EditCell(corridorX + 3, corridorY, corridorZ, df::world::MaterialId::BrittleConcrete);
            world.EditCell(corridorX - 1, corridorY, corridorZ, df::world::MaterialId::ShallowWater);
            world.EditCell(corridorX, corridorY, corridorZ, df::world::MaterialId::ShallowWater);

            world.Tick(0.10f);

            bool advancedSurfaceFlow = false;
            for (int x = corridorX + 1; x <= corridorX + 3; ++x)
            {
                if (world.MaterialAtCell(x, corridorY, corridorZ) == df::world::MaterialId::ShallowWater)
                {
                    advancedSurfaceFlow = true;
                    break;
                }
            }

            allPassed &= Expect(
                world.MaterialAtCell(corridorX, corridorY, corridorZ) == df::world::MaterialId::Air && advancedSurfaceFlow,
                "Water spread pass did not advance a one-neighbor surface flow through the corridor.");
        }

        {
            df::world::DemoWorld world;
            df::world::WorldGenerationSettings settings = world.GenerationSettings();
            settings.worldWidth = 8;
            settings.worldHeight = 8;
            settings.worldDepth = 8;
            settings.activeChunkSize = 8;
            world.SetGenerationSettings(settings);
            world.Reset();

            for (int z = 0; z < settings.worldDepth; ++z)
            {
                for (int y = 0; y < settings.worldHeight; ++y)
                {
                    for (int x = 0; x < settings.worldWidth; ++x)
                    {
                        world.EditCell(x, y, z, df::world::MaterialId::BrittleConcrete);
                    }
                }
            }

            allPassed &= Expect(FlushWorldMesh(world), "Filled-world boundary mesh validation did not converge.");
            allPassed &= Expect(world.OpaqueTerrainVertexCount() > 0u, "Filled-world boundary mesh validation produced no outer shell geometry.");
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
