#pragma once

#include "world/demo_world.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace df
{
class ServerApplication
{
public:
    int Run(int argc, char** argv);

private:
    struct Options
    {
        std::string serverName = "DonCraft Dedicated";
        std::filesystem::path savePath = std::filesystem::current_path() / "server_world.bin";
        world::WorldGenerationSettings worldSettings{};
        int maxPlayers = 8;
        double autosaveIntervalSeconds = 20.0;
        std::uint16_t gamePort = 27035;
        std::uint16_t queryPort = 27036;
        int tickRate = 60;
        bool advertisePublic = true;
        bool loadExistingWorld = true;
        std::string region = "world";
        std::string gameTags = "doncraft,dedicated";
        std::string mapName = "frontier";
    };

    [[nodiscard]] bool ParseCommandLine(int argc, char** argv, Options& options) const;
    static void PrintUsage();
    int RunWithOptions(const Options& options);
};
}
