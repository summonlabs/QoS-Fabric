// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "harness.hpp"
#include "tempdir.hpp"

#include <string>
#include <vector>

#include "qosfabric/qosfabric.hpp"

using namespace qosfabric;

namespace {

constexpr std::uint64_t kNow = 1'900'000'000'000ull;

RegistryOptions registry_options(const std::string& directory) {
  RegistryOptions options;
  options.directory = directory;
  options.create_if_missing = true;
  return options;
}

QoSClass make_class(const char* id, std::uint64_t generation) {
  QoSClass value;
  value.id = QoSClassId{id};
  value.generation = Generation{generation};
  value.min_rate_bps = 1'000'000;
  value.max_latency_us = 1'500;
  value.isolation = IsolationLevel::QueueIsolated;
  value.treatment = TreatmentMode::RateLimited;
  return value;
}

Policy make_policy(const char* id, std::uint64_t generation) {
  Policy value;
  value.id = PolicyId{id};
  value.generation = Generation{generation};
  value.capability_max_age_ms = 600'000;
  return value;
}

Path make_path(const char* id, std::uint64_t generation, std::uint64_t capability_generation) {
  Path value;
  value.id = PathId{id};
  value.generation = Generation{generation};
  PathComponent component;
  component.resource = ResourceId{"r1"};
  component.capability_generation = Generation{capability_generation};
  component.diversity_domain = DiversityDomain{"d1"};
  component.role = PathRole::Primary;
  value.components.push_back(component);
  return value;
}

ResourceCapability make_capability(const char* resource, std::uint64_t generation,
                                   const char* publisher, std::uint64_t incarnation,
                                   FabricEpoch epoch, std::uint64_t now) {
  ResourceCapability value;
  value.resource = ResourceId{resource};
  value.generation = Generation{generation};
  value.epoch = epoch;
  value.publisher = PublisherId{publisher};
  value.incarnation = PublisherIncarnation{incarnation};
  value.published_at_ms = now;
  value.expires_at_ms = now + 600'000;
  value.state = CapabilityState::Available;
  value.supported_rate_bps = 5'000'000;
  value.latency_bound_us = 500;
  value.max_isolation = IsolationLevel::QueueIsolated;
  value.max_treatment = TreatmentMode::RateLimited;
  value.diversity_domain = DiversityDomain{"d1"};
  return value;
}

ContractRequest make_request(const char* contract, std::uint64_t generation) {
  ContractRequest request;
  request.contract = QoSContractId{contract};
  request.contract_generation = Generation{generation};
  request.subject = SubjectId{"svc-a"};
  request.subject_generation = Generation{1};
  request.class_id = QoSClassId{"net.voice"};
  request.path = PathId{"p1"};
  request.policy = PolicyId{"strict"};
  request.priority = PriorityClassId{"gold"};
  request.priority_generation = Generation{1};
  request.request_ms = kNow;
  return request;
}

}  // namespace

QOS_TEST(publication_assigns_and_advances_generations) {
  qostest::TempDir directory("registry-generations");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);

  const auto first = (*registry)->PublishClass(make_class("net.voice", 1), "unit-test");
  QOS_REQUIRE(first.has_value());
  QOS_CHECK_EQ(first->generation.value(), 1ull);

  // Idempotent re-publication of identical content.
  const auto repeated = (*registry)->PublishClass(make_class("net.voice", 1), "unit-test");
  QOS_REQUIRE(repeated.has_value());
  QOS_CHECK(repeated->digest == first->digest);
  QOS_CHECK_EQ(repeated->sequence.value(), 0ull);

  const auto second = (*registry)->PublishClass(make_class("net.voice", 2), "unit-test");
  QOS_REQUIRE(second.has_value());
  QOS_CHECK_EQ(second->generation.value(), 2ull);

  const auto skipped = (*registry)->PublishClass(make_class("net.voice", 5), "unit-test");
  QOS_CHECK(!skipped.has_value());
  QOS_CHECK_EQ(skipped.code(), ErrorCode::InvalidArgument);

  // A generation older than the retained one is stale authority, never a
  // silent overwrite.
  const auto older = (*registry)->PublishClass(make_class("net.voice", 1), "unit-test");
  QOS_CHECK(!older.has_value());
  QOS_CHECK_EQ(older.code(), ErrorCode::Stale);

  const auto wrong_first = (*registry)->PublishClass(make_class("fresh.class", 3), "unit-test");
  QOS_CHECK(!wrong_first.has_value());
  QOS_CHECK_EQ(wrong_first.code(), ErrorCode::Stale);

  const auto latest = (*registry)->LatestClass(QoSClassId{"net.voice"});
  QOS_REQUIRE(latest.has_value());
  QOS_CHECK_EQ(latest->generation.value(), 2ull);
  const auto pinned = (*registry)->GetClass(QoSClassId{"net.voice"}, Generation{1});
  QOS_REQUIRE(pinned.has_value());
  QOS_CHECK_EQ(pinned->generation.value(), 1ull);
  const auto absent = (*registry)->GetClass(QoSClassId{"net.voice"}, Generation{9});
  QOS_CHECK(!absent.has_value());
  QOS_REQUIRE((*registry)->Close().has_value());
}

QOS_TEST(equivocation_is_recorded_and_outlives_the_rejection) {
  qostest::TempDir directory("registry-equivocation");
  const std::string store_path = directory.path();
  std::string decision_digest;
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    (*registry)->set_now_millis(kNow);
    QOS_REQUIRE((*registry)->PublishClass(make_class("net.voice", 1), "publisher-a").has_value());

    QoSClass divergent = make_class("net.voice", 1);
    divergent.max_latency_us = 999;
    const auto conflicted = (*registry)->PublishClass(divergent, "publisher-b");
    QOS_CHECK(!conflicted.has_value());
    QOS_CHECK_EQ(conflicted.code(), ErrorCode::Conflict);
    QOS_CHECK_EQ((*registry)->conflicts().size(), std::size_t{1});
    QOS_CHECK_EQ((*registry)->conflicts().front().id, std::string("net.voice"));

    // Publications that do not depend on the equivocated definition are
    // unaffected: the marker is scoped evidence, not a global failure.
    QOS_REQUIRE((*registry)->PublishPolicy(make_policy("strict", 1), "unit-test").has_value());
    QOS_REQUIRE((*registry)->PublishPath(make_path("p1", 1, 1), "unit-test").has_value());

    const auto decision = (*registry)->Evaluate(make_request("c1", 1));
    QOS_REQUIRE(decision.has_value());
    QOS_CHECK(decision->outcome == Outcome::Conflict);
    decision_digest = decision->decision_digest.hex();
    QOS_REQUIRE((*registry)->Close().has_value());
  }
  {
    // The conflict evidence is durable: a restart cannot launder it away.
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    QOS_CHECK_EQ((*registry)->conflicts().size(), std::size_t{1});
    const auto recorded = (*registry)->GetDecision(QoSContractId{"c1"}, Generation{1});
    QOS_REQUIRE(recorded.has_value());
    QOS_CHECK(recorded->outcome == Outcome::Conflict);
    QOS_CHECK_EQ(recorded->decision_digest.hex(), decision_digest);
    QOS_REQUIRE((*registry)->Close().has_value());
  }
}

QOS_TEST(publisher_incarnations_are_fenced_and_never_reused) {
  qostest::TempDir directory("registry-fencing");
  const std::string store_path = directory.path();
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    (*registry)->set_now_millis(kNow);
    const auto first = (*registry)->RegisterPublisher(
        PublisherId{"bus-1"}, PublisherIncarnation{1}, BootId::generate(), 60'000, "boot-1");
    QOS_REQUIRE(first.has_value());
    QOS_CHECK(first->epoch == (*registry)->current_epoch());

    const auto reused = (*registry)->RegisterPublisher(
        PublisherId{"bus-1"}, PublisherIncarnation{1}, BootId::generate(), 60'000, "boot-1-again");
    QOS_CHECK(!reused.has_value());
    QOS_CHECK_EQ(reused.code(), ErrorCode::Fenced);

    const auto rewound = (*registry)->RegisterPublisher(
        PublisherId{"bus-1"}, PublisherIncarnation{0}, BootId::generate(), 60'000, "boot-0");
    QOS_CHECK(!rewound.has_value());

    const auto second = (*registry)->RegisterPublisher(
        PublisherId{"bus-1"}, PublisherIncarnation{2}, BootId::generate(), 60'000, "boot-2");
    QOS_REQUIRE(second.has_value());
    QOS_CHECK_EQ((*registry)->highest_incarnation(PublisherId{"bus-1"}).value(), 2ull);
    QOS_REQUIRE((*registry)->Close().has_value());
  }
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    // The watermark is durable, the lease is not: liveness never survives a
    // restart.
    QOS_CHECK_EQ((*registry)->highest_incarnation(PublisherId{"bus-1"}).value(), 2ull);
    QOS_CHECK(!(*registry)->FindLease(PublisherId{"bus-1"}).has_value());
    QOS_CHECK_EQ((*registry)->counts().live_leases, std::size_t{0});
    const auto stale = (*registry)->RegisterPublisher(
        PublisherId{"bus-1"}, PublisherIncarnation{2}, BootId::generate(), 60'000, "boot-2-again");
    QOS_CHECK(!stale.has_value());
    QOS_CHECK_EQ(stale.code(), ErrorCode::Fenced);
    const auto next = (*registry)->RegisterPublisher(
        PublisherId{"bus-1"}, PublisherIncarnation{3}, BootId::generate(), 60'000, "boot-3");
    QOS_REQUIRE(next.has_value());
    QOS_REQUIRE((*registry)->Close().has_value());
  }
}

QOS_TEST(capability_publication_requires_live_authority) {
  qostest::TempDir directory("registry-capability");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);
  const FabricEpoch epoch = (*registry)->current_epoch();

  // No lease at all.
  const auto orphan = (*registry)->PublishCapability(
      make_capability("r1", 1, "bus-1", 1, epoch, kNow));
  QOS_CHECK(!orphan.has_value());
  QOS_CHECK_EQ(orphan.code(), ErrorCode::Unauthorized);

  QOS_REQUIRE((*registry)
                  ->RegisterPublisher(PublisherId{"bus-1"}, PublisherIncarnation{1},
                                      BootId::generate(), 60'000, "boot-1")
                  .has_value());

  // Wrong incarnation for a live lease.
  const auto wrong_incarnation = (*registry)->PublishCapability(
      make_capability("r1", 1, "bus-1", 9, epoch, kNow));
  QOS_CHECK(!wrong_incarnation.has_value());
  QOS_CHECK_EQ(wrong_incarnation.code(), ErrorCode::Fenced);

  // Right incarnation, stale epoch.
  const auto wrong_epoch = (*registry)->PublishCapability(
      make_capability("r1", 1, "bus-1", 1, FabricEpoch{epoch.value() + 5}, kNow));
  QOS_CHECK(!wrong_epoch.has_value());
  QOS_CHECK_EQ(wrong_epoch.code(), ErrorCode::Stale);

  const auto published =
      (*registry)->PublishCapability(make_capability("r1", 1, "bus-1", 1, epoch, kNow));
  QOS_REQUIRE(published.has_value());
  QOS_CHECK_EQ(published->generation.value(), 1ull);

  // Expiring the lease removes authority without removing the declaration.
  (*registry)->set_now_millis(kNow + 120'000);
  QOS_REQUIRE((*registry)->ExpireLeases().has_value());
  QOS_CHECK(!(*registry)->FindLease(PublisherId{"bus-1"}).has_value());
  const auto after_expiry =
      (*registry)->PublishCapability(make_capability("r1", 2, "bus-1", 1, epoch, kNow + 120'000));
  QOS_CHECK(!after_expiry.has_value());
  QOS_CHECK_EQ(after_expiry.code(), ErrorCode::Unauthorized);
  const auto still_held = (*registry)->LatestCapability(ResourceId{"r1"});
  QOS_REQUIRE(still_held.has_value());
  QOS_CHECK_EQ(still_held->generation.value(), 1ull);
  QOS_REQUIRE((*registry)->Close().has_value());
}

QOS_TEST(epoch_advances_monotonically_and_invalidates_authority) {
  qostest::TempDir directory("registry-epoch");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);
  const FabricEpoch start = (*registry)->current_epoch();
  QOS_CHECK_EQ(start.value(), 1ull);
  QOS_REQUIRE((*registry)
                  ->RegisterPublisher(PublisherId{"bus-1"}, PublisherIncarnation{1},
                                      BootId::generate(), 60'000, "boot-1")
                  .has_value());
  QOS_CHECK((*registry)->FindLease(PublisherId{"bus-1"}).has_value());

  const auto advanced = (*registry)->AdvanceEpoch("handover");
  QOS_REQUIRE(advanced.has_value());
  QOS_CHECK_EQ(advanced->value(), 2ull);
  QOS_CHECK(!(*registry)->FindLease(PublisherId{"bus-1"}).has_value());
  QOS_REQUIRE((*registry)->Close().has_value());
}

QOS_TEST(evaluation_is_durable_and_replayable) {
  qostest::TempDir directory("registry-decisions");
  const std::string store_path = directory.path();
  const FabricEpoch epoch = FabricEpoch{1};
  std::string outcome_text;
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    (*registry)->set_now_millis(kNow);
    QOS_REQUIRE((*registry)->PublishClass(make_class("net.voice", 1), "unit-test").has_value());
    QOS_REQUIRE((*registry)->PublishPolicy(make_policy("strict", 1), "unit-test").has_value());
    QOS_REQUIRE((*registry)->PublishPath(make_path("p1", 1, 1), "unit-test").has_value());
    QOS_REQUIRE((*registry)
                    ->RegisterPublisher(PublisherId{"bus-1"}, PublisherIncarnation{1},
                                        BootId::generate(), 600'000, "boot-1")
                    .has_value());
    QOS_REQUIRE((*registry)
                    ->PublishCapability(make_capability("r1", 1, "bus-1", 1, epoch, kNow))
                    .has_value());
    const auto decision = (*registry)->Evaluate(make_request("c1", 1));
    QOS_REQUIRE(decision.has_value());
    QOS_CHECK(decision->outcome == Outcome::Supported);
    outcome_text = to_string(decision->outcome);
    QOS_REQUIRE((*registry)->Close().has_value());
  }
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    const auto recorded = (*registry)->GetDecision(QoSContractId{"c1"}, Generation{1});
    QOS_REQUIRE(recorded.has_value());
    QOS_CHECK_EQ(to_string(recorded->outcome), outcome_text);
    QOS_CHECK(!recorded->obligations.empty());
    QOS_CHECK(recorded->binding_obligation.has_value());
    QOS_REQUIRE((*registry)->Close().has_value());
  }
}

QOS_TEST(compaction_round_trips_the_whole_registry) {
  qostest::TempDir directory("registry-compaction");
  const std::string store_path = directory.path();
  RegistryCounts before{};
  std::string digest_before;
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    (*registry)->set_now_millis(kNow);
    QOS_REQUIRE((*registry)->PublishClass(make_class("net.voice", 1), "unit-test").has_value());
    QOS_REQUIRE((*registry)->PublishClass(make_class("net.voice", 2), "unit-test").has_value());
    QOS_REQUIRE((*registry)->PublishPolicy(make_policy("strict", 1), "unit-test").has_value());
    QOS_REQUIRE((*registry)->PublishPath(make_path("p1", 1, 1), "unit-test").has_value());
    QOS_REQUIRE((*registry)
                    ->RegisterPublisher(PublisherId{"bus-1"}, PublisherIncarnation{1},
                                        BootId::generate(), 600'000, "boot-1")
                    .has_value());
    QOS_REQUIRE((*registry)
                    ->PublishCapability(make_capability("r1", 1, "bus-1", 1,
                                                        (*registry)->current_epoch(), kNow))
                    .has_value());
    const auto decision = (*registry)->Evaluate(make_request("c1", 1));
    QOS_REQUIRE(decision.has_value());
    digest_before = decision->decision_digest.hex();
    before = (*registry)->counts();
    QOS_REQUIRE((*registry)->Compact().has_value());
    QOS_REQUIRE((*registry)->Close().has_value());
  }
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    QOS_CHECK((*registry)->recovery().snapshot_loaded);
    const RegistryCounts after = (*registry)->counts();
    QOS_CHECK_EQ(after.classes, before.classes);
    QOS_CHECK_EQ(after.policies, before.policies);
    QOS_CHECK_EQ(after.paths, before.paths);
    QOS_CHECK_EQ(after.capabilities, before.capabilities);
    QOS_CHECK_EQ(after.decisions, before.decisions);
    QOS_CHECK_EQ(after.publishers, before.publishers);
    QOS_CHECK_EQ(after.conflicts, before.conflicts);
    const auto recorded = (*registry)->GetDecision(QoSContractId{"c1"}, Generation{1});
    QOS_REQUIRE(recorded.has_value());
    QOS_CHECK_EQ(recorded->decision_digest.hex(), digest_before);
    QOS_REQUIRE((*registry)->Verify().has_value());
    QOS_REQUIRE((*registry)->Close().has_value());
  }
}

QOS_TEST(observation_only_open_changes_nothing) {
  qostest::TempDir directory("registry-observation");
  const std::string store_path = directory.path();
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    QOS_REQUIRE((*registry)->PublishClass(make_class("net.voice", 1), "unit-test").has_value());
    QOS_REQUIRE((*registry)->Close().has_value());
  }
  FabricEpoch epoch = FabricEpoch{0};
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    epoch = (*registry)->current_epoch();
    QOS_REQUIRE((*registry)->Close().has_value());
  }
  RegistryOptions observational = registry_options(store_path);
  observational.claim_authority = false;
  {
    auto registry = Registry::Open(observational);
    QOS_REQUIRE(registry.has_value());
    QOS_CHECK((*registry)->current_epoch() == epoch);
    QOS_CHECK_EQ((*registry)->counts().classes, std::size_t{1});
    QOS_CHECK((*registry)->Close().has_value());
  }
  {
    auto registry = Registry::Open(observational);
    QOS_REQUIRE(registry.has_value());
    QOS_CHECK((*registry)->current_epoch() == epoch);
    QOS_REQUIRE((*registry)->Close().has_value());
  }
}

QOS_TEST(retention_bounds_generations_and_compaction_prunes) {
  qostest::TempDir directory("registry-retention");
  RegistryOptions options = registry_options(directory.path());
  options.max_generations_per_id = 3;
  auto registry = Registry::Open(options);
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);
  for (std::uint64_t generation = 1; generation <= 6; ++generation) {
    QoSClass value = make_class("net.voice", generation);
    value.max_latency_us = 1'000 + generation;
    QOS_REQUIRE((*registry)->PublishClass(value, "unit-test").has_value());
  }
  const auto newest = (*registry)->LatestClass(QoSClassId{"net.voice"});
  QOS_REQUIRE(newest.has_value());
  QOS_CHECK_EQ(newest->generation.value(), 6ull);
  const auto pruned = (*registry)->GetClass(QoSClassId{"net.voice"}, Generation{1});
  QOS_CHECK(!pruned.has_value());
  QOS_CHECK((*registry)->GetClass(QoSClassId{"net.voice"}, Generation{4}).has_value());
  QOS_REQUIRE((*registry)->Compact().has_value());
  QOS_REQUIRE((*registry)->Close().has_value());

  auto reopened = Registry::Open(options);
  QOS_REQUIRE(reopened.has_value());
  QOS_CHECK((*reopened)->GetClass(QoSClassId{"net.voice"}, Generation{4}).has_value());
  QOS_CHECK(!(*reopened)->GetClass(QoSClassId{"net.voice"}, Generation{3}).has_value());
  QOS_REQUIRE((*reopened)->Close().has_value());
}

QOS_TEST(malformed_and_oversized_publications_are_refused) {
  qostest::TempDir directory("registry-malformed");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);

  QoSClass bad_id = make_class("net.voice", 1);
  bad_id.id = QoSClassId{"bad id"};
  QOS_CHECK_EQ((*registry)->PublishClass(bad_id, "unit-test").code(), ErrorCode::InvalidArgument);

  QoSClass zero_generation = make_class("net.voice", 1);
  zero_generation.generation = Generation{0};
  QOS_CHECK_EQ((*registry)->PublishClass(zero_generation, "unit-test").code(),
               ErrorCode::InvalidArgument);

  QoSClass contradictory = make_class("net.voice", 1);
  contradictory.max_jitter_us = 10'000;
  QOS_CHECK_EQ((*registry)->PublishClass(contradictory, "unit-test").code(), ErrorCode::Conflict);

  Path path = make_path("p1", 1, 1);
  PathComponent duplicate = path.components.front();
  path.components.push_back(duplicate);
  QOS_CHECK_EQ((*registry)->PublishPath(path, "unit-test").code(), ErrorCode::Duplicate);

  QOS_CHECK(!(*registry)->LatestClass(QoSClassId{"absent"}).has_value());
  QOS_CHECK(!(*registry)->GetPolicy(PolicyId{"absent"}, Generation{1}).has_value());
  QOS_REQUIRE((*registry)->Close().has_value());
}

QOS_TEST(evaluation_without_definitions_is_rejected_not_guessed) {
  qostest::TempDir directory("registry-absent");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);
  const auto decision = (*registry)->Evaluate(make_request("c1", 1));
  QOS_REQUIRE(decision.has_value());
  QOS_CHECK(decision->outcome == Outcome::Rejected);
  QOS_CHECK(decision->primary_reason.find("absent") != std::string::npos);
  QOS_REQUIRE((*registry)->Close().has_value());
}

QOS_TEST(verify_reports_a_tampered_log) {
  qostest::TempDir directory("registry-tamper");
  const std::string store_path = directory.path();
  {
    auto registry = Registry::Open(registry_options(store_path));
    QOS_REQUIRE(registry.has_value());
    QOS_REQUIRE((*registry)->PublishClass(make_class("net.voice", 1), "unit-test").has_value());
    QOS_REQUIRE((*registry)->Close().has_value());
  }
  const std::string wal = store_path + "/WAL.qwl";
  auto content = read_file(wal, 1u << 20);
  QOS_REQUIRE(content.has_value());
  QOS_REQUIRE(content->size() > 20);
  (*content)[content->size() / 2] = static_cast<std::uint8_t>((*content)[content->size() / 2] ^ 0x08u);
  QOS_REQUIRE(write_file_atomic(wal, *content).has_value());
  auto registry = Registry::Open(registry_options(store_path));
  QOS_CHECK(!registry.has_value());
  QOS_CHECK_EQ(registry.code(), ErrorCode::Corrupt);
}
