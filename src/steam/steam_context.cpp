#include "steam/steam_context.hpp"

#include "core/log.hpp"

#include <steam/isteamfriends.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/isteamuser.h>
#include <steam/steam_api.h>

namespace df::steam
{
SteamContext::~SteamContext()
{
    Shutdown();
}

bool SteamContext::Initialize()
{
    if (initialized_)
    {
        return true;
    }

    SteamErrMsg error{};
    const ESteamAPIInitResult initResult = SteamAPI_InitEx(&error);
    if (initResult != k_ESteamAPIInitResult_OK)
    {
        failureMessage_ = error[0] != '\0' ? error : "SteamAPI_InitEx failed without a descriptive error.";
        return false;
    }

    if (SteamNetworkingUtils() != nullptr)
    {
        SteamNetworkingUtils()->InitRelayNetworkAccess();
    }

    initialized_ = true;
    failureMessage_.clear();
    LogInfo("Steam initialized as ", PersonaName(), " (", LocalSteamId(), ")");
    return true;
}

void SteamContext::Shutdown()
{
    if (!initialized_)
    {
        return;
    }

    SteamAPI_Shutdown();
    initialized_ = false;
}

void SteamContext::PumpCallbacks()
{
    if (initialized_)
    {
        SteamAPI_RunCallbacks();
    }
}

std::uint64_t SteamContext::LocalSteamId() const
{
    if (!initialized_ || SteamUser() == nullptr)
    {
        return 0;
    }

    return SteamUser()->GetSteamID().ConvertToUint64();
}

std::string SteamContext::PersonaName() const
{
    if (!initialized_ || SteamFriends() == nullptr)
    {
        return {};
    }

    return SteamFriends()->GetPersonaName();
}
}
