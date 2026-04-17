#include "core/profiler.hpp"

#include "core/config.hpp"
#include "core/log.hpp"

#include <algorithm>

namespace df
{
namespace
{
auto BlendTowards(const double current, const double target, const double factor) -> double
{
    return current + (target - current) * factor;
}
}

void FrameProfiler::BeginFrame()
{
    frameStopwatch_.Reset();
    activeSectionCount_ = 0;
    for (std::size_t stageIndex = 0; stageIndex < stageCount_; ++stageIndex)
    {
        stages_[stageIndex].currentMilliseconds = 0.0;
        stages_[stageIndex].currentSelfMilliseconds = 0.0;
        stages_[stageIndex].currentCalls = 0;
    }
}

void FrameProfiler::EndFrame()
{
    ++frameIndex_;

    const double frameMilliseconds = frameStopwatch_.ElapsedMilliseconds();
    frameAverageMilliseconds_ = (frameIndex_ == 1u)
        ? frameMilliseconds
        : BlendTowards(frameAverageMilliseconds_, frameMilliseconds, 0.12);
    frameMaxMilliseconds_ = std::max(frameMaxMilliseconds_, frameMilliseconds);

    latestSnapshot_.stageCount = stageCount_;
    latestSnapshot_.frameIndex = frameIndex_;
    latestSnapshot_.frameLastMilliseconds = frameMilliseconds;
    latestSnapshot_.frameAverageMilliseconds = frameAverageMilliseconds_;
    latestSnapshot_.frameMaxMilliseconds = frameMaxMilliseconds_;
    latestSnapshot_.hottestStageName = {};
    latestSnapshot_.hottestStageMilliseconds = 0.0;
    latestSnapshot_.hottestInclusiveStageName = {};
    latestSnapshot_.hottestInclusiveStageMilliseconds = 0.0;

    for (std::size_t stageIndex = 0; stageIndex < stageCount_; ++stageIndex)
    {
        StageState& stage = stages_[stageIndex];
        stage.lastMilliseconds = stage.currentMilliseconds;
        stage.lastSelfMilliseconds = stage.currentSelfMilliseconds;
        stage.lastCalls = stage.currentCalls;
        stage.averageMilliseconds = (frameIndex_ == 1u)
            ? stage.currentMilliseconds
            : BlendTowards(stage.averageMilliseconds, stage.currentMilliseconds, stage.currentMilliseconds > stage.averageMilliseconds ? 0.20 : 0.10);
        stage.averageSelfMilliseconds = (frameIndex_ == 1u)
            ? stage.currentSelfMilliseconds
            : BlendTowards(stage.averageSelfMilliseconds, stage.currentSelfMilliseconds, stage.currentSelfMilliseconds > stage.averageSelfMilliseconds ? 0.20 : 0.10);
        stage.maxMilliseconds = std::max(stage.maxMilliseconds, stage.currentMilliseconds);
        stage.maxSelfMilliseconds = std::max(stage.maxSelfMilliseconds, stage.currentSelfMilliseconds);

        latestSnapshot_.stages[stageIndex] = {
            .name = stage.name,
            .lastMilliseconds = stage.lastMilliseconds,
            .averageMilliseconds = stage.averageMilliseconds,
            .maxMilliseconds = stage.maxMilliseconds,
            .lastSelfMilliseconds = stage.lastSelfMilliseconds,
            .averageSelfMilliseconds = stage.averageSelfMilliseconds,
            .maxSelfMilliseconds = stage.maxSelfMilliseconds,
            .lastCalls = stage.lastCalls,
        };
        latestSnapshot_.sortOrder[stageIndex] = stageIndex;

        if (stage.lastSelfMilliseconds > latestSnapshot_.hottestStageMilliseconds)
        {
            latestSnapshot_.hottestStageMilliseconds = stage.lastSelfMilliseconds;
            latestSnapshot_.hottestStageName = stage.name;
        }
        if (stage.lastMilliseconds > latestSnapshot_.hottestInclusiveStageMilliseconds)
        {
            latestSnapshot_.hottestInclusiveStageMilliseconds = stage.lastMilliseconds;
            latestSnapshot_.hottestInclusiveStageName = stage.name;
        }
    }

    std::sort(
        latestSnapshot_.sortOrder.begin(),
        latestSnapshot_.sortOrder.begin() + static_cast<std::ptrdiff_t>(stageCount_),
        [this](const std::size_t lhs, const std::size_t rhs)
        {
            const StageState& left = stages_[lhs];
            const StageState& right = stages_[rhs];
            if (left.lastSelfMilliseconds != right.lastSelfMilliseconds)
            {
                return left.lastSelfMilliseconds > right.lastSelfMilliseconds;
            }
            if (left.lastMilliseconds != right.lastMilliseconds)
            {
                return left.lastMilliseconds > right.lastMilliseconds;
            }
            return left.averageSelfMilliseconds > right.averageSelfMilliseconds;
        });

    const bool slowFrame =
        frameMilliseconds >= config::kProfilerSpikeWarningMilliseconds ||
        latestSnapshot_.hottestStageMilliseconds >= config::kProfilerSpikeWarningMilliseconds ||
        latestSnapshot_.hottestInclusiveStageMilliseconds >= config::kProfilerSpikeWarningMilliseconds;
    if (slowFrame && (lastSpikeLogFrame_ == 0u || frameIndex_ - lastSpikeLogFrame_ >= config::kProfilerSpikeLogCooldownFrames))
    {
        lastSpikeLogFrame_ = frameIndex_;

        LogWarning(
            "Profiler spike | frame=", frameIndex_,
            " frame_ms=", frameMilliseconds,
            " hottest_self=", latestSnapshot_.hottestStageName,
            " hottest_self_ms=", latestSnapshot_.hottestStageMilliseconds,
            " hottest_total=", latestSnapshot_.hottestInclusiveStageName,
            " hottest_total_ms=", latestSnapshot_.hottestInclusiveStageMilliseconds);

        const std::size_t logCount = std::min<std::size_t>(3u, stageCount_);
        for (std::size_t orderIndex = 0; orderIndex < logCount; ++orderIndex)
        {
            const StageSnapshot& stage = latestSnapshot_.stages[latestSnapshot_.sortOrder[orderIndex]];
            LogWarning(
                "Profiler stage | rank=", (orderIndex + 1u),
                " name=", stage.name,
                " self_ms=", stage.lastSelfMilliseconds,
                " total_ms=", stage.lastMilliseconds,
                " avg_self_ms=", stage.averageSelfMilliseconds,
                " avg_total_ms=", stage.averageMilliseconds,
                " max_self_ms=", stage.maxSelfMilliseconds,
                " max_total_ms=", stage.maxMilliseconds,
                " calls=", stage.lastCalls);
        }
    }

    activeSectionCount_ = 0;
}

void FrameProfiler::Record(const std::string_view name, const double elapsedMilliseconds)
{
    const std::size_t stageIndex = FindOrCreateStage(name);
    if (stageIndex >= kMaxTrackedStages)
    {
        return;
    }

    stages_[stageIndex].currentMilliseconds += elapsedMilliseconds;
    stages_[stageIndex].currentSelfMilliseconds += elapsedMilliseconds;
    ++stages_[stageIndex].currentCalls;
}

bool FrameProfiler::Enter(const std::string_view name)
{
    const std::size_t stageIndex = FindOrCreateStage(name);
    if (stageIndex >= kMaxTrackedStages || activeSectionCount_ >= kMaxSectionDepth)
    {
        return false;
    }

    activeSections_[activeSectionCount_++] = {
        .stageIndex = stageIndex,
        .childMilliseconds = 0.0,
    };
    return true;
}

void FrameProfiler::Leave(const std::string_view name, const double elapsedMilliseconds)
{
    if (activeSectionCount_ == 0u)
    {
        Record(name, elapsedMilliseconds);
        return;
    }

    const ActiveSection section = activeSections_[--activeSectionCount_];
    if (section.stageIndex >= kMaxTrackedStages)
    {
        Record(name, elapsedMilliseconds);
        return;
    }

    StageState& stage = stages_[section.stageIndex];
    stage.currentMilliseconds += elapsedMilliseconds;
    stage.currentSelfMilliseconds += std::max(0.0, elapsedMilliseconds - section.childMilliseconds);
    ++stage.currentCalls;

    if (activeSectionCount_ > 0u)
    {
        activeSections_[activeSectionCount_ - 1u].childMilliseconds += elapsedMilliseconds;
    }
}

std::size_t FrameProfiler::FindOrCreateStage(const std::string_view name)
{
    for (std::size_t stageIndex = 0; stageIndex < stageCount_; ++stageIndex)
    {
        if (stages_[stageIndex].name == name)
        {
            return stageIndex;
        }
    }

    if (stageCount_ >= kMaxTrackedStages)
    {
        return kMaxTrackedStages;
    }

    StageState& stage = stages_[stageCount_];
    stage = {};
    stage.name = name;
    return stageCount_++;
}

ScopedProfileSection::ScopedProfileSection(FrameProfiler* profiler, const std::string_view name)
    : profiler_(profiler)
    , name_(name)
{
    if (profiler_ != nullptr)
    {
        entered_ = profiler_->Enter(name_);
    }
}

ScopedProfileSection::~ScopedProfileSection()
{
    if (profiler_ == nullptr)
    {
        return;
    }

    const double elapsedMilliseconds = stopwatch_.ElapsedMilliseconds();
    if (entered_)
    {
        profiler_->Leave(name_, elapsedMilliseconds);
        return;
    }

    profiler_->Record(name_, elapsedMilliseconds);
}
}
