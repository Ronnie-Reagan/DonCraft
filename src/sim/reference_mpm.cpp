#include "sim/reference_mpm.hpp"

#include "core/assert.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <sstream>
#include <thread>
#include <cmath>

namespace df::sim
{

namespace
{
auto EnvFlagEnabled(const char* const name) -> bool
{
    const char* const value = std::getenv(name);
    if (value == nullptr)
    {
        return false;
    }

    return value[0] == '1' || value[0] == 'T' || value[0] == 't' || value[0] == 'Y' || value[0] == 'y';
}

template <typename... Args>
void EmitImmediateTrace(const char* const scope, const Args&... args)
{
    return;
    static std::atomic<std::uint64_t> sequence{1u};

    std::ostringstream stream;
    (stream << ... << args);
    const std::string message = stream.str();
    const std::uint64_t sequenceId = sequence.fetch_add(1u, std::memory_order_relaxed);
    const unsigned long long threadId = static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    std::fprintf(
        stderr,
        "[MPM-TRACE][%s][#%llu][tid=%llu] %s\n",
        scope,
        static_cast<unsigned long long>(sequenceId),
        threadId,
        message.c_str());
    std::fflush(stderr);

    try
    {
        std::ofstream traceFile("doncraft_mpm_trace.log", std::ios::app);
        if (traceFile)
        {
            traceFile
                << "[MPM-TRACE][" << scope << "][#" << sequenceId << "][tid=" << threadId << "] "
                << message
                << '\n';
            traceFile.flush();
        }
    }
    catch (...)
    {
    }
}
}

ReferenceMpm2D::ReferenceMpm2D(const int gridWidth, const int gridHeight, const float cellSize, const float dt)
    : gridWidth_(gridWidth)
    , gridHeight_(gridHeight)
    , cellSize_(cellSize)
    , dt_(dt)
    , grid_(static_cast<std::size_t>(gridWidth) * static_cast<std::size_t>(gridHeight))
{
    DF_ASSERT(gridWidth > 1 && gridHeight > 1, "ReferenceMpm2D requires at least a 2x2 node grid.");
    DF_ASSERT(cellSize > 0.0f, "ReferenceMpm2D requires a positive cell size.");
    DF_ASSERT(dt > 0.0f, "ReferenceMpm2D requires a positive timestep.");

    SetWorkerCount(JobSystem::RecommendWorkerCount(1));
}

void ReferenceMpm2D::ClearParticles()
{
    particles_.clear();
}

void ReferenceMpm2D::AddParticle(const Particle& particle)
{
    particles_.push_back(particle);
}

void ReferenceMpm2D::SetWorkerCount(const std::size_t workerCount)
{
    const std::size_t resolvedWorkerCount = std::max<std::size_t>(1, workerCount);
    if (jobs_ != nullptr && workerCount_ == resolvedWorkerCount)
    {
        return;
    }

    if (jobs_ != nullptr)
    {
        jobs_->WaitIdle();
    }

    workerCount_ = resolvedWorkerCount;
    jobs_ = std::make_unique<JobSystem>(workerCount_);
}

void ReferenceMpm2D::Step(const float gravityY)
{
    const bool forceSerial = EnvFlagEnabled("DONCRAFT_MPM_FORCE_SERIAL_CPU");
    EmitImmediateTrace(
        "ReferenceMpm2D::Step",
        "enter particles=", particles_.size(),
        " worker_count=", workerCount_,
        " jobs_ptr=", static_cast<const void*>(jobs_.get()),
        " grid=", gridWidth_, "x", gridHeight_,
        " gravityY=", gravityY,
        " force_serial=", forceSerial);

    if (forceSerial || particles_.size() < 256 || workerCount_ <= 1 || jobs_ == nullptr)
    {
        EmitImmediateTrace("ReferenceMpm2D::Step", "dispatch_serial");
        StepSerial(gravityY);
        EmitImmediateTrace("ReferenceMpm2D::Step", "return_serial total_grid_mass=", TotalGridMass());
        return;
    }

    const std::size_t particleCount = particles_.size();
    const std::size_t nodeCount = grid_.size();
    const std::size_t taskCount = std::min<std::size_t>(std::max<std::size_t>(1, workerCount_), particleCount);
    const std::size_t particleTaskGrain = std::max<std::size_t>(1, (particleCount + taskCount - 1) / taskCount);

    EmitImmediateTrace(
        "ReferenceMpm2D::Step",
        "dispatch_parallel particle_count=", particleCount,
        " node_count=", nodeCount,
        " task_count=", taskCount,
        " particle_task_grain=", particleTaskGrain);

    for (GridNode& node : grid_)
    {
        node.mass = 0.0f;
        node.velocity = {};
    }

    std::vector<std::vector<GridNode>> localGrids(taskCount, std::vector<GridNode>(nodeCount));
    EmitImmediateTrace("ReferenceMpm2D::Step", "before_parallel_scatter");
    jobs_->ParallelFor(taskCount, 1, [&](const std::size_t taskBegin, const std::size_t taskEnd)
    {
        for (std::size_t taskIndex = taskBegin; taskIndex < taskEnd; ++taskIndex)
        {
            std::vector<GridNode>& localGrid = localGrids[taskIndex];
            const std::size_t beginParticle = taskIndex * particleTaskGrain;
            const std::size_t endParticle = std::min(beginParticle + particleTaskGrain, particleCount);
            for (std::size_t particleIndex = beginParticle; particleIndex < endParticle; ++particleIndex)
            {
                const Particle& particle = particles_[particleIndex];
                const float gridX = particle.position.x / cellSize_;
                const float gridY = particle.position.y / cellSize_;
                const int baseX = std::clamp(static_cast<int>(std::floor(gridX)), 0, gridWidth_ - 2);
                const int baseY = std::clamp(static_cast<int>(std::floor(gridY)), 0, gridHeight_ - 2);
                const float fx = std::clamp(gridX - static_cast<float>(baseX), 0.0f, 1.0f);
                const float fy = std::clamp(gridY - static_cast<float>(baseY), 0.0f, 1.0f);

                for (int offsetY = 0; offsetY < 2; ++offsetY)
                {
                    const float wy = offsetY == 0 ? 1.0f - fy : fy;
                    for (int offsetX = 0; offsetX < 2; ++offsetX)
                    {
                        const float wx = offsetX == 0 ? 1.0f - fx : fx;
                        const float weight = wx * wy;
                        GridNode& node = localGrid[static_cast<std::size_t>(GridIndex(baseX + offsetX, baseY + offsetY))];
                        node.mass += particle.mass * weight;
                        node.velocity += particle.velocity * (particle.mass * weight);
                    }
                }
            }
        }
    });
    EmitImmediateTrace("ReferenceMpm2D::Step", "after_parallel_scatter");

    const std::size_t nodeGrain = std::max<std::size_t>(64, nodeCount / std::max<std::size_t>(1, workerCount_ * 4));
    EmitImmediateTrace("ReferenceMpm2D::Step", "before_parallel_reduce node_grain=", nodeGrain);
    jobs_->ParallelFor(nodeCount, nodeGrain, [&](const std::size_t begin, const std::size_t end)
    {
        for (std::size_t nodeIndex = begin; nodeIndex < end; ++nodeIndex)
        {
            GridNode reduced{};
            for (const auto& localGrid : localGrids)
            {
                reduced.mass += localGrid[nodeIndex].mass;
                reduced.velocity += localGrid[nodeIndex].velocity;
            }
            grid_[nodeIndex] = reduced;
        }
    });
    EmitImmediateTrace("ReferenceMpm2D::Step", "after_parallel_reduce");

    EmitImmediateTrace("ReferenceMpm2D::Step", "before_parallel_node_update");
    jobs_->ParallelFor(nodeCount, nodeGrain, [&](const std::size_t begin, const std::size_t end)
    {
        for (std::size_t nodeIndex = begin; nodeIndex < end; ++nodeIndex)
        {
            GridNode& node = grid_[nodeIndex];
            if (node.mass <= 0.0f)
            {
                continue;
            }

            const int x = static_cast<int>(nodeIndex % static_cast<std::size_t>(gridWidth_));
            const int y = static_cast<int>(nodeIndex / static_cast<std::size_t>(gridWidth_));
            node.velocity /= node.mass;
            node.velocity.y += gravityY * dt_;

            if ((x == 0 && node.velocity.x < 0.0f) || (x == gridWidth_ - 1 && node.velocity.x > 0.0f))
            {
                node.velocity.x = 0.0f;
            }
            if ((y == 0 && node.velocity.y < 0.0f) || (y == gridHeight_ - 1 && node.velocity.y > 0.0f))
            {
                node.velocity.y = 0.0f;
            }
        }
    });
    EmitImmediateTrace("ReferenceMpm2D::Step", "after_parallel_node_update");

    const float maxX = static_cast<float>(gridWidth_ - 1) * cellSize_;
    const float maxY = static_cast<float>(gridHeight_ - 1) * cellSize_;
    const std::size_t particleGrain = std::max<std::size_t>(64, particleCount / std::max<std::size_t>(1, workerCount_ * 4));
    EmitImmediateTrace("ReferenceMpm2D::Step", "before_parallel_advect particle_grain=", particleGrain);
    jobs_->ParallelFor(particleCount, particleGrain, [&](const std::size_t begin, const std::size_t end)
    {
        for (std::size_t particleIndex = begin; particleIndex < end; ++particleIndex)
        {
            Particle& particle = particles_[particleIndex];
            const float gridX = particle.position.x / cellSize_;
            const float gridY = particle.position.y / cellSize_;
            const int baseX = std::clamp(static_cast<int>(std::floor(gridX)), 0, gridWidth_ - 2);
            const int baseY = std::clamp(static_cast<int>(std::floor(gridY)), 0, gridHeight_ - 2);
            const float fx = std::clamp(gridX - static_cast<float>(baseX), 0.0f, 1.0f);
            const float fy = std::clamp(gridY - static_cast<float>(baseY), 0.0f, 1.0f);

            Vec2 gatheredVelocity{};
            for (int offsetY = 0; offsetY < 2; ++offsetY)
            {
                const float wy = offsetY == 0 ? 1.0f - fy : fy;
                for (int offsetX = 0; offsetX < 2; ++offsetX)
                {
                    const float wx = offsetX == 0 ? 1.0f - fx : fx;
                    const float weight = wx * wy;
                    const GridNode& node = grid_[static_cast<std::size_t>(GridIndex(baseX + offsetX, baseY + offsetY))];
                    gatheredVelocity += node.velocity * weight;
                }
            }

            particle.velocity = gatheredVelocity;
            particle.position += particle.velocity * dt_;
            particle.position.x = Clamp(particle.position.x, 0.0f, maxX);
            particle.position.y = Clamp(particle.position.y, 0.0f, maxY);
        }
    });
    EmitImmediateTrace("ReferenceMpm2D::Step", "after_parallel_advect");

    EmitImmediateTrace("ReferenceMpm2D::Step", "exit_parallel total_grid_mass=", TotalGridMass());
}

void ReferenceMpm2D::StepSerial(const float gravityY)
{
    EmitImmediateTrace(
        "ReferenceMpm2D::StepSerial",
        "enter particles=", particles_.size(),
        " grid=", gridWidth_, "x", gridHeight_,
        " gravityY=", gravityY);

    for (GridNode& node : grid_)
    {
        node.mass = 0.0f;
        node.velocity = {};
    }

    for (const Particle& particle : particles_)
    {
        const float gridX = particle.position.x / cellSize_;
        const float gridY = particle.position.y / cellSize_;
        const int baseX = std::clamp(static_cast<int>(std::floor(gridX)), 0, gridWidth_ - 2);
        const int baseY = std::clamp(static_cast<int>(std::floor(gridY)), 0, gridHeight_ - 2);
        const float fx = std::clamp(gridX - static_cast<float>(baseX), 0.0f, 1.0f);
        const float fy = std::clamp(gridY - static_cast<float>(baseY), 0.0f, 1.0f);

        for (int offsetY = 0; offsetY < 2; ++offsetY)
        {
            const float wy = offsetY == 0 ? 1.0f - fy : fy;
            for (int offsetX = 0; offsetX < 2; ++offsetX)
            {
                const float wx = offsetX == 0 ? 1.0f - fx : fx;
                const float weight = wx * wy;
                GridNode& node = grid_[static_cast<std::size_t>(GridIndex(baseX + offsetX, baseY + offsetY))];
                node.mass += particle.mass * weight;
                node.velocity += particle.velocity * (particle.mass * weight);
            }
        }
    }

    for (int y = 0; y < gridHeight_; ++y)
    {
        for (int x = 0; x < gridWidth_; ++x)
        {
            GridNode& node = grid_[static_cast<std::size_t>(GridIndex(x, y))];
            if (node.mass <= 0.0f)
            {
                continue;
            }

            node.velocity /= node.mass;
            node.velocity.y += gravityY * dt_;

            if ((x == 0 && node.velocity.x < 0.0f) || (x == gridWidth_ - 1 && node.velocity.x > 0.0f))
            {
                node.velocity.x = 0.0f;
            }

            if ((y == 0 && node.velocity.y < 0.0f) || (y == gridHeight_ - 1 && node.velocity.y > 0.0f))
            {
                node.velocity.y = 0.0f;
            }
        }
    }

    const float maxX = static_cast<float>(gridWidth_ - 1) * cellSize_;
    const float maxY = static_cast<float>(gridHeight_ - 1) * cellSize_;

    for (Particle& particle : particles_)
    {
        const float gridX = particle.position.x / cellSize_;
        const float gridY = particle.position.y / cellSize_;
        const int baseX = std::clamp(static_cast<int>(std::floor(gridX)), 0, gridWidth_ - 2);
        const int baseY = std::clamp(static_cast<int>(std::floor(gridY)), 0, gridHeight_ - 2);
        const float fx = std::clamp(gridX - static_cast<float>(baseX), 0.0f, 1.0f);
        const float fy = std::clamp(gridY - static_cast<float>(baseY), 0.0f, 1.0f);

        Vec2 gatheredVelocity{};
        for (int offsetY = 0; offsetY < 2; ++offsetY)
        {
            const float wy = offsetY == 0 ? 1.0f - fy : fy;
            for (int offsetX = 0; offsetX < 2; ++offsetX)
            {
                const float wx = offsetX == 0 ? 1.0f - fx : fx;
                const float weight = wx * wy;
                const GridNode& node = grid_[static_cast<std::size_t>(GridIndex(baseX + offsetX, baseY + offsetY))];
                gatheredVelocity += node.velocity * weight;
            }
        }

        particle.velocity = gatheredVelocity;
        particle.position += particle.velocity * dt_;
        particle.position.x = Clamp(particle.position.x, 0.0f, maxX);
        particle.position.y = Clamp(particle.position.y, 0.0f, maxY);
    }

    EmitImmediateTrace("ReferenceMpm2D::StepSerial", "exit total_grid_mass=", TotalGridMass());
}

float ReferenceMpm2D::TotalParticleMass() const
{
    float totalMass = 0.0f;
    for (const Particle& particle : particles_)
    {
        totalMass += particle.mass;
    }

    return totalMass;
}

float ReferenceMpm2D::TotalGridMass() const
{
    float totalMass = 0.0f;
    for (const GridNode& node : grid_)
    {
        totalMass += node.mass;
    }

    return totalMass;
}

std::size_t ReferenceMpm2D::WorkerCount() const
{
    return workerCount_;
}

int ReferenceMpm2D::GridIndex(const int x, const int y) const
{
    return y * gridWidth_ + x;
}
}
