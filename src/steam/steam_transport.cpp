#include "steam/steam_transport.hpp"

#include "core/log.hpp"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingtypes.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <string>

namespace df::steam
{
namespace
{
constexpr int kTransportLaneCount = 2;
// SteamNetworkingSockets services lower numeric priorities first; keep state/input ahead of reliable world transfers.
constexpr int kTransportLanePriorities[kTransportLaneCount] = {10, 0};
constexpr uint16 kTransportLaneWeights[kTransportLaneCount] = {1, 1};

auto ConnectionStateName(const ESteamNetworkingConnectionState state) -> const char*
{
    switch (state)
    {
    case k_ESteamNetworkingConnectionState_None: return "none";
    case k_ESteamNetworkingConnectionState_Connecting: return "connecting";
    case k_ESteamNetworkingConnectionState_FindingRoute: return "finding-route";
    case k_ESteamNetworkingConnectionState_Connected: return "connected";
    case k_ESteamNetworkingConnectionState_ClosedByPeer: return "closed-by-peer";
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: return "problem-local";
    case k_ESteamNetworkingConnectionState_FinWait: return "fin-wait";
    case k_ESteamNetworkingConnectionState_Linger: return "linger";
    case k_ESteamNetworkingConnectionState_Dead: return "dead";
    default: return "unknown";
    }
}

auto DescribeRemoteIdentity(const SteamNetworkingIdentity& identity) -> std::string
{
    if (const std::uint64_t steamId = identity.GetSteamID64(); steamId != 0u)
    {
        return "steamid:" + std::to_string(steamId);
    }

    if (const SteamNetworkingIPAddr* const ip = identity.GetIPAddr(); ip != nullptr)
    {
        return "ip:" + std::to_string(ip->GetIPv4()) + ":" + std::to_string(ip->m_port);
    }

    if (const char* const genericString = identity.GetGenericString(); genericString != nullptr)
    {
        return "generic:" + std::string(genericString);
    }

    return "type:" + std::to_string(static_cast<int>(identity.m_eType));
}

auto TransportLaneIndex(const net::TransportChannel channel) -> uint16
{
    return channel == net::TransportChannel::State ? 1u : 0u;
}
}

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
        LogError("Steam transport failed to start P2P listen socket: sockets interface unavailable.");
        return false;
    }

    listenSocket_ = sockets_->CreateListenSocketP2P(virtualPort, 0, nullptr);
    serverMode_ = listenSocket_ != k_HSteamListenSocket_Invalid;
    if (serverMode_)
    {
        LogInfo("Steam transport listening for P2P connections on virtual_port=", virtualPort, " socket=", listenSocket_);
    }
    else
    {
        LogError("Steam transport failed to create a P2P listen socket on virtual_port=", virtualPort);
    }
    return serverMode_;
}

bool SteamSocketsTransport::StartListenIp(const std::uint16_t port)
{
    if (sockets_ == nullptr)
    {
        LogError("Steam transport failed to start IP listen socket: sockets interface unavailable.");
        return false;
    }

    SteamNetworkingIPAddr address{};
    address.Clear();
    address.m_port = port;
    listenSocket_ = sockets_->CreateListenSocketIP(address, 0, nullptr);
    serverMode_ = listenSocket_ != k_HSteamListenSocket_Invalid;
    if (serverMode_)
    {
        LogInfo("Steam transport listening for IP connections on port=", port, " socket=", listenSocket_);
    }
    else
    {
        LogError("Steam transport failed to create an IP listen socket on port=", port);
    }
    return serverMode_;
}

bool SteamSocketsTransport::ConnectP2P(const std::uint64_t remoteSteamId, const int virtualPort)
{
    if (sockets_ == nullptr)
    {
        LogError("Steam transport failed P2P connect: sockets interface unavailable.");
        return false;
    }

    if (remoteSteamId == 0u)
    {
        LogError("Steam transport rejected P2P connect with an invalid remote Steam ID.");
        return false;
    }

    SteamNetworkingIdentity remoteIdentity{};
    remoteIdentity.SetSteamID64(remoteSteamId);
    const HSteamNetConnection connection = sockets_->ConnectP2P(remoteIdentity, virtualPort, 0, nullptr);
    if (connection == k_HSteamNetConnection_Invalid)
    {
        LogError("Steam transport failed to connect P2P to steam_id=", remoteSteamId, " virtual_port=", virtualPort);
        return false;
    }

    (void)EnsurePeer(connection, net::ConnectionOrigin::Outgoing);
    LogInfo("Steam transport connecting P2P to steam_id=", remoteSteamId, " virtual_port=", virtualPort, " handle=", connection);
    return true;
}

bool SteamSocketsTransport::ConnectIp(const std::string& address, const std::uint16_t port)
{
    if (sockets_ == nullptr)
    {
        LogError("Steam transport failed IP connect: sockets interface unavailable.");
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
            LogError("Steam transport rejected invalid remote address '", address, "'");
            return false;
        }

        remoteAddress.SetIPv6(ipv6Address.u.Byte, port);
    }

    const HSteamNetConnection connection = sockets_->ConnectByIPAddress(remoteAddress, 0, nullptr);
    if (connection == k_HSteamNetConnection_Invalid)
    {
        LogError("Steam transport failed to connect to address=", address, " port=", port);
        return false;
    }

    (void)EnsurePeer(connection, net::ConnectionOrigin::Outgoing);
    LogInfo("Steam transport connecting by IP address=", address, " port=", port, " handle=", connection);
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
    if (iter->second.lanesConfigured)
    {
        ISteamNetworkingUtils* const utils = SteamNetworkingUtils();
        if (utils != nullptr)
        {
            SteamNetworkingMessage_t* const message = utils->AllocateMessage(static_cast<int>(packet.payload.size()));
            if (message != nullptr)
            {
                message->m_conn = iter->second.handle;
                message->m_nFlags = sendFlags;
                message->m_idxLane = TransportLaneIndex(packet.channel);
                if (!packet.payload.empty())
                {
                    std::memcpy(message->m_pData, packet.payload.data(), packet.payload.size());
                }

                SteamNetworkingMessage_t* messages[] = {message};
                int64 result = 0;
                sockets_->SendMessages(1, messages, &result);
                return result > 0;
            }
        }
    }

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
        if (messageCount < 0)
        {
            LogWarning("Steam transport failed to receive messages on handle=", connection.handle, " peer=", peerId);
            continue;
        }
        for (int index = 0; index < messageCount; ++index)
        {
            SteamNetworkingMessage_t* const message = messages[index];
            net::TransportPacket packet{};
            packet.peerId = peerId;
            packet.payload.resize(static_cast<std::size_t>(message->m_cbSize));
            std::memcpy(packet.payload.data(), message->m_pData, packet.payload.size());
            packet.reliable = (message->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0;
            packet.channel = message->m_idxLane == TransportLaneIndex(net::TransportChannel::State)
                ? net::TransportChannel::State
                : net::TransportChannel::Reliable;
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

    LogInfo(
        "Steam transport closing peer=", peerId,
        " handle=", iter->second.handle,
        " reason=", reason,
        " detail='", (debugText != nullptr ? debugText : ""), "'");
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

    LogInfo(
        "Steam transport state change handle=", callback->m_hConn,
        " peer=", peerId,
        " origin=", static_cast<int>(connection->origin),
        " remote='", DescribeRemoteIdentity(callback->m_info.m_identityRemote),
        "' state=", ConnectionStateName(callback->m_info.m_eState),
        " detail='", callback->m_info.m_szEndDebug, "'");

    switch (callback->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
        connection->info.status = net::ConnectionStatus::Connecting;
        if (incoming)
        {
            const EResult acceptResult = sockets_->AcceptConnection(callback->m_hConn);
            if (acceptResult != k_EResultOK)
            {
                LogError(
                    "Steam transport failed to accept incoming connection handle=", callback->m_hConn,
                    " result=", static_cast<int>(acceptResult));
                pendingPeerEvents_.push_back({peerId, connection->origin, net::ConnectionStatus::Closed, "accept failed"});
                (void)sockets_->CloseConnection(callback->m_hConn, 0, "accept failed", false);
                ReleaseConnection(callback->m_hConn);
                break;
            }
        }
        pendingPeerEvents_.push_back({peerId, connection->origin, net::ConnectionStatus::Connecting, callback->m_info.m_szEndDebug});
        break;

    case k_ESteamNetworkingConnectionState_Connected:
        connection->info.status = net::ConnectionStatus::Connected;
        connection->lanesConfigured =
            sockets_->ConfigureConnectionLanes(
                callback->m_hConn,
                kTransportLaneCount,
                kTransportLanePriorities,
                kTransportLaneWeights) == k_EResultOK;
        if (!connection->lanesConfigured)
        {
            LogWarning("Steam transport failed to configure lanes for handle=", callback->m_hConn, " peer=", peerId);
        }
        pendingPeerEvents_.push_back({peerId, connection->origin, net::ConnectionStatus::Connected, callback->m_info.m_szEndDebug});
        break;

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        connection->info.status = net::ConnectionStatus::Closed;
        pendingPeerEvents_.push_back({peerId, connection->origin, net::ConnectionStatus::Closed, callback->m_info.m_szEndDebug});
        (void)sockets_->CloseConnection(callback->m_hConn, 0, "closed", false);
        ReleaseConnection(callback->m_hConn);
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

void SteamSocketsTransport::ReleaseConnection(const HSteamNetConnection handle)
{
    const auto handleIter = handleToPeer_.find(handle);
    if (handleIter == handleToPeer_.end())
    {
        return;
    }

    peers_.erase(handleIter->second);
    handleToPeer_.erase(handleIter);
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
