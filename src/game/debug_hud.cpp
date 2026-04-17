#include "game/debug_hud.hpp"

#include <array>
#include <cctype>
#include <cstdint>

namespace df::game
{
namespace
{
auto ToClipX(const float x, const float screenWidth) -> float
{
    return (x / screenWidth) * 2.0f - 1.0f;
}

auto ToClipY(const float y, const float screenHeight) -> float
{
    return (y / screenHeight) * 2.0f - 1.0f;
}

auto GlyphRows(const char character) -> std::array<std::uint8_t, 7>
{
    switch (character)
    {
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    case 'D': return {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G': return {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F};
    case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    case 'J': return {0x1F, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11};
    case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1': return {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F};
    case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3': return {0x1E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x1E};
    case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
    case ':': return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
    case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06};
    case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    case '/': return {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
    case ' ': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    default:  return {0x1F, 0x01, 0x02, 0x04, 0x04, 0x00, 0x04};
    }
}
}

void AppendRect(
    std::vector<render::ColorVertex2D>& triangles,
    const float x,
    const float y,
    const float width,
    const float height,
    const Vec4 color,
    const float screenWidth,
    const float screenHeight)
{
    const Vec2 a{ToClipX(x, screenWidth), ToClipY(y, screenHeight)};
    const Vec2 b{ToClipX(x + width, screenWidth), ToClipY(y, screenHeight)};
    const Vec2 c{ToClipX(x + width, screenWidth), ToClipY(y + height, screenHeight)};
    const Vec2 d{ToClipX(x, screenWidth), ToClipY(y + height, screenHeight)};

    triangles.push_back({a, color});
    triangles.push_back({b, color});
    triangles.push_back({c, color});
    triangles.push_back({a, color});
    triangles.push_back({c, color});
    triangles.push_back({d, color});
}

void AppendText(
    std::vector<render::ColorVertex2D>& triangles,
    const float x,
    const float y,
    const float scale,
    const std::string_view text,
    const Vec4 color,
    const float screenWidth,
    const float screenHeight)
{
    float penX = x;
    for (const char rawCharacter : text)
    {
        const char character = static_cast<char>(std::toupper(static_cast<unsigned char>(rawCharacter)));
        const auto rows = GlyphRows(character);

        for (int row = 0; row < 7; ++row)
        {
            for (int column = 0; column < 5; ++column)
            {
                if ((rows[static_cast<std::size_t>(row)] & (1u << (4 - column))) == 0)
                {
                    continue;
                }

                AppendRect(
                    triangles,
                    penX + static_cast<float>(column) * scale,
                    y + static_cast<float>(row) * scale,
                    scale,
                    scale,
                    color,
                    screenWidth,
                    screenHeight);
            }
        }

        penX += scale * 6.0f;
    }
}

void AppendCrosshair(std::vector<render::ColorVertex2D>& triangles, const float screenWidth, const float screenHeight, const Vec4 color)
{
    const float centerX = screenWidth * 0.5f;
    const float centerY = screenHeight * 0.5f;

    AppendRect(triangles, centerX - 9.0f, centerY - 1.0f, 7.0f, 2.0f, color, screenWidth, screenHeight);
    AppendRect(triangles, centerX + 2.0f, centerY - 1.0f, 7.0f, 2.0f, color, screenWidth, screenHeight);
    AppendRect(triangles, centerX - 1.0f, centerY - 9.0f, 2.0f, 7.0f, color, screenWidth, screenHeight);
    AppendRect(triangles, centerX - 1.0f, centerY + 2.0f, 2.0f, 7.0f, color, screenWidth, screenHeight);
}

auto ToUpperAscii(const std::string_view text) -> std::string
{
    std::string result;
    result.reserve(text.size());

    for (const unsigned char character : text)
    {
        result.push_back(static_cast<char>(std::toupper(character)));
    }

    return result;
}
}
