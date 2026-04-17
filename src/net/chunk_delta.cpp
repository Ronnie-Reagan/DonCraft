#include "net/chunk_delta.hpp"

#include "core/bit_packer.hpp"

#include <stdexcept>

namespace df::net
{
namespace
{
constexpr std::uint32_t kChunkDeltaMagic = 0x44434644u;
constexpr std::uint16_t kChunkDeltaVersion = 1u;
}

std::vector<std::byte> EncodeChunkDelta(const ChunkDelta& delta)
{
    ByteWriter writer;
    writer.WritePod(kChunkDeltaMagic);
    writer.WritePod(kChunkDeltaVersion);
    writer.WritePod(delta.chunk.x);
    writer.WritePod(delta.chunk.y);
    writer.WritePod(delta.chunk.z);
    writer.WritePod(static_cast<std::uint16_t>(delta.entries.size()));

    for (const ChunkDeltaEntry& entry : delta.entries)
    {
        writer.WritePod(entry.cellIndex);
        writer.WritePod(static_cast<std::uint8_t>(entry.material));
    }

    return writer.TakeData();
}

ChunkDelta DecodeChunkDelta(const std::span<const std::byte> bytes)
{
    ByteReader reader(bytes);

    const std::uint32_t magic = reader.ReadPod<std::uint32_t>();
    const std::uint16_t version = reader.ReadPod<std::uint16_t>();
    if (magic != kChunkDeltaMagic || version != kChunkDeltaVersion)
    {
        throw std::runtime_error("Chunk delta payload did not match the expected header.");
    }

    ChunkDelta delta{};
    delta.chunk.x = reader.ReadPod<int>();
    delta.chunk.y = reader.ReadPod<int>();
    delta.chunk.z = reader.ReadPod<int>();

    const std::uint16_t entryCount = reader.ReadPod<std::uint16_t>();
    delta.entries.reserve(entryCount);
    for (std::uint16_t entryIndex = 0; entryIndex < entryCount; ++entryIndex)
    {
        ChunkDeltaEntry entry{};
        entry.cellIndex = reader.ReadPod<std::uint16_t>();
        entry.material = static_cast<world::MaterialId>(reader.ReadPod<std::uint8_t>());
        delta.entries.push_back(entry);
    }

    if (!reader.Empty())
    {
        throw std::runtime_error("Chunk delta payload contained trailing bytes.");
    }

    return delta;
}
}
