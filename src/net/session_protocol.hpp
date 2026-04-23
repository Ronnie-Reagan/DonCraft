#pragma once

#include "core/bit_packer.hpp"
#include "game/session_types.hpp"
#include "net/chunk_delta.hpp"
#include "world/demo_world.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace df::net
{
inline constexpr std::uint32_t kSessionProtocolVersion = 8u;
inline constexpr std::size_t kMaxWorldSnapshotPayloadBytes = 768u;
inline constexpr std::size_t kMaxWorldSnapshotCellsPerMessage = kMaxWorldSnapshotPayloadBytes;
inline constexpr std::size_t kMaxChunkDeltaBatchPayloadBytes = 896u;
inline constexpr std::size_t kMaxCommandBundleFrames = 8u;

enum class MessageType : std::uint8_t
{
    ClientHello = 1,
    Welcome = 2,
    CommandFrame = 3,
    WorldSnapshot = 4,
    ChunkDeltaBatch = 5,
    ActorSnapshotFrame = 6,
    Disconnect = 7,
    ClientStateFrame = 8,
    CommandBundle = 9,
};

enum class BrowserEntryType : std::uint8_t
{
    ListenHost = 0,
    DedicatedServer = 1,
};

struct NetworkStats
{
    int pingMilliseconds = 0;
    float lossPercent = 0.0f;
    float connectionQuality = 0.0f;
};

struct SessionBrowserEntry
{
    BrowserEntryType type = BrowserEntryType::ListenHost;
    std::uint64_t lobbyId = 0;
    std::uint64_t ownerSteamId = 0;
    std::string name;
    std::string summary;
    std::string address;
    std::uint16_t port = 0;
    int currentPlayers = 0;
    int maxPlayers = 0;
    bool joinable = true;
    bool dedicated = false;
};

struct ClientHello
{
    std::uint32_t protocolVersion = kSessionProtocolVersion;
    std::string playerName;
};

struct Welcome
{
    std::uint32_t protocolVersion = kSessionProtocolVersion;
    game::PlayerId playerId = game::kInvalidPlayerId;
    game::SessionMode sessionMode = game::SessionMode::Offline;
    std::uint64_t serverTick = 0;
    std::string sessionName;
    int maxPlayers = 0;
};

struct ActorSnapshot
{
    game::PlayerId id = game::kInvalidPlayerId;
    std::string name;
    Vec3 position{};
    Vec3 cameraPosition{};
    Vec3 forward{};
    Vec3 flatForward{};
    Vec3 leftFootPosition{};
    Vec3 rightFootPosition{};
    bool leftFootGrounded = false;
    bool rightFootGrounded = false;
    bool onGround = false;
    bool drivingTruck = false;
    game::ToolType tool = game::ToolType::Rifle;
    world::MaterialId crosshairMaterial = world::MaterialId::Air;
    float horizontalSpeed = 0.0f;
    float walkCycleRadians = 0.0f;
    float yawRadians = 0.0f;
    float pitchRadians = 0.0f;
    float weaponCycle = 0.0f;
    int ammoInMagazine = 0;
    int reserveAmmo = 0;
    bool reloading = false;
    float reloadSecondsRemaining = 0.0f;
    float reloadSecondsTotal = 0.0f;
    std::uint32_t lastAppliedCommandSequence = 0;
};

struct TruckSnapshot
{
    Vec3 position{};
    Vec3 forward{};
    float speedMetersPerSecond = 0.0f;
    float engineLoad = 0.0f;
    float averageSink = 0.0f;
    world::MaterialId contactMaterial = world::MaterialId::Air;
    bool occupied = false;
    game::PlayerId driverId = game::kInvalidPlayerId;
};

struct GrenadeSnapshot
{
    Vec3 position{};
    Vec3 forward{};
    Vec3 up{};
};

struct BulletSnapshot
{
    Vec3 previousPosition{};
    Vec3 position{};
};

struct BeamSnapshot
{
    Vec3 start{};
    Vec3 end{};
    Vec4 color{};
    float ttl = 0.0f;
};

struct AudioCueSnapshot
{
    Vec3 position{};
    float baseFrequency = 220.0f;
    float durationSeconds = 0.1f;
    float amplitude = 0.18f;
    float noise = 0.0f;
    float sweep = 0.0f;
};

struct ActorSnapshotFrame
{
    std::uint64_t serverTick = 0;
    std::vector<ActorSnapshot> players;
    TruckSnapshot truck;
    std::vector<GrenadeSnapshot> grenades;
    std::vector<BulletSnapshot> bullets;
    std::vector<BeamSnapshot> beams;
    std::vector<AudioCueSnapshot> audioCues;
};

struct WorldSnapshotMessage
{
    enum class Encoding : std::uint8_t
    {
        Raw = 0,
        Rle = 1,
    };

    std::uint64_t serverTick = 0;
    world::WorldGenerationSettings settings{};
    std::uint32_t totalCellCount = 0;
    std::uint32_t cellOffset = 0;
    std::uint32_t decodedCellCount = 0;
    Encoding encoding = Encoding::Raw;
    std::vector<std::uint8_t> cells;
};

struct ChunkDeltaBatchMessage
{
    std::uint64_t serverTick = 0;
    std::vector<ChunkDelta> deltas;
};

struct DisconnectMessage
{
    int reason = 0;
    std::string text;
};

struct DecodedMessage
{
    MessageType type = MessageType::Disconnect;
    std::span<const std::byte> payload;
};

[[nodiscard]] auto EncodeMessage(MessageType type, std::span<const std::byte> payload) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeMessage(std::span<const std::byte> bytes) -> DecodedMessage;

[[nodiscard]] auto EncodeClientHello(const ClientHello& hello) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeClientHello(std::span<const std::byte> bytes) -> ClientHello;

[[nodiscard]] auto EncodeWelcome(const Welcome& welcome) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeWelcome(std::span<const std::byte> bytes) -> Welcome;

[[nodiscard]] auto EncodeCommandFrame(const game::PlayerCommandFrame& frame) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeCommandFrame(std::span<const std::byte> bytes) -> game::PlayerCommandFrame;
[[nodiscard]] auto EncodeCommandBundle(std::span<const game::PlayerCommandFrame> frames) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeCommandBundle(std::span<const std::byte> bytes) -> std::vector<game::PlayerCommandFrame>;

[[nodiscard]] auto EncodeClientStateFrame(const ActorSnapshot& snapshot) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeClientStateFrame(std::span<const std::byte> bytes) -> ActorSnapshot;

[[nodiscard]] auto EncodeWorldSnapshotMessage(const WorldSnapshotMessage& message) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeWorldSnapshotMessage(std::span<const std::byte> bytes) -> WorldSnapshotMessage;
[[nodiscard]] auto DecodeWorldSnapshotCells(const WorldSnapshotMessage& message) -> std::vector<std::uint8_t>;
[[nodiscard]] auto BuildWorldSnapshotMessages(
    std::uint64_t serverTick,
    const world::DenseWorldSnapshot& snapshot,
    std::size_t maxPayloadBytes = kMaxWorldSnapshotPayloadBytes) -> std::vector<WorldSnapshotMessage>;

[[nodiscard]] auto EncodeChunkDeltaBatchMessage(const ChunkDeltaBatchMessage& message) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeChunkDeltaBatchMessage(std::span<const std::byte> bytes) -> ChunkDeltaBatchMessage;
[[nodiscard]] auto BuildChunkDeltaBatches(
    std::span<const ChunkDelta> deltas,
    std::uint64_t serverTick,
    std::size_t maxPayloadBytes = kMaxChunkDeltaBatchPayloadBytes) -> std::vector<ChunkDeltaBatchMessage>;

[[nodiscard]] auto EncodeActorSnapshotFrame(const ActorSnapshotFrame& frame) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeActorSnapshotFrame(std::span<const std::byte> bytes) -> ActorSnapshotFrame;

[[nodiscard]] auto EncodeDisconnectMessage(const DisconnectMessage& message) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeDisconnectMessage(std::span<const std::byte> bytes) -> DisconnectMessage;

[[nodiscard]] auto EncodeBrowserEntry(const SessionBrowserEntry& entry) -> std::vector<std::byte>;
[[nodiscard]] auto DecodeBrowserEntry(std::span<const std::byte> bytes) -> SessionBrowserEntry;

[[nodiscard]] auto BuildChunkDeltas(const world::DenseWorldSnapshot& baseline, const world::DenseWorldSnapshot& current) -> std::vector<ChunkDelta>;
[[nodiscard]] bool ApplyChunkDeltas(world::DenseWorldSnapshot& target, std::span<const ChunkDelta> deltas);
}
