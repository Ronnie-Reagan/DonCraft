#include "platform/sdl_platform.hpp"

#include "core/config.hpp"
#include "core/log.hpp"

#include <stdexcept>

namespace df::platform
{
std::unique_ptr<SdlPlatform> SdlPlatform::Create(const WindowConfig& config)
{
    SDL_SetAppMetadata(config::kApplicationName, "0.1.0", "com.doncraft.client");
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "1");

    const SDL_InitFlags initFlags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
    if (!SDL_Init(initFlags))
    {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    constexpr SDL_WindowFlags windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window = SDL_CreateWindow(config.title.c_str(), config.width, config.height, windowFlags);
    if (window == nullptr)
    {
        const std::string error = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("SDL_CreateWindow failed: " + error);
    }

    return std::unique_ptr<SdlPlatform>(new SdlPlatform(window));
}

SdlPlatform::SdlPlatform(SDL_Window* window)
    : window_(window)
{
}

SdlPlatform::~SdlPlatform()
{
    if (window_ != nullptr)
    {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_Quit();
}

FrameEvents SdlPlatform::PumpEvents()
{
    FrameEvents frameEvents{};
    frameEvents.focused = focused_;
    input_.previousKeys = input_.keys;
    input_.previousMouseButtons = input_.mouseButtons;
    input_.mouseWheelX = 0.0f;
    input_.mouseWheelY = 0.0f;
    input_.mouseDeltaX = 0.0f;
    input_.mouseDeltaY = 0.0f;

    SDL_Event event{};
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_EVENT_QUIT:
            frameEvents.quitRequested = true;
            break;

        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            frameEvents.framebufferResized = true;
            minimized_ = false;
            break;

        case SDL_EVENT_WINDOW_MINIMIZED:
            minimized_ = true;
            break;

        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            minimized_ = false;
            frameEvents.framebufferResized = true;
            break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            focused_ = true;
            frameEvents.focused = true;
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            focused_ = false;
            frameEvents.focused = false;
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            input_.mouseWheelX += event.wheel.x;
            input_.mouseWheelY += event.wheel.y;
            break;

        default:
            break;
        }
    }

    int keyCount = 0;
    const bool* keyboardState = SDL_GetKeyboardState(&keyCount);
    input_.keys.fill(false);
    for (int keyIndex = 0; keyIndex < keyCount && keyIndex < static_cast<int>(input_.keys.size()); ++keyIndex)
    {
        input_.keys[static_cast<std::size_t>(keyIndex)] = keyboardState[keyIndex];
    }

    input_.mouseButtons = SDL_GetMouseState(&input_.mouseX, &input_.mouseY);
    SDL_GetRelativeMouseState(&input_.mouseDeltaX, &input_.mouseDeltaY);

    return frameEvents;
}

void SdlPlatform::SetRelativeMouseMode(const bool enabled)
{
    if (!SDL_SetWindowRelativeMouseMode(window_, enabled))
    {
        LogWarning("SDL_SetWindowRelativeMouseMode failed: ", SDL_GetError());
    }
}

std::pair<int, int> SdlPlatform::DrawableSize() const
{
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &width, &height))
    {
        return {0, 0};
    }

    return {width, height};
}
}
