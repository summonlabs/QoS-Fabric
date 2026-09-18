// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The decision surface: outcomes, per-obligation and per-resource
// assessments, the authority vector, degradation reasons, and the decision
// digest that binds a verdict to the exact evidence that justified it.

#ifndef QOSFABRIC_DECISION_HPP
#define QOSFABRIC_DECISION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "qosfabric/crypto.hpp"
#include "qosfabric/ids.hpp"
#include "qosfabric/limits.hpp"
#include "qosfabric/model.hpp"

namespace qosfabric {

// Outcome precedence, from weakest to strongest. The enumerator order *is*
// the precedence order: the dominant outcome of any set of findings is the
// one with the largest enumerator value. A definitive negative always
// outranks missing evidence, missing or superseded authority always outranks
// a negative, and contradiction always outranks everything short of a
// malformed request.
enum class Outcome : std::uint8_t {
  // Every required obligation is satisfied by every required component with
  // live, current evidence, and nothing was relaxed.
  Supported = 0,
  // Everything is enforceable, but at least one explicit, policy-permitted
  // relaxation or resource-declared degradation is in force. Never silent.
  SupportedDegraded = 1,
  // At least one required component offers no evidence. UNKNOWN never
  // becomes SUPPORTED.
  Unknown = 2,
  // At least one required component is definitively unable to satisfy a
  // binding obligation, or is declared unavailable.
  Unsupported = 3,
  // Applicability is invalidated: superseded or expired authority, path,
  // capability or epoch.
  Stale = 4,
  // Contradiction: an equivocated definition, a policy that denies the
  // referenced class, or mutually impossible requirements.
  Conflict = 5,
  // The contract cannot be formed at all: structurally invalid input, or a
  // referenced authority definition that does not exist.
  Rejected = 6,
};

[[nodiscard]] const char* to_string(Outcome value) noexcept;
[[nodiscard]] bool parse_outcome(std::string_view text, Outcome& out) noexcept;
[[nodiscard]] constexpr bool is_positive(Outcome value) noexcept {
  return value == Outcome::Supported || value == Outcome::SupportedDegraded;
}
// The dominant (highest-precedence) of two outcomes.
[[nodiscard]] constexpr Outcome dominant(Outcome a, Outcome b) noexcept {
  return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
}
constexpr void merge(Outcome& target, Outcome value) noexcept {
  target = dominant(target, value);
}

// Status of one obligation, composed across the whole path.
enum class ObligationStatus : std::uint8_t {
  // The composed end-to-end value satisfies the requirement.
  Satisfied = 0,
  // The requirement is met only inside an explicit, permitted relaxation.
  Degraded = 1,
  // The composed end-to-end value violates a binding requirement.
  Violated = 2,
  // A required component did not declare the attribute the obligation needs.
  Unknown = 3,
  // A required component declared itself unavailable.
  Unavailable = 4,
  // The evidence behind a required component is superseded or expired.
  Stale = 5,
  // The obligation does not apply to this path.
  NotApplicable = 6,
};

[[nodiscard]] const char* to_string(ObligationStatus value) noexcept;
[[nodiscard]] constexpr bool is_negative(ObligationStatus value) noexcept {
  return value == ObligationStatus::Violated || value == ObligationStatus::Unavailable ||
         value == ObligationStatus::Stale || value == ObligationStatus::Unknown;
}

// Rolled-up status of one required path component.
enum class ComponentStatus : std::uint8_t {
  Satisfied = 0,
  // The resource itself declared a degraded state, or an obligation on it was
  // satisfied only under an explicit relaxation.
  Degraded = 1,
  Unknown = 2,
  Unavailable = 3,
  Violated = 4,
  Stale = 5,
};

[[nodiscard]] const char* to_string(ComponentStatus value) noexcept;

// ---------------------------------------------------------------------------
// Authority vector
// ---------------------------------------------------------------------------
class AuthorityEntry {
 public:
  enum class Dimension : std::uint8_t {
    Class = 0,
    Subject = 1,
    Path = 2,
    Policy = 3,
    Priority = 4,
    Capability = 5,
    Reservation = 6,
    FabricEpoch = 7,
    Count = 8,
  };

  Dimension dimension{Dimension::Class};
  std::string id{};
  Generation generation{};
  std::string digest_hex{};
  FabricEpoch epoch{};
  std::string source{};
};

[[nodiscard]] const char* to_string(AuthorityEntry::Dimension value) noexcept;

// ---------------------------------------------------------------------------
// Per-obligation assessment
// ---------------------------------------------------------------------------
class ObligationAssessment {
 public:
  ObligationKind kind{ObligationKind::Isolation};
  ObligationId id{};
  bool binding{true};
  bool relaxable{false};
  CompareDirection direction{CompareDirection::AtLeast};
  AggregateKind aggregate{AggregateKind::WeakestLink};
  bool path_level{false};

  // The requirement as declared by the class, and the limit actually applied
  // after any permitted relaxation.
  std::uint64_t required_value{0};
  std::uint64_t effective_limit{0};
  // The composed end-to-end value. Absent when a required component offered
  // no evidence for this attribute.
  std::optional<std::uint64_t> composed_value{};

  std::uint32_t relaxation_applied_ppm{0};
  bool relaxation_reported_only{false};
  ObligationStatus status{ObligationStatus::Satisfied};

  // The component that determines the composed value (weakest link, largest
  // contributor, or the component that failed), and the component with the
  // least headroom.
  std::optional<ResourceId> binding_resource{};
  std::optional<ResourceId> weakest_resource{};
  std::uint32_t headroom_ppm{0};

  std::string detail{};
};

// ---------------------------------------------------------------------------
// Per-resource assessment
// ---------------------------------------------------------------------------
class ResourceAssessment {
 public:
  ResourceId resource{};
  DiversityDomain diversity_domain{};
  PathRole role{PathRole::Member};
  Generation bound_capability_generation{};
  std::optional<Generation> observed_capability_generation{};
  std::optional<PublisherIncarnation> observed_incarnation{};
  ComponentStatus status{ComponentStatus::Unknown};
  bool self_declared_degraded{false};
  std::string reason{};
};

// ---------------------------------------------------------------------------
// Degradation record
// ---------------------------------------------------------------------------
class DegradationReason {
 public:
  ObligationKind kind{ObligationKind::Isolation};
  ObligationId id{};
  std::optional<ResourceId> resource{};
  std::uint32_t relaxation_ppm{0};
  bool reported_only{false};
  std::string detail{};
};

// ---------------------------------------------------------------------------
// Conflict evidence
// ---------------------------------------------------------------------------
// Recorded when a publisher has asserted two different contents for the same
// (identifier, generation). Equivocation is durable evidence, not a transient
// error: every later contract that depends on the equivocated definition is
// CONFLICT until the definition is superseded.
class ConflictMarker {
 public:
  enum class Scope : std::uint8_t {
    Class = 0,
    Policy = 1,
    Path = 2,
    Capability = 3,
    Reservation = 4,
    Count = 5,
  };

  Scope scope{Scope::Class};
  std::string id{};
  Generation generation{};
  Digest256 incumbent_digest{};
  Digest256 challenger_digest{};
  FabricEpoch epoch{};
  std::string detail{};
};

[[nodiscard]] const char* to_string(ConflictMarker::Scope value) noexcept;

// ---------------------------------------------------------------------------
// Contract decision
// ---------------------------------------------------------------------------
class ContractDecision {
 public:
  QoSContractId contract{};
  Generation contract_generation{};
  Outcome outcome{Outcome::Rejected};

  QoSClassId class_id{};
  Generation class_generation{};
  Digest256 class_digest{};

  SubjectId subject{};
  Generation subject_generation{};

  PathId path{};
  Generation path_generation{};
  Digest256 path_digest{};

  PolicyId policy{};
  Generation policy_generation{};
  Digest256 policy_digest{};

  PriorityClassId priority{};
  Generation priority_generation{};

  FabricEpoch fabric_epoch{};
  FabricEpoch observed_epoch{};

  std::uint64_t decided_at_ms{0};

  std::vector<ObligationAssessment> obligations{};
  std::vector<ResourceAssessment> resources{};
  std::vector<DegradationReason> degradations{};
  std::vector<AuthorityEntry> authority{};

  // The obligation that determined the verdict: for a negative verdict the
  // most severe finding in canonical order, for a degraded verdict the
  // largest applied relaxation, and for a clean verdict the obligation with
  // the least relative headroom.
  std::optional<ObligationKind> binding_obligation{};
  std::optional<ResourceId> binding_resource{};

  // Total number of required components that offered no evidence.
  std::uint32_t unknown_components{0};
  std::uint32_t violated_obligations{0};
  std::uint32_t unsatisfied_diversity_domains{0};

  std::string primary_reason{};
  Digest256 request_digest{};
  Digest256 decision_digest{};
  Provenance provenance{};

  [[nodiscard]] const ObligationAssessment* find(ObligationKind kind) const noexcept;
  [[nodiscard]] bool has_degradation() const noexcept { return !degradations.empty(); }
};

// Serializes the decision into the canonical form whose digest becomes
// ContractDecision::decision_digest.
void canonical_encode(const ContractDecision& value, ByteWriter& writer);
// Reconstructs a recorded decision from its canonical form. Every structural
// budget is enforced on the way in, so a corrupted or hostile audit record is
// rejected rather than partially interpreted.
[[nodiscard]] Result<void> canonical_decode(ByteReader& reader, ContractDecision& out);
[[nodiscard]] Digest256 compute_decision_digest(const ContractDecision& value);

// Renders the decision as bounded, human-readable text. The renderer obeys
// kMaxExplainBytes, kMaxExplainObligations and kMaxExplainComponents.
class ExplainOptions {
 public:
  std::size_t max_bytes{limits::kMaxExplainBytes};
  std::size_t max_obligations{limits::kMaxExplainObligations};
  std::size_t max_resources{limits::kMaxExplainComponents};
  bool include_authority{true};
  bool include_timings{false};
};

[[nodiscard]] std::string explain(const ContractDecision& decision,
                                  const ExplainOptions& options = {});

}  // namespace qosfabric

#endif  // QOSFABRIC_DECISION_HPP
