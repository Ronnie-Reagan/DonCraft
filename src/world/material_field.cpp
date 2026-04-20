#include "world/material_field.hpp"

#include "core/bit_packer.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace df::world
{
namespace
{
constexpr std::uint32_t kMaterialFieldMagic = 0x46465744u;
constexpr std::uint32_t kMaterialFieldVersion = 1u;

int FloorDiv(const int value, const int divisor)
{
    int quotient = value / divisor;
    const int remainder = value % divisor;

    if (remainder != 0 && ((remainder < 0) != (divisor < 0)))
    {
        --quotient;
    }

    return quotient;
}
}

bool MaterialChunk::Empty() const
{
    return std::all_of(cells.begin(), cells.end(), [](const std::uint8_t value) { return value == static_cast<std::uint8_t>(MaterialId::Air); });
}

MaterialId MaterialChunk::Get(const std::uint32_t x, const std::uint32_t y, const std::uint32_t z) const
{
    return static_cast<MaterialId>(cells[LinearIndex(x, y, z)]);
}

void MaterialChunk::Set(const std::uint32_t x, const std::uint32_t y, const std::uint32_t z, const MaterialId material)
{
    cells[LinearIndex(x, y, z)] = static_cast<std::uint8_t>(material);
}

MaterialId MaterialField::GetCell(const int x, const int y, const int z) const
{
    const ChunkCoord coord = WorldToChunk(x, y, z);
    const auto iter = chunks_.find(coord);
    if (iter == chunks_.end())
    {
        return MaterialId::Air;
    }

    return iter->second.Get(LocalCoord(x), LocalCoord(y), LocalCoord(z));
}

void MaterialField::SetCell(const int x, const int y, const int z, const MaterialId material)
{
    const ChunkCoord coord = WorldToChunk(x, y, z);
    auto iter = chunks_.find(coord);

    if (iter == chunks_.end())
    {
        if (material == MaterialId::Air)
        {
            return;
        }

        iter = FindOrCreateChunk(coord);
    }

    iter->second.Set(LocalCoord(x), LocalCoord(y), LocalCoord(z), material);

    if (iter->second.Empty())
    {
        chunks_.erase(iter);
    }
}

void MaterialField::Clear()
{
    chunks_.clear();
}

bool MaterialField::Save(const std::filesystem::path& path) const
{
    LogInfo("MaterialField save begin path='", path.string(), "' chunks=", chunks_.size());
    ByteWriter writer;
    writer.WritePod(kMaterialFieldMagic);
    writer.WritePod(kMaterialFieldVersion);
    writer.WritePod(static_cast<std::uint32_t>(chunks_.size()));

    for (const auto& [coord, chunk] : chunks_)
    {
        writer.WritePod(coord.x);
        writer.WritePod(coord.y);
        writer.WritePod(coord.z);
        writer.WriteBytes(std::as_bytes(std::span(chunk.cells)));
    }

    if (path.has_parent_path())
    {
        std::error_code createError;
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError)
        {
            LogError("MaterialField save failed to create directory path='", path.parent_path().string(), "' error='", createError.message(), "'");
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary);
    if (!output)
    {
        LogError("MaterialField save failed to open path='", path.string(), "'.");
        return false;
    }

    const auto bytes = writer.Span();
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output.good())
    {
        LogError("MaterialField save failed while writing path='", path.string(), "' bytes=", bytes.size());
        return false;
    }

    LogInfo("MaterialField save complete path='", path.string(), "' bytes=", bytes.size());
    return true;
}

bool MaterialField::Load(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        LogWarning("MaterialField load failed to open path='", path.string(), "'.");
        return false;
    }

    try
    {
        const std::vector<char> rawBytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto bytes = std::as_bytes(std::span(rawBytes));
        ByteReader reader(bytes);

        const std::uint32_t magic = reader.ReadPod<std::uint32_t>();
        const std::uint32_t version = reader.ReadPod<std::uint32_t>();
        if (magic != kMaterialFieldMagic || version != kMaterialFieldVersion)
        {
            LogWarning(
                "MaterialField load rejected path='", path.string(),
                "' magic=0x", std::hex, magic,
                " version=", std::dec, version);
            return false;
        }

        Clear();

        const std::uint32_t chunkCount = reader.ReadPod<std::uint32_t>();
        for (std::uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
        {
            ChunkCoord coord{};
            coord.x = reader.ReadPod<int>();
            coord.y = reader.ReadPod<int>();
            coord.z = reader.ReadPod<int>();

            MaterialChunk chunk{};
            const auto cellBytes = reader.ReadBytes(chunk.cells.size());
            std::memcpy(chunk.cells.data(), cellBytes.data(), cellBytes.size());

            if (!chunk.Empty())
            {
                chunks_.emplace(coord, std::move(chunk));
            }
        }

        if (!reader.Empty())
        {
            LogWarning("MaterialField load left trailing bytes path='", path.string(), "'.");
            return false;
        }

        LogInfo("MaterialField load complete path='", path.string(), "' chunks=", chunks_.size());
        return true;
    }
    catch (const std::exception& error)
    {
        LogError("MaterialField load exception path='", path.string(), "' error='", error.what(), "'");
        return false;
    }
}

ChunkCoord MaterialField::WorldToChunk(const int x, const int y, const int z)
{
    ChunkCoord coord{};
    coord.x = FloorDiv(x, static_cast<int>(kChunkSize));
    coord.y = FloorDiv(y, static_cast<int>(kChunkSize));
    coord.z = FloorDiv(z, static_cast<int>(kChunkSize));
    return coord;
}

std::uint32_t MaterialField::LocalCoord(const int value)
{
    const int modulo = value % static_cast<int>(kChunkSize);
    return static_cast<std::uint32_t>(modulo < 0 ? modulo + static_cast<int>(kChunkSize) : modulo);
}

std::size_t MaterialField::CellIndex(const int x, const int y, const int z)
{
    return LinearIndex(LocalCoord(x), LocalCoord(y), LocalCoord(z));
}

MaterialField::ChunkMap::iterator MaterialField::FindOrCreateChunk(const ChunkCoord& coord)
{
    return chunks_.try_emplace(coord, MaterialChunk{}).first;
}
}
