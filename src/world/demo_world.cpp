#include "world/demo_world.hpp"

#include "core/bit_packer.hpp"
#include "core/log.hpp"
#include "core/job_system.hpp"
#include "core/timer.hpp"
#include "world/material_properties.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <unordered_map>

namespace df::world
{
namespace
{
constexpr float kActiveChunkLifetimeSeconds = 2.5f;
constexpr std::uint32_t kDemoWorldMagic = 0x57464444u;
constexpr std::uint32_t kDemoWorldVersion = 3u;
constexpr int kMinWorldWidthDepthCells = 8;
constexpr int kMaxWorldWidthDepthCells = 384;
constexpr int kMinWorldHeightCells = 8;
constexpr int kMaxWorldHeightCells = 160;
constexpr int kMinActiveChunkSizeCells = 8;
constexpr int kMaxActiveChunkSizeCells = 128;
constexpr float kDrySandTravelSpeedMetersPerSecond = 4.0f;
constexpr float kWetMudTravelSpeedMetersPerSecond = 1.3f;
constexpr float kWaterFallSpeedMetersPerSecond = 5.8f;
constexpr float kWaterSpreadSpeedMetersPerSecond = 1.7f;
constexpr std::uint32_t kWaterBacktrackCooldownPasses = 4u;
constexpr int kLooseSimulationPaddingCells = 1;
constexpr float kMpmSimulationIntervalSeconds = 1.0f / 60.0f;
constexpr int kMpmSliceBudgetPerAxisPass = 16;
constexpr float kSurfaceIsoLevel = 0.5f;
constexpr float kSurfaceIntersectionEpsilon = 1.0e-4f;
constexpr std::size_t kMinMeshChunkBudgetPerPass = 32u;
constexpr std::size_t kMeshChunkBudgetPerWorker = 16u;

const std::array<Vec3, 8> kCubeCorners = {
    Vec3{0.0f, 0.0f, 0.0f},
    Vec3{1.0f, 0.0f, 0.0f},
    Vec3{1.0f, 1.0f, 0.0f},
    Vec3{0.0f, 1.0f, 0.0f},
    Vec3{0.0f, 0.0f, 1.0f},
    Vec3{1.0f, 0.0f, 1.0f},
    Vec3{1.0f, 1.0f, 1.0f},
    Vec3{0.0f, 1.0f, 1.0f},
};

struct FaceDefinition
{
    std::array<int, 4> corners{};
    Int3 neighborOffset{};
    float lighting = 1.0f;
};

const std::array<FaceDefinition, 6> kFaces = {
    FaceDefinition{{1, 5, 6, 2}, {1, 0, 0}, 0.82f},
    FaceDefinition{{4, 0, 3, 7}, {-1, 0, 0}, 0.82f},
    FaceDefinition{{3, 2, 6, 7}, {0, 1, 0}, 1.00f},
    FaceDefinition{{4, 5, 1, 0}, {0, -1, 0}, 0.62f},
    FaceDefinition{{5, 4, 7, 6}, {0, 0, 1}, 0.90f},
    FaceDefinition{{0, 1, 2, 3}, {0, 0, -1}, 0.74f},
};

enum class SurfaceField
{
    Solid,
    Water,
};

constexpr std::array<std::array<int, 4>, 6> kMarchingTetrahedra = {
    std::array<int, 4>{0, 5, 1, 6},
    std::array<int, 4>{0, 1, 2, 6},
    std::array<int, 4>{0, 2, 3, 6},
    std::array<int, 4>{0, 3, 7, 6},
    std::array<int, 4>{0, 7, 4, 6},
    std::array<int, 4>{0, 4, 5, 6},
};

constexpr std::array<std::array<int, 2>, 6> kTetrahedronEdges = {
    std::array<int, 2>{0, 1},
    std::array<int, 2>{1, 2},
    std::array<int, 2>{2, 0},
    std::array<int, 2>{0, 3},
    std::array<int, 2>{1, 3},
    std::array<int, 2>{2, 3},
};

auto Shade(const Vec4& color, const float lighting) -> Vec4
{
    return {
        color.x * lighting,
        color.y * lighting,
        color.z * lighting,
        color.w,
    };
}

auto Smooth01(const float value) -> float
{
    const float clamped = Clamp(value, 0.0f, 1.0f);
    return clamped * clamped * (3.0f - 2.0f * clamped);
}

auto HashCoords(std::uint32_t seed, const int x, const int y, const int z = 0) -> std::uint32_t
{
    std::uint32_t hash = seed ^ 0x9e3779b9u;
    hash ^= static_cast<std::uint32_t>(x) * 0x85ebca6bu;
    hash = (hash << 13u) | (hash >> 19u);
    hash ^= static_cast<std::uint32_t>(y) * 0xc2b2ae35u;
    hash = (hash << 15u) | (hash >> 17u);
    hash ^= static_cast<std::uint32_t>(z) * 0x27d4eb2du;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16u;
    return hash;
}

auto HashUnitFloat(std::uint32_t seed, const int x, const int y, const int z = 0) -> float
{
    const std::uint32_t hash = HashCoords(seed, x, y, z);
    return static_cast<float>(hash & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

auto ValueNoise2D(const float x, const float z, const std::uint32_t seed) -> float
{
    const int x0 = static_cast<int>(std::floor(x));
    const int z0 = static_cast<int>(std::floor(z));
    const int x1 = x0 + 1;
    const int z1 = z0 + 1;

    const float fx = Smooth01(x - static_cast<float>(x0));
    const float fz = Smooth01(z - static_cast<float>(z0));

    const float v00 = HashUnitFloat(seed, x0, z0);
    const float v10 = HashUnitFloat(seed, x1, z0);
    const float v01 = HashUnitFloat(seed, x0, z1);
    const float v11 = HashUnitFloat(seed, x1, z1);

    const float ix0 = Lerp(v00, v10, fx);
    const float ix1 = Lerp(v01, v11, fx);
    return Lerp(ix0, ix1, fz) * 2.0f - 1.0f;
}

struct ScanOrder
{
    int xStart = 0;
    int xEnd = 0;
    int xStep = 1;
    int zStart = 0;
    int zEnd = 0;
    int zStep = 1;
};

auto BuildScanOrder(const std::uint64_t passIndex, const int width, const int depth) -> ScanOrder
{
    const bool reverseX = (passIndex & 1u) != 0u;
    const bool reverseZ = (passIndex & 2u) != 0u;

    return {
        .xStart = reverseX ? width - 1 : 0,
        .xEnd = reverseX ? -1 : width,
        .xStep = reverseX ? -1 : 1,
        .zStart = reverseZ ? depth - 1 : 0,
        .zEnd = reverseZ ? -1 : depth,
        .zStep = reverseZ ? -1 : 1,
    };
}

auto CardinalDirectionCode(const Int3& direction) -> std::int8_t
{
    if (direction.x < 0)
    {
        return -1;
    }
    if (direction.x > 0)
    {
        return 1;
    }
    if (direction.z < 0)
    {
        return -2;
    }
    if (direction.z > 0)
    {
        return 2;
    }
    return 0;
}

auto WaterSpreadDirections(const int x, const int z, const std::uint32_t seed, const std::uint32_t passIndex) -> std::array<Int3, 4>
{
    constexpr std::array<Int3, 4> kDirections = {
        Int3{1, 0, 0},
        Int3{-1, 0, 0},
        Int3{0, 0, 1},
        Int3{0, 0, -1},
    };

    std::array<Int3, 4> ordered = kDirections;
    const std::uint32_t hash = HashCoords(seed ^ (passIndex * 0x9e3779b9u), x, z);
    const int rotation = static_cast<int>(hash & 3u);
    std::rotate(ordered.begin(), ordered.begin() + rotation, ordered.end());
    if ((hash & 4u) != 0u)
    {
        std::swap(ordered[1], ordered[2]);
    }
    return ordered;
}

auto WaterFallDirections(const int x, const int z, const std::uint32_t seed, const std::uint64_t passIndex) -> std::array<Int3, 4>
{
    constexpr std::array<Int3, 4> kDirections = {
        Int3{1, 0, 0},
        Int3{-1, 0, 0},
        Int3{0, 0, 1},
        Int3{0, 0, -1},
    };

    std::array<Int3, 4> ordered = kDirections;
    const std::uint32_t hash = HashCoords(seed ^ static_cast<std::uint32_t>(passIndex), x, z);
    const int rotation = static_cast<int>((hash >> 1u) & 3u);
    std::rotate(ordered.begin(), ordered.begin() + rotation, ordered.end());
    return ordered;
}

auto SurfaceFieldContainsMaterial(const MaterialId material, const SurfaceField field) -> bool
{
    if (field == SurfaceField::Water)
    {
        return material == MaterialId::ShallowWater;
    }

    return material != MaterialId::Air && material != MaterialId::ShallowWater;
}

auto WaterSurfaceColor() -> Vec4
{
    Vec4 color = GetMaterialProperties(MaterialId::ShallowWater).color;
    color.w = 0.42f;
    return color;
}

auto PositionsNearlyEqual(const Vec3& lhs, const Vec3& rhs, const float epsilon = kSurfaceIntersectionEpsilon) -> bool
{
    const Vec3 delta = lhs - rhs;
    return LengthSquared(delta) <= epsilon * epsilon;
}

void AppendUniqueSurfacePoint(std::array<Vec3, 6>& points, int& pointCount, const Vec3& point)
{
    for (int existingIndex = 0; existingIndex < pointCount; ++existingIndex)
    {
        if (PositionsNearlyEqual(points[existingIndex], point))
        {
            return;
        }
    }

    if (pointCount < static_cast<int>(points.size()))
    {
        points[pointCount++] = point;
    }
}

auto FindStableSurfaceNormal(const std::array<Vec3, 6>& points, const int pointCount) -> Vec3
{
    for (int a = 0; a < pointCount; ++a)
    {
        for (int b = a + 1; b < pointCount; ++b)
        {
            for (int c = b + 1; c < pointCount; ++c)
            {
                const Vec3 normal = Cross(points[b] - points[a], points[c] - points[a]);
                if (LengthSquared(normal) > 1.0e-6f)
                {
                    return Normalize(normal);
                }
            }
        }
    }

    return {};
}
}

DemoWorld::DemoWorld()
{
    ResizeStorage();
    Reset();
}

auto DemoWorld::ClampGenerationSettings(WorldGenerationSettings settings) -> WorldGenerationSettings
{
    settings.worldWidth = std::clamp(settings.worldWidth, kMinWorldWidthDepthCells, kMaxWorldWidthDepthCells);
    settings.worldHeight = std::clamp(settings.worldHeight, kMinWorldHeightCells, kMaxWorldHeightCells);
    settings.worldDepth = std::clamp(settings.worldDepth, kMinWorldWidthDepthCells, kMaxWorldWidthDepthCells);
    const int maxMeaningfulChunkSize = std::max({settings.worldWidth, settings.worldHeight, settings.worldDepth, 1});
    settings.activeChunkSize = std::clamp(
        settings.activeChunkSize,
        std::min(kMinActiveChunkSizeCells, maxMeaningfulChunkSize),
        std::min(kMaxActiveChunkSizeCells, maxMeaningfulChunkSize));
    settings.cellSize = Clamp(settings.cellSize, 0.01f, 3.0f);
    settings.terrainRelief = Clamp(settings.terrainRelief, 0.0f, 3.0f);
    settings.waterLevel = Clamp(settings.waterLevel, 0.0f, 0.95f);
    return settings;
}

void DemoWorld::SetGenerationSettings(const WorldGenerationSettings& settings)
{
    generationSettings_ = ClampGenerationSettings(settings);
    width_ = generationSettings_.worldWidth;
    height_ = generationSettings_.worldHeight;
    depth_ = generationSettings_.worldDepth;
    activeChunkSize_ = generationSettings_.activeChunkSize;
    cellSize_ = generationSettings_.cellSize;
    if (cells_.size() != static_cast<std::size_t>(width_ * height_ * depth_))
    {
        ResizeStorage();
        ResetTransientState();
    }
    terrainDirty_ = true;
}

void DemoWorld::ResizeStorage()
{
    const std::size_t cellCount = static_cast<std::size_t>(width_ * height_ * depth_);
    cells_.assign(cellCount, static_cast<std::uint8_t>(MaterialId::Air));
    waterLastLateralDirection_.assign(cellCount, 0);
    waterLastLateralPass_.assign(cellCount, 0u);
    chunkRuntimeStates_.clear();
    activeChunkCount_ = 0;
}

void DemoWorld::ResetTransientState()
{
    for (auto& [chunk, state] : chunkRuntimeStates_)
    {
        static_cast<void>(chunk);
        state.activityLifetime = 0.0f;
        state.meshDirty = true;
        state.meshInitialized = false;
        state.opaqueTriangles.clear();
        state.translucentTriangles.clear();
    }
    activeChunkCount_ = 0;
    solidSurfaceHeightMap_.clear();
    waterSurfaceHeightMap_.clear();
    terrainTriangleCache_.clear();
    translucentTerrainTriangleCache_.clear();
    terrainWireCache_.clear();
    simulationStep_ = 0;
    sandTravelAccumulator_ = 0.0f;
    mudTravelAccumulator_ = 0.0f;
    waterFallAccumulator_ = 0.0f;
    waterSpreadAccumulator_ = 0.0f;
    waterSpreadPass_ = 0u;
    mpmTimeAccumulator_ = 0.0f;
    mpmNextXySlice_ = 0;
    mpmNextZySlice_ = 0;
    xySliceMpm_.reset();
    zySliceMpm_.reset();
    activeMpmBackendName_ = "CPU Reference";
    mpmWorkerCount_ = JobSystem::RecommendWorkerCount(1);
    meshWorkerCount_ = JobSystem::RecommendWorkerCount(1);
    terrainRebuildCooldown_ = 0.0f;
    lastTerrainRebuildMilliseconds_ = 0.0;
    std::fill(waterLastLateralDirection_.begin(), waterLastLateralDirection_.end(), static_cast<std::int8_t>(0));
    std::fill(waterLastLateralPass_.begin(), waterLastLateralPass_.end(), 0u);
}

void DemoWorld::RebuildChunkRuntimeState()
{
    chunkRuntimeStates_.clear();
    activeChunkCount_ = 0;

    for (int z = 0; z < depth_; ++z)
    {
        for (int y = 0; y < height_; ++y)
        {
            for (int x = 0; x < width_; ++x)
            {
                const MaterialId material = GetCell(x, y, z);
                if (material == MaterialId::Air)
                {
                    continue;
                }

                ChunkRuntimeState& state = chunkRuntimeStates_[CellToChunkCoord(x, y, z)];
                ++state.nonAirCellCount;
                if (IsLooseMaterial(material))
                {
                    ++state.looseCellCount;
                }
            }
        }
    }

    for (auto& [chunk, state] : chunkRuntimeStates_)
    {
        static_cast<void>(chunk);
        state.meshDirty = true;
        state.meshInitialized = false;
        state.activityLifetime = 0.0f;
        state.opaqueTriangles.clear();
        state.translucentTriangles.clear();
    }
}

void DemoWorld::NoteCellMaterialChange(
    const int x,
    const int y,
    const int z,
    const MaterialId previousMaterial,
    const MaterialId nextMaterial)
{
    if (previousMaterial == nextMaterial)
    {
        return;
    }

    ChunkRuntimeState& state = chunkRuntimeStates_[CellToChunkCoord(x, y, z)];
    if (previousMaterial != MaterialId::Air && state.nonAirCellCount > 0u)
    {
        --state.nonAirCellCount;
    }
    if (IsLooseMaterial(previousMaterial) && state.looseCellCount > 0u)
    {
        --state.looseCellCount;
    }

    if (nextMaterial != MaterialId::Air)
    {
        ++state.nonAirCellCount;
    }
    if (IsLooseMaterial(nextMaterial))
    {
        ++state.looseCellCount;
    }
}

void DemoWorld::TouchChunk(const ChunkCoord& chunk)
{
    ChunkRuntimeState& state = chunkRuntimeStates_[chunk];
    if (state.activityLifetime <= 0.0f)
    {
        ++activeChunkCount_;
    }
    state.activityLifetime = kActiveChunkLifetimeSeconds;
}

void DemoWorld::MarkChunkMeshDirty(const ChunkCoord& chunk)
{
    const int chunkSpan = std::max(activeChunkSize_, 1);
    const int chunkCountX = std::max(1, (width_ + chunkSpan - 1) / chunkSpan);
    const int chunkCountY = std::max(1, (height_ + chunkSpan - 1) / chunkSpan);
    const int chunkCountZ = std::max(1, (depth_ + chunkSpan - 1) / chunkSpan);

    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                const ChunkCoord neighbor{
                    chunk.x + dx,
                    chunk.y + dy,
                    chunk.z + dz,
                };
                if (neighbor.x < 0 || neighbor.y < 0 || neighbor.z < 0 ||
                    neighbor.x >= chunkCountX || neighbor.y >= chunkCountY || neighbor.z >= chunkCountZ)
                {
                    continue;
                }

                if (dx == 0 && dy == 0 && dz == 0)
                {
                    chunkRuntimeStates_[neighbor].meshDirty = true;
                    continue;
                }

                if (auto neighborIter = chunkRuntimeStates_.find(neighbor); neighborIter != chunkRuntimeStates_.end())
                {
                    neighborIter->second.meshDirty = true;
                }
            }
        }
    }

    terrainDirty_ = true;
}

void DemoWorld::MarkDirtyChunk(const ChunkCoord& chunk)
{
    TouchChunk(chunk);
    MarkChunkMeshDirty(chunk);
}

void DemoWorld::Reset()
{
    generationSettings_ = ClampGenerationSettings(generationSettings_);
    width_ = generationSettings_.worldWidth;
    height_ = generationSettings_.worldHeight;
    depth_ = generationSettings_.worldDepth;
    activeChunkSize_ = generationSettings_.activeChunkSize;
    cellSize_ = generationSettings_.cellSize;
    if (cells_.size() != static_cast<std::size_t>(width_ * height_ * depth_))
    {
        ResizeStorage();
    }

    std::fill(cells_.begin(), cells_.end(), static_cast<std::uint8_t>(MaterialId::Air));
    field_.Clear();
    ResetTransientState();

    const Vec3 minCorner = WorldMin();
    const float basinCenterX = Lerp(-0.12f, 0.10f, HashUnitFloat(generationSettings_.seed, 7, 11));
    const float basinCenterZ = Lerp(-0.08f, 0.14f, HashUnitFloat(generationSettings_.seed, 13, 3));
    const float basinRadiusX = Lerp(0.16f, 0.24f, HashUnitFloat(generationSettings_.seed, 29, 5));
    const float basinRadiusZ = Lerp(0.18f, 0.28f, HashUnitFloat(generationSettings_.seed, 31, 9));
    const int waterSurfaceCell = std::clamp(static_cast<int>(std::round(generationSettings_.waterLevel * static_cast<float>(height_))), 3, height_ - 3);

    for (int z = 0; z < depth_; ++z)
    {
        for (int x = 0; x < width_; ++x)
        {
            const float normalizedX = ((static_cast<float>(x) + 0.5f) / static_cast<float>(width_)) * 2.0f - 1.0f;
            const float normalizedZ = ((static_cast<float>(z) + 0.5f) / static_cast<float>(depth_)) * 2.0f - 1.0f;
            const float worldX = minCorner.x + (static_cast<float>(x) + 0.5f) * cellSize_;
            const float worldZ = minCorner.z + (static_cast<float>(z) + 0.5f) * cellSize_;
            const float broadNoise = 0.5f + 0.5f * ValueNoise2D((normalizedX + 1.0f) * 2.4f, (normalizedZ + 1.0f) * 2.4f, generationSettings_.seed);
            const float detailNoise = 0.5f + 0.5f * ValueNoise2D((normalizedX + 1.0f) * 7.3f, (normalizedZ + 1.0f) * 7.3f, generationSettings_.seed ^ 0x68bc21ebu);
            const float ridgeNoise = 1.0f - std::abs(ValueNoise2D((normalizedX + 1.0f) * 4.5f, (normalizedZ + 1.0f) * 4.5f, generationSettings_.seed ^ 0x51633e2du));
            const float terrainSignal = broadNoise * 0.58f + detailNoise * 0.22f + ridgeNoise * 0.20f;
            const float baseHeightNormalized = 0.18f + terrainSignal * (0.18f * generationSettings_.terrainRelief);

            const float basinX = (normalizedX - basinCenterX) / basinRadiusX;
            const float basinZ = (normalizedZ - basinCenterZ) / basinRadiusZ;
            const float basinDistance = basinX * basinX + basinZ * basinZ;
            const float basinShape = Clamp(1.0f - basinDistance, 0.0f, 1.0f);
            const int basinCarve = static_cast<int>(std::round(basinShape * 3.5f));
            int baseHeight = std::clamp(static_cast<int>(std::round(baseHeightNormalized * static_cast<float>(height_))) - basinCarve, 2, height_ - 4);

            const bool sandBand = normalizedX < (-0.35f + 0.10f * ValueNoise2D(worldZ * 0.11f, worldX * 0.09f, generationSettings_.seed ^ 0x1234567u));
            const bool mudFlat = normalizedZ > 0.08f && normalizedX < 0.32f && basinDistance < 1.5f;
            const bool scrubSand = normalizedX > 0.32f && detailNoise > 0.55f;
            const bool grassField = !sandBand && !mudFlat && broadNoise > 0.52f && normalizedZ < 0.32f && basinDistance > 0.35f;
            const bool gravelFan = !sandBand && ridgeNoise > 0.62f && detailNoise < 0.46f;
            const bool basaltShelf = ridgeNoise > 0.76f && broadNoise < 0.58f;

            for (int y = 0; y <= baseHeight; ++y)
            {
                MaterialId material = MaterialId::CompactedSoil;
                const bool topLayer = y >= baseHeight - 1;
                const bool shallowLayer = y >= baseHeight - 3;

                if (sandBand)
                {
                    material = (y >= baseHeight - 2) ? MaterialId::DrySand : MaterialId::CompactedSoil;
                }
                else if (mudFlat)
                {
                    material = (y >= baseHeight - 1) ? MaterialId::WetMud : MaterialId::CompactedSoil;
                }
                else if (scrubSand && y >= baseHeight - 1)
                {
                    material = MaterialId::DrySand;
                }
                else if (grassField && topLayer)
                {
                    material = MaterialId::Grass;
                }
                else if (gravelFan && shallowLayer)
                {
                    material = topLayer ? MaterialId::Gravel : MaterialId::CompactedSoil;
                }

                if (basaltShelf && y <= baseHeight - 2)
                {
                    material = shallowLayer ? MaterialId::Gravel : MaterialId::BasaltRock;
                }

                SetCellUnchecked(x, y, z, material);
            }

            if (basinShape > 0.05f && baseHeight < waterSurfaceCell)
            {
                for (int y = baseHeight + 1; y <= waterSurfaceCell; ++y)
                {
                    SetCellUnchecked(x, y, z, MaterialId::ShallowWater);
                }
            }
        }
    }

    const int wallMinX = width_ * 29 / 48;
    const int wallMaxX = wallMinX + std::max(2, width_ / 16);
    const int wallMinZ = depth_ * 18 / 48;
    const int wallMaxZ = depth_ * 30 / 48;
    const int wallMinY = height_ * 6 / 20;
    const int wallMaxY = height_ * 11 / 20;
    for (int z = wallMinZ; z <= wallMaxZ; ++z)
    {
        for (int x = wallMinX; x <= wallMaxX; ++x)
        {
            for (int y = wallMinY; y <= wallMaxY; ++y)
            {
                SetCellUnchecked(x, y, z, MaterialId::BrittleConcrete);
            }
        }
    }

    const int breachZStart = depth_ * 21 / 48;
    const int breachZEnd = depth_ * 27 / 48;
    const int breachY = height_ * 8 / 20;
    for (int z = breachZStart; z <= breachZEnd; ++z)
    {
        SetCellUnchecked(wallMinX + 1, breachY, z, MaterialId::Air);
        if (wallMinX + 2 <= wallMaxX)
        {
            SetCellUnchecked(wallMinX + 2, breachY, z, MaterialId::Air);
        }
    }

    RebuildChunkRuntimeState();
    terrainDirty_ = true;
    EnsureMpmBackend();
}

void DemoWorld::Tick(const float dt)
{
    const ScopedProfileSection tickScope(profiler_, "World Tick Internal");
    terrainRebuildCooldown_ = std::max(terrainRebuildCooldown_ - dt, 0.0f);
    std::optional<LooseSimulationBounds> looseBounds;
    {
        const ScopedProfileSection scope(profiler_, "World Loose Bounds");
        looseBounds = ComputeLooseSimulationBounds();
    }
    if (looseBounds.has_value())
    {
        sandTravelAccumulator_ += dt * kDrySandTravelSpeedMetersPerSecond;
        mudTravelAccumulator_ += dt * kWetMudTravelSpeedMetersPerSecond;
        waterFallAccumulator_ += dt * kWaterFallSpeedMetersPerSecond;
        waterSpreadAccumulator_ += dt * kWaterSpreadSpeedMetersPerSecond;

        int sandSteps = 0;
        {
            const ScopedProfileSection scope(profiler_, "World Sand");
            while (sandTravelAccumulator_ >= cellSize_ && sandSteps < 4)
            {
                SimulateDrySandPass(*looseBounds);
                sandTravelAccumulator_ -= cellSize_;
                ++sandSteps;
            }
        }

        int mudSteps = 0;
        {
            const ScopedProfileSection scope(profiler_, "World Mud");
            while (mudTravelAccumulator_ >= cellSize_ && mudSteps < 2)
            {
                SimulateWetMudPass(*looseBounds);
                mudTravelAccumulator_ -= cellSize_;
                ++mudSteps;
            }
        }

        int waterFallSteps = 0;
        {
            const ScopedProfileSection scope(profiler_, "World Water Fall");
            while (waterFallAccumulator_ >= cellSize_ && waterFallSteps < 4)
            {
                SimulateWaterFallPass(*looseBounds);
                waterFallAccumulator_ -= cellSize_;
                ++waterFallSteps;
            }
        }

        int waterSpreadSteps = 0;
        {
            const ScopedProfileSection scope(profiler_, "World Water Spread");
            while (waterSpreadAccumulator_ >= cellSize_ && waterSpreadSteps < 2)
            {
                ++waterSpreadPass_;
                SimulateWaterSpreadPass(*looseBounds);
                waterSpreadAccumulator_ -= cellSize_;
                ++waterSpreadSteps;
            }
        }

        {
            const ScopedProfileSection scope(profiler_, "World MPM");
            SimulateLooseMaterialMpm(dt);
        }
    }
    else
    {
        sandTravelAccumulator_ = 0.0f;
        mudTravelAccumulator_ = 0.0f;
        waterFallAccumulator_ = 0.0f;
        waterSpreadAccumulator_ = 0.0f;
    }

    {
        const ScopedProfileSection scope(profiler_, "World Chunk Decay");
        for (auto& [chunk, state] : chunkRuntimeStates_)
        {
            static_cast<void>(chunk);
            if (state.activityLifetime <= 0.0f)
            {
                continue;
            }

            state.activityLifetime = std::max(state.activityLifetime - dt, 0.0f);
            if (state.activityLifetime <= 0.0f && activeChunkCount_ > 0u)
            {
                --activeChunkCount_;
            }
        }
    }

    PruneRetiredChunkStates();
}

bool DemoWorld::Save(const std::filesystem::path& path) const
{
    ByteWriter writer;
    writer.WritePod(kDemoWorldMagic);
    writer.WritePod(kDemoWorldVersion);
    writer.WritePod(width_);
    writer.WritePod(height_);
    writer.WritePod(depth_);
    writer.WritePod(activeChunkSize_);
    writer.WritePod(generationSettings_.seed);
    writer.WritePod(generationSettings_.cellSize);
    writer.WritePod(generationSettings_.terrainRelief);
    writer.WritePod(generationSettings_.waterLevel);
    writer.WriteBytes(std::as_bytes(std::span(cells_)));

    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream output(path, std::ios::binary);
    if (!output)
    {
        return false;
    }

    const auto bytes = writer.Span();
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

auto DemoWorld::CaptureSnapshot() const -> DenseWorldSnapshot
{
    DenseWorldSnapshot snapshot{};
    snapshot.settings = generationSettings_;
    snapshot.cells = cells_;
    return snapshot;
}

bool DemoWorld::ApplySnapshot(const DenseWorldSnapshot& snapshot)
{
    const WorldGenerationSettings clampedSettings = ClampGenerationSettings(snapshot.settings);
    const std::size_t expectedCellCount =
        static_cast<std::size_t>(clampedSettings.worldWidth) *
        static_cast<std::size_t>(clampedSettings.worldHeight) *
        static_cast<std::size_t>(clampedSettings.worldDepth);
    if (snapshot.cells.size() != expectedCellCount)
    {
        return false;
    }

    generationSettings_ = clampedSettings;
    width_ = generationSettings_.worldWidth;
    height_ = generationSettings_.worldHeight;
    depth_ = generationSettings_.worldDepth;
    activeChunkSize_ = generationSettings_.activeChunkSize;
    cellSize_ = generationSettings_.cellSize;

    ResizeStorage();
    cells_ = snapshot.cells;
    field_.Clear();
    ResetTransientState();
    RebuildChunkRuntimeState();
    terrainDirty_ = true;
    EnsureMpmBackend();
    return true;
}

bool DemoWorld::Load(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    const std::vector<char> rawBytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (rawBytes.size() < sizeof(std::uint32_t) * 2u)
    {
        return LoadLegacyMaterialField(path);
    }

    const auto bytes = std::as_bytes(std::span(rawBytes));
    try
    {
        ByteReader reader(bytes);
        const std::uint32_t magic = reader.ReadPod<std::uint32_t>();
        if (magic != kDemoWorldMagic)
        {
            return LoadLegacyMaterialField(path);
        }

        const std::uint32_t version = reader.ReadPod<std::uint32_t>();
        if (version != 2u && version != kDemoWorldVersion)
        {
            return false;
        }

        WorldGenerationSettings loadedSettings{};
        loadedSettings.worldWidth = reader.ReadPod<int>();
        loadedSettings.worldHeight = reader.ReadPod<int>();
        loadedSettings.worldDepth = reader.ReadPod<int>();
        if (version >= 3u)
        {
            loadedSettings.activeChunkSize = reader.ReadPod<int>();
        }

        loadedSettings.seed = reader.ReadPod<std::uint32_t>();
        loadedSettings.cellSize = reader.ReadPod<float>();
        loadedSettings.terrainRelief = reader.ReadPod<float>();
        loadedSettings.waterLevel = reader.ReadPod<float>();
        generationSettings_ = ClampGenerationSettings(loadedSettings);
        width_ = generationSettings_.worldWidth;
        height_ = generationSettings_.worldHeight;
        depth_ = generationSettings_.worldDepth;
        activeChunkSize_ = generationSettings_.activeChunkSize;
        cellSize_ = generationSettings_.cellSize;

        if (width_ <= 0 || height_ <= 0 || depth_ <= 0)
        {
            return false;
        }

        ResizeStorage();
        const auto cellBytes = reader.ReadBytes(cells_.size());
        std::memcpy(cells_.data(), cellBytes.data(), cellBytes.size());
        if (!reader.Empty())
        {
            return false;
        }

        field_.Clear();
        ResetTransientState();
        RebuildChunkRuntimeState();
        terrainDirty_ = true;
        EnsureMpmBackend();
        return true;
    }
    catch (const std::exception&)
    {
        return LoadLegacyMaterialField(path);
    }
}

auto DemoWorld::MaterialAtWorldPosition(const Vec3& position) const -> MaterialId
{
    const Int3 cell = WorldToCell(position);
    return MaterialAtCell(cell.x, cell.y, cell.z);
}

auto DemoWorld::CellAtWorldPosition(const Vec3& position) const -> Int3
{
    return WorldToCell(position);
}

auto DemoWorld::MaterialAtCell(const int x, const int y, const int z) const -> MaterialId
{
    if (!InBounds(x, y, z))
    {
        return MaterialId::Air;
    }

    return GetCell(x, y, z);
}

bool DemoWorld::OverlapsBlocking(const Vec3& center, const Vec3& halfExtents) const
{
    const Vec3 minimum = center - halfExtents;
    const Vec3 maximum = center + halfExtents;

    const Int3 minCell = WorldToCell(minimum);
    const Int3 maxCell = WorldToCell(maximum);

    if (maximum.y < 0.0f)
    {
        return true;
    }

    for (int z = minCell.z; z <= maxCell.z; ++z)
    {
        for (int y = minCell.y; y <= maxCell.y; ++y)
        {
            for (int x = minCell.x; x <= maxCell.x; ++x)
            {
                if (!InBounds(x, y, z))
                {
                    if (x < 0 || x >= width_ || z < 0 || z >= depth_ || y < 0)
                    {
                        return true;
                    }
                    continue;
                }

                if (IsBlocking(GetCell(x, y, z)))
                {
                    return true;
                }
            }
        }
    }

    return false;
}

auto DemoWorld::Raycast(const Ray& ray, const float maxDistance) const -> RaycastHit
{
    RaycastHit hit{};
    const Vec3 direction = Normalize(ray.direction);
    if (LengthSquared(direction) <= 0.0f)
    {
        return hit;
    }

    Int3 previousCell = WorldToCell(ray.origin);
    for (float distance = 0.0f; distance <= maxDistance; distance += cellSize_ * 0.2f)
    {
        const Vec3 samplePosition = ray.origin + direction * distance;
        const Int3 cell = WorldToCell(samplePosition);

        if (!InBounds(cell.x, cell.y, cell.z))
        {
            if (distance > 0.0f)
            {
                break;
            }
            previousCell = cell;
            continue;
        }

        const MaterialId material = GetCell(cell.x, cell.y, cell.z);
        if (material != MaterialId::Air)
        {
            hit.hit = true;
            hit.position = samplePosition;
            hit.cell = cell;
            hit.material = material;
            hit.distance = distance;

            const Int3 delta{
                cell.x - previousCell.x,
                cell.y - previousCell.y,
                cell.z - previousCell.z,
            };
            hit.normal = Normalize(Vec3{
                delta.x != 0 ? -static_cast<float>(delta.x) : 0.0f,
                delta.y != 0 ? -static_cast<float>(delta.y) : 0.0f,
                delta.z != 0 ? -static_cast<float>(delta.z) : 0.0f,
            });
            if (LengthSquared(hit.normal) <= 0.0f)
            {
                hit.normal = -direction;
            }
            return hit;
        }

        previousCell = cell;
    }

    return hit;
}

void DemoWorld::ApplyDig(const Vec3& center, const float radius, const float power)
{
    const Int3 minCell = WorldToCell(center - Vec3{radius, radius, radius});
    const Int3 maxCell = WorldToCell(center + Vec3{radius, radius, radius});
    std::vector<ChunkCoord> changedChunks;

    for (int z = minCell.z; z <= maxCell.z; ++z)
    {
        for (int y = minCell.y; y <= maxCell.y; ++y)
        {
            for (int x = minCell.x; x <= maxCell.x; ++x)
            {
                if (!InBounds(x, y, z))
                {
                    continue;
                }

                const MaterialId material = GetCell(x, y, z);
                if (material == MaterialId::Air)
                {
                    continue;
                }

                const Vec3 offset = CellCenterToWorld(x, y, z) - center;
                const float distance = Length(offset);
                if (distance > radius)
                {
                    continue;
                }

                const MaterialProperties properties = GetMaterialProperties(material);
                const float intensity = power * (1.0f - (distance / radius));
                if (intensity < properties.digResistance)
                {
                    continue;
                }

                SetCellBatched(x, y, z, MaterialId::Air, changedChunks);
            }
        }
    }

    CommitChunkEdits(changedChunks);
}

void DemoWorld::ApplyRifleImpact(const Vec3& center, const Vec3& direction, const MaterialId impactMaterial)
{
    float radius = 0.9f;
    float power = 0.75f;

    if (impactMaterial == MaterialId::BrittleConcrete)
    {
        const float forwardAlignment = std::abs(Dot(Normalize(direction), Vec3{0.0f, 0.0f, 1.0f}));
        radius = forwardAlignment < 0.25f ? 0.45f : 1.0f;
        power = forwardAlignment < 0.25f ? 0.35f : 0.85f;
    }

    const Int3 minCell = WorldToCell(center - Vec3{radius, radius, radius});
    const Int3 maxCell = WorldToCell(center + Vec3{radius, radius, radius});
    std::vector<ChunkCoord> changedChunks;
    const Vec3 normalizedDirection = LengthSquared(direction) > 1.0e-6f ? Normalize(direction) : Vec3{0.0f, 0.0f, 1.0f};

    const auto displacedMaterialForImpact = [](const MaterialId material, const float intensity) -> MaterialId
    {
        switch (material)
        {
        case MaterialId::Grass:
            return intensity > 0.42f ? MaterialId::DrySand : MaterialId::WetMud;
        case MaterialId::CompactedSoil:
            return intensity > 0.54f ? MaterialId::DrySand : MaterialId::WetMud;
        case MaterialId::BrittleConcrete:
            return intensity > 0.74f ? MaterialId::Gravel : MaterialId::CompactedSoil;
        case MaterialId::BasaltRock:
            return intensity > 0.94f ? MaterialId::Gravel : MaterialId::BasaltRock;
        default:
            return material;
        }
    };

    const auto tryDisplaceCell = [&](const int x, const int y, const int z, const MaterialId displacedMaterial, const Vec3& impulse) -> bool
    {
        const Vec3 pushDirection = LengthSquared(impulse) > 1.0e-6f ? Normalize(impulse) : Vec3{0.0f, 1.0f, 0.0f};
        const int stepX = pushDirection.x > 0.15f ? 1 : (pushDirection.x < -0.15f ? -1 : 0);
        const int stepY = pushDirection.y > 0.10f ? 1 : (pushDirection.y < -0.10f ? -1 : 0);
        const int stepZ = pushDirection.z > 0.15f ? 1 : (pushDirection.z < -0.15f ? -1 : 0);

        const std::array<Int3, 10> candidateCells = {{
            {x + stepX, y + std::max(1, stepY), z + stepZ},
            {x + stepX, y + 1, z + stepZ},
            {x + stepX, y, z + stepZ},
            {x + stepX, y + 1, z},
            {x, y + 1, z + stepZ},
            {x, y + 1, z},
            {x + stepX, y, z},
            {x, y, z + stepZ},
            {x + 1, y + 1, z},
            {x - 1, y + 1, z},
        }};

        for (const Int3& cell : candidateCells)
        {
            if (!InBounds(cell.x, cell.y, cell.z))
            {
                continue;
            }

            const MaterialId targetMaterial = GetCell(cell.x, cell.y, cell.z);
            if (targetMaterial != MaterialId::Air && targetMaterial != MaterialId::ShallowWater)
            {
                continue;
            }

            SetCellBatched(cell.x, cell.y, cell.z, displacedMaterial, changedChunks);
            SetCellBatched(x, y, z, targetMaterial == MaterialId::ShallowWater ? MaterialId::ShallowWater : MaterialId::Air, changedChunks);
            return true;
        }

        return false;
    };

    for (int z = minCell.z; z <= maxCell.z; ++z)
    {
        for (int y = minCell.y; y <= maxCell.y; ++y)
        {
            for (int x = minCell.x; x <= maxCell.x; ++x)
            {
                if (!InBounds(x, y, z))
                {
                    continue;
                }

                const MaterialId material = GetCell(x, y, z);
                if (material == MaterialId::Air)
                {
                    continue;
                }

                const Vec3 offset = CellCenterToWorld(x, y, z) - center;
                const float distance = Length(offset);
                if (distance > radius)
                {
                    continue;
                }

                const MaterialProperties properties = GetMaterialProperties(material);
                const float intensity = power * (1.0f - (distance / radius));
                if (intensity < properties.rifleResistance)
                {
                    continue;
                }

                const MaterialId displacedMaterial = displacedMaterialForImpact(material, intensity);
                const Vec3 radialDirection = LengthSquared(offset) > 1.0e-6f ? Normalize(offset) : -normalizedDirection;
                const Vec3 impulse = -normalizedDirection * 0.75f + radialDirection * 0.45f + Vec3{0.0f, 0.22f, 0.0f};
                if (tryDisplaceCell(x, y, z, displacedMaterial, impulse))
                {
                    continue;
                }

                if (displacedMaterial != material)
                {
                    SetCellBatched(x, y, z, displacedMaterial, changedChunks);
                }
                else if (intensity > properties.rifleResistance + 0.22f)
                {
                    SetCellBatched(x, y, z, MaterialId::Air, changedChunks);
                }
            }
        }
    }

    CommitChunkEdits(changedChunks);
}

void DemoWorld::ApplyExplosion(const Vec3& center, float radius, float power)
{
    const MaterialId centerMaterial = MaterialAtWorldPosition(center);
    if (centerMaterial == MaterialId::ShallowWater)
    {
        radius *= 1.35f;
        power *= 0.55f;
    }
    else if (centerMaterial == MaterialId::WetMud)
    {
        radius *= 1.10f;
        power *= 0.70f;
    }

    const Int3 minCell = WorldToCell(center - Vec3{radius, radius, radius});
    const Int3 maxCell = WorldToCell(center + Vec3{radius, radius, radius});
    std::vector<ChunkCoord> changedChunks;

    const auto displacedMaterialForBlast = [](const MaterialId material, const float intensity) -> MaterialId
    {
        switch (material)
        {
        case MaterialId::Grass:
            return intensity > 0.34f ? MaterialId::DrySand : MaterialId::WetMud;
        case MaterialId::CompactedSoil:
            return intensity > 0.40f ? MaterialId::DrySand : MaterialId::WetMud;
        case MaterialId::BrittleConcrete:
            return intensity > 0.50f ? MaterialId::Gravel : MaterialId::CompactedSoil;
        case MaterialId::BasaltRock:
            return intensity > 0.88f ? MaterialId::Gravel : MaterialId::BasaltRock;
        default:
            return material;
        }
    };

    const auto tryDisplaceCell = [&](const int x, const int y, const int z, const MaterialId displacedMaterial, const Vec3& impulse) -> bool
    {
        const Vec3 pushDirection = LengthSquared(impulse) > 1.0e-6f ? Normalize(impulse) : Vec3{0.0f, 1.0f, 0.0f};
        const int stepX = pushDirection.x > 0.15f ? 1 : (pushDirection.x < -0.15f ? -1 : 0);
        const int stepY = pushDirection.y > 0.10f ? 1 : (pushDirection.y < -0.10f ? -1 : 0);
        const int stepZ = pushDirection.z > 0.15f ? 1 : (pushDirection.z < -0.15f ? -1 : 0);

        const std::array<Int3, 12> candidateCells = {{
            {x + stepX, y + std::max(1, stepY), z + stepZ},
            {x + stepX, y + 1, z + stepZ},
            {x + stepX, y, z + stepZ},
            {x + stepX, y + 1, z},
            {x, y + 1, z + stepZ},
            {x, y + 1, z},
            {x + stepX, y, z},
            {x, y, z + stepZ},
            {x + 1, y + 1, z},
            {x - 1, y + 1, z},
            {x, y + 1, z + 1},
            {x, y + 1, z - 1},
        }};

        for (const Int3& cell : candidateCells)
        {
            if (!InBounds(cell.x, cell.y, cell.z))
            {
                continue;
            }

            const MaterialId targetMaterial = GetCell(cell.x, cell.y, cell.z);
            if (targetMaterial != MaterialId::Air && targetMaterial != MaterialId::ShallowWater)
            {
                continue;
            }

            SetCellBatched(cell.x, cell.y, cell.z, displacedMaterial, changedChunks);
            SetCellBatched(x, y, z, targetMaterial == MaterialId::ShallowWater ? MaterialId::ShallowWater : MaterialId::Air, changedChunks);
            return true;
        }

        return false;
    };

    for (int z = minCell.z; z <= maxCell.z; ++z)
    {
        for (int y = minCell.y; y <= maxCell.y; ++y)
        {
            for (int x = minCell.x; x <= maxCell.x; ++x)
            {
                if (!InBounds(x, y, z))
                {
                    continue;
                }

                const MaterialId material = GetCell(x, y, z);
                if (material == MaterialId::Air)
                {
                    continue;
                }

                const Vec3 offset = CellCenterToWorld(x, y, z) - center;
                const float distance = Length(offset);
                if (distance > radius)
                {
                    continue;
                }

                const MaterialProperties properties = GetMaterialProperties(material);
                const float intensity = power * (1.0f - (distance / radius));

                if (intensity >= properties.blastResistance)
                {
                    const MaterialId displacedMaterial = displacedMaterialForBlast(material, intensity);
                    const Vec3 impulse = (LengthSquared(offset) > 1.0e-6f ? Normalize(offset) : Vec3{0.0f, 1.0f, 0.0f}) + Vec3{0.0f, 0.35f, 0.0f};
                    if (tryDisplaceCell(x, y, z, displacedMaterial, impulse))
                    {
                        continue;
                    }

                    if (displacedMaterial != material)
                    {
                        SetCellBatched(x, y, z, displacedMaterial, changedChunks);
                    }
                    else
                    {
                        SetCellBatched(x, y, z, MaterialId::Air, changedChunks);
                    }
                }
                else if (material == MaterialId::CompactedSoil && intensity > 0.26f)
                {
                    SetCellBatched(x, y, z, MaterialId::WetMud, changedChunks);
                }
            }
        }
    }

    CommitChunkEdits(changedChunks);
}

void DemoWorld::EditCell(const int x, const int y, const int z, const MaterialId material)
{
    SetCell(x, y, z, material);
}

void DemoWorld::GatherRenderGeometry(
    std::span<const render::ColorVertex3D>& terrainTriangles,
    std::vector<render::ColorVertex3D>& debugLines,
    const bool showWireframe,
    const bool showActiveChunks)
{
    std::span<const render::ColorVertex3D> unusedTranslucentTriangles;
    GatherRenderGeometryExtended(terrainTriangles, unusedTranslucentTriangles, debugLines, showWireframe, showActiveChunks);
}

void DemoWorld::GatherRenderGeometryExtended(
    std::span<const render::ColorVertex3D>& opaqueTerrainTriangles,
    std::span<const render::ColorVertex3D>& translucentTerrainTriangles,
    std::vector<render::ColorVertex3D>& debugLines,
    const bool showWireframe,
    const bool showActiveChunks)
{
    GatherRenderGeometrySmoothed(opaqueTerrainTriangles, translucentTerrainTriangles, debugLines, showWireframe, showActiveChunks);
}

void DemoWorld::GatherRenderGeometrySmoothed(
    std::span<const render::ColorVertex3D>& opaqueTerrainTriangles,
    std::span<const render::ColorVertex3D>& translucentTerrainTriangles,
    std::vector<render::ColorVertex3D>& debugLines,
    const bool showWireframe,
    const bool showActiveChunks)
{
    const bool meshCacheInitialized = !solidSurfaceHeightMap_.empty() && !waterSurfaceHeightMap_.empty();
    if (terrainDirty_ && (!meshCacheInitialized || terrainRebuildCooldown_ <= 0.0f))
    {
        RebuildMeshCache();
    }

    opaqueTerrainTriangles = terrainTriangleCache_;
    translucentTerrainTriangles = translucentTerrainTriangleCache_;
    debugLines.clear();
    const std::size_t wireVertexReserve = showWireframe
        ? (terrainTriangleCache_.size() + translucentTerrainTriangleCache_.size()) * 2u
        : 0u;
    debugLines.reserve(wireVertexReserve + activeChunkCount_ * 24u);

    if (showWireframe)
    {
        const auto appendWireframe = [&debugLines](const std::span<const render::ColorVertex3D> triangles, const Vec4& color)
        {
            for (std::size_t index = 0; index + 2 < triangles.size(); index += 3)
            {
                const Vec3& a = triangles[index + 0].position;
                const Vec3& b = triangles[index + 1].position;
                const Vec3& c = triangles[index + 2].position;
                debugLines.push_back({a, color});
                debugLines.push_back({b, color});
                debugLines.push_back({b, color});
                debugLines.push_back({c, color});
                debugLines.push_back({c, color});
                debugLines.push_back({a, color});
            }
        };

        appendWireframe(terrainTriangleCache_, MakeColor(0.05f, 0.05f, 0.05f, 0.85f));
        appendWireframe(translucentTerrainTriangleCache_, MakeColor(0.05f, 0.16f, 0.24f, 0.65f));
    }

    if (showActiveChunks)
    {
        for (const auto& [chunk, state] : chunkRuntimeStates_)
        {
            if (state.activityLifetime <= 0.0f)
            {
                continue;
            }

            const float lifetime = state.activityLifetime;
            const float intensity = Clamp(lifetime / kActiveChunkLifetimeSeconds, 0.2f, 1.0f);
            const Vec4 color = MakeColor(0.25f, 0.95f, 1.0f, intensity);
            const int chunkSpan = std::max(activeChunkSize_, 1);
            const Vec3 minCorner = WorldMin() + Vec3{
                static_cast<float>(chunk.x * chunkSpan) * cellSize_,
                static_cast<float>(chunk.y * chunkSpan) * cellSize_,
                static_cast<float>(chunk.z * chunkSpan) * cellSize_,
            };
            const Vec3 maxCorner = minCorner + Vec3{
                static_cast<float>(chunkSpan) * cellSize_,
                static_cast<float>(chunkSpan) * cellSize_,
                static_cast<float>(chunkSpan) * cellSize_,
            };
            AppendBoxLines(debugLines, minCorner, maxCorner, color);
        }
    }
}

auto DemoWorld::WorldMin() const -> Vec3
{
    return {
        -0.5f * static_cast<float>(width_) * cellSize_,
        0.0f,
        -0.5f * static_cast<float>(depth_) * cellSize_,
    };
}

auto DemoWorld::WorldMax() const -> Vec3
{
    return {
        WorldMin().x + static_cast<float>(width_) * cellSize_,
        static_cast<float>(height_) * cellSize_,
        WorldMin().z + static_cast<float>(depth_) * cellSize_,
    };
}

bool DemoWorld::InBounds(const int x, const int y, const int z) const
{
    return x >= 0 && x < width_ &&
           y >= 0 && y < height_ &&
           z >= 0 && z < depth_;
}

auto DemoWorld::WorldToCell(const Vec3& position) const -> Int3
{
    const Vec3 minimum = WorldMin();
    return {
        static_cast<int>(std::floor((position.x - minimum.x) / cellSize_)),
        static_cast<int>(std::floor(position.y / cellSize_)),
        static_cast<int>(std::floor((position.z - minimum.z) / cellSize_)),
    };
}

auto DemoWorld::CellCenterToWorld(const int x, const int y, const int z) const -> Vec3
{
    const Vec3 minimum = WorldMin();
    return {
        minimum.x + (static_cast<float>(x) + 0.5f) * cellSize_,
        (static_cast<float>(y) + 0.5f) * cellSize_,
        minimum.z + (static_cast<float>(z) + 0.5f) * cellSize_,
    };
}

auto DemoWorld::CellMinToWorld(const int x, const int y, const int z) const -> Vec3
{
    const Vec3 minimum = WorldMin();
    return {
        minimum.x + static_cast<float>(x) * cellSize_,
        static_cast<float>(y) * cellSize_,
        minimum.z + static_cast<float>(z) * cellSize_,
    };
}

auto DemoWorld::CellToChunkCoord(const int x, const int y, const int z) const -> ChunkCoord
{
    const int chunkSpan = std::max(activeChunkSize_, 1);
    return {
        x / chunkSpan,
        y / chunkSpan,
        z / chunkSpan,
    };
}

auto DemoWorld::CellIndex(const int x, const int y, const int z) const -> std::size_t
{
    return static_cast<std::size_t>(((z * height_) + y) * width_ + x);
}

auto DemoWorld::GetCell(const int x, const int y, const int z) const -> MaterialId
{
    return static_cast<MaterialId>(cells_[CellIndex(x, y, z)]);
}

void DemoWorld::SetCellUnchecked(const int x, const int y, const int z, const MaterialId material)
{
    cells_[CellIndex(x, y, z)] = static_cast<std::uint8_t>(material);
}

auto DemoWorld::IsRenderable(const MaterialId material) const -> bool
{
    return material != MaterialId::Air;
}

auto DemoWorld::IsBlocking(const MaterialId material) const -> bool
{
    return BlocksMovement(material);
}

void DemoWorld::MarkDirty(const int x, const int y, const int z)
{
    MarkDirtyChunk(CellToChunkCoord(x, y, z));
}

void DemoWorld::ClearCellWaterMetadata(const std::size_t index)
{
    if (index >= waterLastLateralDirection_.size() || index >= waterLastLateralPass_.size())
    {
        return;
    }

    waterLastLateralDirection_[index] = 0;
    waterLastLateralPass_[index] = 0u;
}

void DemoWorld::SetCellBatched(
    const int x,
    const int y,
    const int z,
    const MaterialId material,
    std::vector<ChunkCoord>& changedChunks)
{
    if (!InBounds(x, y, z))
    {
        return;
    }

    const MaterialId previousMaterial = GetCell(x, y, z);
    if (previousMaterial == material)
    {
        return;
    }

    NoteCellMaterialChange(x, y, z, previousMaterial, material);
    SetCellUnchecked(x, y, z, material);
    ClearCellWaterMetadata(CellIndex(x, y, z));
    changedChunks.push_back(CellToChunkCoord(x, y, z));
}

void DemoWorld::CommitChunkEdits(std::vector<ChunkCoord>& changedChunks)
{
    if (changedChunks.empty())
    {
        return;
    }

    std::sort(changedChunks.begin(), changedChunks.end(), [](const ChunkCoord& lhs, const ChunkCoord& rhs)
    {
        if (lhs.z != rhs.z)
        {
            return lhs.z < rhs.z;
        }
        if (lhs.y != rhs.y)
        {
            return lhs.y < rhs.y;
        }
        return lhs.x < rhs.x;
    });
    changedChunks.erase(std::unique(changedChunks.begin(), changedChunks.end()), changedChunks.end());

    for (const ChunkCoord& chunk : changedChunks)
    {
        MarkDirtyChunk(chunk);
    }

    changedChunks.clear();
}

void DemoWorld::PruneRetiredChunkStates()
{
    for (auto iter = chunkRuntimeStates_.begin(); iter != chunkRuntimeStates_.end();)
    {
        const ChunkRuntimeState& state = iter->second;
        const bool retired =
            state.nonAirCellCount == 0u &&
            state.looseCellCount == 0u &&
            state.activityLifetime <= 0.0f &&
            !state.meshDirty &&
            state.opaqueTriangles.empty() &&
            state.translucentTriangles.empty();
        if (retired)
        {
            iter = chunkRuntimeStates_.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}

void DemoWorld::SetCell(const int x, const int y, const int z, const MaterialId material)
{
    if (!InBounds(x, y, z))
    {
        return;
    }

    const MaterialId previousMaterial = GetCell(x, y, z);
    if (previousMaterial == material)
    {
        return;
    }

    NoteCellMaterialChange(x, y, z, previousMaterial, material);
    SetCellUnchecked(x, y, z, material);
    ClearCellWaterMetadata(CellIndex(x, y, z));
    MarkDirty(x, y, z);
}

auto DemoWorld::TryMoveCell(
    const int fromX,
    const int fromY,
    const int fromZ,
    const int toX,
    const int toY,
    const int toZ,
    const bool allowSwapWithWater,
    const std::int8_t waterLateralDirection,
    std::vector<ChunkCoord>* changedChunks) -> bool
{
    if (!InBounds(fromX, fromY, fromZ) || !InBounds(toX, toY, toZ))
    {
        return false;
    }

    const MaterialId fromMaterial = GetCell(fromX, fromY, fromZ);
    const MaterialId toMaterial = GetCell(toX, toY, toZ);
    const std::size_t fromIndex = CellIndex(fromX, fromY, fromZ);
    const std::size_t toIndex = CellIndex(toX, toY, toZ);

    if (toMaterial == MaterialId::Air)
    {
        NoteCellMaterialChange(toX, toY, toZ, toMaterial, fromMaterial);
        NoteCellMaterialChange(fromX, fromY, fromZ, fromMaterial, MaterialId::Air);
        SetCellUnchecked(toX, toY, toZ, fromMaterial);
        SetCellUnchecked(fromX, fromY, fromZ, MaterialId::Air);
        if (fromMaterial == MaterialId::ShallowWater && fromY == toY && waterLateralDirection != 0)
        {
            waterLastLateralDirection_[toIndex] = waterLateralDirection;
            waterLastLateralPass_[toIndex] = waterSpreadPass_;
        }
        else
        {
            ClearCellWaterMetadata(toIndex);
        }
        ClearCellWaterMetadata(fromIndex);
        if (changedChunks != nullptr)
        {
            changedChunks->push_back(CellToChunkCoord(fromX, fromY, fromZ));
            changedChunks->push_back(CellToChunkCoord(toX, toY, toZ));
        }
        else
        {
            MarkDirty(fromX, fromY, fromZ);
            MarkDirty(toX, toY, toZ);
        }
        return true;
    }

    if (allowSwapWithWater && toMaterial == MaterialId::ShallowWater && fromMaterial != MaterialId::ShallowWater)
    {
        NoteCellMaterialChange(toX, toY, toZ, toMaterial, fromMaterial);
        NoteCellMaterialChange(fromX, fromY, fromZ, fromMaterial, MaterialId::ShallowWater);
        SetCellUnchecked(toX, toY, toZ, fromMaterial);
        SetCellUnchecked(fromX, fromY, fromZ, MaterialId::ShallowWater);
        ClearCellWaterMetadata(toIndex);
        ClearCellWaterMetadata(fromIndex);
        if (changedChunks != nullptr)
        {
            changedChunks->push_back(CellToChunkCoord(fromX, fromY, fromZ));
            changedChunks->push_back(CellToChunkCoord(toX, toY, toZ));
        }
        else
        {
            MarkDirty(fromX, fromY, fromZ);
            MarkDirty(toX, toY, toZ);
        }
        return true;
    }

    return false;
}

auto DemoWorld::CountAdjacentWater(const int x, const int y, const int z) const -> int
{
    int count = 0;
    constexpr std::array<Int3, 4> kDirections = {
        Int3{1, 0, 0},
        Int3{-1, 0, 0},
        Int3{0, 0, 1},
        Int3{0, 0, -1},
    };

    for (const Int3& direction : kDirections)
    {
        const int neighborX = x + direction.x;
        const int neighborZ = z + direction.z;
        if (InBounds(neighborX, y, neighborZ) && GetCell(neighborX, y, neighborZ) == MaterialId::ShallowWater)
        {
            ++count;
        }
    }

    return count;
}

auto DemoWorld::CanWaterSpreadLaterally(const int x, const int y, const int z) const -> bool
{
    if (y + 1 < height_ && GetCell(x, y + 1, z) == MaterialId::ShallowWater)
    {
        return true;
    }

    return CountAdjacentWater(x, y, z) >= 2;
}

auto DemoWorld::ComputeLooseSimulationBounds() const -> std::optional<LooseSimulationBounds>
{
    const int chunkSpan = std::max(activeChunkSize_, 1);
    LooseSimulationBounds bounds{};
    bool foundBounds = false;

    for (const auto& [chunk, state] : chunkRuntimeStates_)
    {
        if (state.activityLifetime <= 0.0f || state.looseCellCount == 0u)
        {
            continue;
        }

        const int chunkMinX = std::max(0, chunk.x * chunkSpan - kLooseSimulationPaddingCells);
        const int chunkMaxX = std::min(width_ - 1, chunk.x * chunkSpan + chunkSpan - 1 + kLooseSimulationPaddingCells);
        const int chunkMinY = std::max(0, chunk.y * chunkSpan - kLooseSimulationPaddingCells);
        const int chunkMaxY = std::min(height_ - 1, chunk.y * chunkSpan + chunkSpan - 1 + kLooseSimulationPaddingCells);
        const int chunkMinZ = std::max(0, chunk.z * chunkSpan - kLooseSimulationPaddingCells);
        const int chunkMaxZ = std::min(depth_ - 1, chunk.z * chunkSpan + chunkSpan - 1 + kLooseSimulationPaddingCells);

        if (!foundBounds)
        {
            bounds.minX = chunkMinX;
            bounds.maxX = chunkMaxX;
            bounds.minY = chunkMinY;
            bounds.maxY = chunkMaxY;
            bounds.minZ = chunkMinZ;
            bounds.maxZ = chunkMaxZ;
            foundBounds = true;
            continue;
        }

        bounds.minX = std::min(bounds.minX, chunkMinX);
        bounds.maxX = std::max(bounds.maxX, chunkMaxX);
        bounds.minY = std::min(bounds.minY, chunkMinY);
        bounds.maxY = std::max(bounds.maxY, chunkMaxY);
        bounds.minZ = std::min(bounds.minZ, chunkMinZ);
        bounds.maxZ = std::max(bounds.maxZ, chunkMaxZ);
    }

    if (!foundBounds)
    {
        return std::nullopt;
    }

    return bounds;
}

void DemoWorld::SimulateDrySandPass(const LooseSimulationBounds& bounds)
{
    const ScanOrder scan = BuildScanOrder(++simulationStep_, width_, depth_);
    std::vector<ChunkCoord> changedChunks;
    const int xStart = scan.xStep > 0 ? bounds.minX : bounds.maxX;
    const int xEnd = scan.xStep > 0 ? bounds.maxX + 1 : bounds.minX - 1;
    const int zStart = scan.zStep > 0 ? bounds.minZ : bounds.maxZ;
    const int zEnd = scan.zStep > 0 ? bounds.maxZ + 1 : bounds.minZ - 1;

    for (int y = std::max(1, bounds.minY); y <= bounds.maxY; ++y)
    {
        for (int z = zStart; z != zEnd; z += scan.zStep)
        {
            for (int x = xStart; x != xEnd; x += scan.xStep)
            {
                if (GetCell(x, y, z) != MaterialId::DrySand)
                {
                    continue;
                }

                if (TryMoveCell(x, y, z, x, y - 1, z, true, 0, &changedChunks))
                {
                    continue;
                }

                const auto directions = WaterFallDirections(x, z, generationSettings_.seed ^ 0x55aa12efu, simulationStep_);
                for (const Int3& direction : directions)
                {
                    if (TryMoveCell(x, y, z, x + direction.x, y - 1, z + direction.z, true, 0, &changedChunks))
                    {
                        break;
                    }
                }
            }
        }
    }

    CommitChunkEdits(changedChunks);
}

void DemoWorld::SimulateWetMudPass(const LooseSimulationBounds& bounds)
{
    const ScanOrder scan = BuildScanOrder(++simulationStep_, width_, depth_);
    std::vector<ChunkCoord> changedChunks;
    const int xStart = scan.xStep > 0 ? bounds.minX : bounds.maxX;
    const int xEnd = scan.xStep > 0 ? bounds.maxX + 1 : bounds.minX - 1;
    const int zStart = scan.zStep > 0 ? bounds.minZ : bounds.maxZ;
    const int zEnd = scan.zStep > 0 ? bounds.maxZ + 1 : bounds.minZ - 1;

    for (int y = std::max(1, bounds.minY); y <= bounds.maxY; ++y)
    {
        for (int z = zStart; z != zEnd; z += scan.zStep)
        {
            for (int x = xStart; x != xEnd; x += scan.xStep)
            {
                if (GetCell(x, y, z) != MaterialId::WetMud)
                {
                    continue;
                }

                if (TryMoveCell(x, y, z, x, y - 1, z, false, 0, &changedChunks))
                {
                    continue;
                }

                const auto directions = WaterFallDirections(x, z, generationSettings_.seed ^ 0x2e4416f3u, simulationStep_);
                for (const Int3& direction : directions)
                {
                    if (TryMoveCell(x, y, z, x + direction.x, y - 1, z + direction.z, false, 0, &changedChunks))
                    {
                        break;
                    }
                }
            }
        }
    }

    CommitChunkEdits(changedChunks);
}

void DemoWorld::SimulateWaterFallPass(const LooseSimulationBounds& bounds)
{
    const ScanOrder scan = BuildScanOrder(++simulationStep_, width_, depth_);
    std::vector<ChunkCoord> changedChunks;
    const int xStart = scan.xStep > 0 ? bounds.minX : bounds.maxX;
    const int xEnd = scan.xStep > 0 ? bounds.maxX + 1 : bounds.minX - 1;
    const int zStart = scan.zStep > 0 ? bounds.minZ : bounds.maxZ;
    const int zEnd = scan.zStep > 0 ? bounds.maxZ + 1 : bounds.minZ - 1;

    for (int y = std::max(1, bounds.minY); y <= bounds.maxY; ++y)
    {
        for (int z = zStart; z != zEnd; z += scan.zStep)
        {
            for (int x = xStart; x != xEnd; x += scan.xStep)
            {
                if (GetCell(x, y, z) != MaterialId::ShallowWater)
                {
                    continue;
                }

                if (TryMoveCell(x, y, z, x, y - 1, z, false, 0, &changedChunks))
                {
                    continue;
                }

                const auto directions = WaterFallDirections(x, z, generationSettings_.seed ^ 0x9a11c34du, simulationStep_);
                for (const Int3& direction : directions)
                {
                    if (TryMoveCell(x, y, z, x + direction.x, y - 1, z + direction.z, false, 0, &changedChunks))
                    {
                        break;
                    }
                }
            }
        }
    }

    CommitChunkEdits(changedChunks);
}

void DemoWorld::SimulateWaterSpreadPass(const LooseSimulationBounds& bounds)
{
    const ScanOrder scan = BuildScanOrder(++simulationStep_, width_, depth_);
    std::vector<ChunkCoord> changedChunks;
    const int xStart = scan.xStep > 0 ? bounds.minX : bounds.maxX;
    const int xEnd = scan.xStep > 0 ? bounds.maxX + 1 : bounds.minX - 1;
    const int zStart = scan.zStep > 0 ? bounds.minZ : bounds.maxZ;
    const int zEnd = scan.zStep > 0 ? bounds.maxZ + 1 : bounds.minZ - 1;

    for (int y = bounds.minY; y <= bounds.maxY; ++y)
    {
        for (int z = zStart; z != zEnd; z += scan.zStep)
        {
            for (int x = xStart; x != xEnd; x += scan.xStep)
            {
                if (GetCell(x, y, z) != MaterialId::ShallowWater)
                {
                    continue;
                }
                if (y > 0 && GetCell(x, y - 1, z) == MaterialId::Air)
                {
                    continue;
                }
                if (!CanWaterSpreadLaterally(x, y, z))
                {
                    continue;
                }

                const std::size_t sourceIndex = CellIndex(x, y, z);
                const auto directions = WaterSpreadDirections(x, z, generationSettings_.seed, waterSpreadPass_);
                for (const Int3& direction : directions)
                {
                    const int targetX = x + direction.x;
                    const int targetZ = z + direction.z;
                    if (!InBounds(targetX, y, targetZ))
                    {
                        continue;
                    }
                    if (GetCell(targetX, y, targetZ) != MaterialId::Air)
                    {
                        continue;
                    }
                    if (y > 0 && GetCell(targetX, y - 1, targetZ) == MaterialId::Air)
                    {
                        continue;
                    }

                    const std::int8_t directionCode = CardinalDirectionCode(direction);
                    if (directionCode != 0 &&
                        waterLastLateralDirection_[sourceIndex] == -directionCode &&
                        waterSpreadPass_ - waterLastLateralPass_[sourceIndex] <= kWaterBacktrackCooldownPasses)
                    {
                        continue;
                    }

                    if (TryMoveCell(x, y, z, targetX, y, targetZ, false, directionCode, &changedChunks))
                    {
                        break;
                    }
                }
            }
        }
    }

    CommitChunkEdits(changedChunks);
}

void DemoWorld::EnsureMpmBackend()
{
    if (mpmWorkerCount_ == 0)
    {
        mpmWorkerCount_ = JobSystem::RecommendWorkerCount(1);
    }

    if (!xySliceMpm_)
    {
        xySliceMpm_ = std::make_unique<sim::VulkanMpm2D>(width_, height_, cellSize_, mpmDt_);
        xySliceMpm_->SetCpuFallbackWorkerCount(mpmWorkerCount_);
    }
    if (!zySliceMpm_)
    {
        zySliceMpm_ = std::make_unique<sim::VulkanMpm2D>(depth_, height_, cellSize_, mpmDt_);
        zySliceMpm_->SetCpuFallbackWorkerCount(mpmWorkerCount_);
    }
}

auto DemoWorld::LooseMaterialParticleMass(const MaterialId material) const -> float
{
    switch (material)
    {
    case MaterialId::DrySand:
        return 1.0f;
    case MaterialId::WetMud:
        return 1.18f;
    case MaterialId::ShallowWater:
        return 0.72f;
    default:
        return 1.0f;
    }
}

void DemoWorld::SimulateLooseMaterialMpm(const float dt)
{
    bool hasActiveLooseChunk = false;
    {
        const ScopedProfileSection scope(profiler_, "World MPM Scan");
        for (const auto& [chunk, state] : chunkRuntimeStates_)
        {
            static_cast<void>(chunk);
            if (state.activityLifetime > 0.0f && state.looseCellCount > 0u)
            {
                hasActiveLooseChunk = true;
                break;
            }
        }
    }

    if (!hasActiveLooseChunk)
    {
        mpmTimeAccumulator_ = 0.0f;
        mpmNextXySlice_ = 0;
        mpmNextZySlice_ = 0;
        return;
    }

    mpmTimeAccumulator_ += dt;
    if (mpmTimeAccumulator_ < kMpmSimulationIntervalSeconds)
    {
        return;
    }
    mpmTimeAccumulator_ = std::max(0.0f, mpmTimeAccumulator_ - kMpmSimulationIntervalSeconds);

    {
        const ScopedProfileSection scope(profiler_, "World MPM Backend");
        EnsureMpmBackend();
    }

    {
        const ScopedProfileSection scope(profiler_, "World MPM Slice XY");
        SimulateLooseMaterialSlicePass(true, kMpmSliceBudgetPerAxisPass);
    }
    {
        const ScopedProfileSection scope(profiler_, "World MPM Slice ZY");
        SimulateLooseMaterialSlicePass(false, kMpmSliceBudgetPerAxisPass);
    }
}

void DemoWorld::SimulateLooseMaterialSlicePass(const bool slicesAlongZ, const int sliceBudget)
{
    const ScopedProfileSection sliceScope(profiler_, "World MPM Slice Internal");
    if (sliceBudget <= 0)
    {
        return;
    }

    sim::VulkanMpm2D* backend = slicesAlongZ ? xySliceMpm_.get() : zySliceMpm_.get();
    if (backend == nullptr)
    {
        return;
    }

    int& nextSliceCursor = slicesAlongZ ? mpmNextXySlice_ : mpmNextZySlice_;
    const int chunkSpan = std::max(activeChunkSize_, 1);
    const int sliceCount = slicesAlongZ ? depth_ : width_;
    const int lateralCount = slicesAlongZ ? width_ : depth_;
    struct SliceBounds
    {
        int minU = std::numeric_limits<int>::max();
        int maxU = std::numeric_limits<int>::min();
        int minY = std::numeric_limits<int>::max();
        int maxY = std::numeric_limits<int>::min();
    };

    {
        const ScopedProfileSection boundsScope(profiler_, "World MPM Slice Bounds");
        std::vector<SliceBounds> activeSliceBounds(static_cast<std::size_t>(sliceCount));
        for (const auto& [chunk, state] : chunkRuntimeStates_)
        {
            if (state.activityLifetime <= 0.0f || state.looseCellCount == 0u)
            {
                continue;
            }

            const int sliceStart = slicesAlongZ ? chunk.z * chunkSpan : chunk.x * chunkSpan;
            const int sliceEnd = std::min(sliceCount - 1, sliceStart + chunkSpan - 1);
            const int lateralStart = std::max(0, (slicesAlongZ ? chunk.x : chunk.z) * chunkSpan - 1);
            const int lateralEnd = std::min(lateralCount - 1, (slicesAlongZ ? chunk.x : chunk.z) * chunkSpan + chunkSpan);
            const int verticalStart = std::max(0, chunk.y * chunkSpan - 1);
            const int verticalEnd = std::min(height_ - 1, chunk.y * chunkSpan + chunkSpan);
            for (int slice = std::max(0, sliceStart); slice <= sliceEnd; ++slice)
            {
                SliceBounds& bounds = activeSliceBounds[static_cast<std::size_t>(slice)];
                bounds.minU = std::min(bounds.minU, lateralStart);
                bounds.maxU = std::max(bounds.maxU, lateralEnd);
                bounds.minY = std::min(bounds.minY, verticalStart);
                bounds.maxY = std::max(bounds.maxY, verticalEnd);
            }
        }

        const auto worldCoords = [&](const int u, const int y, const int slice) -> Int3
        {
            return slicesAlongZ ? Int3{u, y, slice} : Int3{slice, y, u};
        };
        const auto sliceLinearIndex = [lateralCount](const int u, const int y) -> std::size_t
        {
            return static_cast<std::size_t>(y * lateralCount + u);
        };

        struct SettlementCandidate
        {
            std::size_t particleIndex = 0;
            int targetU = 0;
            int targetY = 0;
        };

        std::vector<MaterialId> particleMaterials;
        std::vector<MaterialId> rebuilt;
        std::vector<std::uint8_t> occupied;
        std::vector<SettlementCandidate> settlementOrder;

        bool anyChange = false;
        int processedSlices = 0;
        int visitedSlices = 0;
        int sliceCursor = sliceCount > 0 ? std::clamp(nextSliceCursor, 0, sliceCount - 1) : 0;
        while (visitedSlices < sliceCount && processedSlices < sliceBudget)
        {
            const int slice = sliceCursor;
            sliceCursor = (sliceCursor + 1) % sliceCount;
            ++visitedSlices;

            const SliceBounds& bounds = activeSliceBounds[static_cast<std::size_t>(slice)];
            if (bounds.maxU < bounds.minU || bounds.maxY < bounds.minY)
            {
                continue;
            }
            ++processedSlices;

            const int patchUBegin = bounds.minU;
            const int patchUEnd = std::min(lateralCount, bounds.maxU + 1);
            const int patchYBegin = bounds.minY;
            const int patchYEnd = std::min(height_, bounds.maxY + 1);
            const int patchWidth = std::max(0, patchUEnd - patchUBegin);
            const int patchHeight = std::max(0, patchYEnd - patchYBegin);
            if (patchWidth <= 0 || patchHeight <= 0)
            {
                continue;
            }

            const auto patchLinearIndex = [patchUBegin, patchYBegin, patchWidth](const int u, const int y) -> std::size_t
            {
                return static_cast<std::size_t>((y - patchYBegin) * patchWidth + (u - patchUBegin));
            };

            const std::size_t patchCellCount = static_cast<std::size_t>(patchWidth * patchHeight);
            {
                const ScopedProfileSection setupScope(profiler_, "World MPM Slice Setup");
                backend->ClearParticles();
                particleMaterials.clear();
                settlementOrder.clear();
                rebuilt.assign(patchCellCount, MaterialId::Air);
                occupied.assign(patchCellCount, static_cast<std::uint8_t>(0));
                particleMaterials.reserve(patchCellCount);
                settlementOrder.reserve(patchCellCount);

                for (int y = patchYBegin; y < patchYEnd; ++y)
                {
                    for (int u = patchUBegin; u < patchUEnd; ++u)
                    {
                        const Int3 cell = worldCoords(u, y, slice);
                        const MaterialId material = GetCell(cell.x, cell.y, cell.z);
                        const std::size_t idx = patchLinearIndex(u, y);
                        if (IsLooseMaterial(material))
                        {
                            backend->AddParticle({
                                Vec2{(static_cast<float>(u) + 0.5f) * cellSize_, (static_cast<float>(y) + 0.5f) * cellSize_},
                                Vec2{},
                                LooseMaterialParticleMass(material),
                            });
                            particleMaterials.push_back(material);
                        }
                        else
                        {
                            rebuilt[idx] = material;
                            occupied[idx] = material != MaterialId::Air ? 1u : 0u;
                        }
                    }
                }
            }

            if (particleMaterials.empty())
            {
                continue;
            }

            {
                const ScopedProfileSection backendScope(profiler_, "World MPM Backend Step");
                backend->Step(-9.81f);
                activeMpmBackendName_ = backend->ActiveBackendName();
                mpmWorkerCount_ = backend->CpuFallbackWorkerCount();
            }

            {
                const ScopedProfileSection rebuildScope(profiler_, "World MPM Slice Rebuild");
                const auto& particles = backend->Particles();
                {
                    const ScopedProfileSection targetsScope(profiler_, "World MPM Rebuild Targets");
                    for (std::size_t particleIndex = 0; particleIndex < particles.size() && particleIndex < particleMaterials.size(); ++particleIndex)
                    {
                        settlementOrder.push_back({
                            particleIndex,
                            std::clamp(static_cast<int>(std::floor(particles[particleIndex].position.x / cellSize_)), patchUBegin, patchUEnd - 1),
                            std::clamp(static_cast<int>(std::floor(particles[particleIndex].position.y / cellSize_)), patchYBegin, patchYEnd - 1),
                        });
                    }
                }

                {
                    const ScopedProfileSection sortScope(profiler_, "World MPM Rebuild Sort");
                    std::stable_sort(settlementOrder.begin(), settlementOrder.end(), [](const SettlementCandidate& lhs, const SettlementCandidate& rhs)
                    {
                        if (lhs.targetY != rhs.targetY)
                        {
                            return lhs.targetY < rhs.targetY;
                        }
                        return lhs.targetU < rhs.targetU;
                    });
                }

                const auto tryPlaceInColumn = [&](const MaterialId material, const int candidateU, const int targetY) -> bool
                {
                    if (candidateU < patchUBegin || candidateU >= patchUEnd)
                    {
                        return false;
                    }

                    for (int deltaY = 0; deltaY < patchHeight; ++deltaY)
                    {
                        const int belowY = targetY - deltaY;
                        if (belowY >= patchYBegin)
                        {
                            const std::size_t idx = patchLinearIndex(candidateU, belowY);
                            if (!occupied[idx])
                            {
                                rebuilt[idx] = material;
                                occupied[idx] = true;
                                return true;
                            }
                        }

                        if (deltaY == 0)
                        {
                            continue;
                        }

                        const int aboveY = targetY + deltaY;
                        if (aboveY < patchYEnd)
                        {
                            const std::size_t idx = patchLinearIndex(candidateU, aboveY);
                            if (!occupied[idx])
                            {
                                rebuilt[idx] = material;
                                occupied[idx] = true;
                                return true;
                            }
                        }
                    }

                    return false;
                };

                {
                    const ScopedProfileSection placeScope(profiler_, "World MPM Rebuild Place");
                    for (const SettlementCandidate& candidate : settlementOrder)
                    {
                        const MaterialId material = particleMaterials[candidate.particleIndex];
                        bool placed = tryPlaceInColumn(material, candidate.targetU, candidate.targetY);
                        for (int lateralOffset = 1; !placed && lateralOffset < patchWidth; ++lateralOffset)
                        {
                            placed = tryPlaceInColumn(material, candidate.targetU - lateralOffset, candidate.targetY);
                            if (!placed)
                            {
                                placed = tryPlaceInColumn(material, candidate.targetU + lateralOffset, candidate.targetY);
                            }
                        }
                    }
                }
            }

            {
                const ScopedProfileSection commitScope(profiler_, "World MPM Slice Commit");
                bool sliceChanged = false;
                std::vector<ChunkCoord> changedChunks;
                changedChunks.reserve(static_cast<std::size_t>(std::max(1, patchWidth * patchHeight / std::max(chunkSpan * chunkSpan, 1))));
                for (int y = patchYBegin; y < patchYEnd; ++y)
                {
                    for (int u = patchUBegin; u < patchUEnd; ++u)
                    {
                        const Int3 cell = worldCoords(u, y, slice);
                        const std::size_t worldIdx = CellIndex(cell.x, cell.y, cell.z);
                        const MaterialId material = rebuilt[patchLinearIndex(u, y)];
                        const MaterialId previousMaterial = GetCell(cell.x, cell.y, cell.z);
                        if (previousMaterial != material)
                        {
                            NoteCellMaterialChange(cell.x, cell.y, cell.z, previousMaterial, material);
                            SetCellUnchecked(cell.x, cell.y, cell.z, material);
                            waterLastLateralDirection_[worldIdx] = 0;
                            waterLastLateralPass_[worldIdx] = 0u;
                            sliceChanged = true;
                            changedChunks.push_back(CellToChunkCoord(cell.x, cell.y, cell.z));
                        }
                        else if (IsLooseMaterial(material))
                        {
                            waterLastLateralDirection_[worldIdx] = 0;
                            waterLastLateralPass_[worldIdx] = 0u;
                        }
                    }
                }

                if (sliceChanged)
                {
                    anyChange = true;
                    CommitChunkEdits(changedChunks);
                }
            }
        }
        nextSliceCursor = processedSlices > 0 ? sliceCursor : 0;

        if (anyChange)
        {
            terrainDirty_ = true;
        }
    }
}

auto DemoWorld::LoadLegacyMaterialField(const std::filesystem::path& path) -> bool
{
    if (!field_.Load(path))
    {
        return false;
    }

    if (cells_.size() != static_cast<std::size_t>(width_ * height_ * depth_))
    {
        ResizeStorage();
    }

    std::fill(cells_.begin(), cells_.end(), static_cast<std::uint8_t>(MaterialId::Air));
    for (int z = 0; z < depth_; ++z)
    {
        for (int y = 0; y < height_; ++y)
        {
            for (int x = 0; x < width_; ++x)
            {
                SetCellUnchecked(x, y, z, field_.GetCell(x, y, z));
            }
        }
    }

    ResetTransientState();
    RebuildChunkRuntimeState();
    terrainDirty_ = true;
    EnsureMpmBackend();
    return true;
}

void DemoWorld::RebuildMeshCache()
{
    if (meshWorkerCount_ == 0)
    {
        meshWorkerCount_ = JobSystem::RecommendWorkerCount(1);
    }
    if (meshJobs_ == nullptr)
    {
        meshJobs_ = std::make_unique<JobSystem>(meshWorkerCount_);
    }

    Stopwatch stopwatch;

    terrainTriangleCache_.clear();
    translucentTerrainTriangleCache_.clear();
    terrainWireCache_.clear();

    const std::size_t columnCount = static_cast<std::size_t>(width_ * depth_);
    if (solidSurfaceHeightMap_.size() != columnCount)
    {
        solidSurfaceHeightMap_.assign(columnCount, -std::numeric_limits<float>::infinity());
    }
    if (waterSurfaceHeightMap_.size() != columnCount)
    {
        waterSurfaceHeightMap_.assign(columnCount, -std::numeric_limits<float>::infinity());
    }

    std::vector<ChunkCoord> dirtyChunks;
    dirtyChunks.reserve(chunkRuntimeStates_.size());
    for (const auto& [chunk, state] : chunkRuntimeStates_)
    {
        if (state.meshDirty || !state.meshInitialized)
        {
            dirtyChunks.push_back(chunk);
        }
    }

    if (dirtyChunks.empty())
    {
        PruneRetiredChunkStates();
        terrainDirty_ = false;
        terrainRebuildCooldown_ = terrainRebuildInterval_;
        ++terrainMeshVersion_;
        lastTerrainRebuildMilliseconds_ = stopwatch.ElapsedMilliseconds();
        return;
    }

    std::sort(dirtyChunks.begin(), dirtyChunks.end(), [](const ChunkCoord& lhs, const ChunkCoord& rhs)
    {
        if (lhs.z != rhs.z)
        {
            return lhs.z < rhs.z;
        }
        if (lhs.y != rhs.y)
        {
            return lhs.y < rhs.y;
        }
        return lhs.x < rhs.x;
    });
    std::vector<std::uint32_t> dirtyChunkNonAirCounts(dirtyChunks.size(), 0u);
    for (std::size_t index = 0; index < dirtyChunks.size(); ++index)
    {
        if (const auto stateIter = chunkRuntimeStates_.find(dirtyChunks[index]); stateIter != chunkRuntimeStates_.end())
        {
            dirtyChunkNonAirCounts[index] = stateIter->second.nonAirCellCount;
        }
    }

    const std::size_t dirtyChunkTotal = dirtyChunks.size();
    const std::size_t dirtyChunkBudget = std::max<std::size_t>(kMinMeshChunkBudgetPerPass, std::max<std::size_t>(meshWorkerCount_, 1u) * kMeshChunkBudgetPerWorker);
    if (dirtyChunks.size() > dirtyChunkBudget)
    {
        dirtyChunks.resize(dirtyChunkBudget);
        dirtyChunkNonAirCounts.resize(dirtyChunkBudget);
    }

    const int chunkSpan = std::max(activeChunkSize_, 1);
    const auto columnIndex = [this](const int x, const int z) -> std::size_t
    {
        return static_cast<std::size_t>(z * width_ + x);
    };

    std::vector<ChunkCoord> dirtyColumnChunks = dirtyChunks;
    for (ChunkCoord& chunk : dirtyColumnChunks)
    {
        chunk.y = 0;
    }
    std::sort(dirtyColumnChunks.begin(), dirtyColumnChunks.end(), [](const ChunkCoord& lhs, const ChunkCoord& rhs)
    {
        if (lhs.z != rhs.z)
        {
            return lhs.z < rhs.z;
        }
        return lhs.x < rhs.x;
    });
    dirtyColumnChunks.erase(std::unique(dirtyColumnChunks.begin(), dirtyColumnChunks.end(), [](const ChunkCoord& lhs, const ChunkCoord& rhs)
    {
        return lhs.x == rhs.x && lhs.z == rhs.z;
    }), dirtyColumnChunks.end());

    meshJobs_->ParallelFor(dirtyColumnChunks.size(), 1, [&](const std::size_t begin, const std::size_t end)
    {
        for (std::size_t index = begin; index < end; ++index)
        {
            const ChunkCoord& chunk = dirtyColumnChunks[index];
            const int xBegin = chunk.x * chunkSpan;
            const int xEnd = std::min(width_, xBegin + chunkSpan);
            const int zBegin = chunk.z * chunkSpan;
            const int zEnd = std::min(depth_, zBegin + chunkSpan);
            if (xBegin >= xEnd || zBegin >= zEnd)
            {
                continue;
            }

            for (int z = zBegin; z < zEnd; ++z)
            {
                for (int x = xBegin; x < xEnd; ++x)
                {
                    float solidSurfaceHeight = -std::numeric_limits<float>::infinity();
                    float waterSurfaceHeight = -std::numeric_limits<float>::infinity();
                    for (int y = 0; y < height_; ++y)
                    {
                        const MaterialId material = GetCell(x, y, z);
                        const float worldTop = (static_cast<float>(y) + 1.0f) * cellSize_;
                        if (material == MaterialId::ShallowWater)
                        {
                            waterSurfaceHeight = std::max(waterSurfaceHeight, worldTop);
                        }
                        else if (material != MaterialId::Air)
                        {
                            solidSurfaceHeight = std::max(solidSurfaceHeight, worldTop);
                        }
                    }

                    solidSurfaceHeightMap_[columnIndex(x, z)] = solidSurfaceHeight;
                    waterSurfaceHeightMap_[columnIndex(x, z)] = waterSurfaceHeight;
                }
            }
        }
    });

    const Vec3 worldMinimum = WorldMin();
    const Vec3 lightDirection = Normalize(Vec3{0.28f, 0.88f, 0.36f});
    const Vec3 waterHighlightDirection = Normalize(lightDirection + Vec3{0.0f, 1.0f, 0.0f});
    constexpr float kVertexQuantizeScale = 4096.0f;

    const auto sampleCornerDensity = [this](const int gx, const int gy, const int gz, const SurfaceField field) -> float
    {
        float filledWeight = 0.0f;
        for (int sampleZ = gz - 1; sampleZ <= gz; ++sampleZ)
        {
            for (int sampleY = gy - 1; sampleY <= gy; ++sampleY)
            {
                for (int sampleX = gx - 1; sampleX <= gx; ++sampleX)
                {
                    if (!InBounds(sampleX, sampleY, sampleZ))
                    {
                        continue;
                    }

                    if (SurfaceFieldContainsMaterial(GetCell(sampleX, sampleY, sampleZ), field))
                    {
                        filledWeight += 1.0f;
                    }
                }
            }
        }

        return filledWeight / 8.0f;
    };

    const auto dominantSurfaceMaterial = [this](const int x, const int y, const int z, const SurfaceField field) -> MaterialId
    {
        if (field == SurfaceField::Water)
        {
            return MaterialId::ShallowWater;
        }

        std::array<int, kMaterialCount> materialCounts{};
        for (int sampleZ = z; sampleZ <= z + 1; ++sampleZ)
        {
            for (int sampleY = y; sampleY <= y + 1; ++sampleY)
            {
                for (int sampleX = x; sampleX <= x + 1; ++sampleX)
                {
                    if (!InBounds(sampleX, sampleY, sampleZ))
                    {
                        continue;
                    }

                    const MaterialId material = GetCell(sampleX, sampleY, sampleZ);
                    if (!SurfaceFieldContainsMaterial(material, field))
                    {
                        continue;
                    }

                    ++materialCounts[static_cast<std::size_t>(material)];
                }
            }
        }

        MaterialId bestMaterial = MaterialId::CompactedSoil;
        int bestCount = 0;
        for (int materialIndex = 0; materialIndex < static_cast<int>(materialCounts.size()); ++materialIndex)
        {
            const MaterialId candidate = static_cast<MaterialId>(materialIndex);
            if (!SurfaceFieldContainsMaterial(candidate, field))
            {
                continue;
            }
            if (materialCounts[static_cast<std::size_t>(materialIndex)] > bestCount)
            {
                bestCount = materialCounts[static_cast<std::size_t>(materialIndex)];
                bestMaterial = candidate;
            }
        }

        return bestMaterial;
    };

    const auto interpolateSurfacePoint = [](const Vec3& a, const Vec3& b, const float densityA, const float densityB) -> Vec3
    {
        const float denominator = densityB - densityA;
        if (std::abs(denominator) <= 0.0001f)
        {
            return (a + b) * 0.5f;
        }

        const float t = Clamp((kSurfaceIsoLevel - densityA) / denominator, 0.0f, 1.0f);
        return Lerp(a, b, t);
    };

    const auto sampleHeightMap = [this, &columnIndex](const std::vector<float>& heights, const Vec3& worldPosition) -> float
    {
        const Vec3 minimum = WorldMin();
        int x = static_cast<int>(std::floor((worldPosition.x - minimum.x) / cellSize_));
        int z = static_cast<int>(std::floor((worldPosition.z - minimum.z) / cellSize_));
        x = std::clamp(x, 0, width_ - 1);
        z = std::clamp(z, 0, depth_ - 1);

        const float height = heights[columnIndex(x, z)];
        return std::isfinite(height) ? height : 0.0f;
    };

    struct QuantizedVertexKey
    {
        int x = 0;
        int y = 0;
        int z = 0;
        MaterialId material = MaterialId::Air;

        auto operator==(const QuantizedVertexKey&) const -> bool = default;
    };

    struct QuantizedVertexKeyHash
    {
        [[nodiscard]] auto operator()(const QuantizedVertexKey& key) const noexcept -> std::size_t
        {
            std::size_t seed = 0xcbf29ce484222325ull;
            seed ^= static_cast<std::size_t>(key.x) + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
            seed ^= static_cast<std::size_t>(key.y) + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
            seed ^= static_cast<std::size_t>(key.z) + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
            seed ^= static_cast<std::size_t>(key.material) + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
            return seed;
        }
    };

    struct SurfaceMeshVertex
    {
        Vec3 position{};
        Vec3 normal{0.0f, 1.0f, 0.0f};
        MaterialId material = MaterialId::Air;
        float waterDepth = 0.0f;
    };

    struct SurfaceMeshBuilder
    {
        std::vector<SurfaceMeshVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::unordered_map<QuantizedVertexKey, std::uint32_t, QuantizedVertexKeyHash> vertexLookup;
    };

    struct BuiltChunkCache
    {
        ChunkCoord coord{};
        std::vector<render::ColorVertex3D> opaqueTriangles;
        std::vector<render::ColorVertex3D> translucentTriangles;
    };

    std::vector<BuiltChunkCache> rebuiltChunks(dirtyChunks.size());
    meshJobs_->ParallelFor(dirtyChunks.size(), 1, [&](const std::size_t begin, const std::size_t end)
    {
        for (std::size_t index = begin; index < end; ++index)
        {
            const ChunkCoord chunk = dirtyChunks[index];
            BuiltChunkCache& rebuiltChunk = rebuiltChunks[index];
            rebuiltChunk.coord = chunk;

            const int xBegin = chunk.x * chunkSpan;
            const int xEnd = std::min(width_, xBegin + chunkSpan);
            const int yBegin = chunk.y * chunkSpan;
            const int yEnd = std::min(height_, yBegin + chunkSpan);
            const int zBegin = chunk.z * chunkSpan;
            const int zEnd = std::min(depth_, zBegin + chunkSpan);
            if (xBegin >= xEnd || yBegin >= yEnd || zBegin >= zEnd)
            {
                continue;
            }

            if (dirtyChunkNonAirCounts[index] == 0u)
            {
                continue;
            }

            const int densityMinX = std::max(0, xBegin - 1);
            const int densityMinY = std::max(0, yBegin - 1);
            const int densityMinZ = std::max(0, zBegin - 1);
            const int densityMaxX = std::min(width_, xEnd + 1);
            const int densityMaxY = std::min(height_, yEnd + 1);
            const int densityMaxZ = std::min(depth_, zEnd + 1);
            const int densityWidth = densityMaxX - densityMinX + 1;
            const int densityHeight = densityMaxY - densityMinY + 1;
            const int densityDepth = densityMaxZ - densityMinZ + 1;

            const auto localCornerIndex = [densityMinX, densityMinY, densityMinZ, densityWidth, densityHeight](
                const int gx,
                const int gy,
                const int gz) -> std::size_t
            {
                return static_cast<std::size_t>(
                    (((gz - densityMinZ) * densityHeight) + (gy - densityMinY)) * densityWidth + (gx - densityMinX));
            };

            std::vector<float> solidCornerDensities(static_cast<std::size_t>(densityWidth * densityHeight * densityDepth), 0.0f);
            std::vector<float> waterCornerDensities(static_cast<std::size_t>(densityWidth * densityHeight * densityDepth), 0.0f);
            for (int gz = densityMinZ; gz <= densityMaxZ; ++gz)
            {
                for (int gy = densityMinY; gy <= densityMaxY; ++gy)
                {
                    for (int gx = densityMinX; gx <= densityMaxX; ++gx)
                    {
                        const std::size_t densityIndex = localCornerIndex(gx, gy, gz);
                        solidCornerDensities[densityIndex] = sampleCornerDensity(gx, gy, gz, SurfaceField::Solid);
                        waterCornerDensities[densityIndex] = sampleCornerDensity(gx, gy, gz, SurfaceField::Water);
                    }
                }
            }

            const auto densityGrid = [&](const SurfaceField field) -> const std::vector<float>&
            {
                return field == SurfaceField::Water ? waterCornerDensities : solidCornerDensities;
            };

            const auto sampleDensityWorld = [&](const Vec3& worldPosition, const SurfaceField field) -> float
            {
                const Vec3 localPosition = (worldPosition - worldMinimum) / cellSize_;

                int x0 = static_cast<int>(std::floor(localPosition.x));
                int y0 = static_cast<int>(std::floor(localPosition.y));
                int z0 = static_cast<int>(std::floor(localPosition.z));
                int x1 = x0 + 1;
                int y1 = y0 + 1;
                int z1 = z0 + 1;

                float tx = localPosition.x - static_cast<float>(x0);
                float ty = localPosition.y - static_cast<float>(y0);
                float tz = localPosition.z - static_cast<float>(z0);

                if (x0 < densityMinX)
                {
                    x0 = x1 = densityMinX;
                    tx = 0.0f;
                }
                else if (x1 > densityMaxX)
                {
                    x0 = x1 = densityMaxX;
                    tx = 0.0f;
                }

                if (y0 < densityMinY)
                {
                    y0 = y1 = densityMinY;
                    ty = 0.0f;
                }
                else if (y1 > densityMaxY)
                {
                    y0 = y1 = densityMaxY;
                    ty = 0.0f;
                }

                if (z0 < densityMinZ)
                {
                    z0 = z1 = densityMinZ;
                    tz = 0.0f;
                }
                else if (z1 > densityMaxZ)
                {
                    z0 = z1 = densityMaxZ;
                    tz = 0.0f;
                }

                const std::vector<float>& grid = densityGrid(field);
                const float d000 = grid[localCornerIndex(x0, y0, z0)];
                const float d100 = grid[localCornerIndex(x1, y0, z0)];
                const float d010 = grid[localCornerIndex(x0, y1, z0)];
                const float d110 = grid[localCornerIndex(x1, y1, z0)];
                const float d001 = grid[localCornerIndex(x0, y0, z1)];
                const float d101 = grid[localCornerIndex(x1, y0, z1)];
                const float d011 = grid[localCornerIndex(x0, y1, z1)];
                const float d111 = grid[localCornerIndex(x1, y1, z1)];

                const float d00 = Lerp(d000, d100, tx);
                const float d10 = Lerp(d010, d110, tx);
                const float d01 = Lerp(d001, d101, tx);
                const float d11 = Lerp(d011, d111, tx);
                const float d0 = Lerp(d00, d10, ty);
                const float d1 = Lerp(d01, d11, ty);
                return Lerp(d0, d1, tz);
            };

            const auto sampleSmoothNormal = [&](const Vec3& worldPosition, const SurfaceField field) -> Vec3
            {
                const float epsilon = std::max(cellSize_ * 0.30f, 0.01f);
                const Vec3 xOffset{epsilon, 0.0f, 0.0f};
                const Vec3 yOffset{0.0f, epsilon, 0.0f};
                const Vec3 zOffset{0.0f, 0.0f, epsilon};

                Vec3 gradient{
                    sampleDensityWorld(worldPosition + xOffset, field) - sampleDensityWorld(worldPosition - xOffset, field),
                    sampleDensityWorld(worldPosition + yOffset, field) - sampleDensityWorld(worldPosition - yOffset, field),
                    sampleDensityWorld(worldPosition + zOffset, field) - sampleDensityWorld(worldPosition - zOffset, field),
                };

                if (LengthSquared(gradient) <= 1.0e-6f)
                {
                    return Vec3{0.0f, 1.0f, 0.0f};
                }

                return Normalize(-gradient);
            };

            const auto buildSurfaceFieldChunk = [&](const SurfaceField field) -> std::vector<render::ColorVertex3D>
            {
                const std::vector<float>& fieldDensityGrid = densityGrid(field);
                SurfaceMeshBuilder builder;
                builder.vertices.reserve(static_cast<std::size_t>(std::max((xEnd - xBegin) * (yEnd - yBegin) * (zEnd - zBegin) / 3, 1)));
                builder.indices.reserve(static_cast<std::size_t>(std::max((xEnd - xBegin) * (yEnd - yBegin) * (zEnd - zBegin), 1) * 3));

                const auto addVertex = [&](const Vec3& position, const MaterialId material) -> std::uint32_t
                {
                    const QuantizedVertexKey key{
                        static_cast<int>(std::lround(position.x * kVertexQuantizeScale)),
                        static_cast<int>(std::lround(position.y * kVertexQuantizeScale)),
                        static_cast<int>(std::lround(position.z * kVertexQuantizeScale)),
                        material,
                    };

                    const auto existing = builder.vertexLookup.find(key);
                    if (existing != builder.vertexLookup.end())
                    {
                        if (field == SurfaceField::Water)
                        {
                            SurfaceMeshVertex& vertex = builder.vertices[existing->second];
                            const float surfaceHeight = sampleHeightMap(waterSurfaceHeightMap_, position);
                            const float floorHeight = sampleHeightMap(solidSurfaceHeightMap_, position);
                            vertex.waterDepth = std::max(vertex.waterDepth, std::max(surfaceHeight - floorHeight, cellSize_ * 0.15f));
                        }
                        return existing->second;
                    }

                    SurfaceMeshVertex vertex{};
                    vertex.position = position;
                    vertex.normal = sampleSmoothNormal(position, field);
                    vertex.material = material;
                    if (field == SurfaceField::Water)
                    {
                        const float surfaceHeight = sampleHeightMap(waterSurfaceHeightMap_, position);
                        const float floorHeight = sampleHeightMap(solidSurfaceHeightMap_, position);
                        vertex.waterDepth = std::max(surfaceHeight - floorHeight, cellSize_ * 0.15f);
                    }

                    const std::uint32_t vertexIndex = static_cast<std::uint32_t>(builder.vertices.size());
                    builder.vertices.push_back(vertex);
                    builder.vertexLookup.emplace(key, vertexIndex);
                    return vertexIndex;
                };

                const auto appendTriangle = [&](const Vec3& a, const Vec3& b, const Vec3& c, const MaterialId material)
                {
                    const Vec3 geometricNormal = Cross(b - a, c - a);
                    if (LengthSquared(geometricNormal) <= 1.0e-6f)
                    {
                        return;
                    }

                    const std::uint32_t ia = addVertex(a, material);
                    const std::uint32_t ib = addVertex(b, material);
                    const std::uint32_t ic = addVertex(c, material);
                    if (ia == ib || ib == ic || ia == ic)
                    {
                        return;
                    }

                    const Vec3 averagedNormal = Normalize(
                        builder.vertices[ia].normal +
                        builder.vertices[ib].normal +
                        builder.vertices[ic].normal);
                    std::uint32_t orderedB = ib;
                    std::uint32_t orderedC = ic;
                    if (LengthSquared(averagedNormal) > 1.0e-6f && Dot(Normalize(geometricNormal), averagedNormal) < 0.0f)
                    {
                        orderedB = ic;
                        orderedC = ib;
                    }

                    builder.indices.push_back(ia);
                    builder.indices.push_back(orderedB);
                    builder.indices.push_back(orderedC);
                };

                const auto appendTetrahedronSurface = [&](const std::array<Vec3, 4>& positions, const std::array<float, 4>& densities, const MaterialId material)
                {
                    std::array<Vec3, 6> intersections{};
                    int intersectionCount = 0;

                    for (const auto& edge : kTetrahedronEdges)
                    {
                        const float signedDistanceA = densities[edge[0]] - kSurfaceIsoLevel;
                        const float signedDistanceB = densities[edge[1]] - kSurfaceIsoLevel;
                        const bool onSurfaceA = std::abs(signedDistanceA) <= kSurfaceIntersectionEpsilon;
                        const bool onSurfaceB = std::abs(signedDistanceB) <= kSurfaceIntersectionEpsilon;
                        const bool strictlyInsideA = signedDistanceA > kSurfaceIntersectionEpsilon;
                        const bool strictlyInsideB = signedDistanceB > kSurfaceIntersectionEpsilon;
                        const bool strictlyOutsideA = signedDistanceA < -kSurfaceIntersectionEpsilon;
                        const bool strictlyOutsideB = signedDistanceB < -kSurfaceIntersectionEpsilon;

                        if ((strictlyInsideA && strictlyInsideB) || (strictlyOutsideA && strictlyOutsideB) || (onSurfaceA && onSurfaceB))
                        {
                            continue;
                        }

                        if (onSurfaceA)
                        {
                            AppendUniqueSurfacePoint(intersections, intersectionCount, positions[edge[0]]);
                            continue;
                        }
                        if (onSurfaceB)
                        {
                            AppendUniqueSurfacePoint(intersections, intersectionCount, positions[edge[1]]);
                            continue;
                        }

                        AppendUniqueSurfacePoint(
                            intersections,
                            intersectionCount,
                            interpolateSurfacePoint(
                                positions[edge[0]],
                                positions[edge[1]],
                                densities[edge[0]],
                                densities[edge[1]]));
                    }

                    if (intersectionCount < 3)
                    {
                        return;
                    }

                    Vec3 centroid{};
                    for (int pointIndex = 0; pointIndex < intersectionCount; ++pointIndex)
                    {
                        centroid += intersections[pointIndex];
                    }
                    centroid /= static_cast<float>(intersectionCount);

                    Vec3 initialAxis{};
                    bool foundAxis = false;
                    for (int pointIndex = 0; pointIndex < intersectionCount; ++pointIndex)
                    {
                        initialAxis = intersections[pointIndex] - centroid;
                        if (LengthSquared(initialAxis) > 1.0e-6f)
                        {
                            foundAxis = true;
                            break;
                        }
                    }
                    if (!foundAxis)
                    {
                        return;
                    }

                    const Vec3 referenceNormal = FindStableSurfaceNormal(intersections, intersectionCount);
                    if (LengthSquared(referenceNormal) <= 1.0e-6f)
                    {
                        return;
                    }

                    const Vec3 axisX = Normalize(initialAxis);
                    const Vec3 axisY = Normalize(Cross(referenceNormal, axisX));
                    if (LengthSquared(axisY) <= 1.0e-6f)
                    {
                        return;
                    }

                    std::array<int, 6> order{};
                    for (int pointIndex = 0; pointIndex < intersectionCount; ++pointIndex)
                    {
                        order[pointIndex] = pointIndex;
                    }
                    std::sort(order.begin(), order.begin() + intersectionCount, [&](const int leftIndex, const int rightIndex)
                    {
                        const Vec3 left = intersections[leftIndex] - centroid;
                        const Vec3 right = intersections[rightIndex] - centroid;
                        const float leftAngle = std::atan2(Dot(left, axisY), Dot(left, axisX));
                        const float rightAngle = std::atan2(Dot(right, axisY), Dot(right, axisX));
                        return leftAngle < rightAngle;
                    });

                    for (int pointIndex = 1; pointIndex + 1 < intersectionCount; ++pointIndex)
                    {
                        appendTriangle(
                            intersections[order[0]],
                            intersections[order[pointIndex]],
                            intersections[order[pointIndex + 1]],
                            material);
                    }
                };

                for (int z = zBegin; z < zEnd; ++z)
                {
                    for (int y = yBegin; y < yEnd; ++y)
                    {
                        for (int x = xBegin; x < xEnd; ++x)
                        {
                            const Vec3 base = CellMinToWorld(x, y, z);
                            std::array<Vec3, 8> cubePositions{};
                            std::array<float, 8> cubeDensities{};

                            float minDensity = 1.0f;
                            float maxDensity = 0.0f;
                            for (int cornerIndexValue = 0; cornerIndexValue < static_cast<int>(kCubeCorners.size()); ++cornerIndexValue)
                            {
                                const Vec3& cubeCorner = kCubeCorners[cornerIndexValue];
                                const int gx = x + static_cast<int>(cubeCorner.x);
                                const int gy = y + static_cast<int>(cubeCorner.y);
                                const int gz = z + static_cast<int>(cubeCorner.z);
                                cubePositions[cornerIndexValue] = base + cubeCorner * cellSize_;
                                cubeDensities[cornerIndexValue] = fieldDensityGrid[localCornerIndex(gx, gy, gz)];
                                minDensity = std::min(minDensity, cubeDensities[cornerIndexValue]);
                                maxDensity = std::max(maxDensity, cubeDensities[cornerIndexValue]);
                            }

                            if (minDensity >= kSurfaceIsoLevel || maxDensity < kSurfaceIsoLevel)
                            {
                                continue;
                            }

                            const MaterialId material = dominantSurfaceMaterial(x, y, z, field);
                            for (const auto& tetrahedron : kMarchingTetrahedra)
                            {
                                std::array<Vec3, 4> tetrahedronPositions{};
                                std::array<float, 4> tetrahedronDensities{};
                                for (int tetrahedronVertex = 0; tetrahedronVertex < 4; ++tetrahedronVertex)
                                {
                                    tetrahedronPositions[tetrahedronVertex] = cubePositions[tetrahedron[tetrahedronVertex]];
                                    tetrahedronDensities[tetrahedronVertex] = cubeDensities[tetrahedron[tetrahedronVertex]];
                                }

                                appendTetrahedronSurface(tetrahedronPositions, tetrahedronDensities, material);
                            }
                        }
                    }
                }

                const auto shadeVertex = [&](const SurfaceMeshVertex& vertex) -> Vec4
                {
                    Vec3 normal = vertex.normal;
                    if (LengthSquared(normal) <= 1.0e-6f)
                    {
                        normal = Vec3{0.0f, 1.0f, 0.0f};
                    }
                    else
                    {
                        normal = Normalize(normal);
                    }

                    if (field == SurfaceField::Water)
                    {
                        const float diffuse = std::max(0.0f, Dot(normal, lightDirection));
                        const float horizonFactor = Clamp(1.0f - normal.y, 0.0f, 1.0f);
                        const float depthFactor = Clamp(vertex.waterDepth / std::max(cellSize_ * 4.0f, 0.001f), 0.0f, 1.0f);
                        const float highlight = std::pow(std::max(0.0f, Dot(normal, waterHighlightDirection)), 24.0f);

                        const Vec3 shallowColor{0.18f, 0.56f, 0.74f};
                        const Vec3 deepColor{0.07f, 0.28f, 0.58f};
                        Vec3 waterColor = {
                            Lerp(shallowColor.x, deepColor.x, depthFactor),
                            Lerp(shallowColor.y, deepColor.y, depthFactor),
                            Lerp(shallowColor.z, deepColor.z, depthFactor),
                        };

                        const float light = 0.28f + 0.34f * diffuse + 0.18f * horizonFactor + 0.20f * Clamp(normal.y * 0.5f + 0.5f, 0.0f, 1.0f);
                        waterColor *= light;
                        waterColor += Vec3{1.0f, 1.0f, 1.0f} * (0.03f + 0.08f * horizonFactor + 0.10f * highlight);

                        return {
                            Clamp(waterColor.x, 0.0f, 1.0f),
                            Clamp(waterColor.y, 0.0f, 1.0f),
                            Clamp(waterColor.z, 0.0f, 1.0f),
                            Clamp(0.24f + 0.24f * depthFactor + 0.16f * horizonFactor, 0.22f, 0.72f),
                        };
                    }

                    const Vec4 baseColor = GetMaterialProperties(vertex.material).color;
                    const float diffuse = std::max(0.0f, Dot(normal, lightDirection));
                    const float skyFactor = Clamp(normal.y * 0.5f + 0.5f, 0.0f, 1.0f);
                    const float bounce = Clamp(0.35f + 0.65f * normal.y, 0.15f, 1.0f);
                    const float light = 0.18f + 0.47f * diffuse + 0.20f * skyFactor + 0.15f * bounce;
                    return {
                        Clamp(baseColor.x * light, 0.0f, 1.0f),
                        Clamp(baseColor.y * light, 0.0f, 1.0f),
                        Clamp(baseColor.z * light, 0.0f, 1.0f),
                        baseColor.w,
                    };
                };

                std::vector<render::ColorVertex3D> outputTriangles;
                outputTriangles.reserve(builder.indices.size());
                for (std::size_t triangleIndex = 0; triangleIndex + 2 < builder.indices.size(); triangleIndex += 3)
                {
                    const SurfaceMeshVertex& a = builder.vertices[builder.indices[triangleIndex + 0]];
                    const SurfaceMeshVertex& b = builder.vertices[builder.indices[triangleIndex + 1]];
                    const SurfaceMeshVertex& c = builder.vertices[builder.indices[triangleIndex + 2]];

                    outputTriangles.push_back({a.position, shadeVertex(a)});
                    outputTriangles.push_back({b.position, shadeVertex(b)});
                    outputTriangles.push_back({c.position, shadeVertex(c)});
                }

                return outputTriangles;
            };

            rebuiltChunk.opaqueTriangles = buildSurfaceFieldChunk(SurfaceField::Solid);
            rebuiltChunk.translucentTriangles = buildSurfaceFieldChunk(SurfaceField::Water);
        }
    });

    for (BuiltChunkCache& rebuiltChunk : rebuiltChunks)
    {
        ChunkRuntimeState& state = chunkRuntimeStates_[rebuiltChunk.coord];
        state.opaqueTriangles = std::move(rebuiltChunk.opaqueTriangles);
        state.translucentTriangles = std::move(rebuiltChunk.translucentTriangles);
        state.meshDirty = false;
        state.meshInitialized = true;
    }

    PruneRetiredChunkStates();

    std::vector<ChunkCoord> orderedChunkCoords;
    orderedChunkCoords.reserve(chunkRuntimeStates_.size());
    for (const auto& [chunk, state] : chunkRuntimeStates_)
    {
        static_cast<void>(state);
        orderedChunkCoords.push_back(chunk);
    }
    std::sort(orderedChunkCoords.begin(), orderedChunkCoords.end(), [](const ChunkCoord& lhs, const ChunkCoord& rhs)
    {
        if (lhs.z != rhs.z)
        {
            return lhs.z < rhs.z;
        }
        if (lhs.y != rhs.y)
        {
            return lhs.y < rhs.y;
        }
        return lhs.x < rhs.x;
    });

    std::size_t totalOpaqueVertices = 0;
    std::size_t totalTranslucentVertices = 0;
    for (const ChunkCoord& chunk : orderedChunkCoords)
    {
        const ChunkRuntimeState& state = chunkRuntimeStates_.at(chunk);
        totalOpaqueVertices += state.opaqueTriangles.size();
        totalTranslucentVertices += state.translucentTriangles.size();
    }

    terrainTriangleCache_.reserve(totalOpaqueVertices);
    translucentTerrainTriangleCache_.reserve(totalTranslucentVertices);
    for (const ChunkCoord& chunk : orderedChunkCoords)
    {
        ChunkRuntimeState& state = chunkRuntimeStates_.at(chunk);
        terrainTriangleCache_.insert(
            terrainTriangleCache_.end(),
            state.opaqueTriangles.begin(),
            state.opaqueTriangles.end());
        translucentTerrainTriangleCache_.insert(
            translucentTerrainTriangleCache_.end(),
            state.translucentTriangles.begin(),
            state.translucentTriangles.end());
    }

    bool hasDirtyBacklog = dirtyChunkTotal > dirtyChunks.size();
    if (!hasDirtyBacklog)
    {
        hasDirtyBacklog = std::any_of(chunkRuntimeStates_.begin(), chunkRuntimeStates_.end(), [](const auto& entry)
        {
            return entry.second.meshDirty || !entry.second.meshInitialized;
        });
    }

    terrainDirty_ = hasDirtyBacklog;
    terrainRebuildCooldown_ = hasDirtyBacklog ? 0.0f : terrainRebuildInterval_;
    ++terrainMeshVersion_;
    lastTerrainRebuildMilliseconds_ = stopwatch.ElapsedMilliseconds();
}

void DemoWorld::AppendFace(
    std::vector<render::ColorVertex3D>& triangles,
    std::vector<render::ColorVertex3D>& lines,
    const int x,
    const int y,
    const int z,
    const int faceIndex,
    const Vec4& color) const
{
    const FaceDefinition& face = kFaces[faceIndex];
    const Vec3 base = CellMinToWorld(x, y, z);
    const MaterialId material = GetCell(x, y, z);

    std::array<Vec3, 8> corners = kCubeCorners;
    const bool isWaterSurface = material == MaterialId::ShallowWater;
    const float defaultTopHeight = isWaterSurface ? 1.0f : 1.0f;
    corners[2].y = defaultTopHeight;
    corners[3].y = defaultTopHeight;
    corners[6].y = defaultTopHeight;
    corners[7].y = defaultTopHeight;

    const bool hasExposedTop =
        y + 1 >= height_ ||
        (isWaterSurface
            ? GetCell(x, y + 1, z) == MaterialId::Air
            : (GetCell(x, y + 1, z) == MaterialId::Air || GetCell(x, y + 1, z) == MaterialId::ShallowWater));
    if (hasExposedTop)
    {
        const auto& surfaceHeightMap = isWaterSurface ? waterSurfaceHeightMap_ : solidSurfaceHeightMap_;
        const auto columnIndex = [this](const int sampleX, const int sampleZ) -> std::size_t
        {
            return static_cast<std::size_t>(sampleZ * width_ + sampleX);
        };
        const float fallbackWorldTop = base.y + defaultTopHeight * cellSize_;
        const auto sampleCornerHeight = [&](const int cornerGridX, const int cornerGridZ) -> float
        {
            float accumulatedHeight = fallbackWorldTop;
            float sampleWeight = 1.0f;
            for (int sampleZ = cornerGridZ - 1; sampleZ <= cornerGridZ; ++sampleZ)
            {
                for (int sampleX = cornerGridX - 1; sampleX <= cornerGridX; ++sampleX)
                {
                    if (sampleX < 0 || sampleX >= width_ || sampleZ < 0 || sampleZ >= depth_)
                    {
                        continue;
                    }

                    const float candidateHeight = surfaceHeightMap[columnIndex(sampleX, sampleZ)];
                    if (!std::isfinite(candidateHeight))
                    {
                        continue;
                    }
                    if (candidateHeight > fallbackWorldTop + cellSize_ * 0.5f)
                    {
                        continue;
                    }
                    if (candidateHeight < fallbackWorldTop - cellSize_ * 1.50f)
                    {
                        continue;
                    }

                    accumulatedHeight += candidateHeight;
                    sampleWeight += 1.0f;
                }
            }

            return Clamp((accumulatedHeight / sampleWeight - base.y) / cellSize_, 0.1f, 1.0f);
        };

        corners[2].y = sampleCornerHeight(x + 1, z);
        corners[3].y = sampleCornerHeight(x, z);
        corners[6].y = sampleCornerHeight(x + 1, z + 1);
        corners[7].y = sampleCornerHeight(x, z + 1);
    }

    const Vec3 a = base + corners[face.corners[0]] * cellSize_;
    const Vec3 b = base + corners[face.corners[1]] * cellSize_;
    const Vec3 c = base + corners[face.corners[2]] * cellSize_;
    const Vec3 d = base + corners[face.corners[3]] * cellSize_;

    triangles.push_back({a, color});
    triangles.push_back({b, color});
    triangles.push_back({c, color});
    triangles.push_back({a, color});
    triangles.push_back({c, color});
    triangles.push_back({d, color});

    const Vec4 wireColor = MakeColor(0.08f, 0.08f, 0.08f, 0.90f);
    lines.push_back({a, wireColor});
    lines.push_back({b, wireColor});
    lines.push_back({b, wireColor});
    lines.push_back({c, wireColor});
    lines.push_back({c, wireColor});
    lines.push_back({d, wireColor});
    lines.push_back({d, wireColor});
    lines.push_back({a, wireColor});
}

void DemoWorld::AppendBoxLines(
    std::vector<render::ColorVertex3D>& lines,
    const Vec3& minCorner,
    const Vec3& maxCorner,
    const Vec4& color) const
{
    const std::array<Vec3, 8> corners = {
        minCorner,
        Vec3{maxCorner.x, minCorner.y, minCorner.z},
        Vec3{maxCorner.x, maxCorner.y, minCorner.z},
        Vec3{minCorner.x, maxCorner.y, minCorner.z},
        Vec3{minCorner.x, minCorner.y, maxCorner.z},
        Vec3{maxCorner.x, minCorner.y, maxCorner.z},
        maxCorner,
        Vec3{minCorner.x, maxCorner.y, maxCorner.z},
    };

    constexpr std::array<std::array<int, 2>, 12> edges = {
        std::array<int, 2>{0, 1},
        std::array<int, 2>{1, 2},
        std::array<int, 2>{2, 3},
        std::array<int, 2>{3, 0},
        std::array<int, 2>{4, 5},
        std::array<int, 2>{5, 6},
        std::array<int, 2>{6, 7},
        std::array<int, 2>{7, 4},
        std::array<int, 2>{0, 4},
        std::array<int, 2>{1, 5},
        std::array<int, 2>{2, 6},
        std::array<int, 2>{3, 7},
    };

    for (const auto& edge : edges)
    {
        lines.push_back({corners[edge[0]], color});
        lines.push_back({corners[edge[1]], color});
    }
}
}
