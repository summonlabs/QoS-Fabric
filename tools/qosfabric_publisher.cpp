// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// qosfabric_publisher: a real publisher agent process.
//
// It owns a durable boot counter, claims a lease from a registry server over a
// real framed TCP connection, and publishes resource capability declarations.
// Restarting it increments its durable incarnation, which is what lets the
// registry fence the previous process incarnation.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "qosfabric/net.hpp"
#include "qosfabric/registry.hpp"
#include "qosfabric/wire.hpp"

#ifdef _WIN32
#include <process.h>
#define QOSFABRIC_GETPID _getpid
#else
#include <unistd.h>
#define QOSFABRIC_GETPID getpid
#endif

namespace {

using namespace qosfabric;

struct Options {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string publisher_id{"publisher"};
  std::string boot_file{};
  std::uint32_t ttl_ms{60000};
  std::string resource{"resource"};
  std::string domain{"domain-0"};
  std::string state{"available"};
  std::uint64_t rate_bps{10'000'000};
  std::uint64_t latency_us{100};
  std::uint64_t jitter_us{10};
  std::uint32_t loss_ppm{0};
  std::string isolation{"queue-isolated"};
  std::string treatment{"rate-limited"};
  std::uint32_t priority_rank{4};
  std::vector<std::string> predicates{};
  std::uint32_t publishes{1};
  std::uint32_t interval_ms{0};
  std::uint32_t timeout_ms{10000};
  bool quiet{false};
};

void usage() {
  std::fputs(
      "usage: qosfabric_publisher --port N [options]\n"
      "  --host H            server host (loopback only)\n"
      "  --port N            server port (required)\n"
      "  --publisher-id ID   publisher identity (default: publisher)\n"
      "  --boot-file PATH    durable incarnation counter file\n"
      "  --ttl-ms N          requested lease duration\n"
      "  --resource ID       resource identity to publish for\n"
      "  --domain ID         failure domain the resource claims\n"
      "  --state S           available|degraded|unavailable|unknown\n"
      "  --rate N            supported rate in bits per second\n"
      "  --latency N         worst-case added latency in microseconds\n"
      "  --jitter N          worst-case added jitter in microseconds\n"
      "  --loss N            loss bound in parts per million\n"
      "  --isolation S       isolation ladder position\n"
      "  --treatment S       service treatment ladder position\n"
      "  --priority N        highest priority rank honoured\n"
      "  --predicates a,b    declared capability predicates\n"
      "  --publishes N       number of declarations to publish\n"
      "  --interval-ms N     delay between declarations\n"
      "  --timeout-ms N      receive timeout\n"
      "  --quiet             suppress progress lines\n",
      stderr);
}

Result<std::uint64_t> read_incarnation(const std::string& path) {
  if (path.empty()) {
    return std::uint64_t{0};
  }
  auto bytes = read_file(path, 64);
  if (!bytes) {
    if (bytes.error().code() == ErrorCode::NotFound) {
      return std::uint64_t{0};
    }
    return bytes.error();
  }
  std::string text(bytes->begin(), bytes->end());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.pop_back();
  }
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
    return make_error(ErrorCode::Corrupt, "boot file does not hold a decimal counter");
  }
  return std::strtoull(text.c_str(), nullptr, 10);
}

Result<void> write_incarnation(const std::string& path, std::uint64_t value) {
  if (path.empty()) {
    return {};
  }
  const std::string text = std::to_string(value) + "\n";
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return write_file_atomic(path, bytes);
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    auto next = [&](std::string& out) {
      if (i + 1 >= argc) {
        return false;
      }
      out = argv[++i];
      return true;
    };
    std::string value;
    if (flag == "--help" || flag == "-h") {
      usage();
      return false;
    } else if (flag == "--quiet") {
      options.quiet = true;
    } else if (flag == "--host" && next(value)) {
      options.host = value;
    } else if (flag == "--port" && next(value)) {
      options.port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--publisher-id" && next(value)) {
      options.publisher_id = value;
    } else if (flag == "--boot-file" && next(value)) {
      options.boot_file = value;
    } else if (flag == "--ttl-ms" && next(value)) {
      options.ttl_ms = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--resource" && next(value)) {
      options.resource = value;
    } else if (flag == "--domain" && next(value)) {
      options.domain = value;
    } else if (flag == "--state" && next(value)) {
      options.state = value;
    } else if (flag == "--rate" && next(value)) {
      options.rate_bps = std::strtoull(value.c_str(), nullptr, 10);
    } else if (flag == "--latency" && next(value)) {
      options.latency_us = std::strtoull(value.c_str(), nullptr, 10);
    } else if (flag == "--jitter" && next(value)) {
      options.jitter_us = std::strtoull(value.c_str(), nullptr, 10);
    } else if (flag == "--loss" && next(value)) {
      options.loss_ppm = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--isolation" && next(value)) {
      options.isolation = value;
    } else if (flag == "--treatment" && next(value)) {
      options.treatment = value;
    } else if (flag == "--priority" && next(value)) {
      options.priority_rank = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--predicates" && next(value)) {
      std::string token;
      for (const char c : value) {
        if (c == ',') {
          if (!token.empty()) {
            options.predicates.push_back(token);
          }
          token.clear();
        } else {
          token.push_back(c);
        }
      }
      if (!token.empty()) {
        options.predicates.push_back(token);
      }
    } else if (flag == "--publishes" && next(value)) {
      options.publishes = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--interval-ms" && next(value)) {
      options.interval_ms = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--timeout-ms" && next(value)) {
      options.timeout_ms = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else {
      std::fprintf(stderr, "unrecognised argument: %s\n", flag.c_str());
      usage();
      return false;
    }
  }
  if (options.port == 0) {
    std::fputs("--port is required\n", stderr);
    return false;
  }
  return true;
}

void report(const Options& options, const char* line) {
  if (!options.quiet) {
    std::fputs(line, stdout);
    std::fflush(stdout);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 1;
  }

  // Establish a strictly newer incarnation before claiming any authority. The
  // counter is durable: it is written and flushed before the lease is
  // requested, so a crash cannot cause an incarnation to be reused.
  auto previous = read_incarnation(options.boot_file);
  if (!previous) {
    std::fprintf(stderr, "boot counter read failed: %s\n", previous.error().message().c_str());
    return 1;
  }
  const std::uint64_t incarnation = *previous + 1;
  const auto persisted = write_incarnation(options.boot_file, incarnation);
  if (!persisted) {
    std::fprintf(stderr, "boot counter write failed: %s\n", persisted.error().message().c_str());
    return 1;
  }

  auto connected = connect_loopback(options.port, options.timeout_ms);
  if (!connected) {
    std::fprintf(stderr, "connect failed: %s\n", connected.error().message().c_str());
    return 1;
  }
  TcpSocket socket = std::move(*connected);

  const auto publisher = parse_string_id<PublisherIdTag>(options.publisher_id);
  if (!publisher) {
    std::fprintf(stderr, "invalid publisher id\n");
    return 1;
  }

  HelloMessage hello;
  hello.publisher = *publisher;
  hello.incarnation = PublisherIncarnation{incarnation};
  hello.boot = BootId::generate();
  hello.ttl_ms = options.ttl_ms;
  hello.origin = "qosfabric_publisher/1.0.0";
  const std::vector<std::uint8_t> hello_frame =
      encode_frame(MessageType::Hello, encode_hello(hello));
  if (hello_frame.empty()) {
    std::fputs("hello encoding failed\n", stderr);
    return 1;
  }
  const auto sent = socket.send_all(hello_frame);
  if (!sent) {
    std::fprintf(stderr, "hello send failed: %s\n", sent.error().message().c_str());
    return 1;
  }

  const auto ack_raw = socket.recv_frame();
  if (!ack_raw) {
    std::fprintf(stderr, "hello acknowledgement failed: %s\n", ack_raw.error().message().c_str());
    return 1;
  }
  const auto ack_frame = decode_frame(*ack_raw);
  if (!ack_frame || ack_frame->type != MessageType::HelloAck) {
    std::fputs("unexpected reply to hello\n", stderr);
    return 1;
  }
  const auto ack = decode_hello_ack(ack_frame->body);
  if (!ack) {
    std::fprintf(stderr, "hello acknowledgement malformed: %s\n", ack.error().message().c_str());
    return 1;
  }
  std::printf("HELLO %s incarnation=%llu epoch=%llu status=%s lease=%llu\n",
              options.publisher_id.c_str(), static_cast<unsigned long long>(incarnation),
              static_cast<unsigned long long>(ack->epoch.value()), to_string(ack->status),
              static_cast<unsigned long long>(ack->lease.value()));
  std::fflush(stdout);
  if (ack->status != WireStatus::Ok) {
    std::fprintf(stderr, "lease refused: %s\n", ack->detail.c_str());
    return 2;
  }

  for (std::uint32_t index = 0; index < options.publishes; ++index) {
    ResourceCapability capability;
    capability.resource = ResourceId{options.resource};
    // Generation zero asks the registry to assign the next generation, which
    // is the only safe way for a restarted publisher to continue the sequence
    // without guessing what it published before the restart.
    capability.generation = Generation{0};
    capability.epoch = ack->epoch;
    capability.publisher = *publisher;
    capability.incarnation = PublisherIncarnation{incarnation};
    capability.published_at_ms = system_now_millis();
    std::uint64_t ttl = options.ttl_ms;
    capability.expires_at_ms = capability.published_at_ms + ttl;
    if (!parse_capability_state(options.state, capability.state)) {
      std::fputs("invalid capability state\n", stderr);
      return 1;
    }
    capability.supported_rate_bps = options.rate_bps;
    capability.latency_bound_us = options.latency_us;
    capability.jitter_bound_us = options.jitter_us;
    capability.loss_ppm = options.loss_ppm;
    capability.max_isolation = IsolationLevel::QueueIsolated;
    if (!parse_isolation_level(options.isolation, capability.max_isolation)) {
      std::fputs("invalid isolation level\n", stderr);
      return 1;
    }
    if (!parse_treatment_mode(options.treatment, capability.max_treatment)) {
      std::fputs("invalid treatment mode\n", stderr);
      return 1;
    }
    capability.max_priority_rank = options.priority_rank;
    capability.max_burst_bytes = 65536;
    capability.burst_window_us = 1000;
    capability.mtu_bytes = 9000;
    for (const std::string& key : options.predicates) {
      const auto parsed = parse_string_id<CapabilityKeyTag>(key, limits::kMaxPredicateKeyLen);
      if (!parsed) {
        std::fputs("invalid predicate key\n", stderr);
        return 1;
      }
      capability.predicates.push_back(*parsed);
    }
    capability.diversity_domain = DiversityDomain{options.domain};

    // Generation is assigned by the registry: the placeholder is replaced
    // server-side because only the registry knows the retained generations.
    capability.generation = Generation{1};
    ByteWriter encoded(limits::kMaxCanonicalRecordBytes);
    canonical_encode(capability, encoded);
    if (!encoded.ok()) {
      std::fputs("capability encoding failed\n", stderr);
      return 1;
    }
    // Body layout: [u8 flags][canonical capability]. Flag bit 0 asks the
    // registry to assign the next generation rather than trusting the sender.
    std::vector<std::uint8_t> body;
    body.reserve(encoded.data().size() + 1);
    body.push_back(0x01);
    body.insert(body.end(), encoded.data().begin(), encoded.data().end());
    const std::vector<std::uint8_t> frame = encode_frame(MessageType::PublishCapability, body);
    if (frame.empty()) {
      std::fputs("frame encoding failed\n", stderr);
      return 1;
    }
    const auto published = socket.send_all(frame);
    if (!published) {
      std::fprintf(stderr, "publish failed: %s\n", published.error().message().c_str());
      return 1;
    }
    const auto reply_raw = socket.recv_frame();
    if (!reply_raw) {
      std::fprintf(stderr, "publish reply failed: %s\n", reply_raw.error().message().c_str());
      return 1;
    }
    const auto reply_frame = decode_frame(*reply_raw);
    if (!reply_frame || reply_frame->type != MessageType::PublishAck) {
      std::fputs("unexpected reply to publish\n", stderr);
      return 1;
    }
    const auto reply = decode_publish_ack(reply_frame->body);
    if (!reply) {
      std::fprintf(stderr, "publish reply malformed: %s\n", reply.error().message().c_str());
      return 1;
    }
    std::printf("PUBLISH %s generation=%llu sequence=%llu status=%s digest=%s\n",
                options.resource.c_str(),
                static_cast<unsigned long long>(reply->generation.value()),
                static_cast<unsigned long long>(reply->sequence.value()),
                to_string(reply->status), reply->digest.hex().c_str());
    std::fflush(stdout);
    if (reply->status != WireStatus::Ok) {
      std::fprintf(stderr, "declaration refused: %s\n", reply->detail.c_str());
      return 2;
    }
    if (options.interval_ms > 0 && index + 1 < options.publishes) {
      std::this_thread::sleep_for(std::chrono::milliseconds(options.interval_ms));
    }
  }

  report(options, "DONE\n");
  const std::vector<std::uint8_t> bye = encode_frame(MessageType::Bye, {});
  (void)socket.send_all(bye);
  return 0;
}
