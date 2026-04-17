#pragma once

#include <SDL3/SDL_audio.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace df::platform
{
struct SynthCue
{
    float baseFrequency = 220.0f;
    float durationSeconds = 0.12f;
    float amplitude = 0.18f;
    float noise = 0.0f;
    float sweep = 0.0f;
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
    void QueueCue(const SynthCue& cue);
    void SetEngineState(float load, float roughness, bool active);

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
    };

    static void SDLCALL StreamCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);
    void Mix(float* samples, int sampleCount);
    [[nodiscard]] auto NextNoise() -> float;

    SDL_AudioStream* stream_ = nullptr;
    std::mutex mutex_;
    std::vector<Voice> voices_;
    std::uint32_t noiseState_ = 0x12345678u;
    float enginePhase_ = 0.0f;
    float engineLoad_ = 0.0f;
    float engineRoughness_ = 0.0f;
    bool engineActive_ = false;
};
}
