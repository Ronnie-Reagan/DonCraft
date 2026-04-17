#pragma once

#include <SDL3/SDL.h>

#include <array>

namespace df::platform
{
struct InputState
{
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    std::array<bool, SDL_SCANCODE_COUNT> previousKeys{};
    SDL_MouseButtonFlags mouseButtons = 0;
    SDL_MouseButtonFlags previousMouseButtons = 0;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    float mouseWheelX = 0.0f;
    float mouseWheelY = 0.0f;

    [[nodiscard]] auto KeyDown(const SDL_Scancode key) const -> bool
    {
        return keys[static_cast<std::size_t>(key)];
    }

    [[nodiscard]] auto KeyPressed(const SDL_Scancode key) const -> bool
    {
        const std::size_t index = static_cast<std::size_t>(key);
        return keys[index] && !previousKeys[index];
    }

    [[nodiscard]] auto KeyReleased(const SDL_Scancode key) const -> bool
    {
        const std::size_t index = static_cast<std::size_t>(key);
        return !keys[index] && previousKeys[index];
    }

    [[nodiscard]] auto MouseDown(const std::uint32_t button) const -> bool
    {
        return (mouseButtons & SDL_BUTTON_MASK(button)) != 0;
    }

    [[nodiscard]] auto MousePressed(const std::uint32_t button) const -> bool
    {
        const SDL_MouseButtonFlags mask = SDL_BUTTON_MASK(button);
        return (mouseButtons & mask) != 0 && (previousMouseButtons & mask) == 0;
    }

    [[nodiscard]] auto MouseReleased(const std::uint32_t button) const -> bool
    {
        const SDL_MouseButtonFlags mask = SDL_BUTTON_MASK(button);
        return (mouseButtons & mask) == 0 && (previousMouseButtons & mask) != 0;
    }

    void ClearFrameEdges()
    {
        previousKeys = keys;
        previousMouseButtons = mouseButtons;
        mouseDeltaX = 0.0f;
        mouseDeltaY = 0.0f;
        mouseWheelX = 0.0f;
        mouseWheelY = 0.0f;
    }
};
}
