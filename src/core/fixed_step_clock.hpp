#pragma once

#include <algorithm>
#include <functional>

namespace df
{
class FixedStepClock
{
public:
    explicit FixedStepClock(const double stepSeconds, const double maxAccumulatedSeconds = 0.25, const int maxTicksPerConsume = 8)
        : stepSeconds_(stepSeconds)
        , maxAccumulatedSeconds_(maxAccumulatedSeconds)
        , maxTicksPerConsume_(std::max(1, maxTicksPerConsume))
    {
    }

    template <typename TickFn>
    int Consume(const double frameDeltaSeconds, TickFn&& tickFn)
    {
        accumulatorSeconds_ = std::min(accumulatorSeconds_ + frameDeltaSeconds, maxAccumulatedSeconds_);

        int tickCount = 0;
        while (accumulatorSeconds_ >= stepSeconds_ && tickCount < maxTicksPerConsume_)
        {
            tickFn(stepSeconds_);
            accumulatorSeconds_ -= stepSeconds_;
            ++tickCount;
        }

        if (accumulatorSeconds_ >= stepSeconds_)
        {
            accumulatorSeconds_ = std::min(accumulatorSeconds_, stepSeconds_);
        }

        return tickCount;
    }

    [[nodiscard]] double InterpolationAlpha() const
    {
        return stepSeconds_ > 0.0 ? accumulatorSeconds_ / stepSeconds_ : 0.0;
    }

    [[nodiscard]] double StepSeconds() const
    {
        return stepSeconds_;
    }

private:
    double stepSeconds_{};
    double accumulatorSeconds_{};
    double maxAccumulatedSeconds_{};
    int maxTicksPerConsume_ = 8;
};
}
