#pragma once

#include "core/timer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace df
{
class FrameProfiler
{
public:
    static constexpr std::size_t kMaxTrackedStages = 64;

    struct StageSnapshot
    {
        std::string_view name{};
        double lastMilliseconds = 0.0;
        double averageMilliseconds = 0.0;
        double maxMilliseconds = 0.0;
        double lastSelfMilliseconds = 0.0;
        double averageSelfMilliseconds = 0.0;
        double maxSelfMilliseconds = 0.0;
        std::uint32_t lastCalls = 0;
    };

    struct Snapshot
    {
        std::array<StageSnapshot, kMaxTrackedStages> stages{};
        std::array<std::size_t, kMaxTrackedStages> sortOrder{};
        std::size_t stageCount = 0;
        std::uint64_t frameIndex = 0;
        double frameLastMilliseconds = 0.0;
        double frameAverageMilliseconds = 0.0;
        double frameMaxMilliseconds = 0.0;
        std::string_view hottestStageName{};
        double hottestStageMilliseconds = 0.0;
        std::string_view hottestInclusiveStageName{};
        double hottestInclusiveStageMilliseconds = 0.0;
    };

    void BeginFrame();
    void EndFrame();
    void Record(std::string_view name, double elapsedMilliseconds);
    [[nodiscard]] bool Enter(std::string_view name);
    void Leave(std::string_view name, double elapsedMilliseconds);

    [[nodiscard]] const Snapshot& LatestSnapshot() const
    {
        return latestSnapshot_;
    }

private:
    struct StageState
    {
        std::string_view name{};
        double currentMilliseconds = 0.0;
        double currentSelfMilliseconds = 0.0;
        double lastMilliseconds = 0.0;
        double averageMilliseconds = 0.0;
        double maxMilliseconds = 0.0;
        double lastSelfMilliseconds = 0.0;
        double averageSelfMilliseconds = 0.0;
        double maxSelfMilliseconds = 0.0;
        std::uint32_t currentCalls = 0;
        std::uint32_t lastCalls = 0;
    };

    struct ActiveSection
    {
        std::size_t stageIndex = kMaxTrackedStages;
        double childMilliseconds = 0.0;
    };

    static constexpr std::size_t kMaxSectionDepth = 64;

    [[nodiscard]] std::size_t FindOrCreateStage(std::string_view name);

    Stopwatch frameStopwatch_{};
    std::array<StageState, kMaxTrackedStages> stages_{};
    std::array<ActiveSection, kMaxSectionDepth> activeSections_{};
    Snapshot latestSnapshot_{};
    std::size_t stageCount_ = 0;
    std::size_t activeSectionCount_ = 0;
    std::uint64_t frameIndex_ = 0;
    std::uint64_t lastSpikeLogFrame_ = 0;
    double frameAverageMilliseconds_ = 0.0;
    double frameMaxMilliseconds_ = 0.0;
};

class ScopedProfileSection
{
public:
    ScopedProfileSection(FrameProfiler* profiler, std::string_view name);
    ~ScopedProfileSection();

    ScopedProfileSection(const ScopedProfileSection&) = delete;
    auto operator=(const ScopedProfileSection&) -> ScopedProfileSection& = delete;

private:
    FrameProfiler* profiler_ = nullptr;
    std::string_view name_{};
    bool entered_ = false;
    Stopwatch stopwatch_{};
};
}
