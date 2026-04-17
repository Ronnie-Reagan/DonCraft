#pragma once

#include <filesystem>
#include <string_view>

namespace df::platform
{
[[nodiscard]] std::filesystem::path GetUserDataPath(std::string_view organization, std::string_view application);
}
