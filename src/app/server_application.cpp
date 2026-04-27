#include "app/server_application.hpp"

#include "core/config.hpp"
#include "core/fixed_step_clock.hpp"
#include "core/log.hpp"
#include "net/session_host.hpp"
#include "steam/steam_server.hpp"
#include "steam/steam_transport.hpp"

#include <steam/isteamnetworkingsockets.h>
#include <steam/steam_gameserver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

namespace df
{
namespace
{
std::atomic_bool gKeepRunning{true};

void HandleSignal(int)
{
    gKeepRunning.store(false);
}

template <typename T, typename ParseFn>
bool ParseValue(std::string_view text, const char* name, ParseFn&& parseFn, T& outValue)
{
    try
    {
        outValue = parseFn(std::string(text));
        return true;
    }
    catch (const std::exception&)
    {
        LogError("Invalid value for ", name, ": ", text);
        return false;
    }
}
}

int ServerApplication::Run(const int argc, char** argv)
{
    Options options{};
    if (!ParseCommandLine(argc, argv, options))
    {
        return 1;
    }

    return RunWithOptions(options);
}

bool ServerApplication::ParseCommandLine(const int argc, char** argv, Options& options) const
{
    const auto requireValue = [&](int& index, const char* optionName) -> const char*
    {
        if (index + 1 >= argc)
        {
            LogError("Missing value for ", optionName);
            return nullptr;
        }

        ++index;
        return argv[index];
    };

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h")
        {
            PrintUsage();
            return false;
        }
        if (argument == "--name")
        {
            if (const char* value = requireValue(index, "--name"))
            {
                options.serverName = value;
                continue;
            }
            return false;
        }
        if (argument == "--save")
        {
            if (const char* value = requireValue(index, "--save"))
            {
                options.savePath = std::filesystem::path(value);
                continue;
            }
            return false;
        }
        if (argument == "--max-players")
        {
            if (const char* value = requireValue(index, "--max-players"))
            {
                if (!ParseValue<int>(value, "--max-players", [](const std::string& text) { return std::stoi(text); }, options.maxPlayers))
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--autosave")
        {
            if (const char* value = requireValue(index, "--autosave"))
            {
                if (!ParseValue<double>(value, "--autosave", [](const std::string& text) { return std::stod(text); }, options.autosaveIntervalSeconds))
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--game-port")
        {
            if (const char* value = requireValue(index, "--game-port"))
            {
                int parsed = 0;
                if (!ParseValue<int>(value, "--game-port", [](const std::string& text) { return std::stoi(text); }, parsed))
                {
                    return false;
                }
                options.gamePort = static_cast<std::uint16_t>(std::clamp(parsed, 1, 65535));
                continue;
            }
            return false;
        }
        if (argument == "--query-port")
        {
            if (const char* value = requireValue(index, "--query-port"))
            {
                int parsed = 0;
                if (!ParseValue<int>(value, "--query-port", [](const std::string& text) { return std::stoi(text); }, parsed))
                {
                    return false;
                }
                options.queryPort = static_cast<std::uint16_t>(std::clamp(parsed, 1, 65535));
                continue;
            }
            return false;
        }
        if (argument == "--tick-rate")
        {
            if (const char* value = requireValue(index, "--tick-rate"))
            {
                if (!ParseValue<int>(value, "--tick-rate", [](const std::string& text) { return std::stoi(text); }, options.tickRate))
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--visibility")
        {
            if (const char* value = requireValue(index, "--visibility"))
            {
                const std::string visibility = value;
                if (visibility == "public")
                {
                    options.advertisePublic = true;
                }
                else if (visibility == "private")
                {
                    options.advertisePublic = false;
                }
                else
                {
                    LogError("Unsupported visibility: ", visibility, ". Expected 'public' or 'private'.");
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--region")
        {
            if (const char* value = requireValue(index, "--region"))
            {
                options.region = value;
                continue;
            }
            return false;
        }
        if (argument == "--tags")
        {
            if (const char* value = requireValue(index, "--tags"))
            {
                options.gameTags = value;
                continue;
            }
            return false;
        }
        if (argument == "--map-name")
        {
            if (const char* value = requireValue(index, "--map-name"))
            {
                options.mapName = value;
                continue;
            }
            return false;
        }
        if (argument == "--world-width")
        {
            if (const char* value = requireValue(index, "--world-width"))
            {
                if (!ParseValue<int>(value, "--world-width", [](const std::string& text) { return std::stoi(text); }, options.worldSettings.worldWidth))
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--world-height")
        {
            if (const char* value = requireValue(index, "--world-height"))
            {
                if (!ParseValue<int>(value, "--world-height", [](const std::string& text) { return std::stoi(text); }, options.worldSettings.worldHeight))
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--world-depth")
        {
            if (const char* value = requireValue(index, "--world-depth"))
            {
                if (!ParseValue<int>(value, "--world-depth", [](const std::string& text) { return std::stoi(text); }, options.worldSettings.worldDepth))
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--active-chunk-size")
        {
            if (const char* value = requireValue(index, "--active-chunk-size"))
            {
                if (!ParseValue<int>(value, "--active-chunk-size", [](const std::string& text) { return std::stoi(text); }, options.worldSettings.activeChunkSize))
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--cell-size")
        {
            if (const char* value = requireValue(index, "--cell-size"))
            {
                float ignoredCellSize = 1.0f;
                if (!ParseValue<float>(value, "--cell-size", [](const std::string& text) { return std::stof(text); }, ignoredCellSize))
                {
                    return false;
                }
                LogWarning("--cell-size is ignored; DonCraft worlds now use fixed 1.0 meter cells.");
                continue;
            }
            return false;
        }
        if (argument == "--seed")
        {
            if (const char* value = requireValue(index, "--seed"))
            {
                unsigned long parsed = 0;
                if (!ParseValue<unsigned long>(value, "--seed", [](const std::string& text) { return std::stoul(text); }, parsed))
                {
                    return false;
                }
                options.worldSettings.seed = static_cast<std::uint32_t>(parsed);
                continue;
            }
            return false;
        }
        if (argument == "--terrain-relief")
        {
            if (const char* value = requireValue(index, "--terrain-relief"))
            {
                if (!ParseValue<float>(value, "--terrain-relief", [](const std::string& text) { return std::stof(text); }, options.worldSettings.terrainRelief))
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--water-level")
        {
            if (const char* value = requireValue(index, "--water-level"))
            {
                if (!ParseValue<float>(value, "--water-level", [](const std::string& text) { return std::stof(text); }, options.worldSettings.waterLevel))
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (argument == "--new-world")
        {
            options.loadExistingWorld = false;
            continue;
        }

        LogError("Unknown argument: ", argument);
        PrintUsage();
        return false;
    }

    options.maxPlayers = std::clamp(options.maxPlayers, 1, 64);
    options.tickRate = std::clamp(options.tickRate, 20, 240);
    options.autosaveIntervalSeconds = std::max(options.autosaveIntervalSeconds, 5.0);
    options.worldSettings = world::DemoWorld::ClampGenerationSettings(options.worldSettings);
    return true;
}

void ServerApplication::PrintUsage()
{
    LogInfo("Usage: Don_Craft_server [options]");
    LogInfo("  --name <server name>");
    LogInfo("  --save <world save path>");
    LogInfo("  --max-players <count>");
    LogInfo("  --autosave <seconds>");
    LogInfo("  --game-port <port>");
    LogInfo("  --query-port <port>");
    LogInfo("  --tick-rate <hz>");
    LogInfo("  --visibility <public|private>");
    LogInfo("  --world-width <cells>");
    LogInfo("  --world-height <cells>");
    LogInfo("  --world-depth <cells>");
    LogInfo("  --active-chunk-size <cells>");
    LogInfo("  --seed <uint32>");
    LogInfo("  --terrain-relief <scalar>");
    LogInfo("  --water-level <0..1>");
    LogInfo("  --region <steam region>");
    LogInfo("  --tags <comma separated tags>");
    LogInfo("  --map-name <server browser map name>");
    LogInfo("  --new-world");
    LogInfo("  --help");
}

int ServerApplication::RunWithOptions(const Options& options)
{
    if (!options.savePath.parent_path().empty())
    {
        std::error_code createError;
        std::filesystem::create_directories(options.savePath.parent_path(), createError);
        if (createError)
        {
            LogError("Dedicated server could not prepare the save directory '", options.savePath.parent_path().string(), "': ", createError.message());
            return 1;
        }
    }

    steam::SteamServerContext steamServer{};
    steam::SteamServerContext::Config steamConfig{};
    steamConfig.serverName = options.serverName;
    steamConfig.gamePort = options.gamePort;
    steamConfig.queryPort = options.queryPort;
    steamConfig.maxPlayers = options.maxPlayers;
    steamConfig.region = options.region;
    steamConfig.gameTags = options.gameTags;
    steamConfig.mapName = options.mapName;
    steamConfig.advertisePublic = options.advertisePublic;
    if (!steamServer.Initialize(steamConfig))
    {
        LogError("Dedicated Steam server initialization failed: ", steamServer.FailureMessage());
        return 1;
    }

    auto transport = std::make_unique<steam::SteamSocketsTransport>(SteamGameServerNetworkingSockets(), true);
    if (!transport->StartListenIp(options.gamePort))
    {
        LogError("Dedicated server failed to create a SteamNetworkingSockets listen socket on port ", options.gamePort);
        steamServer.Shutdown();
        return 1;
    }

    net::SessionHost sessionHost{};
    net::SessionHost::Config hostConfig{};
    hostConfig.runtime.mode = game::SessionMode::DedicatedServer;
    hostConfig.runtime.sessionName = options.serverName;
    hostConfig.runtime.savePath = options.savePath;
    hostConfig.runtime.generationSettings = options.worldSettings;
    hostConfig.runtime.loadExistingWorld = options.loadExistingWorld;
    hostConfig.runtime.autosaveEnabled = true;
    hostConfig.runtime.autosaveIntervalSeconds = options.autosaveIntervalSeconds;
    hostConfig.runtime.maxPlayers = options.maxPlayers;
    try
    {
        sessionHost.Initialize(hostConfig, std::move(transport));
    }
    catch (const std::exception& error)
    {
        LogError("Dedicated world startup failed: ", error.what());
        steamServer.Shutdown();
        return 1;
    }

    LogInfo(
        "Dedicated world started. name='", options.serverName,
        "' save='", options.savePath.string(),
        "' visibility=", options.advertisePublic ? "public" : "private",
        " game_port=", options.gamePort,
        " query_port=", options.queryPort,
        " max_players=", options.maxPlayers);

    gKeepRunning.store(true);
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    using Clock = std::chrono::steady_clock;
    const double tickSeconds = 1.0 / static_cast<double>(options.tickRate);
    FixedStepClock fixedStepClock(tickSeconds, 0.25, std::max(4, options.tickRate / 10));
    auto previousFrameTime = Clock::now();
    auto nextStatusLogTime = previousFrameTime;

    while (gKeepRunning.load())
    {
        steamServer.PumpCallbacks();

        const auto now = Clock::now();
        const double frameDeltaSeconds = std::min(std::chrono::duration<double>(now - previousFrameTime).count(), 0.25);
        previousFrameTime = now;

        fixedStepClock.Consume(frameDeltaSeconds, [&](const double dt)
        {
            sessionHost.Tick(static_cast<float>(dt));
        });

        if (now >= nextStatusLogTime)
        {
            const auto peers = sessionHost.PeerInfos();
            LogInfo(
                "Dedicated status: logged_on=", steamServer.IsLoggedOn() ? "yes" : "no",
                " tick=", sessionHost.Runtime().TickIndex(),
                " players=", sessionHost.Runtime().Players().size(),
                " peers=", peers.size());
            nextStatusLogTime = now + std::chrono::seconds(5);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    LogInfo("Dedicated shutdown requested. Saving world and stopping Steam server.");
    sessionHost.Shutdown();
    steamServer.Shutdown();
    return 0;
}
}
