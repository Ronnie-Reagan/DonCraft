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
constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};

struct SpatialMix
{
    float leftGain = 0.0f;
    float rightGain = 0.0f;
};

auto ResolveListenerForward(const Vec3& forward) -> Vec3
{
    const Vec3 normalized = Normalize(Vec3{forward.x, 0.0f, forward.z});
    return LengthSquared(normalized) > 1.0e-6f ? normalized : Vec3{0.0f, 0.0f, 1.0f};
}

auto ResolveListenerRight(const Vec3& forward) -> Vec3
{
    const Vec3 right = Normalize(Cross(ResolveListenerForward(forward), kWorldUp));
    return LengthSquared(right) > 1.0e-6f ? right : Vec3{1.0f, 0.0f, 0.0f};
}

auto ComputeSpatialMix(const Vec3& listenerPosition, const Vec3& listenerForward, const Vec3& sourcePosition) -> SpatialMix
{
    const Vec3 delta = sourcePosition - listenerPosition;
    const float distance = Length(delta);
    constexpr float kNearDistance = 1.25f;
    constexpr float kFarDistance = 42.0f;
    if (distance >= kFarDistance)
    {
        return {};
    }

    float attenuation = 1.0f;
    if (distance > kNearDistance)
    {
        const float fade = Clamp((distance - kNearDistance) / (kFarDistance - kNearDistance), 0.0f, 1.0f);
        attenuation = (1.0f - fade) * (1.0f - fade * 0.35f);
    }

    float pan = 0.0f;
    const Vec3 flatDelta = Normalize(Vec3{delta.x, 0.0f, delta.z});
    if (LengthSquared(flatDelta) > 1.0e-6f)
    {
        pan = Clamp(Dot(flatDelta, ResolveListenerRight(listenerForward)), -1.0f, 1.0f);
    }

    const float panAngle = (pan + 1.0f) * (kPi * 0.25f);
    return {
        .leftGain = attenuation * std::cos(panAngle),
        .rightGain = attenuation * std::sin(panAngle),
    };
}
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

    shuttingDown_.store(false, std::memory_order_release);

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
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
        shuttingDown_.store(true, std::memory_order_release);
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }

    std::scoped_lock lock(mutex_);
    voices_.clear();
    listenerPosition_ = {};
    listenerForward_ = Vec3{0.0f, 0.0f, 1.0f};
    enginePosition_ = {};
    engineActive_ = false;
    engineLoad_ = 0.0f;
    engineRoughness_ = 0.0f;
}

void AudioDevice::SetListener(const Vec3& position, const Vec3& forward)
{
    std::scoped_lock lock(mutex_);
    listenerPosition_ = position;
    listenerForward_ = ResolveListenerForward(forward);
}

void AudioDevice::QueueCue(const SynthCue& cue)
{
    std::scoped_lock lock(mutex_);
    const SpatialMix spatialMix = cue.positional
        ? ComputeSpatialMix(listenerPosition_, listenerForward_, cue.position)
        : SpatialMix{0.70710678f, 0.70710678f};
    if (cue.positional && spatialMix.leftGain <= 1.0e-4f && spatialMix.rightGain <= 1.0e-4f)
    {
        return;
    }
    voices_.push_back({
        .phase = 0.0f,
        .frequency = cue.baseFrequency,
        .amplitude = cue.amplitude,
        .noise = cue.noise,
        .sweep = cue.sweep,
        .remainingSeconds = cue.durationSeconds,
        .totalSeconds = cue.durationSeconds,
        .leftGain = spatialMix.leftGain,
        .rightGain = spatialMix.rightGain,
    });
}

void AudioDevice::SetEngineState(const float load, const float roughness, const bool active, const Vec3& position)
{
    std::scoped_lock lock(mutex_);
    engineLoad_ = Clamp(load, 0.0f, 1.0f);
    engineRoughness_ = Clamp(roughness, 0.0f, 1.0f);
    engineActive_ = active;
    enginePosition_ = position;
}

void SDLCALL AudioDevice::StreamCallback(void* userdata, SDL_AudioStream* stream, const int additionalAmount, int)
{
    auto* device = static_cast<AudioDevice*>(userdata);
    if (device == nullptr || device->shuttingDown_.load(std::memory_order_acquire))
    {
        return;
    }

    const int frameCount = additionalAmount / static_cast<int>(sizeof(float) * 2);
    if (frameCount <= 0)
    {
        return;
    }

    std::vector<float> samples(static_cast<std::size_t>(frameCount) * 2u, 0.0f);
    device->Mix(samples.data(), frameCount);
    SDL_PutAudioStreamData(stream, samples.data(), static_cast<int>(samples.size() * sizeof(float)));
}

void AudioDevice::Mix(float* samples, const int frameCount)
{
    if (shuttingDown_.load(std::memory_order_acquire))
    {
        return;
    }

    std::scoped_lock lock(mutex_);
    const SpatialMix engineSpatialMix = ComputeSpatialMix(listenerPosition_, listenerForward_, enginePosition_);

    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex)
    {
        float mixedLeft = 0.0f;
        float mixedRight = 0.0f;

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
            const float engineSample = (std::sin(enginePhase_) * 0.18f + harmonic * 0.10f + roughNoise) * (0.18f + engineLoad_ * 0.26f);
            mixedLeft += engineSample * engineSpatialMix.leftGain;
            mixedRight += engineSample * engineSpatialMix.rightGain;
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
            const float voiceSample = (tone + noise) * voiceIter->amplitude * envelope;
            mixedLeft += voiceSample * voiceIter->leftGain;
            mixedRight += voiceSample * voiceIter->rightGain;

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

        samples[frameIndex * 2 + 0] = Clamp(mixedLeft, -0.95f, 0.95f);
        samples[frameIndex * 2 + 1] = Clamp(mixedRight, -0.95f, 0.95f);
    }
}

auto AudioDevice::NextNoise() -> float
{
    noiseState_ = noiseState_ * 1664525u + 1013904223u;
    return (static_cast<float>((noiseState_ >> 8u) & 0x00ffffffu) / static_cast<float>(0x00800000u)) - 1.0f;
}
}
