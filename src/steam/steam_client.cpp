#include "steam/steam_client.hpp"

#include "core/log.hpp"

#include <steam/isteamfriends.h>
#include <steam/isteammatchmaking.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/isteamuser.h>
#include <steam/isteamutils.h>
#include <steam/matchmakingtypes.h>

#include <algorithm>
#include <charconv>
#include <memory>

namespace df::steam
{
namespace
{
constexpr char kGameDirectory[] = "doncraft";
constexpr char kSessionVersion[] = "1";

[[nodiscard]] auto SafeSteamString(const char* value) -> std::string
{
    return value != nullptr ? std::string(value) : std::string();
}

[[nodiscard]] auto SanitizeBrowserText(const char* value, const std::size_t maxLength) -> std::string
{
    std::string text = SafeSteamString(value);
    text.erase(std::remove_if(text.begin(), text.end(), [](const unsigned char character)
    {
        return character < 32u;
    }), text.end());
    if (text.size() > maxLength)
    {
        text.resize(maxLength);
    }
    return text;
}

[[nodiscard]] auto LobbyDataString(const CSteamID lobbyId, const char* key) -> std::string
{
    if (SteamMatchmaking() == nullptr || !lobbyId.IsValid())
    {
        return {};
    }

    return SafeSteamString(SteamMatchmaking()->GetLobbyData(lobbyId, key));
}

[[nodiscard]] auto ParseSteamIdString(const std::string_view text) -> std::uint64_t
{
    std::uint64_t value = 0u;
    const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || ptr != text.data() + text.size())
    {
        return 0u;
    }
    return value;
}

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

class SteamClientContext::ServerBrowserResponse final : public ISteamMatchmakingServerListResponse
{
public:
    explicit ServerBrowserResponse(SteamClientContext& owner)
        : owner_(owner)
    {
    }

    ~ServerBrowserResponse()
    {
        Cancel();
    }

    void Refresh(const AppId_t appId)
    {
        Cancel();

        if (SteamMatchmakingServers() == nullptr || appId == 480u)
        {
            owner_.browserServerRefreshPending_ = false;
            return;
        }

        filters_.clear();
        filterPtrs_.clear();
        filters_.emplace_back("gamedir", kGameDirectory);
        filterPtrs_.reserve(filters_.size());
        for (auto& filter : filters_)
        {
            filterPtrs_.push_back(&filter);
        }

        request_ = SteamMatchmakingServers()->RequestInternetServerList(appId, filterPtrs_.data(), static_cast<std::uint32_t>(filterPtrs_.size()), this);
        requestCompleted_ = false;
        owner_.browserServerRefreshPending_ = request_ != nullptr;
        if (request_ == nullptr)
        {
            owner_.browserServerRefreshPending_ = false;
        }
    }

    void Cancel()
    {
        if (request_ != nullptr && SteamMatchmakingServers() != nullptr)
        {
            if (!requestCompleted_)
            {
                SteamMatchmakingServers()->CancelQuery(request_);
            }
            SteamMatchmakingServers()->ReleaseRequest(request_);
            request_ = nullptr;
        }
        requestCompleted_ = false;
    }

    void ServerResponded(HServerListRequest request, int serverIndex) override
    {
        if (request == nullptr || request != request_ || SteamMatchmakingServers() == nullptr)
        {
            return;
        }

        gameserveritem_t* const details = SteamMatchmakingServers()->GetServerDetails(request, serverIndex);
        if (details == nullptr)
        {
            return;
        }

        net::SessionBrowserEntry entry{};
        entry.type = net::BrowserEntryType::DedicatedServer;
        entry.name = SanitizeBrowserText(details->GetName(), 48u);
        entry.summary = SanitizeBrowserText(details->m_szGameDescription, 64u);
        entry.address = details->m_NetAdr.GetConnectionAddressString();
        entry.port = details->m_NetAdr.GetConnectionPort();
        entry.currentPlayers = details->m_nPlayers;
        entry.maxPlayers = details->m_nMaxPlayers;
        entry.joinable = details->m_bHadSuccessfulResponse && details->m_nPlayers < details->m_nMaxPlayers;
        entry.dedicated = true;
        entry.ownerSteamId = details->m_steamID.ConvertToUint64();
        if (entry.joinable)
        {
            owner_.browserEntries_.push_back(std::move(entry));
        }
    }

    void ServerFailedToRespond(HServerListRequest, int) override
    {
        LogWarning("Steam dedicated browser server failed to respond.");
    }

    void RefreshComplete(HServerListRequest request, EMatchMakingServerResponse) override
    {
        if (request == request_)
        {
            requestCompleted_ = true;
            owner_.browserServerRefreshPending_ = false;
            LogInfo("Steam dedicated browser refresh completed.");
            owner_.PushBrowserUpdated();
        }
    }

private:
    SteamClientContext& owner_;
    HServerListRequest request_ = nullptr;
    bool requestCompleted_ = false;
    std::vector<MatchMakingKeyValuePair_t> filters_;
    std::vector<MatchMakingKeyValuePair_t*> filterPtrs_;
};

SteamClientContext::SteamClientContext() = default;

SteamClientContext::~SteamClientContext()
{
    Shutdown();
}

bool SteamClientContext::Initialize()
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
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, SteamNetworkingDebugOutput);
    }

    serverBrowserResponse_ = new ServerBrowserResponse(*this);
    lobbyJoinRequestedCallback_.Register(this, &SteamClientContext::OnLobbyJoinRequested);
    initialized_ = true;
    failureMessage_.clear();
    LogInfo("Steam client initialized as ", PersonaName(), " (", LocalSteamId(), ")");
    return true;
}

void SteamClientContext::Shutdown()
{
    if (!initialized_)
    {
        return;
    }

    LeaveLobby();
    lobbyJoinRequestedCallback_.Unregister();
    delete serverBrowserResponse_;
    serverBrowserResponse_ = nullptr;
    SteamAPI_Shutdown();
    initialized_ = false;
}

void SteamClientContext::PumpCallbacks()
{
    if (initialized_)
    {
        SteamAPI_RunCallbacks();
    }
}

std::uint64_t SteamClientContext::LocalSteamId() const
{
    if (!initialized_ || SteamUser() == nullptr)
    {
        return 0;
    }

    return SteamUser()->GetSteamID().ConvertToUint64();
}

std::string SteamClientContext::PersonaName() const
{
    if (!initialized_ || SteamFriends() == nullptr)
    {
        return {};
    }

    return SteamFriends()->GetPersonaName();
}

void SteamClientContext::CreateLobby(const std::string& sessionName, const std::string& summary, const int maxPlayers)
{
    if (!initialized_ || SteamMatchmaking() == nullptr)
    {
        PushError("Steam lobby creation was requested before Steam client initialization.");
        return;
    }

    pendingLobbyName_ = sessionName;
    pendingLobbySummary_ = summary;
    pendingLobbyMaxPlayers_ = std::max(2, maxPlayers);
    LogInfo(
        "Steam client creating lobby name='", pendingLobbyName_,
        "' summary='", pendingLobbySummary_,
        "' max_players=", pendingLobbyMaxPlayers_);
    const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, pendingLobbyMaxPlayers_);
    lobbyCreatedCallResult_.Set(call, this, &SteamClientContext::OnLobbyCreated);
}

void SteamClientContext::JoinLobby(const std::uint64_t lobbyId)
{
    if (!initialized_ || SteamMatchmaking() == nullptr)
    {
        PushError("Steam lobby join was requested before Steam client initialization.");
        return;
    }

    LogInfo("Steam client joining lobby_id=", lobbyId);
    const SteamAPICall_t call = SteamMatchmaking()->JoinLobby(CSteamID(lobbyId));
    lobbyEnterCallResult_.Set(call, this, &SteamClientContext::OnLobbyEntered);
}

void SteamClientContext::LeaveLobby()
{
    if (!initialized_ || SteamMatchmaking() == nullptr || !currentLobby_.IsValid())
    {
        return;
    }

    LogInfo("Steam client leaving lobby_id=", currentLobby_.ConvertToUint64());
    SteamMatchmaking()->LeaveLobby(currentLobby_);
    events_.push_back({Event::Type::LobbyLeft, currentLobby_.ConvertToUint64(), {}});
    currentLobby_.Clear();
}

void SteamClientContext::RefreshBrowser()
{
    if (!initialized_ || SteamMatchmaking() == nullptr)
    {
        PushError("Steam browser refresh was requested before Steam client initialization.");
        return;
    }

    browserEntries_.clear();
    browserLobbyRefreshPending_ = true;
    browserServerRefreshPending_ = false;
    LogInfo("Steam client refreshing browser.");
    const AppId_t appId = SteamUtils() != nullptr ? SteamUtils()->GetAppID() : 480u;
    if (serverBrowserResponse_ != nullptr)
    {
        serverBrowserResponse_->Refresh(appId);
    }

    SteamMatchmaking()->AddRequestLobbyListStringFilter(kLobbyGameKey, kGameDirectory, k_ELobbyComparisonEqual);
    const SteamAPICall_t call = SteamMatchmaking()->RequestLobbyList();
    lobbyMatchListCallResult_.Set(call, this, &SteamClientContext::OnLobbyMatchList);
}

auto SteamClientContext::ConsumeEvents() -> std::vector<Event>
{
    std::vector<Event> drained;
    drained.swap(events_);
    return drained;
}

std::uint64_t SteamClientContext::CurrentLobbyId() const
{
    return currentLobby_.IsValid() ? currentLobby_.ConvertToUint64() : 0;
}

bool SteamClientContext::InLobby() const
{
    return currentLobby_.IsValid();
}

std::uint64_t SteamClientContext::CurrentLobbyOwnerSteamId() const
{
    if (!initialized_ || SteamMatchmaking() == nullptr || !currentLobby_.IsValid())
    {
        return 0;
    }

    return SteamMatchmaking()->GetLobbyOwner(currentLobby_).ConvertToUint64();
}

std::string SteamClientContext::CurrentLobbyData(const char* key) const
{
    if (!initialized_ || SteamMatchmaking() == nullptr || !currentLobby_.IsValid())
    {
        return {};
    }

    return LobbyDataString(currentLobby_, key);
}

bool SteamClientContext::SetCurrentLobbyData(const char* key, const std::string& value)
{
    if (!initialized_ || SteamMatchmaking() == nullptr || !currentLobby_.IsValid())
    {
        return false;
    }

    return SteamMatchmaking()->SetLobbyData(currentLobby_, key, value.c_str());
}

bool SteamClientContext::OpenInviteOverlay() const
{
    if (!initialized_ || SteamFriends() == nullptr || !currentLobby_.IsValid())
    {
        return false;
    }

    SteamFriends()->ActivateGameOverlayInviteDialog(currentLobby_);
    return true;
}

void SteamClientContext::PushError(std::string text)
{
    LogError("Steam client error: ", text);
    events_.push_back({Event::Type::Error, 0, std::move(text)});
}

void SteamClientContext::PushBrowserUpdated()
{
    if (!browserLobbyRefreshPending_ && !browserServerRefreshPending_)
    {
        std::sort(browserEntries_.begin(), browserEntries_.end(), [](const net::SessionBrowserEntry& left, const net::SessionBrowserEntry& right)
        {
            if (left.dedicated != right.dedicated)
            {
                return left.dedicated;
            }
            return left.name < right.name;
        });
        LogInfo("Steam client browser refresh complete entries=", browserEntries_.size());
        events_.push_back({Event::Type::BrowserUpdated, 0, {}});
    }
}

void SteamClientContext::OnLobbyJoinRequested(GameLobbyJoinRequested_t* const result)
{
    if (result == nullptr)
    {
        return;
    }

    const std::uint64_t lobbyId = result->m_steamIDLobby.ConvertToUint64();
    LogInfo("Steam invite requested lobby_id=", lobbyId);
    events_.push_back({Event::Type::LobbyJoinRequested, lobbyId, {}});
}

void SteamClientContext::OnLobbyCreated(LobbyCreated_t* const result, const bool ioFailure)
{
    if (ioFailure || result == nullptr || result->m_eResult != k_EResultOK)
    {
        PushError("Steam lobby creation failed.");
        return;
    }

    currentLobby_ = CSteamID(result->m_ulSteamIDLobby);
    SteamMatchmaking()->SetLobbyData(currentLobby_, kLobbyGameKey, kGameDirectory);
    SteamMatchmaking()->SetLobbyData(currentLobby_, kLobbyKindKey, "listen");
    SteamMatchmaking()->SetLobbyData(currentLobby_, kLobbyVersionKey, kSessionVersion);
    SteamMatchmaking()->SetLobbyData(currentLobby_, kLobbyNameKey, pendingLobbyName_.c_str());
    SteamMatchmaking()->SetLobbyData(currentLobby_, kLobbySummaryKey, pendingLobbySummary_.c_str());
    const std::string ownerString = std::to_string(LocalSteamId());
    SteamMatchmaking()->SetLobbyData(currentLobby_, kLobbyOwnerKey, ownerString.c_str());
    SteamMatchmaking()->SetLobbyData(currentLobby_, kLobbyHostSteamIdKey, ownerString.c_str());
    SteamMatchmaking()->SetLobbyData(currentLobby_, kLobbyHostReadyKey, "1");
    SteamMatchmaking()->SetLobbyData(currentLobby_, kLobbyVirtualPortKey, "0");
    LogInfo("Steam lobby created lobby_id=", currentLobby_.ConvertToUint64(), " owner=", ownerString);
    events_.push_back({Event::Type::LobbyCreated, currentLobby_.ConvertToUint64(), pendingLobbyName_});
}

void SteamClientContext::OnLobbyEntered(LobbyEnter_t* const result, const bool ioFailure)
{
    if (ioFailure || result == nullptr || result->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
    {
        PushError("Steam lobby join failed.");
        return;
    }

    currentLobby_ = CSteamID(result->m_ulSteamIDLobby);
    LogInfo(
        "Steam lobby entered lobby_id=", currentLobby_.ConvertToUint64(),
        " owner=", CurrentLobbyOwnerSteamId(),
        " host_steam_id=", CurrentLobbyData(kLobbyHostSteamIdKey),
        " ready=", CurrentLobbyData(kLobbyHostReadyKey));
    events_.push_back({Event::Type::LobbyJoined, currentLobby_.ConvertToUint64(), CurrentLobbyData(kLobbyNameKey)});
}

void SteamClientContext::OnLobbyMatchList(LobbyMatchList_t* const result, const bool ioFailure)
{
    browserLobbyRefreshPending_ = false;
    if (ioFailure || result == nullptr || SteamMatchmaking() == nullptr)
    {
        PushError("Steam lobby browser refresh failed.");
        PushBrowserUpdated();
        return;
    }

    for (std::uint32_t lobbyIndex = 0; lobbyIndex < result->m_nLobbiesMatching; ++lobbyIndex)
    {
        const CSteamID lobbyId = SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(lobbyIndex));
        if (!lobbyId.IsValid())
        {
            continue;
        }

        if (LobbyDataString(lobbyId, kLobbyKindKey) != "listen")
        {
            continue;
        }

        net::SessionBrowserEntry entry{};
        entry.type = net::BrowserEntryType::ListenHost;
        entry.lobbyId = lobbyId.ConvertToUint64();
        const std::string hostSteamIdText = LobbyDataString(lobbyId, kLobbyHostSteamIdKey);
        entry.ownerSteamId = ParseSteamIdString(hostSteamIdText);
        if (entry.ownerSteamId == 0u)
        {
            entry.ownerSteamId = SteamMatchmaking()->GetLobbyOwner(lobbyId).ConvertToUint64();
        }
        entry.name = LobbyDataString(lobbyId, kLobbyNameKey);
        if (entry.name.empty())
        {
            entry.name = "Listen Lobby";
        }
        if (entry.name.size() > 48u)
        {
            entry.name.resize(48u);
        }
        entry.summary = LobbyDataString(lobbyId, kLobbySummaryKey);
        if (entry.summary.size() > 64u)
        {
            entry.summary.resize(64u);
        }
        entry.currentPlayers = SteamMatchmaking()->GetNumLobbyMembers(lobbyId);
        entry.maxPlayers = SteamMatchmaking()->GetLobbyMemberLimit(lobbyId);
        const bool versionCompatible = LobbyDataString(lobbyId, kLobbyVersionKey) == kSessionVersion;
        const bool hostReady = LobbyDataString(lobbyId, kLobbyHostReadyKey) == "1";
        entry.joinable =
            versionCompatible &&
            hostReady &&
            entry.ownerSteamId != 0u &&
            entry.maxPlayers > 0 &&
            entry.currentPlayers < entry.maxPlayers;
        entry.dedicated = false;
        if (entry.joinable)
        {
            browserEntries_.push_back(std::move(entry));
        }
    }

    PushBrowserUpdated();
}
}
