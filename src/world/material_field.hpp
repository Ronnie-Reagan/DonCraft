#pragma once

#include "world/material.hpp"

#include <array>
#include <filesystem>
#include <unordered_map>

namespace df::world
{
struct MaterialChunk
{
    std::array<std::uint8_t, kChunkVolume> cells{};

    [[nodiscard]] bool Empty() const;
    [[nodiscard]] MaterialId Get(std::uint32_t x, std::uint32_t y, std::uint32_t z) const;
    void Set(std::uint32_t x, std::uint32_t y, std::uint32_t z, MaterialId material);
};

class MaterialField
{
public:
    [[nodiscard]] MaterialId GetCell(int x, int y, int z) const;
    void SetCell(int x, int y, int z, MaterialId material);

    void Clear();

    [[nodiscard]] bool Save(const std::filesystem::path& path) const;
    [[nodiscard]] bool Load(const std::filesystem::path& path);

    [[nodiscard]] std::size_t ActiveChunkCount() const
    {
        return chunks_.size();
    }

private:
    using ChunkMap = std::unordered_map<ChunkCoord, MaterialChunk, ChunkCoordHash>;

    static ChunkCoord WorldToChunk(int x, int y, int z);
    static std::uint32_t LocalCoord(int value);
    static std::size_t CellIndex(int x, int y, int z);

    ChunkMap::iterator FindOrCreateChunk(const ChunkCoord& coord);

    ChunkMap chunks_;
};
}
