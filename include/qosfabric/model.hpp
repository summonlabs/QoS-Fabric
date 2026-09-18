// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The QoS Fabric domain model.
//
// Boundary: QoS Fabric owns service-class semantics and the end-to-end
// obligations those semantics imply. It consumes resource capability
// declarations, path descriptions, reservations made by other systems,
// policy, and generation/epoch bookkeeping, and it produces an authoritative
// verdict. It never admits traffic, reserves bandwidth, arbitrates capacity,
// schedules flows, places paths, enforces rates, configures queues, or
// collects telemetry.

#ifndef QOSFABRIC_MODEL_HPP
#define QOSFABRIC_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "qosfabric/crypto.hpp"
#include "qosfabric/ids.hpp"
#include "qosfabric/limits.hpp"
#include "qosfabric/serialize.hpp"

namespace qosfabric {

struct DiversityDomainTag;
using DiversityDomain = StringId<DiversityDomainTag>;

// ---------------------------------------------------------------------------
// Ordered service semantics
// ---------------------------------------------------------------------------

// Isolation ladder. Higher is stronger. A capability declares the strongest
// isolation it can provide; a class declares the weakest it will accept.
enum class IsolationLevel : std::uint8_t {
  None = 0,
  Shared = 1,
  QueueIsolated = 2,
  ResourceIsolated = 3,
};

// Service treatment ladder. Higher is stronger. Used only for compatibility
// comparison; QoS Fabric does not configure the treatment it names.
enum class TreatmentMode : std::uint8_t {
  BestEffort = 0,
  WeightedFair = 1,
  RateLimited = 2,
  PriorityQueue = 3,
  StrictPriorityLowLatency = 4,
};

// What a class says must happen when a binding obligation cannot be met.
enum class ViolationPolicy : std::uint8_t {
  // Never degrade this class, whatever policy permits. A violation is a
  // definitive negative.
  Reject = 0,
  // Degrade inside the explicit window, otherwise reject. The default.
  DegradeThenReject = 1,
  // The class author declares the bounded obligations advisory: a violation is
  // reported and forces SUPPORTED_DEGRADED, and can never yield a clean
  // SUPPORTED. Requires both a class-declared degrade window and policy
  // permission; validation refuses the class otherwise.
  ReportOnly = 2,
};

enum class CapabilityState : std::uint8_t {
  Unknown = 0,
  Unavailable = 1,
  Degraded = 2,
  Available = 3,
};

enum class PathRole : std::uint8_t {
  Member = 0,
  Primary = 1,
  Protection = 2,
};

// The kinds of obligation a service class can imply. The numeric values are
// the canonical ordering used whenever obligations are listed or digested.
enum class ObligationKind : std::uint8_t {
  MinRate = 0,
  MaxRate = 1,
  PeakRate = 2,
  Latency = 3,
  Jitter = 4,
  Loss = 5,
  BurstBytes = 6,
  BurstInterval = 7,
  Mtu = 8,
  Isolation = 9,
  Treatment = 10,
  PriorityRank = 11,
  Reservation = 12,
  DisjointPaths = 13,
  Predicate = 14,
  // The declared liveness state of a required resource. Present in every
  // obligation set so that a resource which is merely "degraded" can never be
  // reported as a clean SUPPORTED without an explicit, policy-permitted
  // degradation record.
  ResourceState = 15,
  Count = 16,
};

// How a per-component capability value is compared with the requirement.
enum class CompareDirection : std::uint8_t {
  AtLeast = 0,  // capability must be >= requirement
  AtMost = 1,   // capability must be <= requirement
};

// How per-component capability values compose into an end-to-end value.
enum class AggregateKind : std::uint8_t {
  WeakestLink = 0,  // min over components
  Additive = 1,     // checked sum over components
  LossCompose = 2,  // 1 - product(1 - loss_i)
  WorstCase = 3,    // max over components
  AllPresent = 4,   // every component must carry the item
  PathDiversity = 5,
};

[[nodiscard]] const char* to_string(IsolationLevel value) noexcept;
[[nodiscard]] const char* to_string(TreatmentMode value) noexcept;
[[nodiscard]] const char* to_string(ViolationPolicy value) noexcept;
[[nodiscard]] const char* to_string(CapabilityState value) noexcept;
[[nodiscard]] const char* to_string(PathRole value) noexcept;
[[nodiscard]] const char* to_string(ObligationKind value) noexcept;
[[nodiscard]] const char* to_string(CompareDirection value) noexcept;
[[nodiscard]] const char* to_string(AggregateKind value) noexcept;

[[nodiscard]] bool parse_isolation_level(std::string_view text, IsolationLevel& out) noexcept;
[[nodiscard]] bool parse_treatment_mode(std::string_view text, TreatmentMode& out) noexcept;
[[nodiscard]] bool parse_path_role(std::string_view text, PathRole& out) noexcept;
[[nodiscard]] bool parse_capability_state(std::string_view text, CapabilityState& out) noexcept;
[[nodiscard]] bool parse_violation_policy(std::string_view text, ViolationPolicy& out) noexcept;

[[nodiscard]] constexpr std::uint8_t rank(IsolationLevel value) noexcept {
  return static_cast<std::uint8_t>(value);
}
[[nodiscard]] constexpr std::uint8_t rank(TreatmentMode value) noexcept {
  return static_cast<std::uint8_t>(value);
}

// Canonical obligation identifier text, for example "latency.max_us".
[[nodiscard]] std::string_view obligation_name(ObligationKind kind) noexcept;
[[nodiscard]] CompareDirection obligation_direction(ObligationKind kind) noexcept;
[[nodiscard]] AggregateKind obligation_aggregate(ObligationKind kind) noexcept;
// A relaxable obligation may be degraded inside an explicit, policy-bound
// window. Structural obligations (isolation, treatment, priority, predicates,
// reservations) are never relaxable: they cannot be silently dropped.
[[nodiscard]] bool obligation_relaxable(ObligationKind kind) noexcept;

// ---------------------------------------------------------------------------
// Service class definition
// ---------------------------------------------------------------------------
class QoSClass {
 public:
  QoSClassId id{};
  Generation generation{};
  std::string label{};

  // Rate envelope in bits per second.
  std::optional<std::uint64_t> min_rate_bps{};
  std::optional<std::uint64_t> max_rate_bps{};
  std::optional<std::uint64_t> peak_rate_bps{};

  // Path bounds.
  std::optional<std::uint64_t> max_latency_us{};
  std::optional<std::uint64_t> max_jitter_us{};
  std::optional<std::uint32_t> max_loss_ppm{};

  // Burst semantics: a burst of burst_bytes must be absorbable inside
  // burst_interval_us.
  std::optional<std::uint64_t> burst_bytes{};
  std::optional<std::uint64_t> burst_interval_us{};

  std::optional<std::uint64_t> min_mtu_bytes{};

  IsolationLevel isolation{IsolationLevel::None};
  TreatmentMode treatment{TreatmentMode::BestEffort};
  std::uint32_t min_priority_rank{0};

  // Path diversity: how many distinct failure domains must independently
  // satisfy every other obligation.
  std::uint32_t min_disjoint_paths{1};

  bool requires_reservation{false};

  std::vector<CapabilityKey> required_predicates{};

  // Explicit, bounded degradation permission carried by the class itself.
  bool allow_degrade{false};
  std::uint32_t degrade_max_relaxation_ppm{0};
  ViolationPolicy violation_policy{ViolationPolicy::DegradeThenReject};

  FabricEpoch published_epoch{};
  Digest256 digest{};
  Provenance provenance{};

  [[nodiscard]] bool has_min_rate() const noexcept { return min_rate_bps.has_value(); }
};

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------
class Policy {
 public:
  PolicyId id{};
  Generation generation{};
  std::string label{};

  // Whether any degradation is permitted at all. When false, no relaxation of
  // a relaxable obligation is ever applied and SUPPORTED_DEGRADED is
  // unreachable.
  bool degrade_permitted{false};
  // Hard ceiling on relaxation, applied together with (never beyond) the
  // class-declared window.
  std::uint32_t max_relaxation_ppm{0};

  // How far behind the current fabric epoch evidence may be before it is
  // considered stale rather than merely old.
  std::uint32_t max_stale_epochs{0};
  // How old a capability declaration may be before it is considered stale.
  std::uint64_t capability_max_age_ms{limits::kMaxLeaseMs};
  // How many required components may be UNKNOWN while still allowing a
  // positive verdict. Zero means UNKNOWN never becomes SUPPORTED.
  std::uint32_t max_unknown_components{0};

  // Predicates the policy itself demands of every component, independent of
  // what the class asks for.
  std::vector<CapabilityKey> required_predicates{};
  // Classes the policy refuses outright. Referencing one is a CONFLICT.
  std::vector<QoSClassId> denied_classes{};

  FabricEpoch published_epoch{};
  Digest256 digest{};
  Provenance provenance{};
};

// ---------------------------------------------------------------------------
// Path
// ---------------------------------------------------------------------------
class PathComponent {
 public:
  ResourceId resource{};
  // The exact ResourceCapabilityGeneration this path was computed against.
  // A capability whose generation differs is not silently accepted.
  Generation capability_generation{};
  DiversityDomain diversity_domain{};
  PathRole role{PathRole::Member};
};

class Path {
 public:
  PathId id{};
  Generation generation{};
  std::vector<PathComponent> components{};
  // Fixed overhead contributed by the path itself (for example, media
  // propagation declared by the path authority).
  std::uint64_t fixed_latency_us{0};
  std::uint64_t fixed_jitter_us{0};
  FabricEpoch epoch{};
  Digest256 digest{};
  Provenance provenance{};

  [[nodiscard]] std::size_t size() const noexcept { return components.size(); }
};

// ---------------------------------------------------------------------------
// Resource capability declaration
// ---------------------------------------------------------------------------
class ResourceCapability {
 public:
  ResourceId resource{};
  Generation generation{};
  FabricEpoch epoch{};

  PublisherId publisher{};
  PublisherIncarnation incarnation{};

  std::uint64_t published_at_ms{0};
  std::uint64_t expires_at_ms{0};

  CapabilityState state{CapabilityState::Unknown};

  // Highest rate the resource declares it can carry for this class.
  std::optional<std::uint64_t> supported_rate_bps{};
  // Worst-case latency and jitter the resource adds.
  std::optional<std::uint64_t> latency_bound_us{};
  std::optional<std::uint64_t> jitter_bound_us{};
  std::optional<std::uint32_t> loss_ppm{};
  // Largest burst the resource can absorb and the window it needs to do so.
  std::optional<std::uint64_t> max_burst_bytes{};
  std::optional<std::uint64_t> burst_window_us{};
  std::optional<std::uint64_t> mtu_bytes{};

  IsolationLevel max_isolation{IsolationLevel::None};
  TreatmentMode max_treatment{TreatmentMode::BestEffort};
  std::uint32_t max_priority_rank{0};

  std::vector<CapabilityKey> predicates{};

  DiversityDomain diversity_domain{};

  Digest256 digest{};
  Provenance provenance{};

  [[nodiscard]] bool declares_treatment(TreatmentMode mode) const noexcept {
    return rank(max_treatment) >= rank(mode);
  }
};

// ---------------------------------------------------------------------------
// Reservation (made by another system; verified here, never created here)
// ---------------------------------------------------------------------------
class Reservation {
 public:
  ResourceId resource{};
  Generation generation{};
  QoSClassId class_id{};
  Generation class_generation{};
  std::uint64_t reserved_min_rate_bps{0};
  std::uint64_t reserved_burst_bytes{0};
  FabricEpoch epoch{};
  PublisherId publisher{};
  PublisherIncarnation incarnation{};
  std::uint64_t expires_at_ms{0};
  Digest256 digest{};
  Provenance provenance{};
};

// ---------------------------------------------------------------------------
// Contract request
// ---------------------------------------------------------------------------
class ContractRequest {
 public:
  QoSContractId contract{};
  Generation contract_generation{};

  SubjectId subject{};
  Generation subject_generation{};

  QoSClassId class_id{};
  Generation class_generation{};  // zero selects the newest published generation

  PathId path{};
  Generation path_generation{};  // zero selects the newest published generation

  PolicyId policy{};
  Generation policy_generation{};  // zero selects the newest published generation

  PriorityClassId priority{};
  Generation priority_generation{};

  // The fabric epoch the requester observed. Zero means "use the current
  // epoch". A lagging observation is a stale authority claim.
  FabricEpoch observed_epoch{};

  std::uint64_t request_ms{0};

  Provenance provenance{};

  // Decoder binding hooks. Each returns the raw storage of a strongly typed
  // identifier that the canonical decoder has already validated.
  [[nodiscard]] std::string& contract_text_ref() noexcept { return contract.bind_unchecked(); }
  [[nodiscard]] std::string& subject_text_ref() noexcept { return subject.bind_unchecked(); }
  [[nodiscard]] std::string& class_text_ref() noexcept { return class_id.bind_unchecked(); }
  [[nodiscard]] std::string& path_text_ref() noexcept { return path.bind_unchecked(); }
  [[nodiscard]] std::string& policy_text_ref() noexcept { return policy.bind_unchecked(); }
  [[nodiscard]] std::string& priority_text_ref() noexcept { return priority.bind_unchecked(); }
};

// ---------------------------------------------------------------------------
// Validation: every externally supplied structure is validated before use.
// ---------------------------------------------------------------------------
[[nodiscard]] Result<void> validate(const QoSClass& value);
[[nodiscard]] Result<void> validate(const Policy& value);
[[nodiscard]] Result<void> validate(const Path& value);
[[nodiscard]] Result<void> validate(const ResourceCapability& value);
[[nodiscard]] Result<void> validate(const Reservation& value);
[[nodiscard]] Result<void> validate(const ContractRequest& value);

// ---------------------------------------------------------------------------
// Canonical encoding, decoding and content digests
// ---------------------------------------------------------------------------
void canonical_encode(const QoSClass& value, ByteWriter& writer);
void canonical_encode(const Policy& value, ByteWriter& writer);
void canonical_encode(const Path& value, ByteWriter& writer);
void canonical_encode(const ResourceCapability& value, ByteWriter& writer);
void canonical_encode(const Reservation& value, ByteWriter& writer);
void canonical_encode(const ContractRequest& value, ByteWriter& writer);

[[nodiscard]] Result<void> canonical_decode(ByteReader& reader, QoSClass& out);
[[nodiscard]] Result<void> canonical_decode(ByteReader& reader, Policy& out);
[[nodiscard]] Result<void> canonical_decode(ByteReader& reader, Path& out);
[[nodiscard]] Result<void> canonical_decode(ByteReader& reader, ResourceCapability& out);
[[nodiscard]] Result<void> canonical_decode(ByteReader& reader, Reservation& out);
[[nodiscard]] Result<void> canonical_decode(ByteReader& reader, ContractRequest& out);

[[nodiscard]] Digest256 compute_digest(const QoSClass& value);
[[nodiscard]] Digest256 compute_digest(const Policy& value);
[[nodiscard]] Digest256 compute_digest(const Path& value);
[[nodiscard]] Digest256 compute_digest(const ResourceCapability& value);
[[nodiscard]] Digest256 compute_digest(const Reservation& value);
[[nodiscard]] Digest256 compute_digest(const ContractRequest& value);

// Renders a bounded single-line summary; used by the CLI and by explanations.
[[nodiscard]] std::string summarize(const QoSClass& value);
[[nodiscard]] std::string summarize(const Path& value);
[[nodiscard]] std::string summarize(const ResourceCapability& value);

}  // namespace qosfabric

#endif  // QOSFABRIC_MODEL_HPP
