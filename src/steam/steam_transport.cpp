#include "steam/steam_transport.hpp"

#include "core/log.hpp"

#include <steam/steamnetworkingtypes.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

namespace df::steam
{
SteamSocketsTransport::SteamSocketsTransport(ISteamNetworkingSockets* const sockets, const bool useGameserverCallbacks)
    : sockets_(sockets)
    , useGameserverCallbacks_(useGameserverCallbacks)
{
    RegisterCallbacks();
}

SteamSocketsTransport::~SteamSocketsTransport()
{
    for (const auto& [peerId, connection] : peers_)
    {
        if (connection.handle != k_HSteamNetConnection_Invalid && sockets_ != nullptr)
        {
            (void)sockets_->CloseConnection(connection.handle, 0, "shutdown", false);
        }
    }
    peers_.clear();
    handleToPeer_.clear();

    if (listenSocket_ != k_HSteamListenSocket_Invalid && sockets_ != nullptr)
    {
        (void)sockets_->CloseListenSocket(listenSocket_);
        listenSocket_ = k_HSteamListenSocket_Invalid;
    }

    UnregisterCallbacks();
}

bool SteamSocketsTransport::StartListenP2P(const int virtualPort)
{
    if (sockets_ == nullptr)
    {
        return false;
    }

    listenSocket_ = sockets_->CreateListenSocketP2P(virtualPort, 0, nullptr);
    serverMode_ = listenSocket_ != k_HSteamListenSocket_Invalid;
    return serverMode_;
}

bool SteamSocketsTransport::StartListenIp(const std::uint16_t port)
{
    if (sockets_ == nullptr)
    {
        return false;
    }

    SteamNetworkingIPAddr address{};
    address.Clear();
    address.m_port = port;
    listenSocket_ = sockets_->CreateListenSocketIP(address, 0, nullptr);
    serverMode_ = listenSocket_ != k_HSteamListenSocket_Invalid;
    return serverMode_;
}

bool SteamSocketsTransport::ConnectP2P(const std::uint64_t remoteSteamId, const int virtualPort)
{
    if (sockets_ == nullptr)
    {
        return false;
    }

    SteamNetworkingIdentity remoteIdentity{};
    remoteIdentity.SetSteamID64(remoteSteamId);
    const HSteamNetConnection connection = sockets_->ConnectP2P(remoteIdentity, virtualPort, 0, nullptr);
    if (connection == k_HSteamNetConnection_Invalid)
    {
        return false;
    }

    (void)EnsurePeer(connection, net::ConnectionOrigin::Outgoing);
    return true;
}

bool SteamSocketsTransport::ConnectIp(const std::string& address, const std::uint16_t port)
{
    if (sockets_ == nullptr)
    {
        return false;
    }

    SteamNetworkingIPAddr remoteAddress{};
    std::string host = address;
    if (!host.empty() && host.front() == '[')
    {
        const std::size_t closingBracket = host.find(']');
        if (closingBracket != std::string::npos)
        {
            host = host.substr(1, closingBracket - 1);
        }
    }
    else if (const std::size_t colon = host.rfind(':'); colon != std::string::npos && host.find(':') == colon)
    {
        host = host.substr(0, colon);
    }

    IN_ADDR ipv4Address{};
    if (host == "localhost")
    {
        remoteAddress.SetIPv4(0x7f000001u, port);
    }
    else if (InetPtonA(AF_INET, host.c_str(), &ipv4Address) == 1)
    {
        remoteAddress.SetIPv4(ntohl(ipv4Address.S_un.S_addr), port);
    }
    else
    {
        IN6_ADDR ipv6Address{};
        if (InetPtonA(AF_INET6, host.c_str(), &ipv6Address) != 1)
        {
            return false;
        }

        remoteAddress.SetIPv6(ipv6Address.u.Byte, port);
    }

    const HSteamNetConnection connection = sockets_->ConnectByIPAddress(remoteAddress, 0, nullptr);
    if (connection == k_HSteamNetConnection_Invalid)
    {
        return false;
    }

    (void)EnsurePeer(connection, net::ConnectionOrigin::Outgoing);
    return true;
}

bool SteamSocketsTransport::IsConnected(const net::PeerId peerId) const
{
    const auto iter = peers_.find(peerId);
    return iter != peers_.end() && iter->second.info.status == net::ConnectionStatus::Connected;
}

void SteamSocketsTransport::Pump()
{
    if (sockets_ == nullptr)
    {
        return;
    }

    for (auto& [peerId, connection] : peers_)
    {
        if (connection.handle == k_HSteamNetConnection_Invalid || connection.info.status == net::ConnectionStatus::Closed)
        {
            continue;
        }

        SteamNetConnectionRealTimeStatus_t status{};
        if (sockets_->GetConnectionRealTimeStatus(connection.handle, &status, 0, nullptr) == k_EResultOK)
        {
            connection.info.stats.pingMilliseconds = status.m_nPing;
            connection.info.stats.connectionQuality = status.m_flConnectionQualityLocal;
            connection.info.stats.lossPercent = (1.0f - status.m_flConnectionQualityLocal) * 100.0f;
        }
    }
}

bool SteamSocketsTransport::Send(const net::TransportPacket& packet)
{
    if (sockets_ == nullptr)
    {
        return false;
    }

    const auto iter = peers_.find(packet.peerId);
    if (iter == peers_.end() || iter->second.handle == k_HSteamNetConnection_Invalid)
    {
        return false;
    }

    const int sendFlags = packet.reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoDelay;
    return sockets_->SendMessageToConnection(
               iter->second.handle,
               packet.payload.data(),
               static_cast<std::uint32_t>(packet.payload.size()),
               sendFlags,
               nullptr) == k_EResultOK;
}

auto SteamSocketsTransport::Receive() -> std::vector<net::TransportPacket>
{
    std::vector<net::TransportPacket> packets;
    if (sockets_ == nullptr)
    {
        return packets;
    }

    for (const auto& [peerId, connection] : peers_)
    {
        if (connection.info.status != net::ConnectionStatus::Connected)
        {
            continue;
        }

        SteamNetworkingMessage_t* messages[32]{};
        const int messageCount = sockets_->ReceiveMessagesOnConnection(connection.handle, messages, 32);
        for (int index = 0; index < messageCount; ++index)
        {
            SteamNetworkingMessage_t* const message = messages[index];
            net::TransportPacket packet{};
            packet.peerId = peerId;
            packet.payload.resize(static_cast<std::size_t>(message->m_cbSize));
            std::memcpy(packet.payload.data(), message->m_pData, packet.payload.size());
            packet.reliable = message->m_nFlags & k_nSteamNetworkingSend_Reliable;
            packet.channel = message->m_nChannel == 1 ? net::TransportChannel::State : net::TransportChannel::Reliable;
            packets.push_back(std::move(packet));
            message->Release();
        }
    }

    return packets;
}

auto SteamSocketsTransport::ConsumePeerEvents() -> std::vector<net::PeerEvent>
{
    std::vector<net::PeerEvent> drained;
    drained.swap(pendingPeerEvents_);
    return drained;
}

auto SteamSocketsTransport::SnapshotPeers() const -> std::vector<net::PeerInfo>
{
    std::vector<net::PeerInfo> snapshot;
    snapshot.reserve(peers_.size());
    for (const auto& [peerId, connection] : peers_)
    {
        snapshot.push_back(connection.info);
    }
    return snapshot;
}

void SteamSocketsTransport::Close(const net::PeerId peerId, const int reason, const char* const debugText)
{
    auto iter = peers_.find(peerId);
    if (iter == peers_.end() || sockets_ == nullptr)
    {
        return;
    }

    (void)sockets_->CloseConnection(iter->second.handle, reason, debugText, false);
    iter->second.info.status = net::ConnectionStatus::Closed;
}

void SteamSocketsTransport::RegisterCallbacks()
{
    if (useGameserverCallbacks_)
    {
        serverCallback_.Register(this, &SteamSocketsTransport::OnConnectionStatusChanged);
    }
    else
    {
        clientCallback_.Register(this, &SteamSocketsTransport::OnConnectionStatusChanged);
    }
}

void SteamSocketsTransport::UnregisterCallbacks()
{
    if (useGameserverCallbacks_)
    {
        serverCallback_.Unregister();
    }
    else
    {
        clientCallback_.Unregister();
    }
}

void SteamSocketsTransport::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* const callback)
{
    if (callback == nullptr || sockets_ == nullptr)
    {
        return;
    }

    const bool incoming = callback->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid;
    const net::PeerId peerId = EnsurePeer(
        callback->m_hConn,
        incoming ? net::ConnectionOrigin::Incoming : net::ConnectionOrigin::Outgoing);
    Connection* const connection = FindConnection(callback->m_hConn);
    if (connection == nullptr)
    {
        return;
    }

    switch (callback->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
        connection->info.status = net::ConnectionStatus::Connecting;
        if (incoming)
        {
            (void)sockets_->AcceptConnection(callback->m_hConn);
        }
        pendingPeerEvents_.push_back({peerId, connection->origin, net::ConnectionStatus::Connecting, callback->m_info.m_szEndDebug});
        break;

    case k_ESteamNetworkingConnectionState_Connected:
        connection->info.status = net::ConnectionStatus::Connected;
        pendingPeerEvents_.push_back({peerId, connection->origin, net::ConnectionStatus::Connected, callback->m_info.m_szEndDebug});
        break;

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        connection->info.status = net::ConnectionStatus::Closed;
        pendingPeerEvents_.push_back({peerId, connection->origin, net::ConnectionStatus::Closed, callback->m_info.m_szEndDebug});
        (void)sockets_->CloseConnection(callback->m_hConn, 0, "closed", false);
        break;

    default:
        break;
    }
}

auto SteamSocketsTransport::EnsurePeer(const HSteamNetConnection handle, const net::ConnectionOrigin origin) -> net::PeerId
{
    const auto handleIter = handleToPeer_.find(handle);
    if (handleIter != handleToPeer_.end())
    {
        return handleIter->second;
    }

    const net::PeerId peerId = nextPeerId_++;
    Connection connection{};
    connection.handle = handle;
    connection.info.peerId = peerId;
    connection.info.status = net::ConnectionStatus::Connecting;
    connection.origin = origin;
    peers_.emplace(peerId, connection);
    handleToPeer_.emplace(handle, peerId);
    return peerId;
}

auto SteamSocketsTransport::FindConnection(const HSteamNetConnection handle) -> Connection*
{
    const auto handleIter = handleToPeer_.find(handle);
    if (handleIter == handleToPeer_.end())
    {
        return nullptr;
    }

    const auto iter = peers_.find(handleIter->second);
    return iter != peers_.end() ? &iter->second : nullptr;
}

auto SteamSocketsTransport::FindConnection(const HSteamNetConnection handle) const -> const Connection*
{
    const auto handleIter = handleToPeer_.find(handle);
    if (handleIter == handleToPeer_.end())
    {
        return nullptr;
    }

    const auto iter = peers_.find(handleIter->second);
    return iter != peers_.end() ? &iter->second : nullptr;
}
}
