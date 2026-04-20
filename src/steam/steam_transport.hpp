#pragma once

#include "net/transport.hpp"

#include <steam/isteamnetworkingsockets.h>
#include <steam/steam_api_common.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace df::steam
{
class SteamSocketsTransport final : public net::ITransport
{
public:
    SteamSocketsTransport(ISteamNetworkingSockets* sockets, bool useGameserverCallbacks);
    ~SteamSocketsTransport() override;

    [[nodiscard]] bool StartListenP2P(int virtualPort = 0);
    [[nodiscard]] bool StartListenIp(std::uint16_t port);
    [[nodiscard]] bool ConnectP2P(std::uint64_t remoteSteamId, int virtualPort = 0);
    [[nodiscard]] bool ConnectIp(const std::string& address, std::uint16_t port);

    [[nodiscard]] bool IsServer() const override
    {
        return serverMode_;
    }

    [[nodiscard]] bool IsListening() const override
    {
        return listenSocket_ != k_HSteamListenSocket_Invalid;
    }

    [[nodiscard]] bool IsConnected(net::PeerId peerId) const override;
    void Pump() override;
    [[nodiscard]] bool Send(const net::TransportPacket& packet) override;
    [[nodiscard]] auto Receive() -> std::vector<net::TransportPacket> override;
    [[nodiscard]] auto ConsumePeerEvents() -> std::vector<net::PeerEvent> override;
    [[nodiscard]] auto SnapshotPeers() const -> std::vector<net::PeerInfo> override;
    void Close(net::PeerId peerId, int reason, const char* debugText) override;

private:
    struct Connection
    {
        HSteamNetConnection handle = k_HSteamNetConnection_Invalid;
        net::PeerInfo info{};
        net::ConnectionOrigin origin = net::ConnectionOrigin::Incoming;
        bool lanesConfigured = false;
    };

    void RegisterCallbacks();
    void UnregisterCallbacks();
    void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* callback);
    [[nodiscard]] auto EnsurePeer(HSteamNetConnection handle, net::ConnectionOrigin origin) -> net::PeerId;
    void ReleaseConnection(HSteamNetConnection handle);
    [[nodiscard]] auto FindConnection(HSteamNetConnection handle) -> Connection*;
    [[nodiscard]] auto FindConnection(HSteamNetConnection handle) const -> const Connection*;

    ISteamNetworkingSockets* sockets_ = nullptr;
    bool useGameserverCallbacks_ = false;
    bool serverMode_ = false;
    HSteamListenSocket listenSocket_ = k_HSteamListenSocket_Invalid;
    net::PeerId nextPeerId_ = 1u;
    std::unordered_map<net::PeerId, Connection> peers_;
    std::unordered_map<HSteamNetConnection, net::PeerId> handleToPeer_;
    std::vector<net::PeerEvent> pendingPeerEvents_;
    CCallbackManual<SteamSocketsTransport, SteamNetConnectionStatusChangedCallback_t> clientCallback_{};
    CCallbackManual<SteamSocketsTransport, SteamNetConnectionStatusChangedCallback_t, true> serverCallback_{};
};
}
