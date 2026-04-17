#pragma once

#include <cstdint>
#include <string>

namespace df::steam
{
class SteamServerContext
{
public:
    struct Config
    {
        std::string version = "1";
        std::string product = "DonCraft";
        std::string gameDescription = "DonCraft Dedicated World";
        std::string modDir = "doncraft";
        std::string serverName = "DonCraft Dedicated";
        std::string mapName = "frontier";
        std::string region = "world";
        std::string gameTags = "doncraft,dedicated";
        std::string gameData;
        std::uint16_t gamePort = 27035;
        std::uint16_t queryPort = 27036;
        int maxPlayers = 8;
        bool advertisePublic = true;
    };

    SteamServerContext() = default;
    ~SteamServerContext();

    SteamServerContext(const SteamServerContext&) = delete;
    auto operator=(const SteamServerContext&) -> SteamServerContext& = delete;

    [[nodiscard]] bool Initialize(const Config& config);
    void Shutdown();
    void PumpCallbacks();

    [[nodiscard]] bool IsInitialized() const
    {
        return initialized_;
    }

    [[nodiscard]] bool IsLoggedOn() const;
    [[nodiscard]] std::uint64_t SteamId() const;
    [[nodiscard]] const std::string& FailureMessage() const
    {
        return failureMessage_;
    }
    [[nodiscard]] auto CurrentConfig() const -> const Config&
    {
        return config_;
    }

private:
    bool initialized_ = false;
    Config config_{};
    std::string failureMessage_;
};
}
