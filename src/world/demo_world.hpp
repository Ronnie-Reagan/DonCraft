#pragma once

#include "core/profiler.hpp"
#include "core/job_system.hpp"
#include "core/math.hpp"
#include "render/frame_data.hpp"
#include "sim/vulkan_mpm_2d.hpp"
#include "world/material_field.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace df::world
{
struct WorldGenerationSettings
{
    int worldWidth = 48;
    int worldHeight = 20;
    int worldDepth = 48;
    int activeChunkSize = 32;
    std::uint32_t seed = 1337u;
    float cellSize = 0.35f;
    float terrainRelief = 1.0f;
    float waterLevel = 0.36f;
};

struct DenseWorldSnapshot
{
    WorldGenerationSettings settings{};
    std::vector<std::uint8_t> cells;
};

struct RaycastHit
{
    bool hit = false;
    Vec3 position{};
    Vec3 normal{};
    Int3 cell{};
    MaterialId material = MaterialId::Air;
    float distance = 0.0f;
};

class DemoWorld
{
public:
    struct CellMaterialEdit
    {
        int x = 0;
        int y = 0;
        int z = 0;
        MaterialId material = MaterialId::Air;
    };

    DemoWorld();

    [[nodiscard]] static auto ClampGenerationSettings(WorldGenerationSettings settings) -> WorldGenerationSettings;

    void Reset();
    void Tick(float dt);
    void TickRenderState(float dt);
    void SetGenerationSettings(const WorldGenerationSettings& settings);
    void SetFrameProfiler(FrameProfiler* profiler)
    {
        profiler_ = profiler;
    }

    [[nodiscard]] bool Save(const std::filesystem::path& path) const;
    [[nodiscard]] bool Load(const std::filesystem::path& path);
    [[nodiscard]] auto CaptureSnapshot() const -> DenseWorldSnapshot;
    [[nodiscard]] bool ApplySnapshot(const DenseWorldSnapshot& snapshot);
    [[nodiscard]] bool ApplyCellEdits(std::span<const CellMaterialEdit> edits);
    void ActivateSimulationRegion(const Vec3& center, int radiusInChunks = 1);

    [[nodiscard]] auto GenerationSettings() const -> const WorldGenerationSettings&
    {
        return generationSettings_;
    }

    [[nodiscard]] auto CellAtWorldPosition(const Vec3& position) const -> Int3;
    [[nodiscard]] auto MaterialAtWorldPosition(const Vec3& position) const -> MaterialId;
    [[nodiscard]] auto MaterialAtCell(int x, int y, int z) const -> MaterialId;
    [[nodiscard]] auto OverlapsBlocking(const Vec3& center, const Vec3& halfExtents) const -> bool;
    [[nodiscard]] auto Raycast(const Ray& ray, float maxDistance) const -> RaycastHit;

    void ApplyDig(const Vec3& center, float radius, float power);
    void ApplyRifleImpact(const Vec3& center, const Vec3& direction, MaterialId impactMaterial);
    void ApplyExplosion(const Vec3& center, float radius, float power);
    void EditCell(int x, int y, int z, MaterialId material);

    void GatherRenderGeometry(
        std::span<const render::ColorVertex3D>& terrainTriangles,
        std::vector<render::ColorVertex3D>& debugLines,
        bool showWireframe,
        bool showActiveChunks);
    void GatherRenderGeometryExtended(
        std::span<const render::ColorVertex3D>& opaqueTerrainTriangles,
        std::span<const render::ColorVertex3D>& translucentTerrainTriangles,
        std::vector<render::ColorVertex3D>& debugLines,
        bool showWireframe,
        bool showActiveChunks);
    void GatherRenderGeometrySmoothed(
        std::span<const render::ColorVertex3D>& opaqueTerrainTriangles,
        std::span<const render::ColorVertex3D>& translucentTerrainTriangles,
        std::vector<render::ColorVertex3D>& debugLines,
        bool showWireframe,
        bool showActiveChunks);
    void GatherRenderGeometrySmoothedCulled(
        std::vector<render::ColorVertex3D>& opaqueTerrainTriangles,
        std::vector<render::ColorVertex3D>& translucentTerrainTriangles,
        std::vector<render::ColorVertex3D>& debugLines,
        const Mat4& worldToClip,
        const Vec3& cameraPosition,
        float maxDistanceMeters,
        bool showWireframe,
        bool showActiveChunks);

    [[nodiscard]] auto ActiveChunkCount() const -> std::size_t
    {
        return activeChunkCount_;
    }

    [[nodiscard]] auto CellSize() const -> float
    {
        return cellSize_;
    }
    [[nodiscard]] auto WorldWidthCells() const -> int
    {
        return width_;
    }
    [[nodiscard]] auto WorldHeightCells() const -> int
    {
        return height_;
    }
    [[nodiscard]] auto WorldDepthCells() const -> int
    {
        return depth_;
    }
    [[nodiscard]] auto ActiveChunkSizeCells() const -> int
    {
        return activeChunkSize_;
    }

    [[nodiscard]] auto WorldMin() const -> Vec3;
    [[nodiscard]] auto WorldMax() const -> Vec3;
    [[nodiscard]] auto ActiveMpmBackendName() const -> std::string_view
    {
        return activeMpmBackendName_;
    }
    [[nodiscard]] auto MpmWorkerCount() const -> std::size_t
    {
        return mpmWorkerCount_;
    }
    [[nodiscard]] auto LastTerrainRebuildMilliseconds() const -> double
    {
        return lastTerrainRebuildMilliseconds_;
    }
    [[nodiscard]] auto TerrainMeshVersion() const -> std::uint64_t
    {
        return terrainMeshVersion_;
    }
    [[nodiscard]] auto TerrainContentVersion() const -> std::uint64_t
    {
        return terrainContentVersion_;
    }
    [[nodiscard]] auto TrackedChunkStateCount() const -> std::size_t
    {
        return chunkRuntimeStates_.size();
    }
    [[nodiscard]] auto DirtyChunkStateCount() const -> std::size_t
    {
        return static_cast<std::size_t>(std::count_if(
            chunkRuntimeStates_.begin(),
            chunkRuntimeStates_.end(),
            [](const auto& entry)
            {
                return entry.second.meshDirty || !entry.second.meshInitialized;
            }));
    }
    [[nodiscard]] auto OpaqueTerrainVertexCount() const -> std::size_t
    {
        return terrainTriangleCache_.size();
    }
    [[nodiscard]] auto TranslucentTerrainVertexCount() const -> std::size_t
    {
        return translucentTerrainTriangleCache_.size();
    }

private:
    struct ChunkRuntimeState
    {
        float activityLifetime = 0.0f;
        std::uint32_t nonAirCellCount = 0u;
        std::uint32_t looseCellCount = 0u;
        std::uint32_t mpmCellCount = 0u;
        bool meshDirty = true;
        bool meshInitialized = false;
        std::vector<render::ColorVertex3D> opaqueTriangles;
        std::vector<render::ColorVertex3D> translucentTriangles;
    };

    struct LooseSimulationBounds
    {
        int minX = 0;
        int maxX = -1;
        int minY = 0;
        int maxY = -1;
        int minZ = 0;
        int maxZ = -1;
    };

    using ChunkRuntimeStateMap = std::unordered_map<ChunkCoord, ChunkRuntimeState, ChunkCoordHash>;

    [[nodiscard]] auto InBounds(int x, int y, int z) const -> bool;
    [[nodiscard]] auto WorldToCell(const Vec3& position) const -> Int3;
    [[nodiscard]] auto CellCenterToWorld(int x, int y, int z) const -> Vec3;
    [[nodiscard]] auto CellMinToWorld(int x, int y, int z) const -> Vec3;
    [[nodiscard]] auto CellToChunkCoord(int x, int y, int z) const -> ChunkCoord;
    [[nodiscard]] auto CellIndex(int x, int y, int z) const -> std::size_t;
    [[nodiscard]] auto GetCell(int x, int y, int z) const -> MaterialId;
    void SetCellUnchecked(int x, int y, int z, MaterialId material);
    [[nodiscard]] auto IsRenderable(MaterialId material) const -> bool;
    [[nodiscard]] auto IsBlocking(MaterialId material) const -> bool;

    void ResetTransientState();
    void ResizeStorage();
    void RebuildChunkRuntimeState();
    void NoteCellMaterialChange(int x, int y, int z, MaterialId previousMaterial, MaterialId nextMaterial);
    void NotifyExternalTerrainEdit();
    void TouchChunk(const ChunkCoord& chunk);
    void MarkChunkMeshDirty(const ChunkCoord& chunk);
    void MarkDirtyChunk(const ChunkCoord& chunk);
    void MarkDirty(int x, int y, int z);
    void ClearCellWaterMetadata(std::size_t index);
    void SetCellBatched(int x, int y, int z, MaterialId material, std::vector<ChunkCoord>& changedChunks);
    void CommitChunkEdits(std::vector<ChunkCoord>& changedChunks);
    void PruneRetiredChunkStates();
    void SetCell(int x, int y, int z, MaterialId material);
    [[nodiscard]] auto TryMoveCell(
        int fromX,
        int fromY,
        int fromZ,
        int toX,
        int toY,
        int toZ,
        bool allowSwapWithWater,
        std::int8_t waterLateralDirection = 0,
        std::vector<ChunkCoord>* changedChunks = nullptr) -> bool;
    [[nodiscard]] auto CanWaterSpreadLaterally(int x, int y, int z) const -> bool;
    [[nodiscard]] auto CountAdjacentWater(int x, int y, int z) const -> int;
    [[nodiscard]] auto ComputeLooseSimulationRegions() const -> std::vector<LooseSimulationBounds>;
    void SimulateDrySandPass(const LooseSimulationBounds& bounds);
    void SimulateWetMudPass(const LooseSimulationBounds& bounds);
    void SimulateWaterFallPass(const LooseSimulationBounds& bounds);
    void SimulateWaterSpreadPass(const LooseSimulationBounds& bounds);
    void EnsureMpmBackend();
    void SimulateLooseMaterialMpm(float dt);
    void SimulateLooseMaterialSlicePass(bool slicesAlongZ, int sliceBudget);
    [[nodiscard]] auto LooseMaterialParticleMass(MaterialId material) const -> float;
    void RebuildMeshCache(bool rebuildGlobalVertexCache);
    [[nodiscard]] auto LoadLegacyMaterialField(const std::filesystem::path& path) -> bool;

    void AppendFace(
        std::vector<render::ColorVertex3D>& triangles,
        std::vector<render::ColorVertex3D>& lines,
        int x,
        int y,
        int z,
        int faceIndex,
        const Vec4& color) const;
    void AppendBoxLines(std::vector<render::ColorVertex3D>& lines, const Vec3& minCorner, const Vec3& maxCorner, const Vec4& color) const;

    MaterialField field_;
    int width_ = 48;
    int height_ = 20;
    int depth_ = 48;
    int activeChunkSize_ = 32;
    WorldGenerationSettings generationSettings_{};
    float cellSize_ = generationSettings_.cellSize;
    bool terrainDirty_ = true;
    std::uint64_t simulationStep_ = 0;
    float sandTravelAccumulator_ = 0.0f;
    float mudTravelAccumulator_ = 0.0f;
    float waterFallAccumulator_ = 0.0f;
    float waterSpreadAccumulator_ = 0.0f;
    std::uint32_t waterSpreadPass_ = 0;
    std::size_t activeChunkCount_ = 0;
    ChunkRuntimeStateMap chunkRuntimeStates_;
    std::vector<std::uint8_t> cells_;
    std::vector<std::int8_t> waterLastLateralDirection_;
    std::vector<std::uint32_t> waterLastLateralPass_;
    std::vector<float> solidSurfaceHeightMap_;
    std::vector<float> waterSurfaceHeightMap_;
    std::vector<render::ColorVertex3D> terrainTriangleCache_;
    std::vector<render::ColorVertex3D> translucentTerrainTriangleCache_;
    std::vector<render::ColorVertex3D> terrainWireCache_;
    float mpmDt_ = 1.0f / 120.0f;
    float mpmTimeAccumulator_ = 0.0f;
    int mpmNextXySlice_ = 0;
    int mpmNextZySlice_ = 0;
    std::size_t mpmWorkerCount_ = 0;
    std::size_t meshWorkerCount_ = 0;
    std::string_view activeMpmBackendName_ = "CPU Reference";
    std::unique_ptr<sim::VulkanMpm2D> xySliceMpm_;
    std::unique_ptr<sim::VulkanMpm2D> zySliceMpm_;
    std::unique_ptr<JobSystem> meshJobs_;
    float terrainRebuildCooldown_ = 0.0f;
    float terrainRebuildInterval_ = 1.0f / 30.0f;
    double lastTerrainRebuildMilliseconds_ = 0.0;
    std::uint64_t terrainMeshVersion_ = 0;
    std::uint64_t terrainContentVersion_ = 0;
    bool globalMeshCacheDirty_ = true;
    int mpmStabilizationTicks_ = 0;
    FrameProfiler* profiler_ = nullptr;
};
}
