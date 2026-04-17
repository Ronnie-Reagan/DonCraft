#pragma once

#include "core/math.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace df::sim
{
class VulkanMpm2D
{
public:
    struct Particle
    {
        Vec2 position{};
        Vec2 velocity{};
        float mass = 1.0f;
    };

    VulkanMpm2D(int gridWidth, int gridHeight, float cellSize, float dt);
    ~VulkanMpm2D();

    VulkanMpm2D(const VulkanMpm2D&) = delete;
    VulkanMpm2D& operator=(const VulkanMpm2D&) = delete;

    void ClearParticles();
    void AddParticle(const Particle& particle);
    void SetCpuFallbackWorkerCount(std::size_t workerCount);
    void Step(float gravityY = -9.81f);

    [[nodiscard]] const std::vector<Particle>& Particles() const;
    [[nodiscard]] float TotalParticleMass() const;
    [[nodiscard]] float TotalGridMass() const;
    [[nodiscard]] bool IsAvailable() const;
    [[nodiscard]] std::size_t CpuFallbackWorkerCount() const;
    [[nodiscard]] std::string_view ActiveBackendName() const;
    [[nodiscard]] const std::string& FailureMessage() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
