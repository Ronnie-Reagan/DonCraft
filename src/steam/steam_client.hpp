#pragma once

#include "net/session_protocol.hpp"

#include <steam/isteammatchmaking.h>
#include <steam/steam_api.h>

#include <cstdint>
#include <string>
#include <vector>

namespace df::steam
{
class SteamClientContext
{
public:
    SteamClientContext();
    ~SteamClientContext();

    SteamClientContext(const SteamClientContext&) = delete;
    auto operator=(const SteamClientContext&) -> SteamClientContext& = delete;

    struct Event
    {
        enum class Type
        {
            LobbyCreated,
            LobbyJoined,
            LobbyLeft,
            BrowserUpdated,
            Error,
        };

        Type type = Type::Error;
        std::uint64_t lobbyId = 0;
        std::string text;
    };

    [[nodiscard]] bool Initialize();
    void Shutdown();
    void PumpCallbacks();

    [[nodiscard]] bool IsInitialized() const
    {
        return initialized_;
    }

    [[nodiscard]] std::uint64_t LocalSteamId() const;
    [[nodiscard]] std::string PersonaName() const;
    [[nodiscard]] const std::string& FailureMessage() const
    {
        return failureMessage_;
    }

    void CreateLobby(const std::string& sessionName, const std::string& summary, int maxPlayers);
    void JoinLobby(std::uint64_t lobbyId);
    void LeaveLobby();
    void RefreshBrowser();
    [[nodiscard]] auto BrowserEntries() const -> const std::vector<net::SessionBrowserEntry>&
    {
        return browserEntries_;
    }
    [[nodiscard]] auto ConsumeEvents() -> std::vector<Event>;

    [[nodiscard]] std::uint64_t CurrentLobbyId() const;
    [[nodiscard]] bool InLobby() const;
    [[nodiscard]] std::uint64_t CurrentLobbyOwnerSteamId() const;
    [[nodiscard]] std::string CurrentLobbyData(const char* key) const;
    [[nodiscard]] bool SetCurrentLobbyData(const char* key, const std::string& value);
    [[nodiscard]] bool OpenInviteOverlay() const;

private:
    class ServerBrowserResponse;

    void PushError(std::string text);
    void PushBrowserUpdated();
    void OnLobbyCreated(LobbyCreated_t* result, bool ioFailure);
    void OnLobbyEntered(LobbyEnter_t* result, bool ioFailure);
    void OnLobbyMatchList(LobbyMatchList_t* result, bool ioFailure);

    bool initialized_ = false;
    bool browserLobbyRefreshPending_ = false;
    bool browserServerRefreshPending_ = false;
    std::string pendingLobbyName_;
    std::string pendingLobbySummary_;
    int pendingLobbyMaxPlayers_ = 8;
    std::string failureMessage_;
    CSteamID currentLobby_{};
    CCallResult<SteamClientContext, LobbyCreated_t> lobbyCreatedCallResult_{};
    CCallResult<SteamClientContext, LobbyEnter_t> lobbyEnterCallResult_{};
    CCallResult<SteamClientContext, LobbyMatchList_t> lobbyMatchListCallResult_{};
    std::vector<Event> events_;
    std::vector<net::SessionBrowserEntry> browserEntries_;
    ServerBrowserResponse* serverBrowserResponse_ = nullptr;
};
}
