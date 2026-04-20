#pragma once

#include <cstddef>
#include <cstdint>

namespace df::config
{
inline constexpr char kApplicationName[] = "DonCraft";
inline constexpr int kDefaultWindowWidth = 1600;
inline constexpr int kDefaultWindowHeight = 900;
inline constexpr double kFixedTickSeconds = 1.0 / 120.0f;
inline constexpr int kMaxFixedTicksPerFrame = 12;
inline constexpr double kMinimumFrameSeconds = 0.014;
inline constexpr double kProfilerSpikeWarningMilliseconds = 150.0;
inline constexpr std::uint32_t kProfilerSpikeLogCooldownFrames = 10;
inline constexpr std::size_t kProfilerHudStageCount = 6;
inline constexpr std::uint32_t kMaxFramesInFlight = 2;
inline constexpr std::uint32_t kChunkSize = 1;
}
