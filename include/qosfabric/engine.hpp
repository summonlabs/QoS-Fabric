// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The end-to-end compatibility engine.
//
// evaluate() is a pure function of its inputs: the same inputs always produce
// byte-identical explanations and an identical decision digest. Everything
// that varies with time or with registry state is passed in explicitly, so a
// decision can be replayed and audited.

#ifndef QOSFABRIC_ENGINE_HPP
#define QOSFABRIC_ENGINE_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "qosfabric/decision.hpp"
#include "qosfabric/model.hpp"

namespace qosfabric {

// The exact generations the registry currently holds for the identifiers a
// request references. The engine compares these with what the request and the
// path bound; a request that pins a superseded generation is STALE.
class CurrencyVector {
 public:
  std::optional<Generation> latest_class_generation{};
  std::optional<Generation> latest_policy_generation{};
  std::optional<Generation> latest_path_generation{};
};

class EvaluationInputs {
 public:
  ContractRequest request{};
  QoSClass klass{};
  Policy policy{};
  Path path{};

  // Exactly one entry per path component, in path order. An absent entry, or
  // an entry whose generation does not match the generation the path bound,
  // is missing evidence rather than an error.
  std::vector<std::optional<ResourceCapability>> capabilities{};
  // Exactly one entry per path component, in path order. Consulted only when
  // the class demands a reservation.
  std::vector<std::optional<Reservation>> reservations{};

  CurrencyVector currency{};
  std::vector<ConflictMarker> conflicts{};

  FabricEpoch current_epoch{};
  std::uint64_t now_ms{0};

  // Provenance recorded on the resulting decision.
  std::string evaluator{};
};

// Pure evaluation. Never throws, never mutates its inputs.
[[nodiscard]] Result<ContractDecision> evaluate(const EvaluationInputs& inputs);

// Builds the canonical obligation set a class and policy imply, in canonical
// order. Exposed so that callers can inspect the obligation set without
// running a full evaluation.
[[nodiscard]] Result<std::vector<ObligationAssessment>> derive_obligations(const QoSClass& klass,
                                                                          const Policy& policy);

}  // namespace qosfabric

#endif  // QOSFABRIC_ENGINE_HPP
