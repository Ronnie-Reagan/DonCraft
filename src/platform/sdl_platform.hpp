#pragma once

#include "platform/input_state.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <utility>

namespace df::platform
{
struct WindowConfig
{
    std::string title;
    int width = 1280;
    int height = 720;
};

struct FrameEvents
{
    bool quitRequested = false;
    bool framebufferResized = false;
    bool focused = true;
};

class SdlPlatform
{
public:
    static std::unique_ptr<SdlPlatform> Create(const WindowConfig& config);
    ~SdlPlatform();

    SdlPlatform(const SdlPlatform&) = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;

    [[nodiscard]] FrameEvents PumpEvents();
    void SetRelativeMouseMode(bool enabled);
    [[nodiscard]] const InputState& Input() const
    {
        return input_;
    }

    [[nodiscard]] SDL_Window* Window() const
    {
        return window_;
    }

    [[nodiscard]] std::pair<int, int> DrawableSize() const;
    [[nodiscard]] bool IsMinimized() const
    {
        return minimized_;
    }

private:
    explicit SdlPlatform(SDL_Window* window);

    SDL_Window* window_ = nullptr;
    bool minimized_ = false;
    bool focused_ = true;
    InputState input_{};
};
}
