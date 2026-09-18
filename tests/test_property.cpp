// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Seeded randomized properties. The generator is a plain xorshift so every
// case is reproducible from its seed, and every failure prints the seed.

#include "harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "qosfabric/qosfabric.hpp"

using namespace qosfabric;

namespace {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

  std::uint64_t between(std::uint64_t low, std::uint64_t high) {
    return low + below(high - low + 1);
  }

  bool coin() { return (next() & 1u) != 0; }

 private:
  std::uint64_t state_;
};

constexpr std::uint64_t kNow = 1'700'000'000'000ull;

std::uint8_t outcome_rank(Outcome value) { return static_cast<std::uint8_t>(value); }

ResourceCapability make_capability(const std::string& resource, const std::string& domain,
                                   Rng& rng) {
  ResourceCapability value;
  value.resource = ResourceId{resource};
  value.generation = Generation{1};
  value.epoch = FabricEpoch{1};
  value.publisher = PublisherId{"bus"};
  value.incarnation = PublisherIncarnation{1};
  value.published_at_ms = kNow;
  value.expires_at_ms = kNow + 600'000;
  value.state = CapabilityState::Available;
  value.supported_rate_bps = rng.between(100'000, 10'000'000);
  value.latency_bound_us = rng.between(10, 2'000);
  value.jitter_bound_us = rng.between(1, 200);
  value.loss_ppm = static_cast<std::uint32_t>(rng.between(0, 500));
  value.max_isolation = IsolationLevel::QueueIsolated;
  value.max_treatment = TreatmentMode::RateLimited;
  value.max_priority_rank = static_cast<std::uint32_t>(rng.between(0, 8));
  value.diversity_domain = DiversityDomain{domain};
  value.digest = compute_digest(value);
  return value;
}

EvaluationInputs make_inputs(std::size_t components, Rng& rng) {
  EvaluationInputs inputs;
  inputs.klass.id = QoSClassId{"prop.class"};
  inputs.klass.generation = Generation{1};
  inputs.klass.min_rate_bps = rng.between(100'000, 5'000'000);
  inputs.klass.max_latency_us = rng.between(500, 8'000);
  inputs.klass.max_jitter_us = 400;
  inputs.klass.max_loss_ppm = static_cast<std::uint32_t>(rng.between(1, 1'000));
  inputs.klass.isolation = IsolationLevel::QueueIsolated;
  inputs.klass.treatment = TreatmentMode::RateLimited;
  inputs.klass.min_priority_rank = static_cast<std::uint32_t>(rng.between(0, 4));
  inputs.klass.min_disjoint_paths = static_cast<std::uint32_t>(rng.between(1, 3));
  inputs.klass.allow_degrade = rng.coin();
  inputs.klass.degrade_max_relaxation_ppm =
      inputs.klass.allow_degrade ? static_cast<std::uint32_t>(rng.between(1, 500'000)) : 0;
  inputs.klass.digest = compute_digest(inputs.klass);

  inputs.policy.id = PolicyId{"prop.policy"};
  inputs.policy.generation = Generation{1};
  inputs.policy.degrade_permitted = inputs.klass.allow_degrade && rng.coin();
  inputs.policy.max_relaxation_ppm =
      inputs.policy.degrade_permitted ? static_cast<std::uint32_t>(rng.between(1, 500'000)) : 0;
  inputs.policy.max_stale_epochs = 0;
  inputs.policy.capability_max_age_ms = 600'000;
  inputs.policy.digest = compute_digest(inputs.policy);

  inputs.path.id = PathId{"prop.path"};
  inputs.path.generation = Generation{1};
  inputs.path.fixed_latency_us = rng.between(0, 200);
  inputs.path.epoch = FabricEpoch{1};
  for (std::size_t i = 0; i < components; ++i) {
    PathComponent component;
    component.resource = ResourceId{"r" + std::to_string(i)};
    component.capability_generation = Generation{1};
    component.diversity_domain = DiversityDomain{"d" + std::to_string(i % 4)};
    component.role = PathRole::Member;
    inputs.path.components.push_back(component);
    ResourceCapability capability =
        make_capability(component.resource.str(), component.diversity_domain.str(), rng);
    if (rng.below(8) == 0) {
      capability.state = CapabilityState::Unavailable;
      capability.digest = compute_digest(capability);
    } else if (rng.below(8) == 0) {
      capability.loss_ppm.reset();
      capability.digest = compute_digest(capability);
    }
    inputs.capabilities.push_back(capability);
    inputs.reservations.push_back(std::nullopt);
  }
  inputs.path.digest = compute_digest(inputs.path);

  inputs.request.contract = QoSContractId{"prop.contract"};
  inputs.request.contract_generation = Generation{1};
  inputs.request.subject = SubjectId{"prop.subject"};
  inputs.request.subject_generation = Generation{1};
  inputs.request.class_id = inputs.klass.id;
  inputs.request.class_generation = inputs.klass.generation;
  inputs.request.path = inputs.path.id;
  inputs.request.path_generation = inputs.path.generation;
  inputs.request.policy = inputs.policy.id;
  inputs.request.policy_generation = inputs.policy.generation;
  inputs.request.priority = PriorityClassId{"gold"};
  inputs.request.priority_generation = Generation{1};
  inputs.request.observed_epoch = FabricEpoch{1};
  inputs.request.request_ms = kNow;

  inputs.current_epoch = FabricEpoch{1};
  inputs.now_ms = kNow;
  inputs.evaluator = "property-test";
  return inputs;
}

}  // namespace

QOS_TEST(evaluation_is_deterministic_for_every_seed) {
  for (std::uint64_t seed = 1; seed <= 300; ++seed) {
    Rng rng(seed);
    const EvaluationInputs inputs = make_inputs(1 + rng.below(6), rng);
    const auto first = evaluate(inputs);
    const auto second = evaluate(inputs);
    QOS_REQUIRE(first.has_value());
    QOS_REQUIRE(second.has_value());
    if (!(first->decision_digest == second->decision_digest)) {
      QOS_CHECK_EQ(seed, std::uint64_t{0});
      continue;
    }
    if (explain(*first) != explain(*second)) {
      QOS_CHECK_EQ(seed, std::uint64_t{0});
      continue;
    }
    // A recorded decision re-digests to exactly the same value.
    if (compute_decision_digest(*first) != first->decision_digest) {
      QOS_CHECK_EQ(seed, std::uint64_t{0});
    }
  }
}

QOS_TEST(missing_evidence_never_yields_a_positive_verdict) {
  for (std::uint64_t seed = 1000; seed <= 1300; ++seed) {
    Rng rng(seed);
    EvaluationInputs inputs = make_inputs(2 + rng.below(4), rng);
    const std::size_t removed = 1 + static_cast<std::size_t>(rng.below(inputs.capabilities.size()));
    for (std::size_t i = 0; i < removed; ++i) {
      inputs.capabilities[rng.below(inputs.capabilities.size())].reset();
    }
    const auto decision = evaluate(inputs);
    QOS_REQUIRE(decision.has_value());
    if (is_positive(decision->outcome)) {
      QOS_CHECK_EQ(seed, std::uint64_t{0});
      QOS_CHECK(false);
    }
    if (decision->unknown_components == 0) {
      QOS_CHECK_EQ(seed, std::uint64_t{0});
    }
  }
}

QOS_TEST(worsening_a_component_never_improves_the_verdict) {
  for (std::uint64_t seed = 2000; seed <= 2300; ++seed) {
    Rng rng(seed);
    EvaluationInputs inputs = make_inputs(2 + rng.below(4), rng);
    const auto baseline = evaluate(inputs);
    QOS_REQUIRE(baseline.has_value());

    EvaluationInputs worse = inputs;
    const std::size_t index = static_cast<std::size_t>(rng.below(worse.capabilities.size()));
    ResourceCapability& capability = *worse.capabilities[index];
    if (rng.coin()) {
      capability.supported_rate_bps = 1;
    } else {
      capability.latency_bound_us = 100'000;
    }
    capability.digest = compute_digest(capability);
    const auto degraded = evaluate(worse);
    QOS_REQUIRE(degraded.has_value());
    if (outcome_rank(degraded->outcome) < outcome_rank(baseline->outcome)) {
      // The verdict became strictly better after evidence got worse.
      QOS_CHECK_EQ(seed, std::uint64_t{0});
    }
  }
}

QOS_TEST(component_permutation_preserves_composed_values) {
  for (std::uint64_t seed = 3000; seed <= 3200; ++seed) {
    Rng rng(seed);
    EvaluationInputs inputs = make_inputs(3 + rng.below(4), rng);
    const auto forward = evaluate(inputs);
    QOS_REQUIRE(forward.has_value());

    EvaluationInputs reversed = inputs;
    std::reverse(reversed.path.components.begin(), reversed.path.components.end());
    std::reverse(reversed.capabilities.begin(), reversed.capabilities.end());
    reversed.path.digest = compute_digest(reversed.path);
    const auto backward = evaluate(reversed);
    QOS_REQUIRE(backward.has_value());

    QOS_CHECK(forward->outcome == backward->outcome);
    for (const ObligationAssessment& item : forward->obligations) {
      const ObligationAssessment* other = backward->find(item.kind);
      if (other == nullptr) {
        QOS_CHECK_EQ(seed, std::uint64_t{0});
        continue;
      }
      if (item.composed_value != other->composed_value) {
        QOS_CHECK_EQ(seed, std::uint64_t{0});
      }
    }
  }
}

QOS_TEST(degradation_boundary_is_exact) {
  for (std::uint64_t seed = 4000; seed <= 4300; ++seed) {
    Rng rng(seed);
    EvaluationInputs inputs = make_inputs(2, rng);
    inputs.klass.allow_degrade = true;
    inputs.klass.degrade_max_relaxation_ppm = 200'000;
    inputs.klass.digest = compute_digest(inputs.klass);
    inputs.policy.degrade_permitted = true;
    inputs.policy.max_relaxation_ppm = 100'000;
    inputs.policy.digest = compute_digest(inputs.policy);

    ResourceCapability& first = *inputs.capabilities[0];
    ResourceCapability& second = *inputs.capabilities[1];
    for (auto* capability : {&first, &second}) {
      // Pin every attribute the boundary property depends on, so a randomly
      // unavailable or partially declared component cannot mask the window
      // arithmetic being measured.
      capability->state = CapabilityState::Available;
      capability->jitter_bound_us = 0;
      capability->loss_ppm = 0;
      capability->supported_rate_bps = 100'000'000;
      capability->latency_bound_us = 100;
      capability->max_priority_rank = 8;
    }
    first.digest = compute_digest(first);
    second.digest = compute_digest(second);
    inputs.path.fixed_latency_us = 0;
    inputs.path.digest = compute_digest(inputs.path);
    inputs.klass.max_jitter_us.reset();
    inputs.klass.max_loss_ppm.reset();
    inputs.klass.min_priority_rank = 0;
    inputs.klass.min_disjoint_paths = 1;
    inputs.klass.isolation = IsolationLevel::None;
    inputs.klass.treatment = TreatmentMode::BestEffort;
    inputs.klass.min_rate_bps.reset();
    inputs.klass.digest = compute_digest(inputs.klass);

    const std::uint64_t limit = inputs.klass.max_latency_us.value();
    const std::uint64_t effective = limit + (limit * 100'000ull) / 1'000'000ull;
    struct Case {
      std::uint64_t composed;
      Outcome expected;
    };
    const Case cases[] = {
        {effective, Outcome::SupportedDegraded},
        {effective - 1, Outcome::SupportedDegraded},
        {effective + 1, Outcome::Unsupported},
        {limit, Outcome::Supported},
        {limit - 1, Outcome::Supported},
    };
    for (const Case& item : cases) {
      EvaluationInputs probe = inputs;
      probe.capabilities[0]->latency_bound_us = item.composed;
      probe.capabilities[0]->digest = compute_digest(*probe.capabilities[0]);
      probe.capabilities[1]->latency_bound_us = 0;
      probe.capabilities[1]->digest = compute_digest(*probe.capabilities[1]);
      const auto decision = evaluate(probe);
      QOS_REQUIRE(decision.has_value());
      if (decision->outcome != item.expected) {
        QOS_CHECK_EQ(seed, std::uint64_t{0});
        QOS_CHECK_EQ(static_cast<int>(decision->outcome), static_cast<int>(item.expected));
      }
    }
  }
}

QOS_TEST(random_bytes_are_never_accepted_as_records) {
  Rng rng(0xC0FFEEull);
  for (std::size_t length = 0; length <= 512; length += 7) {
    std::vector<std::uint8_t> buffer(length);
    for (std::uint8_t& byte : buffer) {
      byte = static_cast<std::uint8_t>(rng.below(256));
    }
    ByteReader reader(buffer);
    QoSClass klass;
    if (canonical_decode(reader, klass).has_value()) {
      // If a random buffer happens to decode, it must re-encode consistently
      // and must satisfy every structural invariant.
      QOS_CHECK(validate(klass).has_value());
      ByteWriter writer;
      canonical_encode(klass, writer);
      QOS_REQUIRE(writer.ok());
      ByteReader again(writer.data());
      QoSClass second;
      QOS_REQUIRE(canonical_decode(again, second).has_value());
      QOS_CHECK(second.digest == klass.digest);
    }
    ByteReader decision_reader(buffer);
    ContractDecision decision;
    (void)canonical_decode(decision_reader, decision);
    const auto frame = decode_frame(buffer);
    if (frame.has_value()) {
      QOS_CHECK(frame->type != MessageType::Unknown);
    }
  }
}

QOS_TEST(wide_paths_stay_bounded_and_explainable) {
  Rng rng(4242);
  EvaluationInputs inputs = make_inputs(limits::kMaxPathComponents, rng);
  const auto decision = evaluate(inputs);
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(inputs.path.components.size() == limits::kMaxPathComponents);
  QOS_CHECK(decision->obligations.size() <= limits::kMaxObligations);
  QOS_CHECK(decision->resources.size() <= limits::kMaxPathComponents);
  QOS_CHECK(decision->authority.size() <= limits::kMaxAuthorityEntries);
  const std::string text = explain(*decision);
  QOS_CHECK(text.size() <= limits::kMaxExplainBytes);
}
