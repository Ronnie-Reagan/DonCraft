#pragma once

#include "core/math.hpp"

#include <SDL3/SDL_audio.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace df::platform
{
struct SynthCue
{
    Vec3 position{};
    float baseFrequency = 220.0f;
    float durationSeconds = 0.12f;
    float amplitude = 0.18f;
    float noise = 0.0f;
    float sweep = 0.0f;
    bool positional = false;
};

class AudioDevice
{
public:
    AudioDevice() = default;
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    [[nodiscard]] bool Initialize();
    void Shutdown();
    void SetListener(const Vec3& position, const Vec3& forward);
    void QueueCue(const SynthCue& cue);
    void SetEngineState(float load, float roughness, bool active, const Vec3& position);

private:
    struct Voice
    {
        float phase = 0.0f;
        float frequency = 220.0f;
        float amplitude = 0.15f;
        float noise = 0.0f;
        float sweep = 0.0f;
        float remainingSeconds = 0.0f;
        float totalSeconds = 0.0f;
        float leftGain = 1.0f;
        float rightGain = 1.0f;
    };

    static void SDLCALL StreamCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);
    void Mix(float* samples, int frameCount);
    [[nodiscard]] auto NextNoise() -> float;

    SDL_AudioStream* stream_ = nullptr;
    std::mutex mutex_;
    std::vector<Voice> voices_;
    std::uint32_t noiseState_ = 0x12345678u;
    Vec3 listenerPosition_{};
    Vec3 listenerForward_{0.0f, 0.0f, 1.0f};
    Vec3 enginePosition_{};
    float enginePhase_ = 0.0f;
    float engineLoad_ = 0.0f;
    float engineRoughness_ = 0.0f;
    bool engineActive_ = false;
    std::atomic<bool> shuttingDown_{false};
};
}
