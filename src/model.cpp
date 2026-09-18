// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/model.hpp"

#include <algorithm>
#include <array>
#include <type_traits>

#include "qosfabric/checked.hpp"

namespace qosfabric {
namespace {

// Domain-separation tags. Every canonical record starts with its type tag and
// schema version so that a digest computed over one record type can never
// collide with a digest over another, and so that a decoder can refuse a
// record that was not written by this schema.
constexpr std::uint16_t kTagQoSClass = 0x0101;
constexpr std::uint16_t kTagPolicy = 0x0102;
constexpr std::uint16_t kTagPath = 0x0103;
constexpr std::uint16_t kTagCapability = 0x0104;
constexpr std::uint16_t kTagReservation = 0x0105;
constexpr std::uint16_t kTagRequest = 0x0106;
constexpr std::uint16_t kSchemaVersion = 1;

template <class T>
void encode_optional_uint(ByteWriter& writer, const std::optional<T>& value) {
  static_assert(std::is_unsigned_v<T>, "only unsigned optionals are encoded");
  writer.presence(value.has_value());
  if (!value.has_value() || !writer.ok()) {
    return;
  }
  if constexpr (sizeof(T) == sizeof(std::uint64_t)) {
    writer.u64(static_cast<std::uint64_t>(*value));
  } else if constexpr (sizeof(T) == sizeof(std::uint32_t)) {
    writer.u32(static_cast<std::uint32_t>(*value));
  } else {
    writer.u16(static_cast<std::uint16_t>(*value));
  }
}

template <class T>
bool decode_optional_uint(ByteReader& reader, std::optional<T>& out) {
  static_assert(std::is_unsigned_v<T>, "only unsigned optionals are decoded");
  bool present = false;
  if (!reader.optional(present, [&](ByteReader& r) {
        if constexpr (sizeof(T) == sizeof(std::uint64_t)) {
          out = static_cast<T>(r.u64());
        } else if constexpr (sizeof(T) == sizeof(std::uint32_t)) {
          out = static_cast<T>(r.u32());
        } else {
          out = static_cast<T>(r.u16());
        }
      })) {
    return false;
  }
  if (!present) {
    out.reset();
  }
  return !reader.failed();
}

template <class Enum>
void encode_enum(ByteWriter& writer, Enum value, std::uint8_t max_value) {
  const auto raw = static_cast<std::uint8_t>(value);
  if (raw > max_value) {
    writer.fail();
    return;
  }
  writer.u8(raw);
}

template <class Enum>
bool decode_enum(ByteReader& reader, Enum& out, std::uint8_t max_value) {
  const std::uint8_t raw = reader.u8();
  if (reader.failed() || raw > max_value) {
    reader.fail();
    return false;
  }
  out = static_cast<Enum>(raw);
  return true;
}

void encode_string_id(ByteWriter& writer, std::string_view value, std::size_t max_len) {
  writer.str(value, max_len);
}

Result<std::string> decode_identifier(ByteReader& reader, std::size_t max_len) {
  std::string value = reader.str(max_len);
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "identifier field truncated or over-long");
  }
  if (!is_valid_identifier(value, max_len)) {
    return make_error(ErrorCode::InvalidArgument, "identifier outside canonical alphabet");
  }
  return value;
}

void encode_gen(ByteWriter& writer, Generation value) { writer.u64(value.value()); }

Generation decode_gen(ByteReader& reader) { return Generation{reader.u64()}; }

void encode_epoch(ByteWriter& writer, FabricEpoch value) { writer.u64(value.value()); }

FabricEpoch decode_epoch(ByteReader& reader) { return FabricEpoch{reader.u64()}; }

void encode_digest(ByteWriter& writer, const Digest256& value) {
  writer.raw(value.bytes().data(), Digest256::kBytes);
}

bool decode_digest(ByteReader& reader, Digest256& out) {
  const std::string_view raw = reader.raw(Digest256::kBytes);
  if (reader.failed()) {
    return false;
  }
  std::array<std::uint8_t, Digest256::kBytes> bytes{};
  std::copy(raw.begin(), raw.end(), bytes.begin());
  out = Digest256{bytes};
  return true;
}

void encode_provenance(ByteWriter& writer, const Provenance& value) {
  writer.str(value.origin(), limits::kMaxOriginLen);
  encode_epoch(writer, value.epoch());
  encode_gen(writer, value.generation());
  writer.u64(value.sequence().value());
}

bool decode_provenance(ByteReader& reader, Provenance& out) {
  std::string origin = reader.str(limits::kMaxOriginLen);
  const FabricEpoch epoch = decode_epoch(reader);
  const Generation generation = decode_gen(reader);
  const Sequence sequence{reader.u64()};
  if (reader.failed()) {
    return false;
  }
  out = Provenance{std::move(origin), epoch, generation, sequence};
  return true;
}

// Predicates are a set, so the canonical form is the sorted form. Two classes
// that differ only in the order their predicates were listed therefore have
// the same content digest, the same authority, and an identical explanation.
void encode_predicates(ByteWriter& writer, const std::vector<CapabilityKey>& keys,
                       std::size_t max_count) {
  if (keys.size() > max_count) {
    writer.fail();
    return;
  }
  std::vector<std::string_view> ordered;
  ordered.reserve(keys.size());
  for (const CapabilityKey& key : keys) {
    ordered.push_back(key.view());
  }
  std::sort(ordered.begin(), ordered.end());
  writer.counted(ordered.size(), [&](ByteWriter& w) {
    for (const std::string_view& key : ordered) {
      encode_string_id(w, key, limits::kMaxPredicateKeyLen);
      if (!w.ok()) {
        return;
      }
    }
  });
}

Result<void> decode_predicates(ByteReader& reader, std::vector<CapabilityKey>& out,
                               std::size_t max_count) {
  std::size_t count = 0;
  if (!reader.count(max_count, count)) {
    return make_error(ErrorCode::TooLarge, "predicate list exceeds structural budget");
  }
  out.clear();
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    auto key = decode_identifier(reader, limits::kMaxPredicateKeyLen);
    if (!key) {
      return key.error();
    }
    out.emplace_back(std::move(key).value());
  }
  return {};
}

bool has_duplicate_predicates(const std::vector<CapabilityKey>& keys) {
  std::vector<std::string_view> views;
  views.reserve(keys.size());
  for (const auto& key : keys) {
    views.push_back(key.view());
  }
  std::sort(views.begin(), views.end());
  return std::adjacent_find(views.begin(), views.end()) != views.end();
}

std::string bounded_label(std::string_view label) {
  std::string out(label.substr(0, std::min(label.size(), limits::kMaxLabelLen)));
  return out;
}

}  // namespace

const char* to_string(IsolationLevel value) noexcept {
  switch (value) {
    case IsolationLevel::None: return "none";
    case IsolationLevel::Shared: return "shared";
    case IsolationLevel::QueueIsolated: return "queue-isolated";
    case IsolationLevel::ResourceIsolated: return "resource-isolated";
  }
  return "invalid";
}

const char* to_string(TreatmentMode value) noexcept {
  switch (value) {
    case TreatmentMode::BestEffort: return "best-effort";
    case TreatmentMode::WeightedFair: return "weighted-fair";
    case TreatmentMode::RateLimited: return "rate-limited";
    case TreatmentMode::PriorityQueue: return "priority-queue";
    case TreatmentMode::StrictPriorityLowLatency: return "strict-priority-low-latency";
  }
  return "invalid";
}

const char* to_string(ViolationPolicy value) noexcept {
  switch (value) {
    case ViolationPolicy::Reject: return "reject";
    case ViolationPolicy::DegradeThenReject: return "degrade-then-reject";
    case ViolationPolicy::ReportOnly: return "report-only";
  }
  return "invalid";
}

const char* to_string(CapabilityState value) noexcept {
  switch (value) {
    case CapabilityState::Unknown: return "unknown";
    case CapabilityState::Unavailable: return "unavailable";
    case CapabilityState::Degraded: return "degraded";
    case CapabilityState::Available: return "available";
  }
  return "invalid";
}

const char* to_string(PathRole value) noexcept {
  switch (value) {
    case PathRole::Member: return "member";
    case PathRole::Primary: return "primary";
    case PathRole::Protection: return "protection";
  }
  return "invalid";
}

const char* to_string(ObligationKind value) noexcept {
  switch (value) {
    case ObligationKind::MinRate: return "rate.min_bps";
    case ObligationKind::MaxRate: return "rate.max_bps";
    case ObligationKind::PeakRate: return "rate.peak_bps";
    case ObligationKind::Latency: return "latency.max_us";
    case ObligationKind::Jitter: return "jitter.max_us";
    case ObligationKind::Loss: return "loss.max_ppm";
    case ObligationKind::BurstBytes: return "burst.bytes";
    case ObligationKind::BurstInterval: return "burst.interval_us";
    case ObligationKind::Mtu: return "mtu.min_bytes";
    case ObligationKind::Isolation: return "isolation.level";
    case ObligationKind::Treatment: return "treatment.mode";
    case ObligationKind::PriorityRank: return "priority.min_rank";
    case ObligationKind::Reservation: return "reservation.min_rate_bps";
    case ObligationKind::DisjointPaths: return "redundancy.disjoint_paths";
    case ObligationKind::Predicate: return "capability.predicate";
    case ObligationKind::ResourceState: return "resource.state";
    case ObligationKind::Count: break;
  }
  return "invalid";
}

const char* to_string(CompareDirection value) noexcept {
  switch (value) {
    case CompareDirection::AtLeast: return "at-least";
    case CompareDirection::AtMost: return "at-most";
  }
  return "invalid";
}

const char* to_string(AggregateKind value) noexcept {
  switch (value) {
    case AggregateKind::WeakestLink: return "weakest-link";
    case AggregateKind::Additive: return "additive";
    case AggregateKind::LossCompose: return "loss-compose";
    case AggregateKind::WorstCase: return "worst-case";
    case AggregateKind::AllPresent: return "all-present";
    case AggregateKind::PathDiversity: return "path-diversity";
  }
  return "invalid";
}

bool parse_isolation_level(std::string_view text, IsolationLevel& out) noexcept {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(IsolationLevel::ResourceIsolated);
       ++raw) {
    const auto candidate = static_cast<IsolationLevel>(raw);
    if (text == to_string(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool parse_treatment_mode(std::string_view text, TreatmentMode& out) noexcept {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(TreatmentMode::StrictPriorityLowLatency);
       ++raw) {
    const auto candidate = static_cast<TreatmentMode>(raw);
    if (text == to_string(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool parse_path_role(std::string_view text, PathRole& out) noexcept {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(PathRole::Protection); ++raw) {
    const auto candidate = static_cast<PathRole>(raw);
    if (text == to_string(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool parse_violation_policy(std::string_view text, ViolationPolicy& out) noexcept {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(ViolationPolicy::ReportOnly); ++raw) {
    const auto candidate = static_cast<ViolationPolicy>(raw);
    if (text == to_string(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool parse_capability_state(std::string_view text, CapabilityState& out) noexcept {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(CapabilityState::Available); ++raw) {
    const auto candidate = static_cast<CapabilityState>(raw);
    if (text == to_string(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::string_view obligation_name(ObligationKind kind) noexcept {
  return to_string(kind);
}

CompareDirection obligation_direction(ObligationKind kind) noexcept {
  switch (kind) {
    case ObligationKind::Latency:
    case ObligationKind::Jitter:
    case ObligationKind::Loss:
    case ObligationKind::BurstInterval:
      return CompareDirection::AtMost;
    default:
      return CompareDirection::AtLeast;
  }
}

AggregateKind obligation_aggregate(ObligationKind kind) noexcept {
  switch (kind) {
    case ObligationKind::Latency:
    case ObligationKind::Jitter:
      return AggregateKind::Additive;
    case ObligationKind::Loss:
      return AggregateKind::LossCompose;
    case ObligationKind::BurstInterval:
      return AggregateKind::WorstCase;
    case ObligationKind::Predicate:
      return AggregateKind::AllPresent;
    case ObligationKind::DisjointPaths:
      return AggregateKind::PathDiversity;
    default:
      return AggregateKind::WeakestLink;
  }
}

bool obligation_relaxable(ObligationKind kind) noexcept {
  switch (kind) {
    case ObligationKind::MinRate:
    case ObligationKind::MaxRate:
    case ObligationKind::PeakRate:
    case ObligationKind::Latency:
    case ObligationKind::Jitter:
    case ObligationKind::Loss:
    case ObligationKind::BurstBytes:
    case ObligationKind::BurstInterval:
    case ObligationKind::Mtu:
    case ObligationKind::DisjointPaths:
    case ObligationKind::ResourceState:
      return true;
    case ObligationKind::Isolation:
    case ObligationKind::Treatment:
    case ObligationKind::PriorityRank:
    case ObligationKind::Reservation:
    case ObligationKind::Predicate:
    case ObligationKind::Count:
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
Result<void> validate(const QoSClass& value) {
  if (!is_valid(value.id)) {
    return make_error(ErrorCode::InvalidArgument, "class id rejected by canonical alphabet");
  }
  if (value.generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "class generation must be non-zero");
  }
  if (value.label.size() > limits::kMaxLabelLen) {
    return make_error(ErrorCode::TooLarge, "class label exceeds budget");
  }
  if (value.min_rate_bps.has_value() && *value.min_rate_bps > limits::kMaxRateBps) {
    return make_error(ErrorCode::TooLarge, "min_rate_bps beyond numeric domain");
  }
  if (value.max_rate_bps.has_value() && *value.max_rate_bps > limits::kMaxRateBps) {
    return make_error(ErrorCode::TooLarge, "max_rate_bps beyond numeric domain");
  }
  if (value.peak_rate_bps.has_value() && *value.peak_rate_bps > limits::kMaxRateBps) {
    return make_error(ErrorCode::TooLarge, "peak_rate_bps beyond numeric domain");
  }
  if (value.min_rate_bps.has_value() && value.max_rate_bps.has_value() &&
      *value.min_rate_bps > *value.max_rate_bps) {
    return make_error(ErrorCode::Conflict, "min_rate_bps exceeds max_rate_bps");
  }
  if (value.max_rate_bps.has_value() && value.peak_rate_bps.has_value() &&
      *value.peak_rate_bps < *value.max_rate_bps) {
    return make_error(ErrorCode::Conflict, "peak_rate_bps below max_rate_bps");
  }
  if (value.min_rate_bps.has_value() && value.peak_rate_bps.has_value() &&
      *value.peak_rate_bps < *value.min_rate_bps) {
    return make_error(ErrorCode::Conflict, "peak_rate_bps below min_rate_bps");
  }
  if (value.max_latency_us.has_value() && *value.max_latency_us > limits::kMaxLatencyUs) {
    return make_error(ErrorCode::TooLarge, "max_latency_us beyond numeric domain");
  }
  if (value.max_jitter_us.has_value() && *value.max_jitter_us > limits::kMaxJitterUs) {
    return make_error(ErrorCode::TooLarge, "max_jitter_us beyond numeric domain");
  }
  if (value.max_jitter_us.has_value() && value.max_latency_us.has_value() &&
      *value.max_jitter_us > *value.max_latency_us) {
    return make_error(ErrorCode::Conflict, "max_jitter_us exceeds max_latency_us");
  }
  if (value.max_loss_ppm.has_value() && *value.max_loss_ppm > limits::kMaxLossPpm) {
    return make_error(ErrorCode::TooLarge, "max_loss_ppm beyond numeric domain");
  }
  if (value.burst_bytes.has_value() && *value.burst_bytes > limits::kMaxBurstBytes) {
    return make_error(ErrorCode::TooLarge, "burst_bytes beyond numeric domain");
  }
  if (value.burst_interval_us.has_value() &&
      *value.burst_interval_us > limits::kMaxBurstIntervalUs) {
    return make_error(ErrorCode::TooLarge, "burst_interval_us beyond numeric domain");
  }
  if (value.burst_bytes.has_value() != value.burst_interval_us.has_value()) {
    return make_error(ErrorCode::Conflict,
                      "burst semantics require both burst_bytes and burst_interval_us");
  }
  if (value.min_mtu_bytes.has_value() && *value.min_mtu_bytes > limits::kMaxMtuBytes) {
    return make_error(ErrorCode::TooLarge, "min_mtu_bytes beyond numeric domain");
  }
  if (value.min_priority_rank > limits::kMaxPriorityRank) {
    return make_error(ErrorCode::TooLarge, "min_priority_rank beyond numeric domain");
  }
  if (value.min_disjoint_paths == 0 || value.min_disjoint_paths > limits::kMaxDisjointPaths) {
    return make_error(ErrorCode::InvalidArgument, "min_disjoint_paths outside [1, 64]");
  }
  if (value.required_predicates.size() > limits::kMaxRequiredPredicates) {
    return make_error(ErrorCode::TooLarge, "required predicate list exceeds budget");
  }
  for (const CapabilityKey& key : value.required_predicates) {
    if (!is_valid(key, limits::kMaxPredicateKeyLen)) {
      return make_error(ErrorCode::InvalidArgument, "required predicate key rejected");
    }
  }
  if (has_duplicate_predicates(value.required_predicates)) {
    return make_error(ErrorCode::Duplicate, "duplicate required predicate key");
  }
  if (value.degrade_max_relaxation_ppm > limits::kMaxRelaxationPpm) {
    return make_error(ErrorCode::TooLarge, "degrade_max_relaxation_ppm beyond 1000000");
  }
  if (!value.allow_degrade && value.degrade_max_relaxation_ppm != 0) {
    return make_error(ErrorCode::Conflict,
                      "degrade window declared while degradation is not allowed");
  }
  if (value.violation_policy == ViolationPolicy::ReportOnly &&
      (!value.allow_degrade || value.degrade_max_relaxation_ppm == 0)) {
    return make_error(ErrorCode::Conflict,
                      "report-only violation policy demands a class-declared degrade window");
  }
  if (value.requires_reservation && !value.min_rate_bps.has_value()) {
    return make_error(ErrorCode::Conflict,
                      "reservation requirement demands an explicit min_rate_bps");
  }
  return {};
}

Result<void> validate(const Policy& value) {
  if (!is_valid(value.id)) {
    return make_error(ErrorCode::InvalidArgument, "policy id rejected by canonical alphabet");
  }
  if (value.generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "policy generation must be non-zero");
  }
  if (value.label.size() > limits::kMaxLabelLen) {
    return make_error(ErrorCode::TooLarge, "policy label exceeds budget");
  }
  if (value.max_relaxation_ppm > limits::kMaxRelaxationPpm) {
    return make_error(ErrorCode::TooLarge, "max_relaxation_ppm beyond 1000000");
  }
  if (!value.degrade_permitted && value.max_relaxation_ppm != 0) {
    return make_error(ErrorCode::Conflict,
                      "policy declares a relaxation window while degradation is not permitted");
  }
  if (value.max_stale_epochs > limits::kMaxStaleEpochs) {
    return make_error(ErrorCode::TooLarge, "max_stale_epochs beyond numeric domain");
  }
  if (value.capability_max_age_ms > limits::kMaxLeaseMs) {
    return make_error(ErrorCode::TooLarge, "capability_max_age_ms beyond lease domain");
  }
  if (value.max_unknown_components > limits::kMaxPathComponents) {
    return make_error(ErrorCode::TooLarge, "max_unknown_components exceeds path budget");
  }
  if (value.required_predicates.size() > limits::kMaxRequiredPredicates) {
    return make_error(ErrorCode::TooLarge, "policy predicate list exceeds budget");
  }
  for (const CapabilityKey& key : value.required_predicates) {
    if (!is_valid(key, limits::kMaxPredicateKeyLen)) {
      return make_error(ErrorCode::InvalidArgument, "policy predicate key rejected");
    }
  }
  if (has_duplicate_predicates(value.required_predicates)) {
    return make_error(ErrorCode::Duplicate, "duplicate policy predicate key");
  }
  if (value.denied_classes.size() > limits::kMaxClassesRetained) {
    return make_error(ErrorCode::TooLarge, "denied class list exceeds budget");
  }
  std::vector<std::string_view> denied;
  denied.reserve(value.denied_classes.size());
  for (const QoSClassId& id : value.denied_classes) {
    if (!is_valid(id)) {
      return make_error(ErrorCode::InvalidArgument, "denied class id rejected");
    }
    denied.push_back(id.view());
  }
  std::sort(denied.begin(), denied.end());
  if (std::adjacent_find(denied.begin(), denied.end()) != denied.end()) {
    return make_error(ErrorCode::Duplicate, "duplicate denied class id");
  }
  return {};
}

Result<void> validate(const Path& value) {
  if (!is_valid(value.id)) {
    return make_error(ErrorCode::InvalidArgument, "path id rejected by canonical alphabet");
  }
  if (value.generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "path generation must be non-zero");
  }
  if (value.components.empty()) {
    return make_error(ErrorCode::InvalidArgument, "path must contain at least one component");
  }
  if (value.components.size() > limits::kMaxPathComponents) {
    return make_error(ErrorCode::TooLarge, "path component count exceeds budget");
  }
  std::vector<std::string_view> seen;
  seen.reserve(value.components.size());
  for (const PathComponent& component : value.components) {
    if (!is_valid(component.resource)) {
      return make_error(ErrorCode::InvalidArgument, "path component resource id rejected");
    }
    if (component.capability_generation.is_zero()) {
      return make_error(ErrorCode::InvalidArgument,
                        "path component must bind a non-zero capability generation");
    }
    if (component.diversity_domain.empty() ||
        !is_valid(component.diversity_domain, limits::kMaxIdentifierLen)) {
      return make_error(ErrorCode::InvalidArgument, "diversity domain rejected");
    }
    if (std::find(seen.begin(), seen.end(), component.resource.view()) != seen.end()) {
      return make_error(ErrorCode::Duplicate, "duplicate resource in path");
    }
    seen.push_back(component.resource.view());
  }
  if (value.fixed_latency_us > limits::kMaxLatencyUs ||
      value.fixed_jitter_us > limits::kMaxJitterUs) {
    return make_error(ErrorCode::TooLarge, "path fixed overhead beyond numeric domain");
  }
  return {};
}

Result<void> validate(const ResourceCapability& value) {
  if (!is_valid(value.resource)) {
    return make_error(ErrorCode::InvalidArgument, "resource id rejected by canonical alphabet");
  }
  if (value.generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "capability generation must be non-zero");
  }
  if (!is_valid(value.publisher)) {
    return make_error(ErrorCode::InvalidArgument, "publisher id rejected by canonical alphabet");
  }
  if (value.incarnation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "publisher incarnation must be non-zero");
  }
  if (value.expires_at_ms < value.published_at_ms) {
    return make_error(ErrorCode::Conflict, "capability expiry precedes publication");
  }
  if (value.supported_rate_bps.has_value() && *value.supported_rate_bps > limits::kMaxRateBps) {
    return make_error(ErrorCode::TooLarge, "supported_rate_bps beyond numeric domain");
  }
  if (value.latency_bound_us.has_value() && *value.latency_bound_us > limits::kMaxLatencyUs) {
    return make_error(ErrorCode::TooLarge, "latency_bound_us beyond numeric domain");
  }
  if (value.jitter_bound_us.has_value() && *value.jitter_bound_us > limits::kMaxJitterUs) {
    return make_error(ErrorCode::TooLarge, "jitter_bound_us beyond numeric domain");
  }
  if (value.loss_ppm.has_value() && *value.loss_ppm > limits::kMaxLossPpm) {
    return make_error(ErrorCode::TooLarge, "loss_ppm beyond numeric domain");
  }
  if (value.max_burst_bytes.has_value() && *value.max_burst_bytes > limits::kMaxBurstBytes) {
    return make_error(ErrorCode::TooLarge, "max_burst_bytes beyond numeric domain");
  }
  if (value.burst_window_us.has_value() &&
      *value.burst_window_us > limits::kMaxBurstIntervalUs) {
    return make_error(ErrorCode::TooLarge, "burst_window_us beyond numeric domain");
  }
  if (value.max_burst_bytes.has_value() != value.burst_window_us.has_value()) {
    return make_error(ErrorCode::Conflict,
                      "burst capability requires both max_burst_bytes and burst_window_us");
  }
  if (value.mtu_bytes.has_value() && *value.mtu_bytes > limits::kMaxMtuBytes) {
    return make_error(ErrorCode::TooLarge, "mtu_bytes beyond numeric domain");
  }
  if (value.max_priority_rank > limits::kMaxPriorityRank) {
    return make_error(ErrorCode::TooLarge, "max_priority_rank beyond numeric domain");
  }
  if (value.predicates.size() > limits::kMaxCapabilityPredicates) {
    return make_error(ErrorCode::TooLarge, "capability predicate list exceeds budget");
  }
  for (const CapabilityKey& key : value.predicates) {
    if (!is_valid(key, limits::kMaxPredicateKeyLen)) {
      return make_error(ErrorCode::InvalidArgument, "capability predicate key rejected");
    }
  }
  if (has_duplicate_predicates(value.predicates)) {
    return make_error(ErrorCode::Duplicate, "duplicate capability predicate key");
  }
  if (value.state != CapabilityState::Unavailable && value.diversity_domain.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "a capable resource must declare its diversity domain");
  }
  if (!value.diversity_domain.empty() && !is_valid(value.diversity_domain)) {
    return make_error(ErrorCode::InvalidArgument, "diversity domain rejected");
  }
  return {};
}

Result<void> validate(const Reservation& value) {
  if (!is_valid(value.resource)) {
    return make_error(ErrorCode::InvalidArgument, "reservation resource id rejected");
  }
  if (value.generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "reservation generation must be non-zero");
  }
  if (!is_valid(value.class_id)) {
    return make_error(ErrorCode::InvalidArgument, "reservation class id rejected");
  }
  if (value.class_generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "reservation class generation must be non-zero");
  }
  if (!is_valid(value.publisher)) {
    return make_error(ErrorCode::InvalidArgument, "reservation publisher id rejected");
  }
  if (value.incarnation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "reservation publisher incarnation non-zero");
  }
  if (value.reserved_min_rate_bps == 0) {
    return make_error(ErrorCode::InvalidArgument, "reservation must commit a non-zero rate");
  }
  if (value.reserved_min_rate_bps > limits::kMaxRateBps) {
    return make_error(ErrorCode::TooLarge, "reserved_min_rate_bps beyond numeric domain");
  }
  if (value.reserved_burst_bytes > limits::kMaxBurstBytes) {
    return make_error(ErrorCode::TooLarge, "reserved_burst_bytes beyond numeric domain");
  }
  return {};
}

Result<void> validate(const ContractRequest& value) {
  if (!is_valid(value.contract)) {
    return make_error(ErrorCode::InvalidArgument, "contract id rejected by canonical alphabet");
  }
  if (!is_valid(value.subject)) {
    return make_error(ErrorCode::InvalidArgument, "subject id rejected by canonical alphabet");
  }
  if (!is_valid(value.class_id)) {
    return make_error(ErrorCode::InvalidArgument, "class id rejected by canonical alphabet");
  }
  if (!is_valid(value.path)) {
    return make_error(ErrorCode::InvalidArgument, "path id rejected by canonical alphabet");
  }
  if (!is_valid(value.policy)) {
    return make_error(ErrorCode::InvalidArgument, "policy id rejected by canonical alphabet");
  }
  if (!is_valid(value.priority)) {
    return make_error(ErrorCode::InvalidArgument, "priority class id rejected");
  }
  if (value.subject_generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "subject generation must be non-zero");
  }
  if (value.priority_generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "priority generation must be non-zero");
  }
  if (value.contract_generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "contract generation must be non-zero");
  }
  return {};
}

// ---------------------------------------------------------------------------
// Canonical encoding
// ---------------------------------------------------------------------------
void canonical_encode(const QoSClass& value, ByteWriter& writer) {
  writer.u16(kTagQoSClass);
  writer.u16(kSchemaVersion);
  writer.str(value.id.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.generation);
  writer.str(value.label, limits::kMaxLabelLen);
  encode_optional_uint(writer, value.min_rate_bps);
  encode_optional_uint(writer, value.max_rate_bps);
  encode_optional_uint(writer, value.peak_rate_bps);
  encode_optional_uint(writer, value.max_latency_us);
  encode_optional_uint(writer, value.max_jitter_us);
  encode_optional_uint(writer, value.max_loss_ppm);
  encode_optional_uint(writer, value.burst_bytes);
  encode_optional_uint(writer, value.burst_interval_us);
  encode_optional_uint(writer, value.min_mtu_bytes);
  encode_enum(writer, value.isolation, 3);
  encode_enum(writer, value.treatment, 4);
  writer.u32(value.min_priority_rank);
  writer.u32(value.min_disjoint_paths);
  writer.presence(value.requires_reservation);
  encode_predicates(writer, value.required_predicates, limits::kMaxRequiredPredicates);
  writer.presence(value.allow_degrade);
  writer.u32(value.degrade_max_relaxation_ppm);
  encode_enum(writer, value.violation_policy, 2);
  encode_epoch(writer, value.published_epoch);
  encode_provenance(writer, value.provenance);
}

void canonical_encode(const Policy& value, ByteWriter& writer) {
  writer.u16(kTagPolicy);
  writer.u16(kSchemaVersion);
  writer.str(value.id.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.generation);
  writer.str(value.label, limits::kMaxLabelLen);
  writer.presence(value.degrade_permitted);
  writer.u32(value.max_relaxation_ppm);
  writer.u32(value.max_stale_epochs);
  writer.u64(value.capability_max_age_ms);
  writer.u32(value.max_unknown_components);
  encode_predicates(writer, value.required_predicates, limits::kMaxRequiredPredicates);
  writer.counted(value.denied_classes.size(), [&](ByteWriter& w) {
    for (const QoSClassId& id : value.denied_classes) {
      w.str(id.view(), limits::kMaxIdentifierLen);
    }
  });
  encode_epoch(writer, value.published_epoch);
  encode_provenance(writer, value.provenance);
}

void canonical_encode(const Path& value, ByteWriter& writer) {
  writer.u16(kTagPath);
  writer.u16(kSchemaVersion);
  writer.str(value.id.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.generation);
  writer.counted(value.components.size(), [&](ByteWriter& w) {
    for (const PathComponent& component : value.components) {
      w.str(component.resource.view(), limits::kMaxIdentifierLen);
      encode_gen(w, component.capability_generation);
      w.str(component.diversity_domain.view(), limits::kMaxIdentifierLen);
      encode_enum(w, component.role, 2);
    }
  });
  writer.u64(value.fixed_latency_us);
  writer.u64(value.fixed_jitter_us);
  encode_epoch(writer, value.epoch);
  encode_provenance(writer, value.provenance);
}

void canonical_encode(const ResourceCapability& value, ByteWriter& writer) {
  writer.u16(kTagCapability);
  writer.u16(kSchemaVersion);
  writer.str(value.resource.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.generation);
  encode_epoch(writer, value.epoch);
  writer.str(value.publisher.view(), limits::kMaxIdentifierLen);
  writer.u64(value.incarnation.value());
  writer.u64(value.published_at_ms);
  writer.u64(value.expires_at_ms);
  encode_enum(writer, value.state, 3);
  encode_optional_uint(writer, value.supported_rate_bps);
  encode_optional_uint(writer, value.latency_bound_us);
  encode_optional_uint(writer, value.jitter_bound_us);
  encode_optional_uint(writer, value.loss_ppm);
  encode_optional_uint(writer, value.max_burst_bytes);
  encode_optional_uint(writer, value.burst_window_us);
  encode_optional_uint(writer, value.mtu_bytes);
  encode_enum(writer, value.max_isolation, 3);
  encode_enum(writer, value.max_treatment, 4);
  writer.u32(value.max_priority_rank);
  encode_predicates(writer, value.predicates, limits::kMaxCapabilityPredicates);
  writer.str(value.diversity_domain.view(), limits::kMaxIdentifierLen);
  encode_provenance(writer, value.provenance);
}

void canonical_encode(const Reservation& value, ByteWriter& writer) {
  writer.u16(kTagReservation);
  writer.u16(kSchemaVersion);
  writer.str(value.resource.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.generation);
  writer.str(value.class_id.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.class_generation);
  writer.u64(value.reserved_min_rate_bps);
  writer.u64(value.reserved_burst_bytes);
  encode_epoch(writer, value.epoch);
  writer.str(value.publisher.view(), limits::kMaxIdentifierLen);
  writer.u64(value.incarnation.value());
  writer.u64(value.expires_at_ms);
  encode_provenance(writer, value.provenance);
}

void canonical_encode(const ContractRequest& value, ByteWriter& writer) {
  writer.u16(kTagRequest);
  writer.u16(kSchemaVersion);
  writer.str(value.contract.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.contract_generation);
  writer.str(value.subject.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.subject_generation);
  writer.str(value.class_id.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.class_generation);
  writer.str(value.path.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.path_generation);
  writer.str(value.policy.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.policy_generation);
  writer.str(value.priority.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.priority_generation);
  encode_epoch(writer, value.observed_epoch);
  writer.u64(value.request_ms);
  encode_provenance(writer, value.provenance);
}

// ---------------------------------------------------------------------------
// Canonical decoding
// ---------------------------------------------------------------------------
namespace {

Result<void> expect_header(ByteReader& reader, std::uint16_t tag) {
  const std::uint16_t actual_tag = reader.u16();
  const std::uint16_t actual_version = reader.u16();
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "record header truncated");
  }
  if (actual_tag != tag) {
    return make_error(ErrorCode::VersionMismatch, "record type tag mismatch");
  }
  if (actual_version != kSchemaVersion) {
    return make_error(ErrorCode::VersionMismatch, "record schema version mismatch");
  }
  return {};
}

}  // namespace

Result<void> canonical_decode(ByteReader& reader, QoSClass& out) {
  auto header = expect_header(reader, kTagQoSClass);
  if (!header) {
    return header.error();
  }
  QoSClass value;
  auto id = decode_identifier(reader, limits::kMaxIdentifierLen);
  if (!id) {
    return id.error();
  }
  value.id = QoSClassId{std::move(id).value()};
  value.generation = decode_gen(reader);
  value.label = reader.str(limits::kMaxLabelLen);
  if (!decode_optional_uint(reader, value.min_rate_bps) ||
      !decode_optional_uint(reader, value.max_rate_bps) ||
      !decode_optional_uint(reader, value.peak_rate_bps) ||
      !decode_optional_uint(reader, value.max_latency_us) ||
      !decode_optional_uint(reader, value.max_jitter_us) ||
      !decode_optional_uint(reader, value.max_loss_ppm) ||
      !decode_optional_uint(reader, value.burst_bytes) ||
      !decode_optional_uint(reader, value.burst_interval_us) ||
      !decode_optional_uint(reader, value.min_mtu_bytes)) {
    return make_error(ErrorCode::Malformed, "class numeric field malformed");
  }
  if (!decode_enum(reader, value.isolation, 3) || !decode_enum(reader, value.treatment, 4)) {
    return make_error(ErrorCode::Malformed, "class enum field out of range");
  }
  value.min_priority_rank = reader.u32();
  value.min_disjoint_paths = reader.u32();
  value.requires_reservation = reader.presence();
  auto predicates = decode_predicates(reader, value.required_predicates,
                                      limits::kMaxRequiredPredicates);
  if (!predicates) {
    return predicates.error();
  }
  value.allow_degrade = reader.presence();
  value.degrade_max_relaxation_ppm = reader.u32();
  if (!decode_enum(reader, value.violation_policy, 2)) {
    return make_error(ErrorCode::Malformed, "violation policy out of range");
  }
  value.published_epoch = decode_epoch(reader);
  if (!decode_provenance(reader, value.provenance)) {
    return make_error(ErrorCode::Malformed, "class provenance malformed");
  }
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "class record truncated");
  }
  value.digest = compute_digest(value);
  auto valid = validate(value);
  if (!valid) {
    return valid.error();
  }
  out = std::move(value);
  return {};
}

Result<void> canonical_decode(ByteReader& reader, Policy& out) {
  auto header = expect_header(reader, kTagPolicy);
  if (!header) {
    return header.error();
  }
  Policy value;
  auto id = decode_identifier(reader, limits::kMaxIdentifierLen);
  if (!id) {
    return id.error();
  }
  value.id = PolicyId{std::move(id).value()};
  value.generation = decode_gen(reader);
  value.label = reader.str(limits::kMaxLabelLen);
  value.degrade_permitted = reader.presence();
  value.max_relaxation_ppm = reader.u32();
  value.max_stale_epochs = reader.u32();
  value.capability_max_age_ms = reader.u64();
  value.max_unknown_components = reader.u32();
  auto predicates = decode_predicates(reader, value.required_predicates,
                                      limits::kMaxRequiredPredicates);
  if (!predicates) {
    return predicates.error();
  }
  std::size_t denied_count = 0;
  if (!reader.count(limits::kMaxClassesRetained, denied_count)) {
    return make_error(ErrorCode::TooLarge, "denied class list exceeds budget");
  }
  value.denied_classes.clear();
  value.denied_classes.reserve(denied_count);
  for (std::size_t i = 0; i < denied_count; ++i) {
    auto denied = decode_identifier(reader, limits::kMaxIdentifierLen);
    if (!denied) {
      return denied.error();
    }
    value.denied_classes.emplace_back(std::move(denied).value());
  }
  value.published_epoch = decode_epoch(reader);
  if (!decode_provenance(reader, value.provenance)) {
    return make_error(ErrorCode::Malformed, "policy provenance malformed");
  }
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "policy record truncated");
  }
  value.digest = compute_digest(value);
  auto valid = validate(value);
  if (!valid) {
    return valid.error();
  }
  out = std::move(value);
  return {};
}

Result<void> canonical_decode(ByteReader& reader, Path& out) {
  auto header = expect_header(reader, kTagPath);
  if (!header) {
    return header.error();
  }
  Path value;
  auto id = decode_identifier(reader, limits::kMaxIdentifierLen);
  if (!id) {
    return id.error();
  }
  value.id = PathId{std::move(id).value()};
  value.generation = decode_gen(reader);
  std::size_t count = 0;
  if (!reader.count(limits::kMaxPathComponents, count)) {
    return make_error(ErrorCode::TooLarge, "path component count exceeds budget");
  }
  value.components.clear();
  value.components.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    PathComponent component;
    auto resource = decode_identifier(reader, limits::kMaxIdentifierLen);
    if (!resource) {
      return resource.error();
    }
    component.resource = ResourceId{std::move(resource).value()};
    component.capability_generation = decode_gen(reader);
    auto domain = decode_identifier(reader, limits::kMaxIdentifierLen);
    if (!domain) {
      return domain.error();
    }
    component.diversity_domain = DiversityDomain{std::move(domain).value()};
    if (!decode_enum(reader, component.role, 2)) {
      return make_error(ErrorCode::Malformed, "path role out of range");
    }
    value.components.push_back(std::move(component));
  }
  value.fixed_latency_us = reader.u64();
  value.fixed_jitter_us = reader.u64();
  value.epoch = decode_epoch(reader);
  if (!decode_provenance(reader, value.provenance)) {
    return make_error(ErrorCode::Malformed, "path provenance malformed");
  }
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "path record truncated");
  }
  value.digest = compute_digest(value);
  auto valid = validate(value);
  if (!valid) {
    return valid.error();
  }
  out = std::move(value);
  return {};
}

Result<void> canonical_decode(ByteReader& reader, ResourceCapability& out) {
  auto header = expect_header(reader, kTagCapability);
  if (!header) {
    return header.error();
  }
  ResourceCapability value;
  auto resource = decode_identifier(reader, limits::kMaxIdentifierLen);
  if (!resource) {
    return resource.error();
  }
  value.resource = ResourceId{std::move(resource).value()};
  value.generation = decode_gen(reader);
  value.epoch = decode_epoch(reader);
  auto publisher = decode_identifier(reader, limits::kMaxIdentifierLen);
  if (!publisher) {
    return publisher.error();
  }
  value.publisher = PublisherId{std::move(publisher).value()};
  value.incarnation = PublisherIncarnation{reader.u64()};
  value.published_at_ms = reader.u64();
  value.expires_at_ms = reader.u64();
  if (!decode_enum(reader, value.state, 3)) {
    return make_error(ErrorCode::Malformed, "capability state out of range");
  }
  if (!decode_optional_uint(reader, value.supported_rate_bps) ||
      !decode_optional_uint(reader, value.latency_bound_us) ||
      !decode_optional_uint(reader, value.jitter_bound_us) ||
      !decode_optional_uint(reader, value.loss_ppm) ||
      !decode_optional_uint(reader, value.max_burst_bytes) ||
      !decode_optional_uint(reader, value.burst_window_us) ||
      !decode_optional_uint(reader, value.mtu_bytes)) {
    return make_error(ErrorCode::Malformed, "capability numeric field malformed");
  }
  if (!decode_enum(reader, value.max_isolation, 3) ||
      !decode_enum(reader, value.max_treatment, 4)) {
    return make_error(ErrorCode::Malformed, "capability enum field out of range");
  }
  value.max_priority_rank = reader.u32();
  auto predicates = decode_predicates(reader, value.predicates, limits::kMaxCapabilityPredicates);
  if (!predicates) {
    return predicates.error();
  }
  auto domain = decode_identifier(reader, limits::kMaxIdentifierLen);
  if (!domain) {
    return domain.error();
  }
  value.diversity_domain = DiversityDomain{std::move(domain).value()};
  if (!decode_provenance(reader, value.provenance)) {
    return make_error(ErrorCode::Malformed, "capability provenance malformed");
  }
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "capability record truncated");
  }
  value.digest = compute_digest(value);
  auto valid = validate(value);
  if (!valid) {
    return valid.error();
  }
  out = std::move(value);
  return {};
}

Result<void> canonical_decode(ByteReader& reader, Reservation& out) {
  auto header = expect_header(reader, kTagReservation);
  if (!header) {
    return header.error();
  }
  Reservation value;
  auto resource = decode_identifier(reader, limits::kMaxIdentifierLen);
  if (!resource) {
    return resource.error();
  }
  value.resource = ResourceId{std::move(resource).value()};
  value.generation = decode_gen(reader);
  auto class_id = decode_identifier(reader, limits::kMaxIdentifierLen);
  if (!class_id) {
    return class_id.error();
  }
  value.class_id = QoSClassId{std::move(class_id).value()};
  value.class_generation = decode_gen(reader);
  value.reserved_min_rate_bps = reader.u64();
  value.reserved_burst_bytes = reader.u64();
  value.epoch = decode_epoch(reader);
  auto publisher = decode_identifier(reader, limits::kMaxIdentifierLen);
  if (!publisher) {
    return publisher.error();
  }
  value.publisher = PublisherId{std::move(publisher).value()};
  value.incarnation = PublisherIncarnation{reader.u64()};
  value.expires_at_ms = reader.u64();
  if (!decode_provenance(reader, value.provenance)) {
    return make_error(ErrorCode::Malformed, "reservation provenance malformed");
  }
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "reservation record truncated");
  }
  value.digest = compute_digest(value);
  auto valid = validate(value);
  if (!valid) {
    return valid.error();
  }
  out = std::move(value);
  return {};
}

Result<void> canonical_decode(ByteReader& reader, ContractRequest& out) {
  auto header = expect_header(reader, kTagRequest);
  if (!header) {
    return header.error();
  }
  ContractRequest value;
  struct Field {
    std::string* text;
    Generation* generation;
  };
  const Field text_fields[] = {
      {&value.contract_text_ref(), &value.contract_generation},
      {&value.subject_text_ref(), &value.subject_generation},
      {&value.class_text_ref(), &value.class_generation},
      {&value.path_text_ref(), &value.path_generation},
      {&value.policy_text_ref(), &value.policy_generation},
      {&value.priority_text_ref(), &value.priority_generation},
  };
  for (const Field& field : text_fields) {
    auto text = decode_identifier(reader, limits::kMaxIdentifierLen);
    if (!text) {
      return text.error();
    }
    *field.text = std::move(text).value();
    *field.generation = decode_gen(reader);
  }
  value.observed_epoch = decode_epoch(reader);
  value.request_ms = reader.u64();
  if (!decode_provenance(reader, value.provenance)) {
    return make_error(ErrorCode::Malformed, "request provenance malformed");
  }
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "request record truncated");
  }
  auto valid = validate(value);
  if (!valid) {
    return valid.error();
  }
  out = std::move(value);
  return {};
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------
namespace {

Digest256 digest_of_encoding(const std::vector<std::uint8_t>& bytes) {
  return sha256(bytes.data(), bytes.size());
}

}  // namespace

// A content digest addresses the definition, not the assertion. Epoch,
// timestamps and provenance describe when and by whom a definition was
// asserted, so re-asserting byte-identical content in a later epoch yields
// the same digest -- which is exactly what makes equivocation detectable and
// idempotent re-publication harmless.

Digest256 compute_digest(const QoSClass& value) {
  QoSClass normalized = value;
  normalized.digest = Digest256{};
  normalized.published_epoch = FabricEpoch{};
  normalized.provenance = Provenance{};
  ByteWriter writer;
  canonical_encode(normalized, writer);
  return writer.ok() ? digest_of_encoding(writer.data()) : Digest256{};
}

Digest256 compute_digest(const Policy& value) {
  Policy normalized = value;
  normalized.digest = Digest256{};
  normalized.published_epoch = FabricEpoch{};
  normalized.provenance = Provenance{};
  ByteWriter writer;
  canonical_encode(normalized, writer);
  return writer.ok() ? digest_of_encoding(writer.data()) : Digest256{};
}

Digest256 compute_digest(const Path& value) {
  Path normalized = value;
  normalized.digest = Digest256{};
  normalized.epoch = FabricEpoch{};
  normalized.provenance = Provenance{};
  ByteWriter writer;
  canonical_encode(normalized, writer);
  return writer.ok() ? digest_of_encoding(writer.data()) : Digest256{};
}

Digest256 compute_digest(const ResourceCapability& value) {
  ResourceCapability normalized = value;
  normalized.digest = Digest256{};
  normalized.epoch = FabricEpoch{};
  normalized.published_at_ms = 0;
  normalized.expires_at_ms = 0;
  normalized.provenance = Provenance{};
  ByteWriter writer;
  canonical_encode(normalized, writer);
  return writer.ok() ? digest_of_encoding(writer.data()) : Digest256{};
}

Digest256 compute_digest(const Reservation& value) {
  Reservation normalized = value;
  normalized.digest = Digest256{};
  normalized.epoch = FabricEpoch{};
  normalized.expires_at_ms = 0;
  normalized.provenance = Provenance{};
  ByteWriter writer;
  canonical_encode(normalized, writer);
  return writer.ok() ? digest_of_encoding(writer.data()) : Digest256{};
}

Digest256 compute_digest(const ContractRequest& value) {
  ByteWriter writer;
  canonical_encode(value, writer);
  return writer.ok() ? digest_of_encoding(writer.data()) : Digest256{};
}

// ---------------------------------------------------------------------------
// Summaries
// ---------------------------------------------------------------------------
namespace {

std::string summarize_obligation_list(const QoSClass& value) {
  std::string out;
  auto append = [&out](std::string_view piece) {
    if (!out.empty()) {
      out += ',';
    }
    out += piece;
  };
  if (value.min_rate_bps.has_value()) {
    append("rate.min_bps");
  }
  if (value.max_rate_bps.has_value()) {
    append("rate.max_bps");
  }
  if (value.peak_rate_bps.has_value()) {
    append("rate.peak_bps");
  }
  if (value.max_latency_us.has_value()) {
    append("latency.max_us");
  }
  if (value.max_jitter_us.has_value()) {
    append("jitter.max_us");
  }
  if (value.max_loss_ppm.has_value()) {
    append("loss.max_ppm");
  }
  if (value.burst_bytes.has_value()) {
    append("burst");
  }
  if (value.min_mtu_bytes.has_value()) {
    append("mtu.min_bytes");
  }
  append("isolation.level");
  append("treatment.mode");
  append("priority.min_rank");
  if (value.requires_reservation) {
    append("reservation.min_rate_bps");
  }
  if (value.min_disjoint_paths > 1) {
    append("redundancy.disjoint_paths");
  }
  if (!value.required_predicates.empty()) {
    append("capability.predicate");
  }
  return out;
}

}  // namespace

std::string summarize(const QoSClass& value) {
  std::string out = "class ";
  out += value.id.view();
  out += "@";
  out += std::to_string(value.generation.value());
  out += " [";
  out += summarize_obligation_list(value);
  out += "] isolation=";
  out += to_string(value.isolation);
  out += " treatment=";
  out += to_string(value.treatment);
  out += " violation=";
  out += to_string(value.violation_policy);
  if (value.allow_degrade) {
    out += " degrade<=";
    out += std::to_string(value.degrade_max_relaxation_ppm);
    out += "ppm";
  } else {
    out += " degrade=forbidden";
  }
  return out;
}

std::string summarize(const Path& value) {
  std::string out = "path ";
  out += value.id.view();
  out += "@";
  out += std::to_string(value.generation.value());
  out += " components=";
  out += std::to_string(value.components.size());
  out += " domains=";
  std::vector<std::string_view> domains;
  for (const PathComponent& component : value.components) {
    if (std::find(domains.begin(), domains.end(), component.diversity_domain.view()) ==
        domains.end()) {
      domains.push_back(component.diversity_domain.view());
    }
  }
  out += std::to_string(domains.size());
  return out;
}

std::string summarize(const ResourceCapability& value) {
  std::string out = "capability ";
  out += value.resource.view();
  out += "@";
  out += std::to_string(value.generation.value());
  out += " state=";
  out += to_string(value.state);
  out += " epoch=";
  out += std::to_string(value.epoch.value());
  out += " publisher=";
  out += value.publisher.view();
  out += "#";
  out += std::to_string(value.incarnation.value());
  return out;
}

}  // namespace qosfabric
