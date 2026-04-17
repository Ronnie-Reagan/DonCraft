#include "net/session_protocol.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace df::net
{
namespace
{
constexpr std::uint32_t kMessageMagic = 0x53464446u;

template <typename T>
void WritePodVector(ByteWriter& writer, const std::vector<T>& values)
{
    writer.WritePod(static_cast<std::uint32_t>(values.size()));
    for (const T& value : values)
    {
        writer.WritePod(value);
    }
}

template <typename T>
auto ReadPodVector(ByteReader& reader) -> std::vector<T>
{
    const std::uint32_t count = reader.ReadPod<std::uint32_t>();
    std::vector<T> values;
    values.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        values.push_back(reader.ReadPod<T>());
    }
    return values;
}

void WriteControlState(ByteWriter& writer, const game::ControlState& control)
{
    writer.WriteBool(control.moveForward);
    writer.WriteBool(control.moveBackward);
    writer.WriteBool(control.moveLeft);
    writer.WriteBool(control.moveRight);
    writer.WriteBool(control.sprint);
    writer.WriteBool(control.jumpPressed);
    writer.WritePod(control.lookYawDelta);
    writer.WritePod(control.lookPitchDelta);
}

auto ReadControlState(ByteReader& reader) -> game::ControlState
{
    game::ControlState control{};
    control.moveForward = reader.ReadBool();
    control.moveBackward = reader.ReadBool();
    control.moveLeft = reader.ReadBool();
    control.moveRight = reader.ReadBool();
    control.sprint = reader.ReadBool();
    control.jumpPressed = reader.ReadBool();
    control.lookYawDelta = reader.ReadPod<float>();
    control.lookPitchDelta = reader.ReadPod<float>();
    return control;
}

void WriteWorldSnapshot(ByteWriter& writer, const world::DenseWorldSnapshot& snapshot)
{
    writer.WritePod(snapshot.settings.worldWidth);
    writer.WritePod(snapshot.settings.worldHeight);
    writer.WritePod(snapshot.settings.worldDepth);
    writer.WritePod(snapshot.settings.activeChunkSize);
    writer.WritePod(snapshot.settings.seed);
    writer.WritePod(snapshot.settings.cellSize);
    writer.WritePod(snapshot.settings.terrainRelief);
    writer.WritePod(snapshot.settings.waterLevel);
    writer.WritePod(static_cast<std::uint32_t>(snapshot.cells.size()));
    if (!snapshot.cells.empty())
    {
        writer.WriteBytes(std::as_bytes(std::span(snapshot.cells)));
    }
}

auto ReadWorldSnapshot(ByteReader& reader) -> world::DenseWorldSnapshot
{
    world::DenseWorldSnapshot snapshot{};
    snapshot.settings.worldWidth = reader.ReadPod<int>();
    snapshot.settings.worldHeight = reader.ReadPod<int>();
    snapshot.settings.worldDepth = reader.ReadPod<int>();
    snapshot.settings.activeChunkSize = reader.ReadPod<int>();
    snapshot.settings.seed = reader.ReadPod<std::uint32_t>();
    snapshot.settings.cellSize = reader.ReadPod<float>();
    snapshot.settings.terrainRelief = reader.ReadPod<float>();
    snapshot.settings.waterLevel = reader.ReadPod<float>();

    const std::uint32_t cellCount = reader.ReadPod<std::uint32_t>();
    snapshot.cells.resize(cellCount);
    if (cellCount > 0)
    {
        const auto cellBytes = reader.ReadBytes(cellCount);
        std::memcpy(snapshot.cells.data(), cellBytes.data(), cellBytes.size());
    }
    return snapshot;
}

void WriteChunkDelta(ByteWriter& writer, const ChunkDelta& delta)
{
    const std::vector<std::byte> bytes = EncodeChunkDelta(delta);
    writer.WritePod(static_cast<std::uint32_t>(bytes.size()));
    writer.WriteBytes(bytes);
}

auto ReadChunkDelta(ByteReader& reader) -> ChunkDelta
{
    const std::uint32_t size = reader.ReadPod<std::uint32_t>();
    return DecodeChunkDelta(reader.ReadBytes(size));
}

void WriteActorSnapshot(ByteWriter& writer, const ActorSnapshot& snapshot)
{
    writer.WritePod(snapshot.id);
    writer.WriteString(snapshot.name);
    writer.WritePod(snapshot.position);
    writer.WritePod(snapshot.cameraPosition);
    writer.WritePod(snapshot.forward);
    writer.WritePod(snapshot.flatForward);
    writer.WritePod(snapshot.leftFootPosition);
    writer.WritePod(snapshot.rightFootPosition);
    writer.WriteBool(snapshot.leftFootGrounded);
    writer.WriteBool(snapshot.rightFootGrounded);
    writer.WriteBool(snapshot.onGround);
    writer.WriteBool(snapshot.drivingTruck);
    writer.WritePod(static_cast<std::uint8_t>(snapshot.tool));
    writer.WritePod(static_cast<std::uint8_t>(snapshot.crosshairMaterial));
    writer.WritePod(snapshot.horizontalSpeed);
    writer.WritePod(snapshot.walkCycleRadians);
    writer.WritePod(snapshot.yawRadians);
    writer.WritePod(snapshot.pitchRadians);
    writer.WritePod(snapshot.lastAppliedCommandSequence);
}

auto ReadActorSnapshot(ByteReader& reader) -> ActorSnapshot
{
    ActorSnapshot snapshot{};
    snapshot.id = reader.ReadPod<game::PlayerId>();
    snapshot.name = reader.ReadString();
    snapshot.position = reader.ReadPod<Vec3>();
    snapshot.cameraPosition = reader.ReadPod<Vec3>();
    snapshot.forward = reader.ReadPod<Vec3>();
    snapshot.flatForward = reader.ReadPod<Vec3>();
    snapshot.leftFootPosition = reader.ReadPod<Vec3>();
    snapshot.rightFootPosition = reader.ReadPod<Vec3>();
    snapshot.leftFootGrounded = reader.ReadBool();
    snapshot.rightFootGrounded = reader.ReadBool();
    snapshot.onGround = reader.ReadBool();
    snapshot.drivingTruck = reader.ReadBool();
    snapshot.tool = static_cast<game::ToolType>(reader.ReadPod<std::uint8_t>());
    snapshot.crosshairMaterial = static_cast<world::MaterialId>(reader.ReadPod<std::uint8_t>());
    snapshot.horizontalSpeed = reader.ReadPod<float>();
    snapshot.walkCycleRadians = reader.ReadPod<float>();
    snapshot.yawRadians = reader.ReadPod<float>();
    snapshot.pitchRadians = reader.ReadPod<float>();
    snapshot.lastAppliedCommandSequence = reader.ReadPod<std::uint32_t>();
    return snapshot;
}

void WriteTruckSnapshot(ByteWriter& writer, const TruckSnapshot& snapshot)
{
    writer.WritePod(snapshot.position);
    writer.WritePod(snapshot.forward);
    writer.WritePod(snapshot.speedMetersPerSecond);
    writer.WritePod(snapshot.engineLoad);
    writer.WritePod(snapshot.averageSink);
    writer.WritePod(static_cast<std::uint8_t>(snapshot.contactMaterial));
    writer.WriteBool(snapshot.occupied);
    writer.WritePod(snapshot.driverId);
}

auto ReadTruckSnapshot(ByteReader& reader) -> TruckSnapshot
{
    TruckSnapshot snapshot{};
    snapshot.position = reader.ReadPod<Vec3>();
    snapshot.forward = reader.ReadPod<Vec3>();
    snapshot.speedMetersPerSecond = reader.ReadPod<float>();
    snapshot.engineLoad = reader.ReadPod<float>();
    snapshot.averageSink = reader.ReadPod<float>();
    snapshot.contactMaterial = static_cast<world::MaterialId>(reader.ReadPod<std::uint8_t>());
    snapshot.occupied = reader.ReadBool();
    snapshot.driverId = reader.ReadPod<game::PlayerId>();
    return snapshot;
}

void WriteGrenadeSnapshot(ByteWriter& writer, const GrenadeSnapshot& snapshot)
{
    writer.WritePod(snapshot.position);
    writer.WritePod(snapshot.forward);
    writer.WritePod(snapshot.up);
}

auto ReadGrenadeSnapshot(ByteReader& reader) -> GrenadeSnapshot
{
    GrenadeSnapshot snapshot{};
    snapshot.position = reader.ReadPod<Vec3>();
    snapshot.forward = reader.ReadPod<Vec3>();
    snapshot.up = reader.ReadPod<Vec3>();
    return snapshot;
}

void WriteBulletSnapshot(ByteWriter& writer, const BulletSnapshot& snapshot)
{
    writer.WritePod(snapshot.previousPosition);
    writer.WritePod(snapshot.position);
}

auto ReadBulletSnapshot(ByteReader& reader) -> BulletSnapshot
{
    BulletSnapshot snapshot{};
    snapshot.previousPosition = reader.ReadPod<Vec3>();
    snapshot.position = reader.ReadPod<Vec3>();
    return snapshot;
}

void WriteBeamSnapshot(ByteWriter& writer, const BeamSnapshot& snapshot)
{
    writer.WritePod(snapshot.start);
    writer.WritePod(snapshot.end);
    writer.WritePod(snapshot.color);
    writer.WritePod(snapshot.ttl);
}

auto ReadBeamSnapshot(ByteReader& reader) -> BeamSnapshot
{
    BeamSnapshot snapshot{};
    snapshot.start = reader.ReadPod<Vec3>();
    snapshot.end = reader.ReadPod<Vec3>();
    snapshot.color = reader.ReadPod<Vec4>();
    snapshot.ttl = reader.ReadPod<float>();
    return snapshot;
}

auto DenseIndex(const world::WorldGenerationSettings& settings, const int x, const int y, const int z) -> std::size_t
{
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(y) * static_cast<std::size_t>(settings.worldWidth) +
           static_cast<std::size_t>(z) * static_cast<std::size_t>(settings.worldWidth) * static_cast<std::size_t>(settings.worldHeight);
}
}

auto EncodeMessage(const MessageType type, const std::span<const std::byte> payload) -> std::vector<std::byte>
{
    ByteWriter writer;
    writer.WritePod(kMessageMagic);
    writer.WritePod(kSessionProtocolVersion);
    writer.WritePod(static_cast<std::uint8_t>(type));
    writer.WritePod(static_cast<std::uint32_t>(payload.size()));
    writer.WriteBytes(payload);
    return writer.TakeData();
}

auto DecodeMessage(const std::span<const std::byte> bytes) -> DecodedMessage
{
    ByteReader reader(bytes);
    const std::uint32_t magic = reader.ReadPod<std::uint32_t>();
    const std::uint32_t version = reader.ReadPod<std::uint32_t>();
    if (magic != kMessageMagic || version != kSessionProtocolVersion)
    {
        throw std::runtime_error("Invalid session protocol message header.");
    }

    const MessageType type = static_cast<MessageType>(reader.ReadPod<std::uint8_t>());
    const std::uint32_t payloadSize = reader.ReadPod<std::uint32_t>();
    const auto payload = reader.ReadBytes(payloadSize);
    if (!reader.Empty())
    {
        throw std::runtime_error("Session protocol message had trailing bytes.");
    }

    return {type, payload};
}

auto EncodeClientHello(const ClientHello& hello) -> std::vector<std::byte>
{
    ByteWriter writer;
    writer.WritePod(hello.protocolVersion);
    writer.WriteString(hello.playerName);
    return writer.TakeData();
}

auto DecodeClientHello(const std::span<const std::byte> bytes) -> ClientHello
{
    ByteReader reader(bytes);
    ClientHello hello{};
    hello.protocolVersion = reader.ReadPod<std::uint32_t>();
    hello.playerName = reader.ReadString();
    if (!reader.Empty())
    {
        throw std::runtime_error("ClientHello had trailing bytes.");
    }
    return hello;
}

auto EncodeWelcome(const Welcome& welcome) -> std::vector<std::byte>
{
    ByteWriter writer;
    writer.WritePod(welcome.protocolVersion);
    writer.WritePod(welcome.playerId);
    writer.WritePod(static_cast<std::uint8_t>(welcome.sessionMode));
    writer.WritePod(welcome.serverTick);
    writer.WriteString(welcome.sessionName);
    writer.WritePod(welcome.maxPlayers);
    return writer.TakeData();
}

auto DecodeWelcome(const std::span<const std::byte> bytes) -> Welcome
{
    ByteReader reader(bytes);
    Welcome welcome{};
    welcome.protocolVersion = reader.ReadPod<std::uint32_t>();
    welcome.playerId = reader.ReadPod<game::PlayerId>();
    welcome.sessionMode = static_cast<game::SessionMode>(reader.ReadPod<std::uint8_t>());
    welcome.serverTick = reader.ReadPod<std::uint64_t>();
    welcome.sessionName = reader.ReadString();
    welcome.maxPlayers = reader.ReadPod<int>();
    if (!reader.Empty())
    {
        throw std::runtime_error("Welcome had trailing bytes.");
    }
    return welcome;
}

auto EncodeCommandFrame(const game::PlayerCommandFrame& frame) -> std::vector<std::byte>
{
    ByteWriter writer;
    writer.WritePod(frame.sequence);
    WriteControlState(writer, frame.control);
    writer.WritePod(static_cast<std::uint8_t>(frame.selectedTool));
    writer.WriteBool(frame.primaryDown);
    writer.WriteBool(frame.primaryPressed);
    writer.WriteBool(frame.secondaryDown);
    writer.WriteBool(frame.quickGrenadePressed);
    writer.WriteBool(frame.interactPressed);
    return writer.TakeData();
}

auto DecodeCommandFrame(const std::span<const std::byte> bytes) -> game::PlayerCommandFrame
{
    ByteReader reader(bytes);
    game::PlayerCommandFrame frame{};
    frame.sequence = reader.ReadPod<std::uint32_t>();
    frame.control = ReadControlState(reader);
    frame.selectedTool = static_cast<game::ToolType>(reader.ReadPod<std::uint8_t>());
    frame.primaryDown = reader.ReadBool();
    frame.primaryPressed = reader.ReadBool();
    frame.secondaryDown = reader.ReadBool();
    frame.quickGrenadePressed = reader.ReadBool();
    frame.interactPressed = reader.ReadBool();
    if (!reader.Empty())
    {
        throw std::runtime_error("PlayerCommandFrame had trailing bytes.");
    }
    return frame;
}

auto EncodeWorldSnapshotMessage(const WorldSnapshotMessage& message) -> std::vector<std::byte>
{
    ByteWriter writer;
    writer.WritePod(message.serverTick);
    WriteWorldSnapshot(writer, message.world);
    return writer.TakeData();
}

auto DecodeWorldSnapshotMessage(const std::span<const std::byte> bytes) -> WorldSnapshotMessage
{
    ByteReader reader(bytes);
    WorldSnapshotMessage message{};
    message.serverTick = reader.ReadPod<std::uint64_t>();
    message.world = ReadWorldSnapshot(reader);
    if (!reader.Empty())
    {
        throw std::runtime_error("WorldSnapshotMessage had trailing bytes.");
    }
    return message;
}

auto EncodeChunkDeltaBatchMessage(const ChunkDeltaBatchMessage& message) -> std::vector<std::byte>
{
    ByteWriter writer;
    writer.WritePod(message.serverTick);
    writer.WritePod(static_cast<std::uint32_t>(message.deltas.size()));
    for (const ChunkDelta& delta : message.deltas)
    {
        WriteChunkDelta(writer, delta);
    }
    return writer.TakeData();
}

auto DecodeChunkDeltaBatchMessage(const std::span<const std::byte> bytes) -> ChunkDeltaBatchMessage
{
    ByteReader reader(bytes);
    ChunkDeltaBatchMessage message{};
    message.serverTick = reader.ReadPod<std::uint64_t>();
    const std::uint32_t count = reader.ReadPod<std::uint32_t>();
    message.deltas.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        message.deltas.push_back(ReadChunkDelta(reader));
    }
    if (!reader.Empty())
    {
        throw std::runtime_error("ChunkDeltaBatchMessage had trailing bytes.");
    }
    return message;
}

auto EncodeActorSnapshotFrame(const ActorSnapshotFrame& frame) -> std::vector<std::byte>
{
    ByteWriter writer;
    writer.WritePod(frame.serverTick);
    writer.WritePod(static_cast<std::uint32_t>(frame.players.size()));
    for (const ActorSnapshot& player : frame.players)
    {
        WriteActorSnapshot(writer, player);
    }
    WriteTruckSnapshot(writer, frame.truck);

    writer.WritePod(static_cast<std::uint32_t>(frame.grenades.size()));
    for (const GrenadeSnapshot& grenade : frame.grenades)
    {
        WriteGrenadeSnapshot(writer, grenade);
    }

    writer.WritePod(static_cast<std::uint32_t>(frame.bullets.size()));
    for (const BulletSnapshot& bullet : frame.bullets)
    {
        WriteBulletSnapshot(writer, bullet);
    }

    writer.WritePod(static_cast<std::uint32_t>(frame.beams.size()));
    for (const BeamSnapshot& beam : frame.beams)
    {
        WriteBeamSnapshot(writer, beam);
    }

    return writer.TakeData();
}

auto DecodeActorSnapshotFrame(const std::span<const std::byte> bytes) -> ActorSnapshotFrame
{
    ByteReader reader(bytes);
    ActorSnapshotFrame frame{};
    frame.serverTick = reader.ReadPod<std::uint64_t>();

    const std::uint32_t playerCount = reader.ReadPod<std::uint32_t>();
    frame.players.reserve(playerCount);
    for (std::uint32_t index = 0; index < playerCount; ++index)
    {
        frame.players.push_back(ReadActorSnapshot(reader));
    }

    frame.truck = ReadTruckSnapshot(reader);

    const std::uint32_t grenadeCount = reader.ReadPod<std::uint32_t>();
    frame.grenades.reserve(grenadeCount);
    for (std::uint32_t index = 0; index < grenadeCount; ++index)
    {
        frame.grenades.push_back(ReadGrenadeSnapshot(reader));
    }

    const std::uint32_t bulletCount = reader.ReadPod<std::uint32_t>();
    frame.bullets.reserve(bulletCount);
    for (std::uint32_t index = 0; index < bulletCount; ++index)
    {
        frame.bullets.push_back(ReadBulletSnapshot(reader));
    }

    const std::uint32_t beamCount = reader.ReadPod<std::uint32_t>();
    frame.beams.reserve(beamCount);
    for (std::uint32_t index = 0; index < beamCount; ++index)
    {
        frame.beams.push_back(ReadBeamSnapshot(reader));
    }

    if (!reader.Empty())
    {
        throw std::runtime_error("ActorSnapshotFrame had trailing bytes.");
    }

    return frame;
}

auto EncodeDisconnectMessage(const DisconnectMessage& message) -> std::vector<std::byte>
{
    ByteWriter writer;
    writer.WritePod(message.reason);
    writer.WriteString(message.text);
    return writer.TakeData();
}

auto DecodeDisconnectMessage(const std::span<const std::byte> bytes) -> DisconnectMessage
{
    ByteReader reader(bytes);
    DisconnectMessage message{};
    message.reason = reader.ReadPod<int>();
    message.text = reader.ReadString();
    if (!reader.Empty())
    {
        throw std::runtime_error("DisconnectMessage had trailing bytes.");
    }
    return message;
}

auto EncodeBrowserEntry(const SessionBrowserEntry& entry) -> std::vector<std::byte>
{
    ByteWriter writer;
    writer.WritePod(static_cast<std::uint8_t>(entry.type));
    writer.WritePod(entry.lobbyId);
    writer.WritePod(entry.ownerSteamId);
    writer.WriteString(entry.name);
    writer.WriteString(entry.summary);
    writer.WriteString(entry.address);
    writer.WritePod(entry.port);
    writer.WritePod(entry.currentPlayers);
    writer.WritePod(entry.maxPlayers);
    writer.WriteBool(entry.joinable);
    writer.WriteBool(entry.dedicated);
    return writer.TakeData();
}

auto DecodeBrowserEntry(const std::span<const std::byte> bytes) -> SessionBrowserEntry
{
    ByteReader reader(bytes);
    SessionBrowserEntry entry{};
    entry.type = static_cast<BrowserEntryType>(reader.ReadPod<std::uint8_t>());
    entry.lobbyId = reader.ReadPod<std::uint64_t>();
    entry.ownerSteamId = reader.ReadPod<std::uint64_t>();
    entry.name = reader.ReadString();
    entry.summary = reader.ReadString();
    entry.address = reader.ReadString();
    entry.port = reader.ReadPod<std::uint16_t>();
    entry.currentPlayers = reader.ReadPod<int>();
    entry.maxPlayers = reader.ReadPod<int>();
    entry.joinable = reader.ReadBool();
    entry.dedicated = reader.ReadBool();
    if (!reader.Empty())
    {
        throw std::runtime_error("SessionBrowserEntry had trailing bytes.");
    }
    return entry;
}

auto BuildChunkDeltas(const world::DenseWorldSnapshot& baseline, const world::DenseWorldSnapshot& current) -> std::vector<ChunkDelta>
{
    if (baseline.settings.worldWidth != current.settings.worldWidth ||
        baseline.settings.worldHeight != current.settings.worldHeight ||
        baseline.settings.worldDepth != current.settings.worldDepth ||
        baseline.cells.size() != current.cells.size())
    {
        return {};
    }

    std::vector<ChunkDelta> deltas;
    const int chunkCountX = (current.settings.worldWidth + static_cast<int>(world::kChunkSize) - 1) / static_cast<int>(world::kChunkSize);
    const int chunkCountY = (current.settings.worldHeight + static_cast<int>(world::kChunkSize) - 1) / static_cast<int>(world::kChunkSize);
    const int chunkCountZ = (current.settings.worldDepth + static_cast<int>(world::kChunkSize) - 1) / static_cast<int>(world::kChunkSize);

    for (int chunkZ = 0; chunkZ < chunkCountZ; ++chunkZ)
    {
        for (int chunkY = 0; chunkY < chunkCountY; ++chunkY)
        {
            for (int chunkX = 0; chunkX < chunkCountX; ++chunkX)
            {
                ChunkDelta delta{};
                delta.chunk = {chunkX, chunkY, chunkZ};

                const int baseX = chunkX * static_cast<int>(world::kChunkSize);
                const int baseY = chunkY * static_cast<int>(world::kChunkSize);
                const int baseZ = chunkZ * static_cast<int>(world::kChunkSize);

                for (int localZ = 0; localZ < static_cast<int>(world::kChunkSize); ++localZ)
                {
                    for (int localY = 0; localY < static_cast<int>(world::kChunkSize); ++localY)
                    {
                        for (int localX = 0; localX < static_cast<int>(world::kChunkSize); ++localX)
                        {
                            const int worldX = baseX + localX;
                            const int worldY = baseY + localY;
                            const int worldZ = baseZ + localZ;
                            if (worldX >= current.settings.worldWidth || worldY >= current.settings.worldHeight || worldZ >= current.settings.worldDepth)
                            {
                                continue;
                            }

                            const std::size_t denseIndex = DenseIndex(current.settings, worldX, worldY, worldZ);
                            if (baseline.cells[denseIndex] == current.cells[denseIndex])
                            {
                                continue;
                            }

                            delta.entries.push_back({
                                static_cast<std::uint16_t>(world::LinearIndex(localX, localY, localZ)),
                                static_cast<world::MaterialId>(current.cells[denseIndex]),
                            });
                        }
                    }
                }

                if (!delta.entries.empty())
                {
                    deltas.push_back(std::move(delta));
                }
            }
        }
    }

    return deltas;
}

bool ApplyChunkDeltas(world::DenseWorldSnapshot& target, const std::span<const ChunkDelta> deltas)
{
    const std::size_t expectedCellCount =
        static_cast<std::size_t>(target.settings.worldWidth) *
        static_cast<std::size_t>(target.settings.worldHeight) *
        static_cast<std::size_t>(target.settings.worldDepth);
    if (target.cells.size() != expectedCellCount)
    {
        return false;
    }

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

            const int worldX = baseX + localX;
            const int worldY = baseY + localY;
            const int worldZ = baseZ + localZ;
            if (worldX < 0 || worldY < 0 || worldZ < 0 ||
                worldX >= target.settings.worldWidth || worldY >= target.settings.worldHeight || worldZ >= target.settings.worldDepth)
            {
                continue;
            }

            const std::size_t denseIndex = DenseIndex(target.settings, worldX, worldY, worldZ);
            target.cells[denseIndex] = static_cast<std::uint8_t>(entry.material);
        }
    }

    return true;
}
}
