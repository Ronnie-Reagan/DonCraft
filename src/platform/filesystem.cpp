#include "platform/filesystem.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <string>

namespace df::platform
{
std::filesystem::path GetUserDataPath(const std::string_view organization, const std::string_view application)
{
    const std::string organizationString(organization);
    const std::string applicationString(application);

    if (char* path = SDL_GetPrefPath(organizationString.c_str(), applicationString.c_str()))
    {
        std::filesystem::path result(path);
        SDL_free(path);
        std::filesystem::create_directories(result);
        return result;
    }

    const std::filesystem::path fallback = std::filesystem::current_path() / "user";
    std::filesystem::create_directories(fallback);
    return fallback;
}
}
