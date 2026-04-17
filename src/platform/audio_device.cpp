#include "platform/audio_device.hpp"

#include "core/math.hpp"

#include <SDL3/SDL_audio.h>

#include <algorithm>
#include <cmath>

namespace df::platform
{
namespace
{
constexpr int kSampleRate = 48000;
}

AudioDevice::~AudioDevice()
{
    Shutdown();
}

bool AudioDevice::Initialize()
{
    if (stream_ != nullptr)
    {
        return true;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = kSampleRate;

    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioDevice::StreamCallback, this);
    if (stream_ == nullptr)
    {
        return false;
    }

    return SDL_ResumeAudioStreamDevice(stream_);
}

void AudioDevice::Shutdown()
{
    if (stream_ != nullptr)
    {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }

    std::scoped_lock lock(mutex_);
    voices_.clear();
    engineActive_ = false;
    engineLoad_ = 0.0f;
    engineRoughness_ = 0.0f;
}

void AudioDevice::QueueCue(const SynthCue& cue)
{
    std::scoped_lock lock(mutex_);
    voices_.push_back({
        .phase = 0.0f,
        .frequency = cue.baseFrequency,
        .amplitude = cue.amplitude,
        .noise = cue.noise,
        .sweep = cue.sweep,
        .remainingSeconds = cue.durationSeconds,
        .totalSeconds = cue.durationSeconds,
    });
}

void AudioDevice::SetEngineState(const float load, const float roughness, const bool active)
{
    std::scoped_lock lock(mutex_);
    engineLoad_ = Clamp(load, 0.0f, 1.0f);
    engineRoughness_ = Clamp(roughness, 0.0f, 1.0f);
    engineActive_ = active;
}

void SDLCALL AudioDevice::StreamCallback(void* userdata, SDL_AudioStream* stream, const int additionalAmount, int)
{
    auto* device = static_cast<AudioDevice*>(userdata);
    const int sampleCount = additionalAmount / static_cast<int>(sizeof(float));
    if (sampleCount <= 0)
    {
        return;
    }

    std::vector<float> samples(static_cast<std::size_t>(sampleCount), 0.0f);
    device->Mix(samples.data(), sampleCount);
    SDL_PutAudioStreamData(stream, samples.data(), sampleCount * static_cast<int>(sizeof(float)));
}

void AudioDevice::Mix(float* samples, const int sampleCount)
{
    std::scoped_lock lock(mutex_);

    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        float mixed = 0.0f;

        if (engineActive_)
        {
            const float engineFrequency = Lerp(46.0f, 108.0f, engineLoad_);
            enginePhase_ += (2.0f * kPi * engineFrequency) / static_cast<float>(kSampleRate);
            if (enginePhase_ > 2.0f * kPi)
            {
                enginePhase_ -= 2.0f * kPi;
            }

            const float harmonic = std::sin(enginePhase_ * 2.0f + 0.35f) * 0.32f;
            const float roughNoise = NextNoise() * engineRoughness_ * 0.12f;
            mixed += (std::sin(enginePhase_) * 0.18f + harmonic * 0.10f + roughNoise) * (0.18f + engineLoad_ * 0.26f);
        }

        for (auto voiceIter = voices_.begin(); voiceIter != voices_.end();)
        {
            const float progress = 1.0f - (voiceIter->remainingSeconds / std::max(voiceIter->totalSeconds, 0.0001f));
            const float envelope = 1.0f - progress;
            const float voiceFrequency = std::max(35.0f, voiceIter->frequency + voiceIter->sweep * progress);

            voiceIter->phase += (2.0f * kPi * voiceFrequency) / static_cast<float>(kSampleRate);
            if (voiceIter->phase > 2.0f * kPi)
            {
                voiceIter->phase -= 2.0f * kPi;
            }

            const float tone = std::sin(voiceIter->phase) * (1.0f - voiceIter->noise);
            const float noise = NextNoise() * voiceIter->noise;
            mixed += (tone + noise) * voiceIter->amplitude * envelope;

            voiceIter->remainingSeconds -= 1.0f / static_cast<float>(kSampleRate);
            if (voiceIter->remainingSeconds <= 0.0f)
            {
                voiceIter = voices_.erase(voiceIter);
            }
            else
            {
                ++voiceIter;
            }
        }

        samples[sampleIndex] = Clamp(mixed, -0.95f, 0.95f);
    }
}

auto AudioDevice::NextNoise() -> float
{
    noiseState_ = noiseState_ * 1664525u + 1013904223u;
    return (static_cast<float>((noiseState_ >> 8u) & 0x00ffffffu) / static_cast<float>(0x00800000u)) - 1.0f;
}
}
