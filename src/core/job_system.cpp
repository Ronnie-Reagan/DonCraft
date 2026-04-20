#include "core/job_system.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <atomic>
#include <exception>

namespace df
{
std::size_t JobSystem::RecommendWorkerCount(const std::size_t reservedLogicalProcessors)
{
    const std::size_t hardwareThreads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    if (hardwareThreads <= reservedLogicalProcessors)
    {
        return 1;
    }
    return std::max<std::size_t>(1, hardwareThreads - reservedLogicalProcessors);
}

JobSystem::JobSystem(std::size_t workerCount)
{
    if (workerCount == 0)
    {
        workerCount = RecommendWorkerCount(1);
    }

    workers_.reserve(workerCount);
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
    {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
}

JobSystem::~JobSystem()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
    }

    workReady_.notify_all();

    for (std::thread& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

void JobSystem::Submit(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push(std::move(job));
    }

    workReady_.notify_one();
}

void JobSystem::WaitIdle()
{
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this]() { return jobs_.empty() && activeWorkers_ == 0; });
}

void JobSystem::ParallelFor(
    const std::size_t count,
    const std::size_t grainSize,
    const std::function<void(std::size_t begin, std::size_t end)>& fn)
{
    if (count == 0)
    {
        return;
    }

    const std::size_t resolvedGrain = std::max<std::size_t>(1, grainSize);
    const std::size_t chunkCount = (count + resolvedGrain - 1) / resolvedGrain;
    if (chunkCount <= 1 || workers_.empty())
    {
        for (std::size_t begin = 0; begin < count; begin += resolvedGrain)
        {
            fn(begin, std::min(begin + resolvedGrain, count));
        }
        return;
    }

    const std::size_t helperTaskCount = std::min<std::size_t>(chunkCount, workers_.size() + 1);
    std::atomic<std::size_t> nextChunk{0};
    std::atomic<std::size_t> remainingTasks{helperTaskCount};
    std::mutex completionMutex;
    std::condition_variable completionCondition;
    std::exception_ptr firstException;
    std::mutex exceptionMutex;

    const auto workerBody = [&]()
    {
        try
        {
            for (;;)
            {
                const std::size_t chunkIndex = nextChunk.fetch_add(1);
                if (chunkIndex >= chunkCount)
                {
                    break;
                }

                const std::size_t begin = chunkIndex * resolvedGrain;
                const std::size_t end = std::min(begin + resolvedGrain, count);
                fn(begin, end);
            }
        }
        catch (...)
        {
            LogError("JobSystem worker task threw an exception.");
            std::lock_guard<std::mutex> lock(exceptionMutex);
            if (firstException == nullptr)
            {
                firstException = std::current_exception();
            }
        }

        // FIX: Lock the mutex BEFORE modifying the atomic counter.
        {
            std::lock_guard<std::mutex> lock(completionMutex);
            if (remainingTasks.fetch_sub(1) == 1)
            {
                completionCondition.notify_all();
            }
        }
    };

    for (std::size_t taskIndex = 1; taskIndex < helperTaskCount; ++taskIndex)
    {
        Submit(workerBody);
    }

    workerBody();

    std::unique_lock<std::mutex> lock(completionMutex);
    completionCondition.wait(lock, [&]() { return remainingTasks.load() == 0; });

    if (firstException != nullptr)
    {
        std::rethrow_exception(firstException);
    }
}

void JobSystem::WorkerLoop()
{
    for (;;)
    {
        std::function<void()> job;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            workReady_.wait(lock, [this]() { return stopRequested_ || !jobs_.empty(); });

            if (stopRequested_ && jobs_.empty())
            {
                return;
            }

            job = std::move(jobs_.front());
            jobs_.pop();
            ++activeWorkers_;
        }

        job();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --activeWorkers_;
            if (jobs_.empty() && activeWorkers_ == 0)
            {
                idle_.notify_all();
            }
        }
    }
}
}
