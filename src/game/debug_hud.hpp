#pragma once

#include "core/math.hpp"
#include "render/frame_data.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace df::game
{
void AppendRect(std::vector<render::ColorVertex2D>& triangles, float x, float y, float width, float height, Vec4 color, float screenWidth, float screenHeight);
void AppendText(std::vector<render::ColorVertex2D>& triangles, float x, float y, float scale, std::string_view text, Vec4 color, float screenWidth, float screenHeight);
void AppendCrosshair(std::vector<render::ColorVertex2D>& triangles, float screenWidth, float screenHeight, Vec4 color);
[[nodiscard]] std::string ToUpperAscii(std::string_view text);
}
