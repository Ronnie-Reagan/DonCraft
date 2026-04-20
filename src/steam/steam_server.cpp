#include "steam/steam_server.hpp"

#include "core/log.hpp"

#include <steam/isteamgameserver.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steam_gameserver.h>

namespace df::steam
{
namespace
{
void SteamNetworkingDebugOutput(const ESteamNetworkingSocketsDebugOutputType type, const char* const message)
{
    const std::string_view text = message != nullptr ? std::string_view(message) : std::string_view{};
    if (type <= k_ESteamNetworkingSocketsDebugOutputType_Error)
    {
        LogError("SteamNetworking: ", text);
    }
    else if (type == k_ESteamNetworkingSocketsDebugOutputType_Warning)
    {
        LogWarning("SteamNetworking: ", text);
    }
    else
    {
        LogInfo("SteamNetworking: ", text);
    }
}
}

SteamServerContext::~SteamServerContext()
{
    Shutdown();
}

bool SteamServerContext::Initialize(const Config& config)
{
    if (initialized_)
    {
        return true;
    }

    config_ = config;
    SteamErrMsg error{};
    const ESteamAPIInitResult result = SteamGameServer_InitEx(
        0u,
        config_.gamePort,
        config_.queryPort,
        eServerModeAuthentication,
        config_.version.c_str(),
        &error);
    if (result != k_ESteamAPIInitResult_OK)
    {
        failureMessage_ = error[0] != '\0' ? error : "SteamGameServer_InitEx failed without a descriptive error.";
        return false;
    }

    if (SteamGameServer() == nullptr)
    {
        failureMessage_ = "SteamGameServer interface was unavailable after initialization.";
        SteamGameServer_Shutdown();
        return false;
    }

    if (SteamNetworkingUtils() != nullptr)
    {
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, SteamNetworkingDebugOutput);
    }

    SteamGameServer()->SetProduct(config_.product.c_str());
    SteamGameServer()->SetGameDescription(config_.gameDescription.c_str());
    SteamGameServer()->SetModDir(config_.modDir.c_str());
    SteamGameServer()->SetDedicatedServer(true);
    SteamGameServer()->SetServerName(config_.serverName.c_str());
    SteamGameServer()->SetMapName(config_.mapName.c_str());
    SteamGameServer()->SetPasswordProtected(false);
    SteamGameServer()->SetMaxPlayerCount(config_.maxPlayers);
    SteamGameServer()->SetBotPlayerCount(0);
    SteamGameServer()->SetRegion(config_.region.c_str());
    SteamGameServer()->SetGameTags(config_.gameTags.c_str());
    SteamGameServer()->SetGameData(config_.gameData.c_str());
    SteamGameServer()->LogOnAnonymous();
    SteamGameServer()->SetAdvertiseServerActive(config_.advertisePublic);

    initialized_ = true;
    failureMessage_.clear();
    LogInfo("Steam dedicated server initialized on game_port=", config_.gamePort, " query_port=", config_.queryPort);
    return true;
}

void SteamServerContext::Shutdown()
{
    if (!initialized_)
    {
        return;
    }

    if (SteamGameServer() != nullptr)
    {
        SteamGameServer()->SetAdvertiseServerActive(false);
    }

    SteamGameServer_Shutdown();
    initialized_ = false;
}

void SteamServerContext::PumpCallbacks()
{
    if (initialized_)
    {
        SteamGameServer_RunCallbacks();
    }
}

bool SteamServerContext::IsLoggedOn() const
{
    return initialized_ && SteamGameServer() != nullptr && SteamGameServer()->BLoggedOn();
}

std::uint64_t SteamServerContext::SteamId() const
{
    return initialized_ && SteamGameServer() != nullptr ? SteamGameServer()->GetSteamID().ConvertToUint64() : 0u;
}
}
