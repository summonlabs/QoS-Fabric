// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Software Labs.

#include "harness.hpp"

#include <string>
#include <vector>

#include "qosfabric/qosfabric.hpp"

using namespace qosfabric;

namespace {

constexpr std::uint64_t kNow = 1'800'000'000'000ull;

QoSClass base_class() {
  QoSClass value;
  value.id = QoSClassId{"voice.toll"};
  value.generation = Generation{1};
  value.label = "voice toll quality";
  value.min_rate_bps = 1'000'000;
  value.max_latency_us = 1'500;
  value.max_jitter_us = 200;
  value.max_loss_ppm = 100;
  value.isolation = IsolationLevel::QueueIsolated;
  value.treatment = TreatmentMode::RateLimited;
  value.min_priority_rank = 3;
  value.min_disjoint_paths = 2;
  value.published_epoch = FabricEpoch{1};
  value.digest = compute_digest(value);
  return value;
}

Policy base_policy() {
  Policy value;
  value.id = PolicyId{"strict"};
  value.generation = Generation{1};
  value.degrade_permitted = true;
  value.max_relaxation_ppm = 200'000;
  value.max_stale_epochs = 1;
  value.capability_max_age_ms = 600'000;
  value.published_epoch = FabricEpoch{1};
  value.digest = compute_digest(value);
  return value;
}

Path base_path() {
  Path value;
  value.id = PathId{"p1"};
  value.generation = Generation{1};
  value.fixed_latency_us = 100;
  value.epoch = FabricEpoch{1};
  PathComponent first;
  first.resource = ResourceId{"r1"};
  first.capability_generation = Generation{1};
  first.diversity_domain = DiversityDomain{"d1"};
  first.role = PathRole::Primary;
  PathComponent second;
  second.resource = ResourceId{"r2"};
  second.capability_generation = Generation{1};
  second.diversity_domain = DiversityDomain{"d2"};
  second.role = PathRole::Protection;
  value.components = {first, second};
  value.digest = compute_digest(value);
  return value;
}

ResourceCapability capability(const char* resource, const char* domain) {
  ResourceCapability value;
  value.resource = ResourceId{resource};
  value.generation = Generation{1};
  value.epoch = FabricEpoch{1};
  value.publisher = PublisherId{"bus"};
  value.incarnation = PublisherIncarnation{1};
  value.published_at_ms = kNow;
  value.expires_at_ms = kNow + 600'000;
  value.state = CapabilityState::Available;
  value.supported_rate_bps = 2'000'000;
  value.latency_bound_us = 600;
  value.jitter_bound_us = 80;
  value.loss_ppm = 20;
  value.max_burst_bytes = 65'536;
  value.burst_window_us = 1'000;
  value.mtu_bytes = 9'000;
  value.max_isolation = IsolationLevel::QueueIsolated;
  value.max_treatment = TreatmentMode::RateLimited;
  value.max_priority_rank = 4;
  value.diversity_domain = DiversityDomain{domain};
  value.provenance = Provenance{"bus", FabricEpoch{1}, Generation{1}, Sequence{1}};
  value.digest = compute_digest(value);
  return value;
}

ContractRequest request_for(const QoSClass& klass, const Policy& policy, const Path& path) {
  ContractRequest request;
  request.contract = QoSContractId{"c1"};
  request.contract_generation = Generation{1};
  request.subject = SubjectId{"svc-a"};
  request.subject_generation = Generation{1};
  request.class_id = klass.id;
  request.class_generation = klass.generation;
  request.path = path.id;
  request.path_generation = path.generation;
  request.policy = policy.id;
  request.policy_generation = policy.generation;
  request.priority = PriorityClassId{"gold"};
  request.priority_generation = Generation{1};
  request.observed_epoch = FabricEpoch{1};
  request.request_ms = kNow;
  return request;
}

EvaluationInputs base_inputs() {
  EvaluationInputs inputs;
  inputs.klass = base_class();
  inputs.policy = base_policy();
  inputs.path = base_path();
  inputs.request = request_for(inputs.klass, inputs.policy, inputs.path);
  inputs.capabilities = {capability("r1", "d1"), capability("r2", "d2")};
  inputs.reservations = {std::nullopt, std::nullopt};
  inputs.current_epoch = FabricEpoch{1};
  inputs.now_ms = kNow;
  inputs.evaluator = "unit-test";
  return inputs;
}

const ObligationAssessment* obligation(const ContractDecision& decision, ObligationKind kind) {
  return decision.find(kind);
}

}  // namespace

QOS_TEST(e2e_composition_is_weakest_link_additive_and_loss_composed) {
  EvaluationInputs inputs = base_inputs();
  inputs.capabilities[0]->supported_rate_bps = 5'000'000;
  inputs.capabilities[1]->supported_rate_bps = 1'500'000;
  inputs.capabilities[0]->latency_bound_us = 600;
  inputs.capabilities[1]->latency_bound_us = 700;
  inputs.capabilities[0]->loss_ppm = 20;
  inputs.capabilities[1]->loss_ppm = 30;
  inputs.capabilities[0]->jitter_bound_us = 80;
  inputs.capabilities[1]->jitter_bound_us = 90;

  const auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Supported);
  const ObligationAssessment* rate = obligation(*decision, ObligationKind::MinRate);
  QOS_REQUIRE(rate != nullptr);
  QOS_REQUIRE(rate->composed_value.has_value());
  QOS_CHECK_EQ(*rate->composed_value, 1'500'000ull);
  QOS_CHECK(rate->binding_resource.has_value());
  QOS_CHECK(rate->binding_resource->str() == std::string("r2"));

  const ObligationAssessment* latency = obligation(*decision, ObligationKind::Latency);
  QOS_REQUIRE(latency != nullptr);
  QOS_REQUIRE(latency->composed_value.has_value());
  // 600 + 700 + 100 fixed overhead.
  QOS_CHECK_EQ(*latency->composed_value, 1'400ull);
  QOS_CHECK(latency->aggregate == AggregateKind::Additive);

  const ObligationAssessment* loss = obligation(*decision, ObligationKind::Loss);
  QOS_REQUIRE(loss != nullptr);
  QOS_REQUIRE(loss->composed_value.has_value());
  // 1 - (1 - 20ppm)(1 - 30ppm) = 50ppm with integer parts-per-million math.
  QOS_CHECK_EQ(*loss->composed_value, 50ull);

  const ObligationAssessment* jitter = obligation(*decision, ObligationKind::Jitter);
  QOS_REQUIRE(jitter != nullptr);
  QOS_REQUIRE(jitter->composed_value.has_value());
  QOS_CHECK_EQ(*jitter->composed_value, 170ull);

  const ObligationAssessment* diversity = obligation(*decision, ObligationKind::DisjointPaths);
  QOS_REQUIRE(diversity != nullptr);
  QOS_REQUIRE(diversity->composed_value.has_value());
  QOS_CHECK_EQ(*diversity->composed_value, 2ull);
  QOS_CHECK_EQ(decision->unsatisfied_diversity_domains, 0u);
}

QOS_TEST(degradation_requires_class_and_policy_permission) {
  // Latency composed 1,600 against a 1,500 requirement: 6.67 percent over,
  // inside both the class window (10 percent) and the policy ceiling.
  EvaluationInputs inputs = base_inputs();
  inputs.klass.allow_degrade = true;
  inputs.klass.degrade_max_relaxation_ppm = 100'000;
  inputs.klass.digest = compute_digest(inputs.klass);
  inputs.capabilities[0]->latency_bound_us = 800;
  inputs.capabilities[1]->latency_bound_us = 700;

  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::SupportedDegraded);
  QOS_REQUIRE(!decision->degradations.empty());
  QOS_CHECK_EQ(decision->degradations.front().id.str(), std::string("latency.max_us"));
  QOS_CHECK(decision->degradations.front().relaxation_ppm > 0);
  QOS_CHECK(decision->binding_obligation.has_value());
  QOS_CHECK(*decision->binding_obligation == ObligationKind::Latency);
  const ObligationAssessment* latency = obligation(*decision, ObligationKind::Latency);
  QOS_REQUIRE(latency != nullptr);
  QOS_CHECK_EQ(latency->effective_limit, 1'650ull);

  // The same shortfall with no class window is a definitive negative, never a
  // silent downgrade.
  EvaluationInputs strict = inputs;
  strict.klass.allow_degrade = false;
  strict.klass.degrade_max_relaxation_ppm = 0;
  strict.klass.digest = compute_digest(strict.klass);
  auto unsupported = evaluate(strict);
  QOS_REQUIRE(unsupported.has_value());
  QOS_CHECK(unsupported->outcome == Outcome::Unsupported);
  QOS_CHECK(unsupported->degradations.empty());
  QOS_CHECK(unsupported->violated_obligations > 0);

  // With a class window but a policy that forbids degradation, the policy wins.
  EvaluationInputs policy_denied = inputs;
  policy_denied.policy.degrade_permitted = false;
  policy_denied.policy.max_relaxation_ppm = 0;
  policy_denied.policy.digest = compute_digest(policy_denied.policy);
  auto denied = evaluate(policy_denied);
  QOS_REQUIRE(denied.has_value());
  QOS_CHECK(denied->outcome == Outcome::Unsupported);
  QOS_CHECK(denied->degradations.empty());

  // The policy ceiling, not the class window, is the binding limit.
  EvaluationInputs capped = inputs;
  capped.policy.max_relaxation_ppm = 20'000;  // 2 percent
  capped.policy.digest = compute_digest(capped.policy);
  auto capped_decision = evaluate(capped);
  QOS_REQUIRE(capped_decision.has_value());
  QOS_CHECK(capped_decision->outcome == Outcome::Unsupported);
}

QOS_TEST(unknown_capability_never_becomes_supported) {
  EvaluationInputs inputs = base_inputs();
  inputs.capabilities[1].reset();
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Unknown);
  QOS_CHECK_EQ(decision->unknown_components, 1u);
  const ObligationAssessment* rate = obligation(*decision, ObligationKind::MinRate);
  QOS_REQUIRE(rate != nullptr);
  QOS_CHECK(rate->status == ObligationStatus::Unknown);
  QOS_CHECK(!rate->composed_value.has_value());

  // A larger unknown budget in policy is a reporting tolerance, never licence.
  EvaluationInputs tolerant = inputs;
  tolerant.policy.max_unknown_components = 4;
  tolerant.policy.digest = compute_digest(tolerant.policy);
  auto still_unknown = evaluate(tolerant);
  QOS_REQUIRE(still_unknown.has_value());
  QOS_CHECK(still_unknown->outcome == Outcome::Unknown);
}

QOS_TEST(undeclared_attribute_is_missing_evidence_not_zero) {
  EvaluationInputs inputs = base_inputs();
  inputs.capabilities[0]->jitter_bound_us.reset();
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Unknown);
  const ObligationAssessment* jitter = obligation(*decision, ObligationKind::Jitter);
  QOS_REQUIRE(jitter != nullptr);
  QOS_CHECK(jitter->status == ObligationStatus::Unknown);
  // The attributes both resources do declare remain supported.
  const ObligationAssessment* rate = obligation(*decision, ObligationKind::MinRate);
  QOS_REQUIRE(rate != nullptr);
  QOS_CHECK(rate->status == ObligationStatus::Satisfied);
}

QOS_TEST(stale_evidence_invalidates_applicability) {
  // Capability generation no longer matches what the path bound.
  EvaluationInputs generation_drift = base_inputs();
  generation_drift.currency.latest_class_generation = Generation{1};
  generation_drift.currency.latest_policy_generation = Generation{1};
  generation_drift.currency.latest_path_generation = Generation{1};
  generation_drift.capabilities[0]->generation = Generation{2};
  generation_drift.capabilities[0]->digest = compute_digest(*generation_drift.capabilities[0]);
  auto drifted = evaluate(generation_drift);
  QOS_REQUIRE(drifted.has_value());
  QOS_CHECK(drifted->outcome == Outcome::Stale);

  // Epoch far behind the fabric epoch.
  EvaluationInputs old_epoch = base_inputs();
  old_epoch.current_epoch = FabricEpoch{50};
  auto epoch_stale = evaluate(old_epoch);
  QOS_REQUIRE(epoch_stale.has_value());
  QOS_CHECK(epoch_stale->outcome == Outcome::Stale);

  // Expired declaration.
  EvaluationInputs expired = base_inputs();
  expired.capabilities[1]->expires_at_ms = kNow - 1;
  auto expired_decision = evaluate(expired);
  QOS_REQUIRE(expired_decision.has_value());
  QOS_CHECK(expired_decision->outcome == Outcome::Stale);

  // Older than the policy freshness window.
  EvaluationInputs aged = base_inputs();
  aged.capabilities[1]->published_at_ms = kNow - 700'000;
  auto aged_decision = evaluate(aged);
  QOS_REQUIRE(aged_decision.has_value());
  QOS_CHECK(aged_decision->outcome == Outcome::Stale);

  // Superseded policy generation.
  EvaluationInputs superseded = base_inputs();
  superseded.currency.latest_policy_generation = Generation{2};
  auto superseded_decision = evaluate(superseded);
  QOS_REQUIRE(superseded_decision.has_value());
  QOS_CHECK(superseded_decision->outcome == Outcome::Stale);

  // A request that observed an epoch behind the current one.
  EvaluationInputs lagging = base_inputs();
  lagging.current_epoch = FabricEpoch{5};
  lagging.request.observed_epoch = FabricEpoch{1};
  lagging.klass.published_epoch = FabricEpoch{5};
  lagging.policy.published_epoch = FabricEpoch{5};
  lagging.path.epoch = FabricEpoch{5};
  for (auto& item : lagging.capabilities) {
    item->epoch = FabricEpoch{5};
    item->published_at_ms = kNow;
  }
  auto lagging_decision = evaluate(lagging);
  QOS_REQUIRE(lagging_decision.has_value());
  QOS_CHECK(lagging_decision->outcome == Outcome::Stale);
}

QOS_TEST(conflict_outranks_every_other_finding) {
  EvaluationInputs inputs = base_inputs();
  ConflictMarker marker;
  marker.scope = ConflictMarker::Scope::Class;
  marker.id = "voice.toll";
  marker.generation = Generation{1};
  marker.incumbent_digest = sha256("a");
  marker.challenger_digest = sha256("b");
  marker.epoch = FabricEpoch{1};
  marker.detail = "equivocation";
  inputs.conflicts.push_back(marker);
  // Make the evidence disagree as well: CONFLICT must still dominate.
  inputs.capabilities[1].reset();
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Conflict);

  // A policy that denies the class is a conflict, not an absence.
  EvaluationInputs denied = base_inputs();
  denied.policy.denied_classes = {QoSClassId{"voice.toll"}};
  denied.policy.digest = compute_digest(denied.policy);
  auto denied_decision = evaluate(denied);
  QOS_REQUIRE(denied_decision.has_value());
  QOS_CHECK(denied_decision->outcome == Outcome::Conflict);

  // The path and the resource disagreeing about the failure domain is a
  // contradiction about a safety-relevant property.
  EvaluationInputs domain = base_inputs();
  domain.capabilities[0]->diversity_domain = DiversityDomain{"d9"};
  domain.capabilities[0]->digest = compute_digest(*domain.capabilities[0]);
  auto domain_decision = evaluate(domain);
  QOS_REQUIRE(domain_decision.has_value());
  QOS_CHECK(domain_decision->outcome == Outcome::Conflict);
}

QOS_TEST(structural_failures_are_rejected_not_guessed) {
  // A capability vector that does not cover the path.
  EvaluationInputs short_vector = base_inputs();
  short_vector.capabilities.pop_back();
  auto rejected = evaluate(short_vector);
  QOS_REQUIRE(rejected.has_value());
  QOS_CHECK(rejected->outcome == Outcome::Rejected);

  // A mismatched class identity.
  EvaluationInputs mismatched = base_inputs();
  mismatched.klass.id = QoSClassId{"other.class"};
  auto mismatch = evaluate(mismatched);
  QOS_REQUIRE(mismatch.has_value());
  QOS_CHECK(mismatch->outcome == Outcome::Rejected);

  // A contradictory class definition.
  EvaluationInputs contradictory = base_inputs();
  contradictory.klass.min_rate_bps = 9'000'000;
  contradictory.klass.max_rate_bps = 1'000;
  auto contradiction = evaluate(contradictory);
  QOS_REQUIRE(contradiction.has_value());
  QOS_CHECK(contradiction->outcome == Outcome::Rejected);

  // A request pinning a generation the resolved definition does not carry.
  EvaluationInputs pinned = base_inputs();
  pinned.request.class_generation = Generation{9};
  auto pin = evaluate(pinned);
  QOS_REQUIRE(pin.has_value());
  QOS_CHECK(pin->outcome == Outcome::Rejected);
}

QOS_TEST(unavailable_component_is_a_definitive_negative) {
  EvaluationInputs inputs = base_inputs();
  inputs.capabilities[1]->state = CapabilityState::Unavailable;
  inputs.capabilities[1]->digest = compute_digest(*inputs.capabilities[1]);
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Unsupported);
  const ObligationAssessment* rate = obligation(*decision, ObligationKind::MinRate);
  QOS_REQUIRE(rate != nullptr);
  QOS_CHECK(rate->status == ObligationStatus::Unavailable);
}

QOS_TEST(resource_declared_degradation_needs_policy_permission) {
  EvaluationInputs inputs = base_inputs();
  inputs.capabilities[0]->state = CapabilityState::Degraded;
  inputs.capabilities[0]->digest = compute_digest(*inputs.capabilities[0]);
  inputs.klass.allow_degrade = true;
  inputs.klass.degrade_max_relaxation_ppm = 100'000;
  inputs.klass.digest = compute_digest(inputs.klass);
  auto degraded = evaluate(inputs);
  QOS_REQUIRE(degraded.has_value());
  QOS_CHECK(degraded->outcome == Outcome::SupportedDegraded);
  QOS_REQUIRE(!degraded->degradations.empty());
  QOS_CHECK_EQ(degraded->degradations.front().id.str(), std::string("resource.state"));

  EvaluationInputs forbidden = base_inputs();
  forbidden.capabilities[0]->state = CapabilityState::Degraded;
  forbidden.capabilities[0]->digest = compute_digest(*forbidden.capabilities[0]);
  auto violation = evaluate(forbidden);
  QOS_REQUIRE(violation.has_value());
  QOS_CHECK(violation->outcome == Outcome::Unsupported);
}

QOS_TEST(report_only_policy_reports_and_never_returns_clean_supported) {
  EvaluationInputs inputs = base_inputs();
  inputs.klass.allow_degrade = true;
  inputs.klass.degrade_max_relaxation_ppm = 10'000;  // 1 percent: too small
  inputs.klass.violation_policy = ViolationPolicy::ReportOnly;
  inputs.klass.digest = compute_digest(inputs.klass);
  inputs.capabilities[0]->latency_bound_us = 1'500;
  inputs.capabilities[1]->latency_bound_us = 1'500;
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::SupportedDegraded);
  QOS_REQUIRE(!decision->degradations.empty());
  QOS_CHECK(decision->degradations.front().reported_only);
}

QOS_TEST(reject_violation_policy_disables_every_window) {
  EvaluationInputs inputs = base_inputs();
  inputs.klass.allow_degrade = true;
  inputs.klass.degrade_max_relaxation_ppm = 900'000;
  inputs.klass.violation_policy = ViolationPolicy::Reject;
  inputs.klass.digest = compute_digest(inputs.klass);
  inputs.capabilities[0]->latency_bound_us = 5'000;
  inputs.capabilities[1]->latency_bound_us = 5'000;
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Unsupported);
  QOS_CHECK(decision->degradations.empty());
}

QOS_TEST(reservation_obligation_is_verified_not_created) {
  EvaluationInputs inputs = base_inputs();
  inputs.klass.requires_reservation = true;
  inputs.klass.digest = compute_digest(inputs.klass);
  auto missing = evaluate(inputs);
  QOS_REQUIRE(missing.has_value());
  QOS_CHECK(missing->outcome == Outcome::Unknown);
  const ObligationAssessment* reservation = obligation(*missing, ObligationKind::Reservation);
  QOS_REQUIRE(reservation != nullptr);
  QOS_CHECK(reservation->status == ObligationStatus::Unknown);

  Reservation first;
  first.resource = ResourceId{"r1"};
  first.generation = Generation{1};
  first.class_id = inputs.klass.id;
  first.class_generation = inputs.klass.generation;
  first.reserved_min_rate_bps = 1'500'000;
  first.epoch = FabricEpoch{1};
  first.publisher = PublisherId{"bus"};
  first.incarnation = PublisherIncarnation{1};
  first.expires_at_ms = kNow + 100'000;
  first.digest = compute_digest(first);
  Reservation second = first;
  second.resource = ResourceId{"r2"};
  second.reserved_min_rate_bps = 900'000;  // below the class minimum
  second.digest = compute_digest(second);
  inputs.reservations = {first, second};
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Unsupported);
  const ObligationAssessment* assessed = obligation(*decision, ObligationKind::Reservation);
  QOS_REQUIRE(assessed != nullptr);
  QOS_CHECK(assessed->status == ObligationStatus::Violated);
  QOS_REQUIRE(assessed->composed_value.has_value());
  QOS_CHECK_EQ(*assessed->composed_value, 900'000ull);
}

QOS_TEST(equality_predicates_are_conjunctive_over_the_path) {
  QoSClass klass = base_class();
  klass.required_predicates = {CapabilityKey{"tsn.tas"}};
  klass.digest = compute_digest(klass);
  EvaluationInputs inputs = base_inputs();
  inputs.klass = klass;
  inputs.request.class_id = klass.id;
  auto missing = evaluate(inputs);
  QOS_REQUIRE(missing.has_value());
  QOS_CHECK(missing->outcome == Outcome::Unknown);

  inputs.capabilities[0]->predicates = {CapabilityKey{"tsn.tas"}};
  inputs.capabilities[0]->digest = compute_digest(*inputs.capabilities[0]);
  auto partial = evaluate(inputs);
  QOS_REQUIRE(partial.has_value());
  QOS_CHECK(partial->outcome == Outcome::Unknown);

  inputs.capabilities[1]->predicates = {CapabilityKey{"tsn.tas"}};
  inputs.capabilities[1]->digest = compute_digest(*inputs.capabilities[1]);
  auto complete = evaluate(inputs);
  QOS_REQUIRE(complete.has_value());
  QOS_CHECK(complete->outcome == Outcome::Supported);
}

QOS_TEST(policy_predicates_extend_the_class_obligation_set) {
  EvaluationInputs inputs = base_inputs();
  inputs.policy.required_predicates = {CapabilityKey{"detnet.explicit"}};
  inputs.policy.digest = compute_digest(inputs.policy);
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Unknown);
  bool found = false;
  for (const ObligationAssessment& item : decision->obligations) {
    if (item.id.str() == std::string("capability.predicate.detnet.explicit")) {
      found = true;
      QOS_CHECK(item.status == ObligationStatus::Unknown);
    }
  }
  QOS_CHECK(found);
}

QOS_TEST(decision_is_deterministic_and_order_canonical) {
  EvaluationInputs inputs = base_inputs();
  inputs.klass.required_predicates = {CapabilityKey{"zzz"}, CapabilityKey{"aaa"}};
  inputs.klass.allow_degrade = true;
  inputs.klass.degrade_max_relaxation_ppm = 100'000;
  inputs.klass.digest = compute_digest(inputs.klass);
  inputs.capabilities[0]->predicates = {CapabilityKey{"aaa"}, CapabilityKey{"zzz"}};
  inputs.capabilities[0]->digest = compute_digest(*inputs.capabilities[0]);
  inputs.capabilities[1]->predicates = {CapabilityKey{"zzz"}, CapabilityKey{"aaa"}};
  inputs.capabilities[1]->digest = compute_digest(*inputs.capabilities[1]);

  const auto first = evaluate(inputs);
  QOS_REQUIRE(first.has_value());
  const auto second = evaluate(inputs);
  QOS_REQUIRE(second.has_value());
  QOS_CHECK(first->decision_digest == second->decision_digest);
  QOS_CHECK_EQ(explain(*first), explain(*second));

  // Predicate obligations are emitted in canonical identifier order, so the
  // digest does not depend on the order the class listed them in.
  EvaluationInputs reordered = inputs;
  reordered.klass.required_predicates = {CapabilityKey{"aaa"}, CapabilityKey{"zzz"}};
  reordered.klass.digest = compute_digest(reordered.klass);
  const auto third = evaluate(reordered);
  QOS_REQUIRE(third.has_value());
  QOS_CHECK(first->decision_digest == third->decision_digest);
  for (std::size_t i = 1; i < first->obligations.size(); ++i) {
    const bool ordered =
        static_cast<std::uint8_t>(first->obligations[i - 1].kind) <
            static_cast<std::uint8_t>(first->obligations[i].kind) ||
        (first->obligations[i - 1].kind == first->obligations[i].kind &&
         first->obligations[i - 1].id.view() < first->obligations[i].id.view());
    QOS_CHECK(ordered);
  }
}

QOS_TEST(authority_vector_names_every_generation_used) {
  EvaluationInputs inputs = base_inputs();
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  bool saw_class = false;
  bool saw_policy = false;
  bool saw_path = false;
  bool saw_epoch = false;
  std::size_t capabilities = 0;
  for (const AuthorityEntry& entry : decision->authority) {
    switch (entry.dimension) {
      case AuthorityEntry::Dimension::Class: saw_class = true; break;
      case AuthorityEntry::Dimension::Policy: saw_policy = true; break;
      case AuthorityEntry::Dimension::Path: saw_path = true; break;
      case AuthorityEntry::Dimension::FabricEpoch: saw_epoch = true; break;
      case AuthorityEntry::Dimension::Capability:
        ++capabilities;
        QOS_CHECK_EQ(entry.digest_hex.size(), std::size_t{64});
        break;
      default: break;
    }
  }
  QOS_CHECK(saw_class);
  QOS_CHECK(saw_policy);
  QOS_CHECK(saw_path);
  QOS_CHECK(saw_epoch);
  QOS_CHECK_EQ(capabilities, std::size_t{2});
}

QOS_TEST(obligation_set_is_complete_and_bounded) {
  QoSClass klass = base_class();
  klass.burst_bytes = 4'096;
  klass.burst_interval_us = 500;
  klass.min_mtu_bytes = 1'500;
  klass.min_priority_rank = 4;
  klass.requires_reservation = true;
  for (std::size_t i = 0; i < limits::kMaxRequiredPredicates; ++i) {
    klass.required_predicates.emplace_back("pred" + std::to_string(i));
  }
  klass.digest = compute_digest(klass);
  Policy policy = base_policy();
  auto obligations = derive_obligations(klass, policy);
  QOS_REQUIRE(obligations.has_value());
  QOS_CHECK(obligations->size() <= limits::kMaxObligations);
  // Every declared requirement produced exactly one obligation.
  // Thirteen kinds are always implied (rate, latency, jitter, loss, burst,
  // MTU, isolation, treatment, priority, reservation, diversity, predicates
  // and declared resource state) plus one obligation per required predicate.
  QOS_CHECK_EQ(obligations->size(), std::size_t{limits::kMaxRequiredPredicates} + 13);
  std::size_t predicates = 0;
  for (const ObligationAssessment& item : *obligations) {
    if (item.kind == ObligationKind::Predicate) {
      ++predicates;
    }
  }
  QOS_CHECK_EQ(predicates, limits::kMaxRequiredPredicates);
}

QOS_TEST(structural_obligations_are_never_relaxable) {
  QOS_CHECK(!obligation_relaxable(ObligationKind::Isolation));
  QOS_CHECK(!obligation_relaxable(ObligationKind::Treatment));
  QOS_CHECK(!obligation_relaxable(ObligationKind::PriorityRank));
  QOS_CHECK(!obligation_relaxable(ObligationKind::Predicate));
  QOS_CHECK(!obligation_relaxable(ObligationKind::Reservation));
  QOS_CHECK(obligation_relaxable(ObligationKind::Latency));
  QOS_CHECK(obligation_relaxable(ObligationKind::MinRate));

  // A structural shortfall is a violation even with a wide-open window.
  EvaluationInputs inputs = base_inputs();
  inputs.klass.allow_degrade = true;
  inputs.klass.degrade_max_relaxation_ppm = 1'000'000;
  inputs.klass.isolation = IsolationLevel::ResourceIsolated;
  inputs.klass.digest = compute_digest(inputs.klass);
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Unsupported);
  const ObligationAssessment* isolation = obligation(*decision, ObligationKind::Isolation);
  QOS_REQUIRE(isolation != nullptr);
  QOS_CHECK(isolation->status == ObligationStatus::Violated);
  QOS_CHECK_EQ(isolation->relaxation_applied_ppm, 0u);
}

QOS_TEST(aggregation_overflow_is_rejected_not_wrapped) {
  EvaluationInputs inputs = base_inputs();
  inputs.path.components.clear();
  inputs.path.components.resize(limits::kMaxPathComponents);
  inputs.capabilities.assign(limits::kMaxPathComponents, capability("r", "d"));
  for (std::size_t i = 0; i < inputs.path.components.size(); ++i) {
    PathComponent& component = inputs.path.components[i];
    component.resource = ResourceId{"r" + std::to_string(i)};
    component.capability_generation = Generation{1};
    component.diversity_domain = DiversityDomain{"d"};
    component.role = PathRole::Member;
    inputs.capabilities[i]->resource = component.resource;
    inputs.capabilities[i]->latency_bound_us = limits::kMaxLatencyUs;
    inputs.capabilities[i]->digest = compute_digest(*inputs.capabilities[i]);
  }
  inputs.path.digest = compute_digest(inputs.path);
  inputs.request.path = inputs.path.id;
  auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  // 256 components at the numeric ceiling cannot be summed into a bound the
  // class can compare against, so the verdict is not a positive one.
  QOS_CHECK(decision->outcome != Outcome::Supported);
  QOS_CHECK(decision->outcome != Outcome::SupportedDegraded);
}
