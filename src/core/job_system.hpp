#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace df
{
class JobSystem
{
public:
    static std::size_t RecommendWorkerCount(std::size_t reservedLogicalProcessors = 1);

    explicit JobSystem(std::size_t workerCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void Submit(std::function<void()> job);
    void WaitIdle();
    void ParallelFor(std::size_t count, std::size_t grainSize, const std::function<void(std::size_t begin, std::size_t end)>& fn);

    [[nodiscard]] std::size_t WorkerCount() const
    {
        return workers_.size();
    }

private:
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable workReady_;
    std::condition_variable idle_;
    bool stopRequested_ = false;
    std::size_t activeWorkers_ = 0;
};
}
