#pragma once

#include "net/session_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace df::net
{
using PeerId = std::uint32_t;
inline constexpr PeerId kInvalidPeerId = 0u;

enum class TransportChannel : std::uint8_t
{
    Reliable = 0,
    State = 1,
};

enum class ConnectionOrigin : std::uint8_t
{
    Incoming = 0,
    Outgoing = 1,
};

enum class ConnectionStatus : std::uint8_t
{
    None = 0,
    Connecting = 1,
    Connected = 2,
    Closed = 3,
};

struct TransportPacket
{
    PeerId peerId = kInvalidPeerId;
    std::vector<std::byte> payload;
    bool reliable = true;
    TransportChannel channel = TransportChannel::Reliable;
};

struct PeerEvent
{
    PeerId peerId = kInvalidPeerId;
    ConnectionOrigin origin = ConnectionOrigin::Incoming;
    ConnectionStatus status = ConnectionStatus::None;
    std::string debugText;
};

struct PeerInfo
{
    PeerId peerId = kInvalidPeerId;
    ConnectionStatus status = ConnectionStatus::None;
    NetworkStats stats{};
};

class ITransport
{
public:
    virtual ~ITransport() = default;

    [[nodiscard]] virtual bool IsServer() const = 0;
    [[nodiscard]] virtual bool IsListening() const = 0;
    [[nodiscard]] virtual bool IsConnected(PeerId peerId) const = 0;
    virtual void Pump() = 0;
    [[nodiscard]] virtual bool Send(const TransportPacket& packet) = 0;
    [[nodiscard]] virtual std::vector<TransportPacket> Receive() = 0;
    [[nodiscard]] virtual std::vector<PeerEvent> ConsumePeerEvents() = 0;
    [[nodiscard]] virtual std::vector<PeerInfo> SnapshotPeers() const = 0;
    virtual void Close(PeerId peerId, int reason, const char* debugText) = 0;
};
}
