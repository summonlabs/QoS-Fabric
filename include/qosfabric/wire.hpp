// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounded, checksummed, versioned framing for the publisher protocol.
//
// Every frame is length-prefixed and checksummed, every length is checked
// against a hard budget before a single byte is allocated or interpreted, and
// the protocol version is refused outright when it does not match. A decoder
// can therefore never be driven to over-read, over-allocate, or interpret a
// frame written by a different protocol.

#ifndef QOSFABRIC_WIRE_HPP
#define QOSFABRIC_WIRE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "qosfabric/error.hpp"
#include "qosfabric/ids.hpp"
#include "qosfabric/limits.hpp"
#include "qosfabric/model.hpp"
#include "qosfabric/version.hpp"

namespace qosfabric {

enum class MessageType : std::uint8_t {
  Unknown = 0,
  Hello = 1,
  HelloAck = 2,
  PublishCapability = 3,
  PublishReservation = 4,
  PublishAck = 5,
  Revoke = 6,
  Bye = 7,
  Ping = 8,
  Pong = 9,
  RegistryStatus = 10,
};

enum class WireStatus : std::uint8_t {
  Ok = 0,
  Rejected = 1,
  Fenced = 2,
  Unauthorized = 3,
  Stale = 4,
  Conflict = 5,
  TooLarge = 6,
  NotFound = 7,
  Internal = 8,
};

[[nodiscard]] const char* to_string(MessageType value) noexcept;
[[nodiscard]] const char* to_string(WireStatus value) noexcept;
// Maps a runtime error code onto the status a peer is allowed to see. Internal
// detail never leaks across the wire.
[[nodiscard]] WireStatus status_from_error(ErrorCode code) noexcept;

// Encodes a frame: payload length, checksum, protocol version, type, body.
[[nodiscard]] std::vector<std::uint8_t> encode_frame(MessageType type,
                                                     const std::vector<std::uint8_t>& body);

class Frame {
 public:
  MessageType type{MessageType::Unknown};
  std::vector<std::uint8_t> body{};
};

// Decodes exactly one frame from a complete buffer. Refuses anything larger
// than the wire budget, any checksum mismatch, any version mismatch and any
// trailing bytes.
[[nodiscard]] Result<Frame> decode_frame(const std::vector<std::uint8_t>& bytes);
// Decodes one frame from a prefix of a stream and reports how many bytes it
// consumed, so a streaming reader never has to buffer more than one frame.
[[nodiscard]] Result<Frame> decode_frame_prefix(const std::vector<std::uint8_t>& bytes,
                                                std::size_t& consumed);

// --- Messages ---------------------------------------------------------------
class HelloMessage {
 public:
  PublisherId publisher{};
  PublisherIncarnation incarnation{};
  BootId boot{};
  std::uint32_t ttl_ms{0};
  std::string origin{};
};

class HelloAckMessage {
 public:
  WireStatus status{WireStatus::Internal};
  FabricEpoch epoch{};
  LeaseId lease{};
  std::uint64_t expires_at_ms{0};
  std::string detail{};
};

class PublishAckMessage {
 public:
  WireStatus status{WireStatus::Internal};
  Generation generation{};
  Sequence sequence{};
  Digest256 digest{};
  std::string detail{};
};

class RegistryStatusMessage {
 public:
  FabricEpoch epoch{};
  std::uint64_t classes{0};
  std::uint64_t policies{0};
  std::uint64_t paths{0};
  std::uint64_t capabilities{0};
  std::uint64_t decisions{0};
  std::uint64_t publishers{0};
  std::uint64_t conflicts{0};
};

[[nodiscard]] std::vector<std::uint8_t> encode_hello(const HelloMessage& value);
[[nodiscard]] Result<HelloMessage> decode_hello(const std::vector<std::uint8_t>& body);
[[nodiscard]] std::vector<std::uint8_t> encode_hello_ack(const HelloAckMessage& value);
[[nodiscard]] Result<HelloAckMessage> decode_hello_ack(const std::vector<std::uint8_t>& body);
[[nodiscard]] std::vector<std::uint8_t> encode_publish_ack(const PublishAckMessage& value);
[[nodiscard]] Result<PublishAckMessage> decode_publish_ack(const std::vector<std::uint8_t>& body);
[[nodiscard]] std::vector<std::uint8_t> encode_registry_status(const RegistryStatusMessage& value);
[[nodiscard]] Result<RegistryStatusMessage> decode_registry_status(
    const std::vector<std::uint8_t>& body);

}  // namespace qosfabric

#endif  // QOSFABRIC_WIRE_HPP
