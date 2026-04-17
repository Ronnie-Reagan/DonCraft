#pragma once

#include <chrono>

namespace df
{
class Stopwatch
{
public:
    using Clock = std::chrono::steady_clock;

    Stopwatch()
        : startTime_(Clock::now())
    {
    }

    void Reset()
    {
        startTime_ = Clock::now();
    }

    [[nodiscard]] double ElapsedSeconds() const
    {
        return std::chrono::duration<double>(Clock::now() - startTime_).count();
    }

    [[nodiscard]] double ElapsedMilliseconds() const
    {
        return ElapsedSeconds() * 1000.0;
    }

private:
    Clock::time_point startTime_;
};
}
