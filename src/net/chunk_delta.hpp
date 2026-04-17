#pragma once

#include "world/material.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace df::net
{
struct ChunkDeltaEntry
{
    std::uint16_t cellIndex = 0;
    world::MaterialId material = world::MaterialId::Air;

    auto operator==(const ChunkDeltaEntry&) const -> bool = default;
};

struct ChunkDelta
{
    world::ChunkCoord chunk{};
    std::vector<ChunkDeltaEntry> entries;

    auto operator==(const ChunkDelta&) const -> bool = default;
};

[[nodiscard]] std::vector<std::byte> EncodeChunkDelta(const ChunkDelta& delta);
[[nodiscard]] ChunkDelta DecodeChunkDelta(std::span<const std::byte> bytes);
}
