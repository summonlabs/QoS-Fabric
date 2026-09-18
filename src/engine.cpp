// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/engine.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "qosfabric/checked.hpp"

namespace qosfabric {
namespace {

constexpr std::uint64_t kPpm = 1'000'000ull;
constexpr std::uint64_t kStateAvailable = 3;

// ---------------------------------------------------------------------------
// Obligation plan
// ---------------------------------------------------------------------------
class ObligationPlan {
 public:
  ObligationKind kind{ObligationKind::Isolation};
  ObligationId id{};
  CapabilityKey predicate{};
  std::uint64_t required{0};
  bool binding{true};
};

[[nodiscard]] bool plan_less(const ObligationPlan& a, const ObligationPlan& b) noexcept {
  if (a.kind != b.kind) {
    return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
  }
  return a.id.view() < b.id.view();
}

[[nodiscard]] ObligationId make_id(ObligationKind kind, std::string_view suffix = {}) {
  std::string text(obligation_name(kind));
  if (!suffix.empty()) {
    text += '.';
    text += suffix;
  }
  if (text.size() > limits::kMaxIdentifierLen) {
    text.resize(limits::kMaxIdentifierLen);
  }
  return ObligationId{std::move(text)};
}

[[nodiscard]] std::vector<CapabilityKey> merged_predicates(const QoSClass& klass,
                                                           const Policy& policy) {
  std::vector<CapabilityKey> merged = klass.required_predicates;
  for (const CapabilityKey& key : policy.required_predicates) {
    bool found = false;
    for (const CapabilityKey& existing : merged) {
      if (existing == key) {
        found = true;
        break;
      }
    }
    if (!found) {
      merged.push_back(key);
    }
  }
  std::sort(merged.begin(), merged.end(),
            [](const CapabilityKey& a, const CapabilityKey& b) { return a.view() < b.view(); });
  return merged;
}

// Builds the canonical obligation set the class and policy imply. The set is
// sorted by (kind, identifier) so that two equal inputs always produce the
// same obligation ordering, and therefore the same decision digest.
[[nodiscard]] std::vector<ObligationPlan> build_plan(const QoSClass& klass, const Policy& policy) {
  std::vector<ObligationPlan> plan;
  plan.reserve(32);
  auto push = [&plan](ObligationKind kind, std::uint64_t required_value) {
    ObligationPlan item;
    item.kind = kind;
    item.id = make_id(kind);
    item.required = required_value;
    plan.push_back(std::move(item));
  };

  if (klass.min_rate_bps.has_value()) {
    push(ObligationKind::MinRate, *klass.min_rate_bps);
  }
  if (klass.max_rate_bps.has_value()) {
    push(ObligationKind::MaxRate, *klass.max_rate_bps);
  }
  if (klass.peak_rate_bps.has_value()) {
    push(ObligationKind::PeakRate, *klass.peak_rate_bps);
  }
  if (klass.max_latency_us.has_value()) {
    push(ObligationKind::Latency, *klass.max_latency_us);
  }
  if (klass.max_jitter_us.has_value()) {
    push(ObligationKind::Jitter, *klass.max_jitter_us);
  }
  if (klass.max_loss_ppm.has_value()) {
    push(ObligationKind::Loss, *klass.max_loss_ppm);
  }
  if (klass.burst_bytes.has_value()) {
    push(ObligationKind::BurstBytes, *klass.burst_bytes);
  }
  if (klass.burst_interval_us.has_value()) {
    push(ObligationKind::BurstInterval, *klass.burst_interval_us);
  }
  if (klass.min_mtu_bytes.has_value()) {
    push(ObligationKind::Mtu, *klass.min_mtu_bytes);
  }
  push(ObligationKind::Isolation, rank(klass.isolation));
  push(ObligationKind::Treatment, rank(klass.treatment));
  push(ObligationKind::PriorityRank, klass.min_priority_rank);
  if (klass.requires_reservation && klass.min_rate_bps.has_value()) {
    push(ObligationKind::Reservation, *klass.min_rate_bps);
  }
  push(ObligationKind::DisjointPaths, klass.min_disjoint_paths);
  for (const CapabilityKey& key : merged_predicates(klass, policy)) {
    ObligationPlan item;
    item.kind = ObligationKind::Predicate;
    item.id = make_id(ObligationKind::Predicate, key.view());
    item.predicate = key;
    item.required = 1;
    plan.push_back(std::move(item));
  }
  push(ObligationKind::ResourceState, kStateAvailable);

  std::sort(plan.begin(), plan.end(), plan_less);
  plan.erase(std::unique(plan.begin(), plan.end(),
                         [](const ObligationPlan& a, const ObligationPlan& b) {
                           return a.kind == b.kind && a.id == b.id;
                         }),
             plan.end());
  if (plan.size() > limits::kMaxObligations) {
    plan.resize(limits::kMaxObligations);
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Per-component resolution
// ---------------------------------------------------------------------------
class Resolution {
 public:
  std::size_t index{0};
  ResourceId resource{};
  DiversityDomain declared_domain{};
  PathRole role{PathRole::Member};
  Generation bound_generation{};
  ComponentStatus status{ComponentStatus::Unknown};
  const ResourceCapability* capability{nullptr};
  const Reservation* reservation{nullptr};
  std::optional<Generation> observed_generation{};
  std::optional<PublisherIncarnation> observed_incarnation{};
  bool self_declared_degraded{false};
  bool domain_contradiction{false};
  std::string reason{};
};

class RawValue {
 public:
  bool present{false};
  std::uint64_t value{0};
  std::string detail{};
};

[[nodiscard]] bool reservation_matches_class(const Reservation& reservation,
                                             const QoSClass& klass) noexcept {
  return reservation.class_id == klass.id && reservation.class_generation == klass.generation;
}

// Reads one component's declared value for one obligation. A missing
// declaration is reported as absent, never as zero: an undeclared bound is
// missing evidence.
[[nodiscard]] RawValue component_value(const ObligationPlan& plan, const Resolution& resolution,
                                       const QoSClass& klass, const Policy& policy,
                                       FabricEpoch current_epoch, std::uint64_t now_ms) {
  RawValue out;
  const ResourceCapability* capability = resolution.capability;

  switch (plan.kind) {
    case ObligationKind::MinRate:
    case ObligationKind::MaxRate:
    case ObligationKind::PeakRate:
      if (capability->supported_rate_bps.has_value()) {
        out.present = true;
        out.value = *capability->supported_rate_bps;
      } else {
        out.detail = "resource declares no supported rate";
      }
      return out;
    case ObligationKind::Latency:
      if (capability->latency_bound_us.has_value()) {
        out.present = true;
        out.value = *capability->latency_bound_us;
      } else {
        out.detail = "resource declares no latency bound";
      }
      return out;
    case ObligationKind::Jitter:
      if (capability->jitter_bound_us.has_value()) {
        out.present = true;
        out.value = *capability->jitter_bound_us;
      } else {
        out.detail = "resource declares no jitter bound";
      }
      return out;
    case ObligationKind::Loss:
      if (capability->loss_ppm.has_value()) {
        out.present = true;
        out.value = *capability->loss_ppm;
      } else {
        out.detail = "resource declares no loss bound";
      }
      return out;
    case ObligationKind::BurstBytes:
      if (capability->max_burst_bytes.has_value()) {
        out.present = true;
        out.value = *capability->max_burst_bytes;
      } else {
        out.detail = "resource declares no burst capacity";
      }
      return out;
    case ObligationKind::BurstInterval:
      if (capability->burst_window_us.has_value()) {
        out.present = true;
        out.value = *capability->burst_window_us;
      } else {
        out.detail = "resource declares no burst window";
      }
      return out;
    case ObligationKind::Mtu:
      if (capability->mtu_bytes.has_value()) {
        out.present = true;
        out.value = *capability->mtu_bytes;
      } else {
        out.detail = "resource declares no MTU";
      }
      return out;
    case ObligationKind::Isolation:
      out.present = true;
      out.value = rank(capability->max_isolation);
      return out;
    case ObligationKind::Treatment:
      out.present = true;
      out.value = rank(capability->max_treatment);
      return out;
    case ObligationKind::PriorityRank:
      out.present = true;
      out.value = capability->max_priority_rank;
      return out;
    case ObligationKind::ResourceState:
      out.present = true;
      out.value = resolution.self_declared_degraded
                      ? static_cast<std::uint64_t>(CapabilityState::Degraded)
                      : static_cast<std::uint64_t>(CapabilityState::Available);
      return out;
    case ObligationKind::Reservation: {
      if (resolution.reservation == nullptr) {
        out.detail = "no reservation evidence for this resource";
        return out;
      }
      const Reservation& reservation = *resolution.reservation;
      if (!reservation_matches_class(reservation, klass)) {
        out.detail = "reservation names a different class or class generation";
        return out;
      }
      if (reservation.expires_at_ms != 0 && now_ms > reservation.expires_at_ms) {
        out.detail = "reservation expired";
        return out;
      }
      if (reservation.epoch.value() + policy.max_stale_epochs < current_epoch.value()) {
        out.detail = "reservation epoch behind the fabric epoch";
        return out;
      }
      out.present = true;
      out.value = reservation.reserved_min_rate_bps;
      return out;
    }
    case ObligationKind::Predicate: {
      for (const CapabilityKey& key : capability->predicates) {
        if (key == plan.predicate) {
          out.present = true;
          out.value = 1;
          return out;
        }
      }
      out.detail = "resource does not declare the required capability predicate";
      return out;
    }
    case ObligationKind::DisjointPaths:
    case ObligationKind::Count:
      out.detail = "obligation is evaluated at path level";
      return out;
  }
  out.detail = "unknown obligation";
  return out;
}

// ---------------------------------------------------------------------------
// End-to-end composition
// ---------------------------------------------------------------------------
class Composition {
 public:
  bool overflow{false};
  std::optional<std::uint64_t> value{};
  std::size_t binding_index{0};
  std::size_t weakest_index{0};
  std::string detail{};
};

[[nodiscard]] Composition compose_values(const AggregateKind aggregate,
                                         const std::vector<RawValue>& values,
                                         std::uint64_t fixed_overhead) {
  Composition out;
  if (values.empty()) {
    out.detail = "no components to compose";
    return out;
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!values[i].present) {
      out.binding_index = i;
      out.detail = values[i].detail;
      return out;
    }
  }

  switch (aggregate) {
    case AggregateKind::WeakestLink:
    case AggregateKind::AllPresent: {
      std::uint64_t best = values[0].value;
      std::size_t best_index = 0;
      for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i].value < best) {
          best = values[i].value;
          best_index = i;
        }
      }
      out.value = best;
      out.binding_index = best_index;
      out.weakest_index = best_index;
      return out;
    }
    case AggregateKind::WorstCase: {
      std::uint64_t worst = values[0].value;
      std::size_t worst_index = 0;
      for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i].value > worst) {
          worst = values[i].value;
          worst_index = i;
        }
      }
      out.value = worst;
      out.binding_index = worst_index;
      out.weakest_index = worst_index;
      return out;
    }
    case AggregateKind::Additive: {
      std::uint64_t total = fixed_overhead;
      std::size_t worst_index = 0;
      std::uint64_t worst = 0;
      for (std::size_t i = 0; i < values.size(); ++i) {
        if (!checked_add<std::uint64_t>(total, values[i].value, total)) {
          out.overflow = true;
          out.detail = "aggregation overflow";
          return out;
        }
        if (i == 0 || values[i].value > worst) {
          worst = values[i].value;
          worst_index = i;
        }
      }
      out.value = total;
      out.binding_index = worst_index;
      out.weakest_index = worst_index;
      return out;
    }
    case AggregateKind::LossCompose: {
      // Independent loss composition, in parts per million:
      //   delivery = product over components of (1 - loss_i)
      //   composed loss = 1 - delivery
      std::uint64_t delivery = kPpm;
      std::size_t worst_index = 0;
      std::uint64_t worst = 0;
      for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint64_t loss = values[i].value > kPpm ? kPpm : values[i].value;
        std::uint64_t product = 0;
        if (!checked_mul<std::uint64_t>(delivery, kPpm - loss, product)) {
          out.overflow = true;
          out.detail = "loss aggregation overflow";
          return out;
        }
        delivery = product / kPpm;
        if (i == 0 || loss > worst) {
          worst = loss;
          worst_index = i;
        }
      }
      out.value = kPpm - delivery;
      out.binding_index = worst_index;
      out.weakest_index = worst_index;
      return out;
    }
    case AggregateKind::PathDiversity:
      out.detail = "diversity is evaluated at path level";
      return out;
  }
  return out;
}

[[nodiscard]] std::uint32_t compute_headroom(CompareDirection direction, std::uint64_t required,
                                             std::uint64_t composed) {
  if (required == 0) {
    return 1000000u;
  }
  std::uint64_t numerator = 0;
  if (direction == CompareDirection::AtLeast) {
    if (composed <= required) {
      return 0;
    }
    numerator = composed - required;
  } else {
    if (composed >= required) {
      return 0;
    }
    numerator = required - composed;
  }
  std::uint64_t scaled = 0;
  if (!checked_mul<std::uint64_t>(numerator, kPpm, scaled)) {
    return 1000000u;
  }
  const std::uint64_t ppm = scaled / required;
  return static_cast<std::uint32_t>(ppm > kPpm ? kPpm : ppm);
}

[[nodiscard]] std::string short_text(std::string_view text) {
  return std::string(text.substr(0, std::min(text.size(), limits::kMaxDetailLen)));
}

[[nodiscard]] std::vector<std::string> denied_class_text(const Policy& policy) {
  std::vector<std::string> ids;
  ids.reserve(policy.denied_classes.size());
  for (const QoSClassId& id : policy.denied_classes) {
    ids.push_back(id.str());
  }
  return ids;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public: obligation derivation
// ---------------------------------------------------------------------------
Result<std::vector<ObligationAssessment>> derive_obligations(const QoSClass& klass,
                                                             const Policy& policy) {
  const auto class_valid = validate(klass);
  if (!class_valid) {
    return class_valid.error();
  }
  const auto policy_valid = validate(policy);
  if (!policy_valid) {
    return policy_valid.error();
  }
  const std::vector<ObligationPlan> plan = build_plan(klass, policy);
  std::vector<ObligationAssessment> out;
  out.reserve(plan.size());
  for (const ObligationPlan& item : plan) {
    ObligationAssessment assessment;
    assessment.kind = item.kind;
    assessment.id = item.id;
    assessment.binding = item.binding;
    assessment.relaxable = obligation_relaxable(item.kind);
    assessment.direction = obligation_direction(item.kind);
    assessment.aggregate = obligation_aggregate(item.kind);
    assessment.path_level = item.kind == ObligationKind::DisjointPaths;
    assessment.required_value = item.required;
    assessment.effective_limit = item.required;
    if (item.kind == ObligationKind::Predicate) {
      assessment.detail = short_text(item.predicate.view());
    }
    out.push_back(std::move(assessment));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Public: pure end-to-end evaluation
// ---------------------------------------------------------------------------
Result<ContractDecision> evaluate(const EvaluationInputs& inputs) {
  const ContractRequest& request = inputs.request;
  const QoSClass& klass = inputs.klass;
  const Policy& policy = inputs.policy;
  const Path& path = inputs.path;

  ContractDecision decision;
  decision.contract = request.contract;
  decision.contract_generation = request.contract_generation;
  decision.class_id = klass.id;
  decision.class_generation = klass.generation;
  decision.class_digest = klass.digest;
  decision.subject = request.subject;
  decision.subject_generation = request.subject_generation;
  decision.path = path.id;
  decision.path_generation = path.generation;
  decision.path_digest = path.digest;
  decision.policy = policy.id;
  decision.policy_generation = policy.generation;
  decision.policy_digest = policy.digest;
  decision.priority = request.priority;
  decision.priority_generation = request.priority_generation;
  decision.fabric_epoch = inputs.current_epoch;
  decision.observed_epoch = request.observed_epoch;
  decision.decided_at_ms = inputs.now_ms;
  decision.request_digest = compute_digest(request);
  decision.provenance = Provenance{inputs.evaluator.empty() ? std::string("qosfabric.engine")
                                                            : inputs.evaluator,
                                   inputs.current_epoch, Generation{1}, Sequence{1}};

  Outcome outcome = Outcome::Supported;
  std::string reason;

  auto note = [&reason](std::string text) {
    if (reason.empty()) {
      reason = short_text(text);
    }
  };

  // -------------------------------------------------------------------------
  // 1. Structural validation. A contract that cannot be formed is REJECTED.
  // -------------------------------------------------------------------------
  const auto request_valid = validate(request);
  if (!request_valid) {
    note(std::string("request invalid: ") + request_valid.error().detail());
    merge(outcome, Outcome::Rejected);
  }
  const auto class_valid = validate(klass);
  if (!class_valid) {
    note(std::string("class invalid: ") + class_valid.error().detail());
    merge(outcome, Outcome::Rejected);
  }
  const auto policy_valid = validate(policy);
  if (!policy_valid) {
    note(std::string("policy invalid: ") + policy_valid.error().detail());
    merge(outcome, Outcome::Rejected);
  }
  const auto path_valid = validate(path);
  if (!path_valid) {
    note(std::string("path invalid: ") + path_valid.error().detail());
    merge(outcome, Outcome::Rejected);
  }
  if (klass.id != request.class_id) {
    note("resolved class does not match the requested class identifier");
    merge(outcome, Outcome::Rejected);
  }
  if (policy.id != request.policy) {
    note("resolved policy does not match the requested policy identifier");
    merge(outcome, Outcome::Rejected);
  }
  if (path.id != request.path) {
    note("resolved path does not match the requested path identifier");
    merge(outcome, Outcome::Rejected);
  }
  if (request.class_generation.value() != 0 && request.class_generation != klass.generation) {
    note("resolved class generation does not match the requested generation");
    merge(outcome, Outcome::Rejected);
  }
  if (request.policy_generation.value() != 0 && request.policy_generation != policy.generation) {
    note("resolved policy generation does not match the requested generation");
    merge(outcome, Outcome::Rejected);
  }
  if (request.path_generation.value() != 0 && request.path_generation != path.generation) {
    note("resolved path generation does not match the requested generation");
    merge(outcome, Outcome::Rejected);
  }
  if (inputs.capabilities.size() != path.components.size()) {
    note("capability vector does not cover every path component");
    merge(outcome, Outcome::Rejected);
  }
  if (!inputs.reservations.empty() && inputs.reservations.size() != path.components.size()) {
    note("reservation vector does not cover every path component");
    merge(outcome, Outcome::Rejected);
  }

  if (outcome == Outcome::Rejected) {
    decision.outcome = Outcome::Rejected;
    decision.primary_reason = reason.empty() ? std::string("rejected at intake") : reason;
    // The authority vector is still emitted so that an operator can see which
    // definitions the rejected request referenced.
    decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::Class, klass.id.str(),
                                                klass.generation, klass.digest.hex(),
                                                klass.published_epoch, klass.provenance.origin()});
    decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::Policy, policy.id.str(),
                                                policy.generation, policy.digest.hex(),
                                                policy.published_epoch, policy.provenance.origin()});
    decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::Path, path.id.str(),
                                                path.generation, path.digest.hex(), path.epoch,
                                                path.provenance.origin()});
    decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::FabricEpoch, "fabric",
                                                Generation{inputs.current_epoch.value()},
                                                std::string{}, inputs.current_epoch,
                                                std::string("registry")});
    decision.decision_digest = compute_decision_digest(decision);
    return decision;
  }

  // -------------------------------------------------------------------------
  // 2. Conflicts. Contradiction outranks every other finding short of a
  //    malformed request, because no per-resource evidence can resolve a
  //    definition that disagrees with itself.
  // -------------------------------------------------------------------------
  for (const std::string& denied : denied_class_text(policy)) {
    if (denied == klass.id.view()) {
      note("policy denies the referenced service class");
      merge(outcome, Outcome::Conflict);
      break;
    }
  }

  for (const ConflictMarker& marker : inputs.conflicts) {
    bool applies = false;
    switch (marker.scope) {
      case ConflictMarker::Scope::Class:
        applies = (marker.id == klass.id.view());
        break;
      case ConflictMarker::Scope::Policy:
        applies = (marker.id == policy.id.view());
        break;
      case ConflictMarker::Scope::Path:
        applies = (marker.id == path.id.view());
        break;
      case ConflictMarker::Scope::Capability:
      case ConflictMarker::Scope::Reservation:
        for (const PathComponent& component : path.components) {
          if (marker.id == component.resource.view()) {
            applies = true;
            break;
          }
        }
        break;
      case ConflictMarker::Scope::Count:
        break;
    }
    if (applies) {
      std::string text = "equivocated ";
      text += to_string(marker.scope);
      text += " definition ";
      text += marker.id;
      text += "@";
      text += std::to_string(marker.generation.value());
      note(text);
      merge(outcome, Outcome::Conflict);
      break;
    }
  }

  // -------------------------------------------------------------------------
  // 3. Per-component capability resolution.
  // -------------------------------------------------------------------------
  std::vector<Resolution> resolutions;
  resolutions.reserve(path.components.size());
  for (std::size_t i = 0; i < path.components.size(); ++i) {
    const PathComponent& component = path.components[i];
    Resolution resolution;
    resolution.index = i;
    resolution.resource = component.resource;
    resolution.declared_domain = component.diversity_domain;
    resolution.role = component.role;
    resolution.bound_generation = component.capability_generation;
    if (!inputs.reservations.empty() && inputs.reservations[i].has_value()) {
      resolution.reservation = &(*inputs.reservations[i]);
    }

    const std::optional<ResourceCapability>& slot = inputs.capabilities[i];
    if (!slot.has_value()) {
      resolution.status = ComponentStatus::Unknown;
      resolution.reason = "no capability evidence is held for this resource";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    const ResourceCapability& capability = *slot;
    resolution.capability = &capability;
    resolution.observed_generation = capability.generation;
    resolution.observed_incarnation = capability.incarnation;

    if (capability.resource != component.resource) {
      resolution.status = ComponentStatus::Unknown;
      resolution.reason = "capability record names a different resource";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    if (capability.generation != component.capability_generation) {
      resolution.status = ComponentStatus::Stale;
      resolution.reason = "capability generation differs from the generation the path bound";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    if (capability.epoch.value() + policy.max_stale_epochs < inputs.current_epoch.value()) {
      resolution.status = ComponentStatus::Stale;
      resolution.reason = "capability epoch is behind the fabric epoch";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    if (capability.expires_at_ms != 0 && inputs.now_ms > capability.expires_at_ms) {
      resolution.status = ComponentStatus::Stale;
      resolution.reason = "capability declaration has expired";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    if (capability.published_at_ms > inputs.now_ms) {
      resolution.status = ComponentStatus::Stale;
      resolution.reason = "capability declaration is dated in the future";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    if (inputs.now_ms - capability.published_at_ms > policy.capability_max_age_ms) {
      resolution.status = ComponentStatus::Stale;
      resolution.reason = "capability declaration is older than the policy freshness window";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    if (capability.state == CapabilityState::Unavailable) {
      resolution.status = ComponentStatus::Unavailable;
      resolution.reason = "resource declared itself unavailable";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    if (capability.state == CapabilityState::Unknown) {
      resolution.status = ComponentStatus::Unknown;
      resolution.reason = "resource declared an unknown state";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    if (capability.state == CapabilityState::Degraded) {
      resolution.self_declared_degraded = true;
    }
    if (!capability.diversity_domain.empty() &&
        capability.diversity_domain != component.diversity_domain) {
      resolution.domain_contradiction = true;
      resolution.status = ComponentStatus::Violated;
      resolution.reason = "path and resource disagree about the failure domain";
      resolutions.push_back(std::move(resolution));
      continue;
    }
    resolution.status = resolution.self_declared_degraded ? ComponentStatus::Degraded
                                                          : ComponentStatus::Satisfied;
    resolutions.push_back(std::move(resolution));
  }

  for (const Resolution& resolution : resolutions) {
    if (resolution.domain_contradiction) {
      note(std::string("failure-domain contradiction for resource ") +
           resolution.resource.str());
      merge(outcome, Outcome::Conflict);
    }
  }

  // -------------------------------------------------------------------------
  // 4. Currency. Superseded authority invalidates applicability.
  // -------------------------------------------------------------------------
  if (inputs.currency.latest_class_generation.has_value() &&
      *inputs.currency.latest_class_generation != klass.generation) {
    note("class generation has been superseded");
    merge(outcome, Outcome::Stale);
  }
  if (inputs.currency.latest_policy_generation.has_value() &&
      *inputs.currency.latest_policy_generation != policy.generation) {
    note("policy generation has been superseded");
    merge(outcome, Outcome::Stale);
  }
  if (inputs.currency.latest_path_generation.has_value() &&
      *inputs.currency.latest_path_generation != path.generation) {
    note("path generation has been superseded");
    merge(outcome, Outcome::Stale);
  }
  if (request.observed_epoch.value() != 0 &&
      request.observed_epoch.value() + policy.max_stale_epochs < inputs.current_epoch.value()) {
    note("request observed an epoch that is behind the current fabric epoch");
    merge(outcome, Outcome::Stale);
  }

  // -------------------------------------------------------------------------
  // 5. Obligation composition.
  // -------------------------------------------------------------------------
  const std::vector<ObligationPlan> plan = build_plan(klass, policy);
  const bool degradation_permitted = klass.allow_degrade && policy.degrade_permitted &&
                                     klass.violation_policy != ViolationPolicy::Reject;
  std::uint32_t relaxation_window_ppm = 0;
  if (degradation_permitted) {
    relaxation_window_ppm = std::min(klass.degrade_max_relaxation_ppm, policy.max_relaxation_ppm);
  }
  const bool report_only = klass.violation_policy == ViolationPolicy::ReportOnly &&
                           klass.allow_degrade && policy.degrade_permitted;

  std::vector<RawValue> scratch;
  scratch.reserve(resolutions.size());

  for (const ObligationPlan& item : plan) {
    ObligationAssessment assessment;
    assessment.kind = item.kind;
    assessment.id = item.id;
    assessment.binding = item.binding;
    assessment.relaxable = obligation_relaxable(item.kind);
    assessment.direction = obligation_direction(item.kind);
    assessment.aggregate = obligation_aggregate(item.kind);
    assessment.path_level = item.kind == ObligationKind::DisjointPaths;
    assessment.required_value = item.required;
    assessment.effective_limit = item.required;
    if (item.kind == ObligationKind::Predicate) {
      assessment.detail = short_text(item.predicate.view());
    }

    if (item.kind == ObligationKind::DisjointPaths) {
      std::vector<std::string_view> domains;
      bool any_unknown = false;
      for (std::size_t i = 0; i < resolutions.size(); ++i) {
        const Resolution& resolution = resolutions[i];
        if (resolution.status == ComponentStatus::Unknown) {
          any_unknown = true;
          continue;
        }
        if (resolution.status != ComponentStatus::Satisfied &&
            resolution.status != ComponentStatus::Degraded) {
          continue;
        }
        bool ok = true;
        bool missing_evidence = false;
        for (const ObligationPlan& other : plan) {
          if (other.kind == ObligationKind::DisjointPaths) {
            continue;
          }
          const RawValue value = component_value(other, resolution, klass, policy,
                                                 inputs.current_epoch, inputs.now_ms);
          if (!value.present) {
            // The component might qualify perfectly well once the evidence
            // exists. Redundancy therefore cannot be reported as definitively
            // lost while a required declaration is simply absent.
            ok = false;
            missing_evidence = true;
            break;
          }
          if (other.kind == ObligationKind::ResourceState) {
            continue;
          }
          const CompareDirection direction = obligation_direction(other.kind);
          const bool compliant = (direction == CompareDirection::AtLeast)
                                     ? (value.value >= other.required)
                                     : (value.value <= other.required);
          if (compliant) {
            continue;
          }
          if (!obligation_relaxable(other.kind) || !degradation_permitted) {
            ok = false;
            break;
          }
          std::uint64_t delta = 0;
          if (!checked_mul<std::uint64_t>(other.required, relaxation_window_ppm, delta)) {
            ok = false;
            break;
          }
          delta /= kPpm;
          const std::uint64_t effective =
              (direction == CompareDirection::AtLeast)
                  ? (other.required - std::min(delta, other.required))
                  : (other.required + delta);
          const bool within = (direction == CompareDirection::AtLeast) ? (value.value >= effective)
                                                                      : (value.value <= effective);
          if (!within && !report_only) {
            ok = false;
            break;
          }
        }
        if (ok) {
          if (std::find(domains.begin(), domains.end(), resolution.declared_domain.view()) ==
              domains.end()) {
            domains.push_back(resolution.declared_domain.view());
          }
        } else if (missing_evidence) {
          any_unknown = true;
        }
      }

      const std::uint64_t diversity = static_cast<std::uint64_t>(domains.size());
      assessment.composed_value = diversity;
      if (diversity >= assessment.required_value) {
        assessment.status = ObligationStatus::Satisfied;
        assessment.headroom_ppm =
            compute_headroom(CompareDirection::AtLeast, assessment.required_value, diversity);
        assessment.detail = short_text(std::to_string(diversity) + " disjoint domains available");
      } else if (any_unknown) {
        assessment.status = ObligationStatus::Unknown;
        assessment.detail =
            "diversity cannot be concluded while a required component offers no evidence";
      } else if (degradation_permitted) {
        std::uint64_t delta = 0;
        if (!checked_mul<std::uint64_t>(assessment.required_value, relaxation_window_ppm, delta)) {
          delta = 0;
        }
        delta /= kPpm;
        std::uint64_t floor_value = assessment.required_value - std::min(delta, assessment.required_value - 1);
        if (floor_value < 1) {
          floor_value = 1;
        }
        if (diversity >= floor_value) {
          assessment.status = ObligationStatus::Degraded;
          assessment.effective_limit = floor_value;
          const std::uint64_t reference =
              assessment.required_value == 0 ? 1 : assessment.required_value;
          std::uint32_t ppm = 0;
          if (!relative_ppm(reference, reference - diversity, ppm)) {
            ppm = static_cast<std::uint32_t>(kPpm);
          }
          assessment.relaxation_applied_ppm = std::min<std::uint32_t>(1000000u, ppm);
          assessment.detail = short_text(std::to_string(diversity) + " of " +
                                         std::to_string(assessment.required_value) +
                                         " disjoint domains, inside the permitted window");
        } else if (report_only) {
          assessment.status = ObligationStatus::Degraded;
          assessment.relaxation_reported_only = true;
          assessment.effective_limit = diversity;
          assessment.relaxation_applied_ppm = 1000000u;
          assessment.detail = short_text(std::to_string(diversity) + " of " +
                                         std::to_string(assessment.required_value) +
                                         " disjoint domains reported under a report-only policy");
        } else {
          assessment.status = ObligationStatus::Violated;
          assessment.detail = short_text(std::to_string(diversity) + " of " +
                                         std::to_string(assessment.required_value) +
                                         " disjoint domains, outside the permitted window");
        }
      } else {
        assessment.status = ObligationStatus::Violated;
        assessment.detail = short_text(std::to_string(diversity) + " of " +
                                       std::to_string(assessment.required_value) +
                                       " required disjoint domains");
      }
      if (diversity < assessment.required_value) {
        decision.unsatisfied_diversity_domains =
            static_cast<std::uint32_t>(assessment.required_value - diversity);
      }
      decision.obligations.push_back(std::move(assessment));
      continue;
    }

    // Ordinary per-component composition.
    bool any_stale = false;
    bool any_unavailable = false;
    bool any_unknown = false;
    std::size_t stale_index = 0;
    std::size_t unavailable_index = 0;
    std::size_t unknown_index = 0;
    scratch.clear();
    for (std::size_t i = 0; i < resolutions.size(); ++i) {
      const Resolution& resolution = resolutions[i];
      if (resolution.status == ComponentStatus::Stale) {
        any_stale = true;
        stale_index = i;
        scratch.push_back(RawValue{});
        continue;
      }
      if (resolution.status == ComponentStatus::Unavailable ||
          resolution.status == ComponentStatus::Violated) {
        if (!any_unavailable) {
          unavailable_index = i;
        }
        any_unavailable = true;
        scratch.push_back(RawValue{});
        continue;
      }
      if (resolution.status == ComponentStatus::Unknown) {
        if (!any_unknown) {
          unknown_index = i;
        }
        any_unknown = true;
        scratch.push_back(RawValue{});
        continue;
      }
      scratch.push_back(component_value(item, resolution, klass, policy, inputs.current_epoch,
                                        inputs.now_ms));
    }

    if (item.kind == ObligationKind::ResourceState) {
      std::uint64_t worst = kStateAvailable;
      std::size_t worst_index = 0;
      bool degraded_seen = false;
      for (std::size_t i = 0; i < scratch.size(); ++i) {
        if (scratch[i].present && scratch[i].value < worst) {
          worst = scratch[i].value;
          worst_index = i;
        }
        if (resolutions[i].self_declared_degraded) {
          degraded_seen = true;
        }
      }
      assessment.binding_resource = resolutions.empty() ? std::optional<ResourceId>{}
                                                        : std::optional<ResourceId>{resolutions[worst_index].resource};
      assessment.weakest_resource = assessment.binding_resource;
      if (any_stale) {
        assessment.status = ObligationStatus::Stale;
        assessment.binding_resource = resolutions[stale_index].resource;
        assessment.weakest_resource = assessment.binding_resource;
        assessment.detail = short_text(resolutions[stale_index].reason);
      } else if (any_unavailable) {
        assessment.status = ObligationStatus::Unavailable;
        assessment.binding_resource = resolutions[unavailable_index].resource;
        assessment.weakest_resource = assessment.binding_resource;
        assessment.detail = short_text(resolutions[unavailable_index].reason);
      } else if (any_unknown) {
        assessment.status = ObligationStatus::Unknown;
        assessment.binding_resource = resolutions[unknown_index].resource;
        assessment.weakest_resource = assessment.binding_resource;
        assessment.detail = short_text(resolutions[unknown_index].reason);
      } else {
        assessment.composed_value = worst;
        if (worst >= kStateAvailable) {
          assessment.status = ObligationStatus::Satisfied;
          assessment.headroom_ppm = 1000000u;
        } else if (degradation_permitted && degraded_seen) {
          assessment.status = ObligationStatus::Degraded;
          assessment.effective_limit = worst;
          assessment.binding_resource = resolutions[worst_index].resource;
          assessment.weakest_resource = assessment.binding_resource;
          assessment.detail = short_text(resolutions[worst_index].resource.str() +
                                         " declared a degraded capability state");
        } else {
          assessment.status = ObligationStatus::Violated;
          assessment.binding_resource = resolutions[worst_index].resource;
          assessment.weakest_resource = assessment.binding_resource;
          assessment.detail =
              degraded_seen
                  ? "a required resource declared a degraded state with no permitted degradation"
                  : "a required resource did not declare an available state";
        }
      }
      decision.obligations.push_back(std::move(assessment));
      continue;
    }

    bool missing_attribute = false;
    std::size_t missing_index = 0;
    if (!any_stale && !any_unavailable && !any_unknown) {
      for (std::size_t i = 0; i < scratch.size(); ++i) {
        if (!scratch[i].present) {
          missing_attribute = true;
          missing_index = i;
          break;
        }
      }
    }

    if (any_stale) {
      assessment.status = ObligationStatus::Stale;
      assessment.binding_resource = resolutions[stale_index].resource;
      assessment.weakest_resource = assessment.binding_resource;
      assessment.detail = short_text(resolutions[stale_index].reason);
      decision.obligations.push_back(std::move(assessment));
      continue;
    }
    if (any_unavailable) {
      assessment.status = ObligationStatus::Unavailable;
      assessment.binding_resource = resolutions[unavailable_index].resource;
      assessment.weakest_resource = assessment.binding_resource;
      assessment.detail = short_text(resolutions[unavailable_index].reason);
      decision.obligations.push_back(std::move(assessment));
      continue;
    }
    if (any_unknown) {
      assessment.status = ObligationStatus::Unknown;
      assessment.binding_resource = resolutions[unknown_index].resource;
      assessment.weakest_resource = assessment.binding_resource;
      assessment.detail = short_text(resolutions[unknown_index].reason);
      decision.obligations.push_back(std::move(assessment));
      continue;
    }
    if (missing_attribute) {
      assessment.status = ObligationStatus::Unknown;
      assessment.binding_resource = resolutions[missing_index].resource;
      assessment.weakest_resource = assessment.binding_resource;
      assessment.detail = short_text(scratch[missing_index].detail);
      decision.obligations.push_back(std::move(assessment));
      continue;
    }

    const Composition composition =
        compose_values(obligation_aggregate(item.kind), scratch,
                       (item.kind == ObligationKind::Latency)    ? path.fixed_latency_us
                       : (item.kind == ObligationKind::Jitter)   ? path.fixed_jitter_us
                                                                 : 0);
    if (composition.overflow) {
      assessment.status = ObligationStatus::Unknown;
      assessment.detail = "aggregation overflow";
      decision.obligations.push_back(std::move(assessment));
      note("end-to-end aggregation overflowed the representable domain");
      merge(outcome, Outcome::Rejected);
      continue;
    }
    if (!composition.value.has_value()) {
      assessment.status = ObligationStatus::Unknown;
      assessment.detail = short_text(composition.detail);
      decision.obligations.push_back(std::move(assessment));
      continue;
    }

    const std::uint64_t composed = *composition.value;
    assessment.composed_value = composed;
    assessment.binding_resource = resolutions[composition.binding_index].resource;
    assessment.weakest_resource = resolutions[composition.weakest_index].resource;

    const bool compliant = (assessment.direction == CompareDirection::AtLeast)
                               ? (composed >= assessment.required_value)
                               : (composed <= assessment.required_value);
    if (compliant) {
      assessment.status = ObligationStatus::Satisfied;
      assessment.headroom_ppm =
          compute_headroom(assessment.direction, assessment.required_value, composed);
      decision.obligations.push_back(std::move(assessment));
      continue;
    }

    if (!assessment.relaxable) {
      assessment.status = ObligationStatus::Violated;
      assessment.detail = "structural requirement is not met and is never relaxable";
      decision.obligations.push_back(std::move(assessment));
      continue;
    }
    if (!degradation_permitted || relaxation_window_ppm == 0) {
      assessment.status = ObligationStatus::Violated;
      assessment.detail = "no degradation permission covers this obligation";
      decision.obligations.push_back(std::move(assessment));
      continue;
    }

    std::uint64_t delta = 0;
    if (!checked_mul<std::uint64_t>(assessment.required_value, relaxation_window_ppm, delta)) {
      assessment.status = ObligationStatus::Violated;
      assessment.detail = "relaxation window overflowed";
      decision.obligations.push_back(std::move(assessment));
      continue;
    }
    delta /= kPpm;
    const std::uint64_t effective =
        (assessment.direction == CompareDirection::AtLeast)
            ? (assessment.required_value - std::min(delta, assessment.required_value))
            : (assessment.required_value + delta);
    const bool within = (assessment.direction == CompareDirection::AtLeast)
                            ? (composed >= effective)
                            : (composed <= effective);
    const std::uint64_t shortfall = (assessment.direction == CompareDirection::AtLeast)
                                        ? (assessment.required_value - composed)
                                        : (composed - assessment.required_value);
    std::uint32_t applied_ppm = 0;
    if (!relative_ppm(assessment.required_value, shortfall, applied_ppm)) {
      applied_ppm = 1000000u;
    }
    if (within) {
      assessment.status = ObligationStatus::Degraded;
      assessment.effective_limit = effective;
      assessment.relaxation_applied_ppm = applied_ppm;
      assessment.detail = "requirement met inside the permitted relaxation window";
    } else if (report_only) {
      assessment.status = ObligationStatus::Degraded;
      assessment.relaxation_reported_only = true;
      assessment.effective_limit = composed;
      assessment.relaxation_applied_ppm = applied_ppm;
      assessment.detail = "violation reported under a report-only violation policy";
    } else {
      assessment.status = ObligationStatus::Violated;
      assessment.effective_limit = effective;
      assessment.detail = "shortfall exceeds the permitted relaxation window";
    }
    decision.obligations.push_back(std::move(assessment));
  }

  // -------------------------------------------------------------------------
  // 6. Roll per-obligation findings into per-resource statuses and the final
  //    outcome.
  // -------------------------------------------------------------------------
  for (const Resolution& resolution : resolutions) {
    ResourceAssessment resource;
    resource.resource = resolution.resource;
    resource.diversity_domain = resolution.declared_domain;
    resource.role = resolution.role;
    resource.bound_capability_generation = resolution.bound_generation;
    resource.observed_capability_generation = resolution.observed_generation;
    resource.observed_incarnation = resolution.observed_incarnation;
    resource.self_declared_degraded = resolution.self_declared_degraded;
    resource.status = resolution.status;
    resource.reason = short_text(resolution.reason);
    decision.resources.push_back(std::move(resource));
  }

  for (const ObligationAssessment& assessment : decision.obligations) {
    switch (assessment.status) {
      case ObligationStatus::Satisfied:
      case ObligationStatus::NotApplicable:
        break;
      case ObligationStatus::Degraded: {
        DegradationReason degradation;
        degradation.kind = assessment.kind;
        degradation.id = assessment.id;
        degradation.resource = assessment.binding_resource;
        degradation.relaxation_ppm = assessment.relaxation_applied_ppm;
        degradation.reported_only = assessment.relaxation_reported_only;
        degradation.detail = assessment.detail;
        decision.degradations.push_back(std::move(degradation));
        if (decision.degradations.size() > limits::kMaxDegradations) {
          decision.degradations.resize(limits::kMaxDegradations);
        }
        merge(outcome, Outcome::SupportedDegraded);
        break;
      }
      case ObligationStatus::Violated:
        ++decision.violated_obligations;
        merge(outcome, Outcome::Unsupported);
        break;
      case ObligationStatus::Unknown:
        merge(outcome, Outcome::Unknown);
        break;
      case ObligationStatus::Unavailable:
        merge(outcome, Outcome::Unsupported);
        break;
      case ObligationStatus::Stale:
        merge(outcome, Outcome::Stale);
        break;
    }
  }

  for (const Resolution& resolution : resolutions) {
    if (resolution.status == ComponentStatus::Unknown) {
      ++decision.unknown_components;
    }
  }

  // The policy budget for unknown components is a reporting tolerance only:
  // it can never turn UNKNOWN into a positive verdict, so no branch here may
  // remove Outcome::Unknown from the merged outcome.

  // -------------------------------------------------------------------------
  // 7. Binding obligation selection and explanation.
  // -------------------------------------------------------------------------
  auto select = [&decision](ObligationStatus status) -> const ObligationAssessment* {
    for (const ObligationAssessment& assessment : decision.obligations) {
      if (assessment.status == status) {
        return &assessment;
      }
    }
    return nullptr;
  };

  const ObligationAssessment* binding = nullptr;
  switch (outcome) {
    case Outcome::Stale:
      binding = select(ObligationStatus::Stale);
      break;
    case Outcome::Unsupported:
      binding = select(ObligationStatus::Unavailable);
      if (binding == nullptr) {
        binding = select(ObligationStatus::Violated);
      }
      break;
    case Outcome::Unknown:
      binding = select(ObligationStatus::Unknown);
      break;
    case Outcome::SupportedDegraded:
      for (const ObligationAssessment& assessment : decision.obligations) {
        if (assessment.status != ObligationStatus::Degraded) {
          continue;
        }
        if (binding == nullptr ||
            assessment.relaxation_applied_ppm > binding->relaxation_applied_ppm) {
          binding = &assessment;
        }
      }
      break;
    case Outcome::Supported:
      for (const ObligationAssessment& assessment : decision.obligations) {
        if (assessment.status != ObligationStatus::Satisfied) {
          continue;
        }
        if (binding == nullptr || assessment.headroom_ppm < binding->headroom_ppm) {
          binding = &assessment;
        }
      }
      break;
    case Outcome::Conflict:
    case Outcome::Rejected:
      break;
  }

  if (binding != nullptr) {
    decision.binding_obligation = binding->kind;
    decision.binding_resource = binding->binding_resource;
    if (reason.empty()) {
      reason = binding->id.str();
      reason += " is the binding obligation on this path";
    }
  }

  // -------------------------------------------------------------------------
  // 8. Authority vector: exactly which generations and digests justified the
  //    verdict.
  // -------------------------------------------------------------------------
  decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::Class, klass.id.str(),
                                              klass.generation, klass.digest.hex(),
                                              klass.published_epoch, klass.provenance.origin()});
  decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::Subject, request.subject.str(),
                                              request.subject_generation, std::string{},
                                              inputs.current_epoch, request.provenance.origin()});
  decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::Path, path.id.str(),
                                              path.generation, path.digest.hex(), path.epoch,
                                              path.provenance.origin()});
  decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::Policy, policy.id.str(),
                                              policy.generation, policy.digest.hex(),
                                              policy.published_epoch, policy.provenance.origin()});
  decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::Priority,
                                              request.priority.str(), request.priority_generation,
                                              std::string{}, inputs.current_epoch, std::string{}});
  decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::FabricEpoch, "fabric",
                                              Generation{inputs.current_epoch.value()},
                                              std::string{}, inputs.current_epoch,
                                              std::string("registry")});
  for (const Resolution& resolution : resolutions) {
    if (resolution.capability != nullptr) {
      decision.authority.push_back(
          AuthorityEntry{AuthorityEntry::Dimension::Capability, resolution.resource.str(),
                         resolution.capability->generation, resolution.capability->digest.hex(),
                         resolution.capability->epoch,
                         resolution.capability->provenance.origin()});
    }
    if (resolution.reservation != nullptr) {
      decision.authority.push_back(
          AuthorityEntry{AuthorityEntry::Dimension::Reservation, resolution.resource.str(),
                         resolution.reservation->generation, resolution.reservation->digest.hex(),
                         resolution.reservation->epoch,
                         resolution.reservation->provenance.origin()});
    }
  }
  if (decision.authority.size() > limits::kMaxAuthorityEntries) {
    decision.authority.resize(limits::kMaxAuthorityEntries);
  }

  decision.outcome = outcome;
  decision.primary_reason =
      reason.empty() ? (is_positive(outcome) ? std::string("all required obligations are satisfied")
                                             : std::string("no binding finding"))
                     : short_text(reason);
  decision.decision_digest = compute_decision_digest(decision);
  return decision;
}



}  // namespace qosfabric
