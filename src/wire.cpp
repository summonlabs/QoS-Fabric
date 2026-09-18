// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/wire.hpp"

#include <algorithm>
#include <functional>

#include "qosfabric/checked.hpp"
#include "qosfabric/crypto.hpp"
#include "qosfabric/serialize.hpp"

namespace qosfabric {
namespace {

constexpr std::size_t kFrameHeaderBytes = 8;   // payload length + checksum
constexpr std::size_t kEnvelopeBytes = 7;      // version, type, body length
constexpr std::uint8_t kMaxMessageType = static_cast<std::uint8_t>(MessageType::RegistryStatus);
constexpr std::uint8_t kMaxWireStatus = static_cast<std::uint8_t>(WireStatus::Internal);

// Message encoders produce a message *body*. Framing is a separate,
// explicitly requested step (encode_frame), so a body can never be mistaken
// for a frame and wrapped twice.
std::vector<std::uint8_t> encode_body(std::function<void(ByteWriter&)>&& emit) {
  ByteWriter body(limits::kMaxWireFrameBytes);
  emit(body);
  if (!body.ok()) {
    return {};
  }
  return body.data();
}

void write_text(ByteWriter& writer, std::string_view text, std::size_t max_len) {
  const std::size_t clipped = std::min(text.size(), max_len);
  writer.str(text.substr(0, clipped), max_len);
}

Result<std::string> read_text(ByteReader& reader, std::size_t max_len) {
  std::string text = reader.str(max_len);
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "text field is malformed or over budget");
  }
  return text;
}

}  // namespace

const char* to_string(MessageType value) noexcept {
  switch (value) {
    case MessageType::Unknown: return "unknown";
    case MessageType::Hello: return "hello";
    case MessageType::HelloAck: return "hello-ack";
    case MessageType::PublishCapability: return "publish-capability";
    case MessageType::PublishReservation: return "publish-reservation";
    case MessageType::PublishAck: return "publish-ack";
    case MessageType::Revoke: return "revoke";
    case MessageType::Bye: return "bye";
    case MessageType::Ping: return "ping";
    case MessageType::Pong: return "pong";
    case MessageType::RegistryStatus: return "registry-status";
  }
  return "invalid";
}

const char* to_string(WireStatus value) noexcept {
  switch (value) {
    case WireStatus::Ok: return "ok";
    case WireStatus::Rejected: return "rejected";
    case WireStatus::Fenced: return "fenced";
    case WireStatus::Unauthorized: return "unauthorized";
    case WireStatus::Stale: return "stale";
    case WireStatus::Conflict: return "conflict";
    case WireStatus::TooLarge: return "too-large";
    case WireStatus::NotFound: return "not-found";
    case WireStatus::Internal: return "internal";
  }
  return "invalid";
}

WireStatus status_from_error(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return WireStatus::Ok;
    case ErrorCode::InvalidArgument:
    case ErrorCode::Malformed: return WireStatus::Rejected;
    case ErrorCode::Fenced: return WireStatus::Fenced;
    case ErrorCode::Unauthorized: return WireStatus::Unauthorized;
    case ErrorCode::Stale: return WireStatus::Stale;
    case ErrorCode::Conflict:
    case ErrorCode::Duplicate: return WireStatus::Conflict;
    case ErrorCode::TooLarge: return WireStatus::TooLarge;
    case ErrorCode::NotFound: return WireStatus::NotFound;
    default: return WireStatus::Internal;
  }
}

std::vector<std::uint8_t> encode_frame(MessageType type, const std::vector<std::uint8_t>& body) {
  std::size_t envelope = 0;
  if (!checked_add<std::size_t>(kEnvelopeBytes, body.size(), envelope) ||
      envelope > limits::kMaxWireFrameBytes) {
    return {};
  }
  ByteWriter payload(limits::kMaxWireFrameBytes);
  payload.u16(kWireProtocolVersion);
  payload.u8(static_cast<std::uint8_t>(type));
  payload.u32(static_cast<std::uint32_t>(body.size()));
  payload.raw(body.data(), body.size());
  if (!payload.ok()) {
    return {};
  }
  const std::vector<std::uint8_t>& inner = payload.data();
  ByteWriter frame(kFrameHeaderBytes + inner.size());
  frame.u32(static_cast<std::uint32_t>(inner.size()));
  frame.u32(crc32c(inner.data(), inner.size()));
  frame.raw(inner.data(), inner.size());
  if (!frame.ok()) {
    return {};
  }
  return frame.data();
}

Result<Frame> decode_frame_prefix(const std::vector<std::uint8_t>& bytes,
                                  std::size_t& consumed) {
  consumed = 0;
  if (bytes.size() < kFrameHeaderBytes) {
    return make_error(ErrorCode::Malformed, "frame header is incomplete");
  }
  ByteReader header(bytes);
  const std::uint32_t length = header.u32();
  const std::uint32_t expected_crc = header.u32();
  if (header.failed()) {
    return make_error(ErrorCode::Malformed, "frame header is malformed");
  }
  if (length > limits::kMaxWireFrameBytes) {
    return make_error(ErrorCode::TooLarge, "frame length exceeds the wire budget");
  }
  std::size_t total = 0;
  if (!checked_add<std::size_t>(kFrameHeaderBytes, length, total)) {
    return make_error(ErrorCode::Overflow, "frame length arithmetic overflowed");
  }
  if (bytes.size() < total) {
    return make_error(ErrorCode::Malformed, "frame body is incomplete");
  }
  const auto* payload = bytes.data() + kFrameHeaderBytes;
  if (crc32c(payload, length) != expected_crc) {
    return make_error(ErrorCode::Corrupt, "frame checksum mismatch");
  }
  if (length < kEnvelopeBytes) {
    return make_error(ErrorCode::Malformed, "frame payload is too small to be an envelope");
  }
  ByteReader reader(payload, length);
  const std::uint16_t version = reader.u16();
  const std::uint8_t raw_type = reader.u8();
  const std::uint32_t body_length = reader.u32();
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "frame envelope is truncated");
  }
  if (version != kWireProtocolVersion) {
    return make_error(ErrorCode::VersionMismatch, "wire protocol version is not supported");
  }
  if (raw_type > kMaxMessageType) {
    return make_error(ErrorCode::Malformed, "message type is not recognised");
  }
  if (body_length > length - kEnvelopeBytes) {
    return make_error(ErrorCode::Malformed, "declared body length exceeds the frame");
  }
  const std::string_view body = reader.raw(body_length);
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "frame body is truncated");
  }
  if (!reader.at_end()) {
    return make_error(ErrorCode::Malformed, "frame carries trailing bytes");
  }
  Frame frame;
  frame.type = static_cast<MessageType>(raw_type);
  frame.body.assign(body.begin(), body.end());
  consumed = total;
  return frame;
}

Result<Frame> decode_frame(const std::vector<std::uint8_t>& bytes) {
  std::size_t consumed = 0;
  auto frame = decode_frame_prefix(bytes, consumed);
  if (!frame) {
    return frame.error();
  }
  if (consumed != bytes.size()) {
    return make_error(ErrorCode::Malformed, "buffer holds more than one frame");
  }
  return frame;
}

std::vector<std::uint8_t> encode_hello(const HelloMessage& value) {
  return encode_body([&value](ByteWriter& writer) {
    if (!is_valid(value.publisher)) {
      writer.fail();
      return;
    }
    writer.str(value.publisher.view(), limits::kMaxIdentifierLen);
    writer.u64(value.incarnation.value());
    writer.raw(value.boot.bytes().data(), BootId::kBytes);
    writer.u32(value.ttl_ms);
    write_text(writer, value.origin, limits::kMaxOriginLen);
  });
}

Result<HelloMessage> decode_hello(const std::vector<std::uint8_t>& body) {
  ByteReader reader(body);
  HelloMessage value;
  auto publisher = read_text(reader, limits::kMaxIdentifierLen);
  if (!publisher) {
    return publisher.error();
  }
  value.publisher = PublisherId{*publisher};
  value.incarnation = PublisherIncarnation{reader.u64()};
  const std::string_view boot = reader.raw(BootId::kBytes);
  value.ttl_ms = reader.u32();
  auto origin = read_text(reader, limits::kMaxOriginLen);
  if (!origin) {
    return origin.error();
  }
  value.origin = *origin;
  if (reader.failed() || !reader.at_end()) {
    return make_error(ErrorCode::Malformed, "hello message is malformed");
  }
  std::array<std::uint8_t, BootId::kBytes> bytes{};
  std::copy(boot.begin(), boot.end(), bytes.begin());
  value.boot = BootId{bytes};
  if (!is_valid(value.publisher) || value.incarnation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "hello message carries an invalid identity");
  }
  return value;
}

std::vector<std::uint8_t> encode_hello_ack(const HelloAckMessage& value) {
  return encode_body([&value](ByteWriter& writer) {
    writer.u8(static_cast<std::uint8_t>(value.status));
    writer.u64(value.epoch.value());
    writer.u64(value.lease.value());
    writer.u64(value.expires_at_ms);
    write_text(writer, value.detail, limits::kMaxReasonLen);
  });
}

Result<HelloAckMessage> decode_hello_ack(const std::vector<std::uint8_t>& body) {
  ByteReader reader(body);
  HelloAckMessage value;
  const std::uint8_t status = reader.u8();
  value.epoch = FabricEpoch{reader.u64()};
  value.lease = LeaseId{reader.u64()};
  value.expires_at_ms = reader.u64();
  auto detail = read_text(reader, limits::kMaxReasonLen);
  if (!detail) {
    return detail.error();
  }
  value.detail = *detail;
  if (reader.failed() || !reader.at_end() || status > kMaxWireStatus) {
    return make_error(ErrorCode::Malformed, "hello acknowledgement is malformed");
  }
  value.status = static_cast<WireStatus>(status);
  return value;
}

std::vector<std::uint8_t> encode_publish_ack(const PublishAckMessage& value) {
  return encode_body([&value](ByteWriter& writer) {
    writer.u8(static_cast<std::uint8_t>(value.status));
    writer.u64(value.generation.value());
    writer.u64(value.sequence.value());
    writer.raw(value.digest.bytes().data(), Digest256::kBytes);
    write_text(writer, value.detail, limits::kMaxReasonLen);
  });
}

Result<PublishAckMessage> decode_publish_ack(const std::vector<std::uint8_t>& body) {
  ByteReader reader(body);
  PublishAckMessage value;
  const std::uint8_t status = reader.u8();
  value.generation = Generation{reader.u64()};
  value.sequence = Sequence{reader.u64()};
  const std::string_view digest = reader.raw(Digest256::kBytes);
  auto detail = read_text(reader, limits::kMaxReasonLen);
  if (!detail) {
    return detail.error();
  }
  value.detail = *detail;
  if (reader.failed() || !reader.at_end() || status > kMaxWireStatus) {
    return make_error(ErrorCode::Malformed, "publish acknowledgement is malformed");
  }
  std::array<std::uint8_t, Digest256::kBytes> digest_bytes{};
  std::copy(digest.begin(), digest.end(), digest_bytes.begin());
  value.digest = Digest256{digest_bytes};
  value.status = static_cast<WireStatus>(status);
  return value;
}

std::vector<std::uint8_t> encode_registry_status(const RegistryStatusMessage& value) {
  return encode_body([&value](ByteWriter& writer) {
    writer.u64(value.epoch.value());
    writer.u64(value.classes);
    writer.u64(value.policies);
    writer.u64(value.paths);
    writer.u64(value.capabilities);
    writer.u64(value.decisions);
    writer.u64(value.publishers);
    writer.u64(value.conflicts);
  });
}

Result<RegistryStatusMessage> decode_registry_status(const std::vector<std::uint8_t>& body) {
  ByteReader reader(body);
  RegistryStatusMessage value;
  value.epoch = FabricEpoch{reader.u64()};
  value.classes = reader.u64();
  value.policies = reader.u64();
  value.paths = reader.u64();
  value.capabilities = reader.u64();
  value.decisions = reader.u64();
  value.publishers = reader.u64();
  value.conflicts = reader.u64();
  if (reader.failed() || !reader.at_end()) {
    return make_error(ErrorCode::Malformed, "registry status message is malformed");
  }
  return value;
}

}  // namespace qosfabric
