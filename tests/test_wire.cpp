// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "harness.hpp"

#include <string>
#include <vector>

#include "qosfabric/qosfabric.hpp"

using namespace qosfabric;

QOS_TEST(frame_round_trips_and_checks_itself) {
  HelloMessage hello;
  hello.publisher = PublisherId{"bus-1"};
  hello.incarnation = PublisherIncarnation{7};
  hello.boot = BootId::generate();
  hello.ttl_ms = 60'000;
  hello.origin = "unit-test";

  const std::vector<std::uint8_t> body = encode_hello(hello);
  QOS_REQUIRE(!body.empty());
  const std::vector<std::uint8_t> frame = encode_frame(MessageType::Hello, body);
  QOS_REQUIRE(!frame.empty());

  const auto decoded = decode_frame(frame);
  QOS_REQUIRE(decoded.has_value());
  QOS_CHECK(decoded->type == MessageType::Hello);
  const auto parsed = decode_hello(decoded->body);
  QOS_REQUIRE(parsed.has_value());
  QOS_CHECK(parsed->publisher == hello.publisher);
  QOS_CHECK(parsed->incarnation == hello.incarnation);
  QOS_CHECK(parsed->boot == hello.boot);
  QOS_CHECK_EQ(parsed->ttl_ms, hello.ttl_ms);
  QOS_CHECK_EQ(parsed->origin, hello.origin);
}

QOS_TEST(every_single_byte_corruption_is_refused) {
  HelloAckMessage ack;
  ack.status = WireStatus::Ok;
  ack.epoch = FabricEpoch{4};
  ack.lease = LeaseId{9};
  ack.expires_at_ms = 1234;
  ack.detail = "granted";
  const std::vector<std::uint8_t> frame =
      encode_frame(MessageType::HelloAck, encode_hello_ack(ack));
  QOS_REQUIRE(!frame.empty());

  std::size_t refused = 0;
  for (std::size_t index = 0; index < frame.size(); ++index) {
    for (std::uint8_t bit = 0; bit < 8; ++bit) {
      std::vector<std::uint8_t> corrupted = frame;
      corrupted[index] = static_cast<std::uint8_t>(corrupted[index] ^ (1u << bit));
      if (!decode_frame(corrupted).has_value()) {
        ++refused;
      }
    }
  }
  // Every single-bit change either fails the checksum or fails a structural
  // bound. None of them may decode into a different well-formed message.
  std::size_t accepted = 0;
  for (std::size_t index = 0; index < frame.size(); ++index) {
    for (std::uint8_t bit = 0; bit < 8; ++bit) {
      std::vector<std::uint8_t> corrupted = frame;
      corrupted[index] = static_cast<std::uint8_t>(corrupted[index] ^ (1u << bit));
      const auto result = decode_frame(corrupted);
      if (result.has_value()) {
        ++accepted;
        QOS_CHECK_EQ(result->type, MessageType::HelloAck);
        QOS_CHECK(decode_hello_ack(result->body).has_value());
      }
    }
  }
  QOS_CHECK_EQ(refused + accepted, frame.size() * 8);
  QOS_CHECK(accepted == 0);
}

QOS_TEST(truncation_at_every_prefix_is_refused) {
  PublishAckMessage ack;
  ack.status = WireStatus::Ok;
  ack.generation = Generation{12};
  ack.sequence = Sequence{34};
  ack.digest = sha256("capability");
  ack.detail = "published";
  const std::vector<std::uint8_t> frame =
      encode_frame(MessageType::PublishAck, encode_publish_ack(ack));
  QOS_REQUIRE(frame.size() > 16);
  for (std::size_t cut = 0; cut < frame.size(); ++cut) {
    std::vector<std::uint8_t> partial(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(cut));
    QOS_CHECK(!decode_frame(partial).has_value());
  }
  const auto complete = decode_frame(frame);
  QOS_REQUIRE(complete.has_value());
  const auto parsed = decode_publish_ack(complete->body);
  QOS_REQUIRE(parsed.has_value());
  QOS_CHECK(parsed->generation == ack.generation);
  QOS_CHECK(parsed->digest == ack.digest);
}

QOS_TEST(oversized_and_trailing_frames_are_refused) {
  // A declared length beyond the wire budget is refused before any body work.
  std::vector<std::uint8_t> hostile;
  const std::uint32_t huge = static_cast<std::uint32_t>(limits::kMaxWireFrameBytes + 1);
  for (std::size_t i = 0; i < 4; ++i) {
    hostile.push_back(static_cast<std::uint8_t>((huge >> (i * 8)) & 0xFFu));
  }
  for (std::size_t i = 0; i < 4; ++i) {
    hostile.push_back(0);
  }
  QOS_CHECK_EQ(decode_frame(hostile).code(), ErrorCode::TooLarge);

  const std::vector<std::uint8_t> valid =
      encode_frame(MessageType::Ping, std::vector<std::uint8_t>{});
  QOS_REQUIRE(!valid.empty());
  std::vector<std::uint8_t> trailing = valid;
  trailing.push_back(0x00);
  QOS_CHECK(!decode_frame(trailing).has_value());
}

QOS_TEST(protocol_version_mismatch_is_refused) {
  const std::vector<std::uint8_t> frame =
      encode_frame(MessageType::Ping, std::vector<std::uint8_t>{});
  QOS_REQUIRE(frame.size() > 8);
  std::vector<std::uint8_t> future = frame;
  // The protocol version is the first two bytes of the payload.
  future[8] = 0xFF;
  future[9] = 0xFF;
  // Rewrite the checksum so the frame is structurally valid but versioned
  // differently: the decoder must refuse it on the version, not the checksum.
  const std::uint32_t crc = crc32c(future.data() + 8, future.size() - 8);
  for (std::size_t i = 0; i < 4; ++i) {
    future[4 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
  }
  QOS_CHECK_EQ(decode_frame(future).code(), ErrorCode::VersionMismatch);
}

QOS_TEST(unknown_message_types_are_refused) {
  std::vector<std::uint8_t> body;
  const std::vector<std::uint8_t> frame = encode_frame(MessageType::Ping, body);
  QOS_REQUIRE(frame.size() > 8);
  std::vector<std::uint8_t> unknown = frame;
  unknown[10] = 200;  // message type byte
  const std::uint32_t crc = crc32c(unknown.data() + 8, unknown.size() - 8);
  for (std::size_t i = 0; i < 4; ++i) {
    unknown[4 + i] = static_cast<std::uint8_t>((crc >> (i * 8)) & 0xFFu);
  }
  QOS_CHECK_EQ(decode_frame(unknown).code(), ErrorCode::Malformed);
}

QOS_TEST(message_bodies_are_not_frames) {
  // A message encoder produces a body, and framing is a separate explicit
  // step. Wrapping a body twice must therefore be detectable rather than
  // silently accepted as a different message.
  const std::vector<std::uint8_t> body = encode_hello([&] {
    HelloMessage hello;
    hello.publisher = PublisherId{"bus-1"};
    hello.incarnation = PublisherIncarnation{1};
    hello.boot = BootId::generate();
    hello.ttl_ms = 1'000;
    hello.origin = "unit-test";
    return hello;
  }());
  QOS_REQUIRE(!body.empty());
  QOS_CHECK(!decode_frame(body).has_value());

  const std::vector<std::uint8_t> frame = encode_frame(MessageType::Hello, body);
  QOS_REQUIRE(!frame.empty());
  const auto decoded = decode_frame(frame);
  QOS_REQUIRE(decoded.has_value());
  QOS_CHECK(decoded->body == body);

  // A frame within a frame decodes to a body that is itself a frame, which is
  // exactly the shape a double-wrapping defect produces.
  const std::vector<std::uint8_t> double_wrapped = encode_frame(MessageType::Hello, frame);
  const auto outer = decode_frame(double_wrapped);
  QOS_REQUIRE(outer.has_value());
  QOS_CHECK(outer->body == frame);
  QOS_CHECK(decode_frame(outer->body).has_value());
}

QOS_TEST(status_mapping_never_leaks_internal_detail) {
  QOS_CHECK(status_from_error(ErrorCode::Ok) == WireStatus::Ok);
  QOS_CHECK(status_from_error(ErrorCode::Fenced) == WireStatus::Fenced);
  QOS_CHECK(status_from_error(ErrorCode::Unauthorized) == WireStatus::Unauthorized);
  QOS_CHECK(status_from_error(ErrorCode::Stale) == WireStatus::Stale);
  QOS_CHECK(status_from_error(ErrorCode::Conflict) == WireStatus::Conflict);
  QOS_CHECK(status_from_error(ErrorCode::Corrupt) == WireStatus::Internal);
  QOS_CHECK(status_from_error(ErrorCode::IoError) == WireStatus::Internal);
  QOS_CHECK(status_from_error(ErrorCode::Internal) == WireStatus::Internal);
}

QOS_TEST(registry_status_round_trips) {
  RegistryStatusMessage status;
  status.epoch = FabricEpoch{11};
  status.classes = 3;
  status.policies = 2;
  status.paths = 5;
  status.capabilities = 8;
  status.decisions = 13;
  status.publishers = 1;
  status.conflicts = 0;
  const std::vector<std::uint8_t> frame =
      encode_frame(MessageType::RegistryStatus, encode_registry_status(status));
  const auto decoded = decode_frame(frame);
  QOS_REQUIRE(decoded.has_value());
  const auto parsed = decode_registry_status(decoded->body);
  QOS_REQUIRE(parsed.has_value());
  QOS_CHECK(parsed->epoch == status.epoch);
  QOS_CHECK_EQ(parsed->capabilities, status.capabilities);
  QOS_CHECK_EQ(parsed->decisions, status.decisions);
}
