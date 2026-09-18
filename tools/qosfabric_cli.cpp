// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// qosfabric_cli: operator tool and the registry server used by the
// multi-process validation suites.
//
// Subcommands:
//   init      create a durable registry directory
//   status    report epoch, store identity and durable counts
//   verify    re-verify every durable artifact
//   compact   write a snapshot and retire the log prefix
//   scenario  execute a line-oriented scenario against a registry
//   serve     run the registry publisher server on loopback
//   selftest  run the built-in self checks

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "qosfabric/net.hpp"
#include "qosfabric/registry.hpp"
#include "qosfabric/version.hpp"
#include "qosfabric/wire.hpp"

namespace {

using namespace qosfabric;

void usage() {
  std::fputs(
      "usage: qosfabric_cli <command> [options]\n"
      "  init     --store DIR [--quiet]\n"
      "  status   --store DIR\n"
      "  verify   --store DIR\n"
      "  compact  --store DIR\n"
      "  scenario --store DIR --file FILE\n"
      "  serve    --store DIR [--port N] [--port-file PATH] [--once] [--quiet]\n"
      "  selftest\n"
      "  version\n",
      stderr);
}

struct Options {
  std::string store{};
  std::string file{};
  std::string port_file{};
  std::uint16_t port{0};
  bool once{false};
  bool observational{false};
};

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 2; i < argc; ++i) {
    const std::string flag = argv[i];
    auto next = [&](std::string& out) {
      if (i + 1 >= argc) {
        return false;
      }
      out = argv[++i];
      return true;
    };
    std::string value;
    if (flag == "--store" && next(value)) {
      options.store = value;
    } else if (flag == "--file" && next(value)) {
      options.file = value;
    } else if (flag == "--port" && next(value)) {
      options.port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--port-file" && next(value)) {
      options.port_file = value;
    } else if (flag == "--once") {
      options.once = true;
    } else if (flag == "--quiet") {
      // Recognised for scripting symmetry; all commands are already quiet.
    } else {
      std::fprintf(stderr, "unrecognised argument: %s\n", flag.c_str());
      return false;
    }
  }
  return true;
}

Result<std::unique_ptr<Registry>> open_registry(const Options& options) {
  if (options.store.empty()) {
    return make_error(ErrorCode::InvalidArgument, "--store DIR is required");
  }
  RegistryOptions registry_options;
  registry_options.directory = options.store;
  registry_options.create_if_missing = true;
  registry_options.claim_authority = !options.observational;
  return Registry::Open(registry_options);
}

void report_error(const Error& error) {
  std::fprintf(stderr, "error: %s\n", error.message().c_str());
}

// --- scenario engine -------------------------------------------------------
class Scenario {
 public:
  explicit Scenario(Registry& registry) : registry_(registry) {}

  Result<void> run(const std::string& path);

 private:
  Result<void> execute(const std::string& verb, const std::vector<std::string>& tokens);
  Result<void> tokenize(const std::string& line, std::vector<std::string>& out) const;

  Registry& registry_;
  std::map<std::string, std::string> named_{};
  std::vector<std::string> positional_{};
  std::uint64_t now_ms_{system_now_millis()};
};

Result<void> Scenario::tokenize(const std::string& line, std::vector<std::string>& out) const {
  out.clear();
  std::string token;
  for (const char c : line) {
    if (c == ' ' || c == '\t' || c == '\r') {
      if (!token.empty()) {
        out.push_back(token);
        token.clear();
      }
    } else {
      token.push_back(c);
    }
  }
  if (!token.empty()) {
    out.push_back(token);
  }
  return {};
}

namespace {

std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> out;
  std::string token;
  for (const char c : text) {
    if (c == separator) {
      out.push_back(token);
      token.clear();
    } else {
      token.push_back(c);
    }
  }
  out.push_back(token);
  return out;
}

Result<std::uint64_t> to_u64(const std::string& text, const char* what) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
    return make_error(ErrorCode::InvalidArgument, std::string(what) + " must be a decimal integer");
  }
  return std::strtoull(text.c_str(), nullptr, 10);
}

Result<bool> to_bool(const std::string& text, const char* what) {
  if (text == "yes" || text == "true" || text == "1") {
    return true;
  }
  if (text == "no" || text == "false" || text == "0") {
    return false;
  }
  return make_error(ErrorCode::InvalidArgument, std::string(what) + " must be yes or no");
}

Result<CapabilityKey> to_predicate(const std::string& text) {
  return parse_string_id<CapabilityKeyTag>(text, limits::kMaxPredicateKeyLen);
}

}  // namespace

Result<void> Scenario::execute(const std::string& verb,
                               const std::vector<std::string>& tokens) {
  named_.clear();
  positional_.clear();
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    const std::size_t equals = token.find('=');
    if (equals == std::string::npos) {
      positional_.push_back(token);
    } else {
      named_[token.substr(0, equals)] = token.substr(equals + 1);
    }
  }
  auto get = [this](const char* key) -> std::string {
    const auto found = named_.find(key);
    return found == named_.end() ? std::string{} : found->second;
  };
  auto has = [this](const char* key) { return named_.find(key) != named_.end(); };

  if (verb == "now") {
    if (positional_.size() != 1) {
      return make_error(ErrorCode::InvalidArgument, "now requires one argument");
    }
    auto value = to_u64(positional_[0], "now");
    if (!value) {
      return value.error();
    }
    now_ms_ = *value;
    registry_.set_now_millis(now_ms_);
    std::printf("NOW %llu\n", static_cast<unsigned long long>(now_ms_));
    return {};
  }

  if (verb == "advance-epoch") {
    registry_.set_now_millis(now_ms_);
    auto epoch = registry_.AdvanceEpoch("scenario");
    if (!epoch) {
      return epoch.error();
    }
    std::printf("EPOCH %llu\n", static_cast<unsigned long long>(epoch->value()));
    return {};
  }

  if (verb == "compact") {
    return registry_.Compact();
  }

  if (verb == "publisher") {
    if (positional_.size() != 2) {
      return make_error(ErrorCode::InvalidArgument, "publisher requires <id> <incarnation>");
    }
    auto id = parse_string_id<PublisherIdTag>(positional_[0]);
    if (!id) {
      return id.error();
    }
    auto incarnation = to_u64(positional_[1], "incarnation");
    if (!incarnation) {
      return incarnation.error();
    }
    std::uint64_t ttl = 60000;
    if (has("ttl_ms")) {
      auto value = to_u64(get("ttl_ms"), "ttl_ms");
      if (!value) {
        return value.error();
      }
      ttl = *value;
    }
    registry_.set_now_millis(now_ms_);
    auto lease = registry_.RegisterPublisher(*id, PublisherIncarnation{*incarnation},
                                             BootId::generate(), ttl, "scenario");
    if (!lease) {
      return lease.error();
    }
    std::printf("PUBLISHER %s incarnation=%llu lease=%llu epoch=%llu\n", id->str().c_str(),
                static_cast<unsigned long long>(*incarnation),
                static_cast<unsigned long long>(lease->lease.value()),
                static_cast<unsigned long long>(lease->epoch.value()));
    return {};
  }

  if (verb == "class") {
    if (positional_.size() != 2) {
      return make_error(ErrorCode::InvalidArgument, "class requires <id> <generation>");
    }
    QoSClass value;
    auto id = parse_string_id<QoSClassIdTag>(positional_[0]);
    if (!id) {
      return id.error();
    }
    auto generation = to_u64(positional_[1], "generation");
    if (!generation) {
      return generation.error();
    }
    value.id = *id;
    value.generation = Generation{*generation};
    value.label = get("label");
    auto set_u64 = [&](const char* key, std::optional<std::uint64_t>& target) -> Result<void> {
      if (!has(key)) {
        return {};
      }
      auto parsed = to_u64(get(key), key);
      if (!parsed) {
        return parsed.error();
      }
      target = *parsed;
      return {};
    };
    if (auto ok = set_u64("min_rate", value.min_rate_bps); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("max_rate", value.max_rate_bps); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("peak_rate", value.peak_rate_bps); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("latency", value.max_latency_us); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("jitter", value.max_jitter_us); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("burst", value.burst_bytes); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("burst_interval", value.burst_interval_us); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("mtu", value.min_mtu_bytes); !ok) {
      return ok.error();
    }
    if (has("loss")) {
      auto parsed = to_u64(get("loss"), "loss");
      if (!parsed) {
        return parsed.error();
      }
      value.max_loss_ppm = static_cast<std::uint32_t>(*parsed);
    }
    if (has("isolation") && !parse_isolation_level(get("isolation"), value.isolation)) {
      return make_error(ErrorCode::InvalidArgument, "invalid isolation level");
    }
    if (has("treatment") && !parse_treatment_mode(get("treatment"), value.treatment)) {
      return make_error(ErrorCode::InvalidArgument, "invalid treatment mode");
    }
    if (has("priority")) {
      auto parsed = to_u64(get("priority"), "priority");
      if (!parsed) {
        return parsed.error();
      }
      value.min_priority_rank = static_cast<std::uint32_t>(*parsed);
    }
    if (has("disjoint")) {
      auto parsed = to_u64(get("disjoint"), "disjoint");
      if (!parsed) {
        return parsed.error();
      }
      value.min_disjoint_paths = static_cast<std::uint32_t>(*parsed);
    }
    if (has("reservation")) {
      auto parsed = to_bool(get("reservation"), "reservation");
      if (!parsed) {
        return parsed.error();
      }
      value.requires_reservation = *parsed;
    }
    if (has("predicates")) {
      for (const std::string& key : split(get("predicates"), ',')) {
        if (key.empty()) {
          continue;
        }
        auto predicate = to_predicate(key);
        if (!predicate) {
          return predicate.error();
        }
        value.required_predicates.push_back(*predicate);
      }
    }
    if (has("degrade")) {
      auto parsed = to_bool(get("degrade"), "degrade");
      if (!parsed) {
        return parsed.error();
      }
      value.allow_degrade = *parsed;
    }
    if (has("relax_ppm")) {
      auto parsed = to_u64(get("relax_ppm"), "relax_ppm");
      if (!parsed) {
        return parsed.error();
      }
      value.degrade_max_relaxation_ppm = static_cast<std::uint32_t>(*parsed);
    }
    if (has("violation") && !parse_violation_policy(get("violation"), value.violation_policy)) {
      return make_error(ErrorCode::InvalidArgument, "invalid violation policy");
    }
    registry_.set_now_millis(now_ms_);
    auto published = registry_.PublishClass(value, "scenario");
    if (!published) {
      return published.error();
    }
    std::printf("CLASS %s@%llu digest=%s\n", id->str().c_str(),
                static_cast<unsigned long long>(published->generation.value()),
                published->digest.hex().c_str());
    return {};
  }

  if (verb == "policy") {
    if (positional_.size() != 2) {
      return make_error(ErrorCode::InvalidArgument, "policy requires <id> <generation>");
    }
    Policy value;
    auto id = parse_string_id<PolicyIdTag>(positional_[0]);
    if (!id) {
      return id.error();
    }
    auto generation = to_u64(positional_[1], "generation");
    if (!generation) {
      return generation.error();
    }
    value.id = *id;
    value.generation = Generation{*generation};
    value.label = get("label");
    value.capability_max_age_ms = 600000;
    if (has("degrade")) {
      auto parsed = to_bool(get("degrade"), "degrade");
      if (!parsed) {
        return parsed.error();
      }
      value.degrade_permitted = *parsed;
    }
    if (has("relax_ppm")) {
      auto parsed = to_u64(get("relax_ppm"), "relax_ppm");
      if (!parsed) {
        return parsed.error();
      }
      value.max_relaxation_ppm = static_cast<std::uint32_t>(*parsed);
    }
    if (has("stale_epochs")) {
      auto parsed = to_u64(get("stale_epochs"), "stale_epochs");
      if (!parsed) {
        return parsed.error();
      }
      value.max_stale_epochs = static_cast<std::uint32_t>(*parsed);
    }
    if (has("max_age_ms")) {
      auto parsed = to_u64(get("max_age_ms"), "max_age_ms");
      if (!parsed) {
        return parsed.error();
      }
      value.capability_max_age_ms = *parsed;
    }
    if (has("unknown")) {
      auto parsed = to_u64(get("unknown"), "unknown");
      if (!parsed) {
        return parsed.error();
      }
      value.max_unknown_components = static_cast<std::uint32_t>(*parsed);
    }
    if (has("predicates")) {
      for (const std::string& key : split(get("predicates"), ',')) {
        if (key.empty()) {
          continue;
        }
        auto predicate = to_predicate(key);
        if (!predicate) {
          return predicate.error();
        }
        value.required_predicates.push_back(*predicate);
      }
    }
    if (has("denied")) {
      for (const std::string& key : split(get("denied"), ',')) {
        if (key.empty()) {
          continue;
        }
        auto denied = parse_string_id<QoSClassIdTag>(key);
        if (!denied) {
          return denied.error();
        }
        value.denied_classes.push_back(*denied);
      }
    }
    registry_.set_now_millis(now_ms_);
    auto published = registry_.PublishPolicy(value, "scenario");
    if (!published) {
      return published.error();
    }
    std::printf("POLICY %s@%llu digest=%s\n", id->str().c_str(),
                static_cast<unsigned long long>(published->generation.value()),
                published->digest.hex().c_str());
    return {};
  }

  if (verb == "path") {
    if (positional_.size() != 2) {
      return make_error(ErrorCode::InvalidArgument, "path requires <id> <generation>");
    }
    Path value;
    auto id = parse_string_id<PathIdTag>(positional_[0]);
    if (!id) {
      return id.error();
    }
    auto generation = to_u64(positional_[1], "generation");
    if (!generation) {
      return generation.error();
    }
    value.id = *id;
    value.generation = Generation{*generation};
    if (has("fixed_latency")) {
      auto parsed = to_u64(get("fixed_latency"), "fixed_latency");
      if (!parsed) {
        return parsed.error();
      }
      value.fixed_latency_us = *parsed;
    }
    if (has("fixed_jitter")) {
      auto parsed = to_u64(get("fixed_jitter"), "fixed_jitter");
      if (!parsed) {
        return parsed.error();
      }
      value.fixed_jitter_us = *parsed;
    }
    if (!has("components")) {
      return make_error(ErrorCode::InvalidArgument, "path requires components=r:gen:domain:role,...");
    }
    for (const std::string& item : split(get("components"), ',')) {
      const std::vector<std::string> fields = split(item, ':');
      if (fields.size() != 4) {
        return make_error(ErrorCode::InvalidArgument,
                          "path component must be resource:generation:domain:role");
      }
      PathComponent component;
      auto resource = parse_string_id<ResourceIdTag>(fields[0]);
      if (!resource) {
        return resource.error();
      }
      auto capability_generation = to_u64(fields[1], "component generation");
      if (!capability_generation) {
        return capability_generation.error();
      }
      auto domain = parse_string_id<DiversityDomainTag>(fields[2]);
      if (!domain) {
        return domain.error();
      }
      component.resource = *resource;
      component.capability_generation = Generation{*capability_generation};
      component.diversity_domain = *domain;
      if (!parse_path_role(fields[3], component.role)) {
        return make_error(ErrorCode::InvalidArgument, "invalid path component role");
      }
      value.components.push_back(std::move(component));
    }
    registry_.set_now_millis(now_ms_);
    auto published = registry_.PublishPath(value, "scenario");
    if (!published) {
      return published.error();
    }
    std::printf("PATH %s@%llu digest=%s\n", id->str().c_str(),
                static_cast<unsigned long long>(published->generation.value()),
                published->digest.hex().c_str());
    return {};
  }

  if (verb == "capability") {
    if (positional_.size() != 2) {
      return make_error(ErrorCode::InvalidArgument, "capability requires <resource> <generation>");
    }
    ResourceCapability value;
    auto resource = parse_string_id<ResourceIdTag>(positional_[0]);
    if (!resource) {
      return resource.error();
    }
    auto generation = to_u64(positional_[1], "generation");
    if (!generation) {
      return generation.error();
    }
    value.resource = *resource;
    value.generation = Generation{*generation};
    auto publisher = parse_string_id<PublisherIdTag>(get("publisher"));
    if (!publisher) {
      return make_error(ErrorCode::InvalidArgument, "capability requires publisher=<id>");
    }
    auto incarnation = to_u64(get("incarnation"), "incarnation");
    if (!incarnation) {
      return incarnation.error();
    }
    value.publisher = *publisher;
    value.incarnation = PublisherIncarnation{*incarnation};
    // An assertion that does not name an epoch asserts the current one; naming
    // a different epoch is how a scenario exercises stale-epoch rejection.
    value.epoch = has("epoch") ? FabricEpoch{std::strtoull(get("epoch").c_str(), nullptr, 10)}
                               : registry_.current_epoch();
    value.published_at_ms = has("published") ? std::strtoull(get("published").c_str(), nullptr, 10)
                                             : now_ms_;
    value.expires_at_ms = has("expires") ? std::strtoull(get("expires").c_str(), nullptr, 10)
                                         : value.published_at_ms + 600000;
    if (has("state") && !parse_capability_state(get("state"), value.state)) {
      return make_error(ErrorCode::InvalidArgument, "invalid capability state");
    }
    auto set_u64 = [&](const char* key, std::optional<std::uint64_t>& target) -> Result<void> {
      if (!has(key)) {
        return {};
      }
      auto parsed = to_u64(get(key), key);
      if (!parsed) {
        return parsed.error();
      }
      target = *parsed;
      return {};
    };
    if (auto ok = set_u64("rate", value.supported_rate_bps); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("latency", value.latency_bound_us); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("jitter", value.jitter_bound_us); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("burst", value.max_burst_bytes); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("burst_window", value.burst_window_us); !ok) {
      return ok.error();
    }
    if (auto ok = set_u64("mtu", value.mtu_bytes); !ok) {
      return ok.error();
    }
    if (has("loss")) {
      auto parsed = to_u64(get("loss"), "loss");
      if (!parsed) {
        return parsed.error();
      }
      value.loss_ppm = static_cast<std::uint32_t>(*parsed);
    }
    if (has("isolation")) {
      if (!parse_isolation_level(get("isolation"), value.max_isolation)) {
        return make_error(ErrorCode::InvalidArgument, "invalid isolation level");
      }
    }
    if (has("treatment")) {
      if (!parse_treatment_mode(get("treatment"), value.max_treatment)) {
        return make_error(ErrorCode::InvalidArgument, "invalid treatment mode");
      }
    }
    if (has("priority")) {
      auto parsed = to_u64(get("priority"), "priority");
      if (!parsed) {
        return parsed.error();
      }
      value.max_priority_rank = static_cast<std::uint32_t>(*parsed);
    }
    if (has("domain")) {
      auto domain = parse_string_id<DiversityDomainTag>(get("domain"));
      if (!domain) {
        return domain.error();
      }
      value.diversity_domain = *domain;
    }
    if (has("predicates")) {
      for (const std::string& key : split(get("predicates"), ',')) {
        if (key.empty()) {
          continue;
        }
        auto predicate = to_predicate(key);
        if (!predicate) {
          return predicate.error();
        }
        value.predicates.push_back(*predicate);
      }
    }
    registry_.set_now_millis(now_ms_);
    auto published = registry_.PublishCapability(value);
    if (!published) {
      return published.error();
    }
    std::printf("CAPABILITY %s@%llu digest=%s\n", resource->str().c_str(),
                static_cast<unsigned long long>(published->generation.value()),
                published->digest.hex().c_str());
    return {};
  }

  if (verb == "reservation") {
    if (positional_.size() != 2) {
      return make_error(ErrorCode::InvalidArgument, "reservation requires <resource> <generation>");
    }
    Reservation value;
    auto resource = parse_string_id<ResourceIdTag>(positional_[0]);
    if (!resource) {
      return resource.error();
    }
    auto generation = to_u64(positional_[1], "generation");
    if (!generation) {
      return generation.error();
    }
    value.resource = *resource;
    value.generation = Generation{*generation};
    const std::vector<std::string> class_ref = split(get("class"), ':');
    if (class_ref.size() != 2) {
      return make_error(ErrorCode::InvalidArgument, "reservation requires class=<id>:<generation>");
    }
    auto class_id = parse_string_id<QoSClassIdTag>(class_ref[0]);
    if (!class_id) {
      return class_id.error();
    }
    auto class_generation = to_u64(class_ref[1], "class generation");
    if (!class_generation) {
      return class_generation.error();
    }
    value.class_id = *class_id;
    value.class_generation = Generation{*class_generation};
    auto rate = to_u64(get("rate"), "rate");
    if (!rate) {
      return rate.error();
    }
    value.reserved_min_rate_bps = *rate;
    if (has("burst")) {
      auto burst = to_u64(get("burst"), "burst");
      if (!burst) {
        return burst.error();
      }
      value.reserved_burst_bytes = *burst;
    }
    auto publisher = parse_string_id<PublisherIdTag>(get("publisher"));
    if (!publisher) {
      return make_error(ErrorCode::InvalidArgument, "reservation requires publisher=<id>");
    }
    auto incarnation = to_u64(get("incarnation"), "incarnation");
    if (!incarnation) {
      return incarnation.error();
    }
    value.publisher = *publisher;
    value.incarnation = PublisherIncarnation{*incarnation};
    value.epoch = has("epoch") ? FabricEpoch{std::strtoull(get("epoch").c_str(), nullptr, 10)}
                               : registry_.current_epoch();
    value.expires_at_ms = has("expires") ? std::strtoull(get("expires").c_str(), nullptr, 10)
                                         : now_ms_ + 600000;
    registry_.set_now_millis(now_ms_);
    auto published = registry_.PublishReservation(value);
    if (!published) {
      return published.error();
    }
    std::printf("RESERVATION %s@%llu digest=%s\n", resource->str().c_str(),
                static_cast<unsigned long long>(published->generation.value()),
                published->digest.hex().c_str());
    return {};
  }

  if (verb == "request") {
    if (positional_.size() != 2) {
      return make_error(ErrorCode::InvalidArgument, "request requires <contract> <generation>");
    }
    ContractRequest request;
    auto contract = parse_string_id<QoSContractIdTag>(positional_[0]);
    if (!contract) {
      return contract.error();
    }
    auto generation = to_u64(positional_[1], "generation");
    if (!generation) {
      return generation.error();
    }
    request.contract = *contract;
    request.contract_generation = Generation{*generation};
    auto set_ref = [&](const char* key, auto& target, auto& target_generation) -> Result<void> {
      const std::vector<std::string> parts = split(get(key), ':');
      if (parts.size() != 2) {
        return make_error(ErrorCode::InvalidArgument, std::string(key) + "=<id>:<generation>");
      }
      auto id = parse_string_id<typename std::decay_t<decltype(target)>::tag_type>(parts[0]);
      if (!id) {
        return id.error();
      }
      auto parsed = to_u64(parts[1], key);
      if (!parsed) {
        return parsed.error();
      }
      target = *id;
      target_generation = Generation{*parsed};
      return {};
    };
    if (auto ok = set_ref("subject", request.subject, request.subject_generation); !ok) {
      return ok.error();
    }
    if (auto ok = set_ref("class", request.class_id, request.class_generation); !ok) {
      return ok.error();
    }
    if (auto ok = set_ref("path", request.path, request.path_generation); !ok) {
      return ok.error();
    }
    if (auto ok = set_ref("policy", request.policy, request.policy_generation); !ok) {
      return ok.error();
    }
    if (auto ok = set_ref("priority", request.priority, request.priority_generation); !ok) {
      return ok.error();
    }
    if (has("epoch")) {
      auto parsed = to_u64(get("epoch"), "epoch");
      if (!parsed) {
        return parsed.error();
      }
      request.observed_epoch = FabricEpoch{*parsed};
    }
    request.request_ms = now_ms_;
    registry_.set_now_millis(now_ms_);
    auto decision = registry_.Evaluate(request);
    if (!decision) {
      return decision.error();
    }
    std::printf("REQUEST %s@%llu\n", contract->str().c_str(),
                static_cast<unsigned long long>(*generation));
    std::printf("OUTCOME %s\n", to_string(decision->outcome));
    std::printf("REASON %s\n", decision->primary_reason.c_str());
    std::printf("DECISION-DIGEST %s\n", decision->decision_digest.hex().c_str());
    if (decision->binding_obligation.has_value()) {
      std::printf("BINDING %s %s\n", obligation_name(*decision->binding_obligation).data(),
                  decision->binding_resource.has_value()
                      ? decision->binding_resource->str().c_str()
                      : "-");
    }
    for (const ObligationAssessment& item : decision->obligations) {
      std::printf("OBLIGATION %s %s required=%llu composed=", item.id.str().c_str(),
                  to_string(item.status),
                  static_cast<unsigned long long>(item.required_value));
      if (item.composed_value.has_value()) {
        std::printf("%llu", static_cast<unsigned long long>(*item.composed_value));
      } else {
        std::printf("UNKNOWN");
      }
      std::printf(" relax=%u headroom=%u\n", item.relaxation_applied_ppm, item.headroom_ppm);
    }
    for (const ResourceAssessment& item : decision->resources) {
      std::printf("RESOURCE %s %s domain=%s\n", item.resource.str().c_str(),
                  to_string(item.status), item.diversity_domain.str().c_str());
    }
    for (const DegradationReason& item : decision->degradations) {
      std::printf("DEGRADATION %s %u %s\n", item.id.str().c_str(), item.relaxation_ppm,
                  item.reported_only ? "reported" : "applied");
    }
    return {};
  }

  return make_error(ErrorCode::InvalidArgument, "unrecognised scenario verb: " + verb);
}

Result<void> Scenario::run(const std::string& path) {
  auto bytes = read_file(path, 1u << 20);
  if (!bytes) {
    return bytes.error();
  }
  const std::string text(bytes->begin(), bytes->end());
  std::size_t start = 0;
  std::size_t line_number = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line =
        text.substr(start, (end == std::string::npos ? text.size() : end) - start);
    start = (end == std::string::npos) ? text.size() + 1 : end + 1;
    ++line_number;
    const std::size_t comment = line.find('#');
    const std::string trimmed =
        (comment == std::string::npos) ? line : line.substr(0, comment);
    std::vector<std::string> tokens;
    const auto tokenized = tokenize(trimmed, tokens);
    if (!tokenized) {
      return tokenized.error();
    }
    if (tokens.empty()) {
      continue;
    }
    const auto executed = execute(tokens[0], tokens);
    if (!executed) {
      return Error{executed.error().code(),
                   "line " + std::to_string(line_number) + ": " + executed.error().detail()};
    }
  }
  return {};
}

// --- server ---------------------------------------------------------------
int run_server(const Options& options) {
  auto registry = open_registry(options);
  if (!registry) {
    report_error(registry.error());
    return 1;
  }
  auto listener = listen_loopback(options.port, limits::kDefaultListenBacklog);
  if (!listener) {
    report_error(listener.error());
    return 1;
  }
  const auto port = local_port(*listener);
  if (!port) {
    report_error(port.error());
    return 1;
  }
  std::printf("LISTEN %u\n", static_cast<unsigned>(*port));
  std::printf("STORE %s\n", (*registry)->store_id().c_str());
  std::fflush(stdout);
  if (!options.port_file.empty()) {
    // Publishing the chosen port through a file lets a supervising process
    // discover an ephemeral port without a pipe and without polling stdout.
    const std::string text = std::to_string(*port) + "\n";
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    const auto written = write_file_atomic(options.port_file, bytes);
    if (!written) {
      report_error(written.error());
      return 1;
    }
  }

  std::uint32_t served = 0;
  while (true) {
    auto accepted = accept_connection(*listener);
    if (!accepted) {
      std::fprintf(stderr, "accept failed: %s\n", accepted.error().message().c_str());
      return 1;
    }
    TcpSocket connection = std::move(*accepted);
    if (options.once) {
      (void)connection.set_receive_timeout_ms(30000);
    }
    bool keep_going = true;
    while (keep_going) {
      const auto raw = connection.recv_frame();
      if (!raw) {
        break;
      }
      const auto frame = decode_frame(*raw);
      if (!frame) {
        std::printf("FRAME-ERROR %s\n", frame.error().message().c_str());
        std::fflush(stdout);
        break;
      }
      if (frame->type == MessageType::Bye) {
        break;
      }
      if (frame->type == MessageType::Ping) {
        const std::vector<std::uint8_t> pong = encode_frame(MessageType::Pong, {});
        (void)connection.send_all(pong);
        continue;
      }
      if (frame->type == MessageType::Hello) {
        const auto hello = decode_hello(frame->body);
        if (!hello) {
          std::printf("HELLO-ERROR %s\n", hello.error().message().c_str());
          std::fflush(stdout);
          break;
        }
        auto lease = (*registry)->RegisterPublisher(hello->publisher, hello->incarnation,
                                                    hello->boot, hello->ttl_ms, hello->origin);
        HelloAckMessage ack;
        ack.epoch = (*registry)->current_epoch();
        if (!lease) {
          ack.status = status_from_error(lease.error().code());
          ack.detail = lease.error().detail();
        } else {
          ack.status = WireStatus::Ok;
          ack.lease = lease->lease;
          ack.epoch = lease->epoch;
          ack.expires_at_ms = lease->expires_at_ms;
        }
        const std::vector<std::uint8_t> reply =
            encode_frame(MessageType::HelloAck, encode_hello_ack(ack));
        if (reply.empty()) {
          break;
        }
        const auto sent = connection.send_all(reply);
        if (!sent) {
          break;
        }
        std::printf("HELLO %s incarnation=%llu epoch=%llu status=%s\n",
                    hello->publisher.str().c_str(),
                    static_cast<unsigned long long>(hello->incarnation.value()),
                    static_cast<unsigned long long>(ack.epoch.value()), to_string(ack.status));
        std::fflush(stdout);
        if (ack.status != WireStatus::Ok) {
          break;
        }
        continue;
      }
      if (frame->type == MessageType::PublishCapability ||
          frame->type == MessageType::PublishReservation) {
        if (frame->body.empty()) {
          break;
        }
        const std::uint8_t flags = frame->body[0];
        const std::vector<std::uint8_t> payload(frame->body.begin() + 1, frame->body.end());
        PublishAckMessage ack;
        ByteReader reader(payload);
        if (frame->type == MessageType::PublishCapability) {
          ResourceCapability capability;
          const auto decoded = canonical_decode(reader, capability);
          if (!decoded) {
            ack.status = status_from_error(decoded.error().code());
            ack.detail = decoded.error().detail();
          } else {
            if ((flags & 0x01u) != 0) {
              const auto latest = (*registry)->LatestCapability(capability.resource);
              Generation next{1};
              if (latest) {
                next = latest->generation;
                if (!next.try_increment()) {
                  ack.status = WireStatus::Internal;
                  ack.detail = "generation space exhausted";
                }
              }
              capability.generation = next;
            }
            const auto published = (*registry)->PublishCapability(capability);
            if (!published) {
              ack.status = status_from_error(published.error().code());
              ack.detail = published.error().detail();
            } else {
              ack.status = WireStatus::Ok;
              ack.generation = published->generation;
              ack.sequence = published->sequence;
              ack.digest = published->digest;
            }
          }
        } else {
          Reservation reservation;
          const auto decoded = canonical_decode(reader, reservation);
          if (!decoded) {
            ack.status = status_from_error(decoded.error().code());
            ack.detail = decoded.error().detail();
          } else {
            if ((flags & 0x01u) != 0) {
              const auto latest = (*registry)->GetReservation(reservation.resource,
                                                              reservation.generation);
              Generation next{1};
              if (latest) {
                next = latest->generation;
                if (!next.try_increment()) {
                  ack.status = WireStatus::Internal;
                  ack.detail = "generation space exhausted";
                }
              }
              reservation.generation = next;
            }
            const auto published = (*registry)->PublishReservation(reservation);
            if (!published) {
              ack.status = status_from_error(published.error().code());
              ack.detail = published.error().detail();
            } else {
              ack.status = WireStatus::Ok;
              ack.generation = published->generation;
              ack.sequence = published->sequence;
              ack.digest = published->digest;
            }
          }
        }
        const std::vector<std::uint8_t> reply =
            encode_frame(MessageType::PublishAck, encode_publish_ack(ack));
        if (reply.empty()) {
          break;
        }
        const auto sent = connection.send_all(reply);
        if (!sent) {
          break;
        }
        std::printf("PUBLISH status=%s generation=%llu sequence=%llu\n", to_string(ack.status),
                    static_cast<unsigned long long>(ack.generation.value()),
                    static_cast<unsigned long long>(ack.sequence.value()));
        std::fflush(stdout);
        continue;
      }
      if (frame->type == MessageType::RegistryStatus) {
        const RegistryCounts counts = (*registry)->counts();
        RegistryStatusMessage status;
        status.epoch = (*registry)->current_epoch();
        status.classes = counts.classes;
        status.policies = counts.policies;
        status.paths = counts.paths;
        status.capabilities = counts.capabilities;
        status.decisions = counts.decisions;
        status.publishers = counts.publishers;
        status.conflicts = counts.conflicts;
        const std::vector<std::uint8_t> reply =
            encode_frame(MessageType::RegistryStatus, encode_registry_status(status));
        if (reply.empty()) {
          break;
        }
        if (!connection.send_all(reply)) {
          break;
        }
        continue;
      }
      keep_going = false;
    }
    connection.close();
    ++served;
    if (options.once && served >= 1) {
      break;
    }
  }
  const auto closed = (*registry)->Close();
  if (!closed) {
    report_error(closed.error());
    return 1;
  }
  std::printf("SERVER-EXIT served=%u\n", served);
  return 0;
}

int run_selftest() {
  int failures = 0;
  auto expect = [&failures](bool condition, const char* what) {
    if (!condition) {
      std::printf("SELFTEST-FAIL %s\n", what);
      ++failures;
    }
  };
  const std::string digest_hex = sha256("abc").hex();
  Digest256 round_trip;
  expect(Digest256::parse_hex(digest_hex, round_trip), "sha256 hex round trip");
  expect(round_trip == sha256("abc"), "sha256 hex round trip is lossless");
  const std::array<std::uint8_t, 3> probe = {1, 2, 3};
  expect(crc32c(probe.data(), probe.size()) != 0, "crc32c is defined");
  const auto id = parse_string_id<QoSClassIdTag>("voice.toll");
  expect(id.has_value(), "identifier accepted");
  expect(!parse_string_id<QoSClassIdTag>("bad id").has_value(), "identifier with a space refused");
  expect(!parse_string_id<QoSClassIdTag>("").has_value(), "empty identifier refused");
  expect(version_string() == "1.0.0", "version string");
  std::printf("SELFTEST %s failures=%d\n", failures == 0 ? "OK" : "FAILED", failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  const std::string command = argv[1];
  Options options;
  if (!parse_options(argc, argv, options)) {
    usage();
    return 1;
  }

  if (command == "version") {
    std::printf("qosfabric %s (%s)\n", version_string().data(), build_configuration().data());
    return 0;
  }
  if (command == "selftest") {
    return run_selftest();
  }
  if (command == "serve") {
    return run_server(options);
  }
  if (command == "init") {
    auto registry = open_registry(options);
    if (!registry) {
      report_error(registry.error());
      return 1;
    }
    const auto closed = (*registry)->Close();
    if (!closed) {
      report_error(closed.error());
      return 1;
    }
    std::printf("INIT %s store=%s epoch=%llu\n", options.store.c_str(),
                (*registry)->store_id().c_str(),
                static_cast<unsigned long long>((*registry)->current_epoch().value()));
    return 0;
  }
  if (command == "status") {
    options.observational = true;
    auto registry = open_registry(options);
    if (!registry) {
      report_error(registry.error());
      return 1;
    }
    const RegistryCounts counts = (*registry)->counts();
    std::printf("STORE %s\n", (*registry)->store_id().c_str());
    std::printf("EPOCH current=%llu durable=%llu\n",
                static_cast<unsigned long long>((*registry)->current_epoch().value()),
                static_cast<unsigned long long>((*registry)->durable_epoch().value()));
    std::printf("COUNTS classes=%zu policies=%zu paths=%zu capabilities=%zu reservations=%zu\n",
                counts.classes, counts.policies, counts.paths, counts.capabilities,
                counts.reservations);
    std::printf("COUNTS publishers=%zu decisions=%zu conflicts=%zu leases=%zu\n",
                counts.publishers, counts.decisions, counts.conflicts, counts.live_leases);
    std::printf("RECOVERY clean=%d manifest_rebuilt=%d snapshot_loaded=%d replayed=%llu "
                "truncated=%llu unfinished=%llu discarded=%llu\n",
                (*registry)->recovery().opened_clean ? 1 : 0,
                (*registry)->recovery().manifest_rebuilt ? 1 : 0,
                (*registry)->recovery().snapshot_loaded ? 1 : 0,
                static_cast<unsigned long long>((*registry)->recovery().replayed_records),
                static_cast<unsigned long long>((*registry)->recovery().truncated_tail_bytes),
                static_cast<unsigned long long>((*registry)->recovery().unfinished_intents),
                static_cast<unsigned long long>((*registry)->recovery().discarded_intents));
    const auto closed = (*registry)->Close();
    if (!closed) {
      report_error(closed.error());
      return 1;
    }
    return 0;
  }
  if (command == "verify") {
    options.observational = true;
    auto registry = open_registry(options);
    if (!registry) {
      report_error(registry.error());
      return 1;
    }
    const auto verified = (*registry)->Verify();
    if (!verified) {
      report_error(verified.error());
      return 1;
    }
    std::printf("VERIFY OK store=%s\n", (*registry)->store_id().c_str());
    const auto closed = (*registry)->Close();
    if (!closed) {
      report_error(closed.error());
      return 1;
    }
    return 0;
  }
  if (command == "compact") {
    auto registry = open_registry(options);
    if (!registry) {
      report_error(registry.error());
      return 1;
    }
    const auto compacted = (*registry)->Compact();
    if (!compacted) {
      report_error(compacted.error());
      return 1;
    }
    std::printf("COMPACT OK epoch=%llu\n",
                static_cast<unsigned long long>((*registry)->current_epoch().value()));
    const auto closed = (*registry)->Close();
    if (!closed) {
      report_error(closed.error());
      return 1;
    }
    return 0;
  }
  if (command == "scenario") {
    if (options.file.empty()) {
      std::fputs("scenario requires --file FILE\n", stderr);
      return 1;
    }
    auto registry = open_registry(options);
    if (!registry) {
      report_error(registry.error());
      return 1;
    }
    Scenario scenario(**registry);
    const auto executed = scenario.run(options.file);
    if (!executed) {
      // A failed scenario still closes the registry: every committed mutation
      // is already durable, and the clean-shutdown marker keeps recovery
      // reporting honest.
      report_error(executed.error());
      const auto shutdown = (*registry)->Close();
      if (!shutdown) {
        report_error(shutdown.error());
      }
      return 1;
    }
    const auto closed = (*registry)->Close();
    if (!closed) {
      report_error(closed.error());
      return 1;
    }
    std::printf("SCENARIO OK\n");
    return 0;
  }

  usage();
  return 1;
}
