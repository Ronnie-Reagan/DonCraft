#pragma once

#include "core/job_system.hpp"
#include "core/math.hpp"

#include <memory>
#include <vector>

namespace df::sim
{
class ReferenceMpm2D
{
public:
    struct Particle
    {
        Vec2 position{};
        Vec2 velocity{};
        float mass = 1.0f;
    };

    ReferenceMpm2D(int gridWidth, int gridHeight, float cellSize, float dt);

    void ClearParticles();
    void AddParticle(const Particle& particle);
    void SetWorkerCount(std::size_t workerCount);
    void Step(float gravityY = -9.81f);

    [[nodiscard]] const std::vector<Particle>& Particles() const
    {
        return particles_;
    }

    [[nodiscard]] float TotalParticleMass() const;
    [[nodiscard]] float TotalGridMass() const;
    [[nodiscard]] std::size_t WorkerCount() const;

private:
    struct GridNode
    {
        float mass = 0.0f;
        Vec2 velocity{};
    };

    void StepSerial(float gravityY);
    [[nodiscard]] int GridIndex(int x, int y) const;

    int gridWidth_ = 0;
    int gridHeight_ = 0;
    float cellSize_ = 1.0f;
    float dt_ = 1.0f / 60.0f;
    std::vector<GridNode> grid_;
    std::vector<Particle> particles_;
    std::size_t workerCount_ = 1;
    std::unique_ptr<JobSystem> jobs_;
};
}
