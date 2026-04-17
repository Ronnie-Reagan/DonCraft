#include "steam/steam_client.hpp"

#include "core/log.hpp"

#include <steam/isteamfriends.h>
#include <steam/isteammatchmaking.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/isteamuser.h>
#include <steam/isteamutils.h>
#include <steam/matchmakingtypes.h>

#include <algorithm>
#include <memory>

namespace df::steam
{
namespace
{
constexpr char kLobbyGameKey[] = "df_game";
constexpr char kLobbyKindKey[] = "df_kind";
constexpr char kLobbyNameKey[] = "df_name";
constexpr char kLobbySummaryKey[] = "df_summary";
constexpr char kLobbyOwnerKey[] = "df_owner";
constexpr char kLobbyVersionKey[] = "df_version";
constexpr char kGameDirectory[] = "doncraft";
constexpr char kSessionVersion[] = "1";

[[nodiscard]] auto SafeSteamString(const char* value) -> std::string
{
    return value != nullptr ? std::string(value) : std::string();
}

[[nodiscard]] auto LobbyDataString(const CSteamID lobbyId, const char* key) -> std::string
{
    if (SteamMatchmaking() == nullptr || !lobbyId.IsValid())
    {
        return {};
    }

    return SafeSteamString(SteamMatchmaking()->GetLobbyData(lobbyId, key));
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

    void Refresh()
    {
        Cancel();

        if (SteamMatchmakingServers() == nullptr)
        {
            owner_.browserServerRefreshPending_ = false;
            owner_.PushBrowserUpdated();
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

        const AppId_t appId = SteamUtils() != nullptr ? SteamUtils()->GetAppID() : 480u;
        request_ = SteamMatchmakingServers()->RequestInternetServerList(appId, filterPtrs_.data(), static_cast<std::uint32_t>(filterPtrs_.size()), this);
        owner_.browserServerRefreshPending_ = true;
    }

    void Cancel()
    {
        if (request_ != nullptr && SteamMatchmakingServers() != nullptr)
        {
            SteamMatchmakingServers()->CancelQuery(request_);
            SteamMatchmakingServers()->ReleaseRequest(request_);
            request_ = nullptr;
        }
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
        entry.name = details->GetName();
        entry.summary = details->m_szGameDescription;
        entry.address = details->m_NetAdr.GetConnectionAddressString();
        entry.port = details->m_NetAdr.GetConnectionPort();
        entry.currentPlayers = details->m_nPlayers;
        entry.maxPlayers = details->m_nMaxPlayers;
        entry.joinable = details->m_bHadSuccessfulResponse;
        entry.dedicated = true;
        entry.ownerSteamId = details->m_steamID.ConvertToUint64();
        owner_.browserEntries_.push_back(std::move(entry));
    }

    void ServerFailedToRespond(HServerListRequest, int) override
    {
    }

    void RefreshComplete(HServerListRequest request, EMatchMakingServerResponse) override
    {
        if (request == request_)
        {
            owner_.browserServerRefreshPending_ = false;
            owner_.PushBrowserUpdated();
            Cancel();
        }
    }

private:
    SteamClientContext& owner_;
    HServerListRequest request_ = nullptr;
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
    }

    serverBrowserResponse_ = new ServerBrowserResponse(*this);
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

    const SteamAPICall_t call = SteamMatchmaking()->JoinLobby(CSteamID(lobbyId));
    lobbyEnterCallResult_.Set(call, this, &SteamClientContext::OnLobbyEntered);
}

void SteamClientContext::LeaveLobby()
{
    if (!initialized_ || SteamMatchmaking() == nullptr || !currentLobby_.IsValid())
    {
        return;
    }

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
    if (serverBrowserResponse_ != nullptr)
    {
        serverBrowserResponse_->Refresh();
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
        events_.push_back({Event::Type::BrowserUpdated, 0, {}});
    }
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

        net::SessionBrowserEntry entry{};
        entry.type = net::BrowserEntryType::ListenHost;
        entry.lobbyId = lobbyId.ConvertToUint64();
        entry.ownerSteamId = SteamMatchmaking()->GetLobbyOwner(lobbyId).ConvertToUint64();
        entry.name = LobbyDataString(lobbyId, kLobbyNameKey);
        if (entry.name.empty())
        {
            entry.name = "Listen Lobby";
        }
        entry.summary = LobbyDataString(lobbyId, kLobbySummaryKey);
        entry.currentPlayers = SteamMatchmaking()->GetNumLobbyMembers(lobbyId);
        entry.maxPlayers = SteamMatchmaking()->GetLobbyMemberLimit(lobbyId);
        entry.joinable = !LobbyDataString(lobbyId, kLobbyVersionKey).empty();
        entry.dedicated = false;
        browserEntries_.push_back(std::move(entry));
    }

    PushBrowserUpdated();
}
}
