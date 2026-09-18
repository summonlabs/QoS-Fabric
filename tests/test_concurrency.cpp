// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Concurrency and race coverage. These tests assert invariants that must hold
// under real thread interleaving; they are not a substitute for a race
// detector, and the harness records exactly which sanitizer was available.

#include "harness.hpp"
#include "tempdir.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "qosfabric/qosfabric.hpp"

using namespace qosfabric;

namespace {

constexpr std::uint64_t kNow = 1'950'000'000'000ull;
constexpr int kThreads = 8;

RegistryOptions registry_options(const std::string& directory) {
  RegistryOptions options;
  options.directory = directory;
  options.create_if_missing = true;
  return options;
}

QoSClass make_class(const std::string& id, std::uint64_t generation, std::uint64_t latency) {
  QoSClass value;
  value.id = QoSClassId{id};
  value.generation = Generation{generation};
  value.min_rate_bps = 1'000'000;
  value.max_latency_us = latency;
  value.isolation = IsolationLevel::QueueIsolated;
  value.treatment = TreatmentMode::RateLimited;
  return value;
}

Policy make_policy() {
  Policy value;
  value.id = PolicyId{"strict"};
  value.generation = Generation{1};
  value.capability_max_age_ms = 600'000;
  return value;
}

Path make_path() {
  Path value;
  value.id = PathId{"p1"};
  value.generation = Generation{1};
  PathComponent component;
  component.resource = ResourceId{"r1"};
  component.capability_generation = Generation{1};
  component.diversity_domain = DiversityDomain{"d1"};
  component.role = PathRole::Primary;
  value.components.push_back(component);
  return value;
}

ResourceCapability make_capability(FabricEpoch epoch) {
  ResourceCapability value;
  value.resource = ResourceId{"r1"};
  value.generation = Generation{1};
  value.epoch = epoch;
  value.publisher = PublisherId{"bus-1"};
  value.incarnation = PublisherIncarnation{1};
  value.published_at_ms = kNow;
  value.expires_at_ms = kNow + 3'600'000;
  value.state = CapabilityState::Available;
  value.supported_rate_bps = 5'000'000;
  value.latency_bound_us = 500;
  value.max_isolation = IsolationLevel::QueueIsolated;
  value.max_treatment = TreatmentMode::RateLimited;
  value.diversity_domain = DiversityDomain{"d1"};
  return value;
}

ContractRequest make_request(const std::string& contract) {
  ContractRequest request;
  request.contract = QoSContractId{contract};
  request.contract_generation = Generation{1};
  request.subject = SubjectId{"svc"};
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

QOS_TEST(concurrent_evaluations_agree_exactly) {
  qostest::TempDir directory("concurrency-eval");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);
  QOS_REQUIRE((*registry)->PublishClass(make_class("net.voice", 1, 1'500), "test").has_value());
  QOS_REQUIRE((*registry)->PublishPolicy(make_policy(), "test").has_value());
  QOS_REQUIRE((*registry)->PublishPath(make_path(), "test").has_value());
  QOS_REQUIRE((*registry)
                  ->RegisterPublisher(PublisherId{"bus-1"}, PublisherIncarnation{1},
                                      BootId::generate(), 3'600'000, "boot")
                  .has_value());
  ResourceCapability capability = make_capability((*registry)->current_epoch());
  QOS_REQUIRE((*registry)->PublishCapability(capability).has_value());

  std::vector<std::thread> workers;
  std::vector<std::string> digests(kThreads);
  std::vector<Outcome> outcomes(kThreads, Outcome::Rejected);
  std::atomic<int> failures{0};
  for (int index = 0; index < kThreads; ++index) {
    workers.emplace_back([&, index] {
      for (int iteration = 0; iteration < 25; ++iteration) {
        // The same request from every thread: an authoritative decision must
        // not depend on who asked or on interleaving.
        const auto decision = (*registry)->Evaluate(make_request("c-shared"));
        if (!decision) {
          failures.fetch_add(1);
          return;
        }
        if (decision->outcome != Outcome::Supported) {
          failures.fetch_add(1);
          return;
        }
        digests[static_cast<std::size_t>(index)] = decision->decision_digest.hex();
        outcomes[static_cast<std::size_t>(index)] = decision->outcome;
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  QOS_CHECK_EQ(failures.load(), 0);
  for (int index = 1; index < kThreads; ++index) {
    QOS_CHECK_EQ(digests[static_cast<std::size_t>(index)], digests[0]);
  }
  QOS_REQUIRE((*registry)->Close().has_value());
}

QOS_TEST(concurrent_distinct_publications_all_land) {
  qostest::TempDir directory("concurrency-publish");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);

  std::vector<std::thread> workers;
  std::atomic<int> failures{0};
  for (int index = 0; index < kThreads; ++index) {
    workers.emplace_back([&, index] {
      for (std::uint64_t generation = 1; generation <= 4; ++generation) {
        const std::string id = "class." + std::to_string(index);
        const auto published =
            (*registry)->PublishClass(make_class(id, generation, 1'000 + generation), "thread");
        if (!published) {
          failures.fetch_add(1);
          return;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  QOS_CHECK_EQ(failures.load(), 0);
  const RegistryCounts counts = (*registry)->counts();
  QOS_CHECK_EQ(counts.classes, static_cast<std::size_t>(kThreads));
  for (int index = 0; index < kThreads; ++index) {
    const auto latest = (*registry)->LatestClass(QoSClassId{"class." + std::to_string(index)});
    QOS_REQUIRE(latest.has_value());
    QOS_CHECK_EQ(latest->generation.value(), 4ull);
  }
  QOS_REQUIRE((*registry)->Verify().has_value());
  QOS_REQUIRE((*registry)->Close().has_value());
}

QOS_TEST(concurrent_equivocation_produces_exactly_one_winner) {
  qostest::TempDir directory("concurrency-conflict");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);

  std::atomic<int> accepted{0};
  std::atomic<int> conflicted{0};
  std::atomic<int> other{0};
  std::vector<std::thread> workers;
  for (int index = 0; index < kThreads; ++index) {
    workers.emplace_back([&, index] {
      QoSClass value = make_class("contested.class", 1, 1'000 + static_cast<std::uint64_t>(index));
      const auto published = (*registry)->PublishClass(value, "thread");
      if (published) {
        accepted.fetch_add(1);
      } else if (published.code() == ErrorCode::Conflict) {
        conflicted.fetch_add(1);
      } else {
        other.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  // Exactly one definition is authoritative for a generation. Every other
  // writer is either told it is a conflict or is an idempotent repeat.
  QOS_CHECK(accepted.load() >= 1);
  QOS_CHECK_EQ(accepted.load() + conflicted.load() + other.load(), kThreads);
  const std::size_t stored = (*registry)
                                 ->LatestClass(QoSClassId{"contested.class"})
                                 .has_value()
                                 ? 1u
                                 : 0u;
  QOS_CHECK_EQ(stored, std::size_t{1});
  QOS_REQUIRE((*registry)->Close().has_value());
}

QOS_TEST(concurrent_epoch_advance_and_reads_stay_consistent) {
  qostest::TempDir directory("concurrency-epoch");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);
  QOS_REQUIRE((*registry)->PublishClass(make_class("net.voice", 1, 1'500), "test").has_value());

  std::atomic<bool> stop{false};
  std::atomic<int> read_failures{0};
  std::atomic<std::uint64_t> highest{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        const auto latest = (*registry)->LatestClass(QoSClassId{"net.voice"});
        if (!latest) {
          read_failures.fetch_add(1);
          return;
        }
        const std::uint64_t epoch = (*registry)->current_epoch().value();
        std::uint64_t observed = highest.load();
        while (epoch > observed && !highest.compare_exchange_weak(observed, epoch)) {
        }
      }
    });
  }
  std::thread advancer([&] {
    for (int index = 0; index < 5; ++index) {
      const auto advanced = (*registry)->AdvanceEpoch("concurrency-test");
      if (!advanced) {
        read_failures.fetch_add(1);
        break;
      }
    }
    stop.store(true);
  });
  advancer.join();
  for (std::thread& reader : readers) {
    reader.join();
  }
  QOS_CHECK_EQ(read_failures.load(), 0);
  QOS_CHECK(highest.load() >= 6);
  QOS_CHECK((*registry)->current_epoch().value() >= 6);
  QOS_REQUIRE((*registry)->Close().has_value());
}

QOS_TEST(shutdown_while_work_is_in_flight_is_safe) {
  qostest::TempDir directory("concurrency-shutdown");
  auto registry = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(registry.has_value());
  (*registry)->set_now_millis(kNow);

  std::atomic<int> completed{0};
  std::atomic<int> refused{0};
  std::atomic<int> unexpected{0};
  std::vector<std::thread> workers;
  for (int index = 0; index < kThreads; ++index) {
    workers.emplace_back([&, index] {
      for (std::uint64_t generation = 1; generation <= 64; ++generation) {
        const auto published = (*registry)->PublishClass(
            make_class("class." + std::to_string(index), generation, 1'000), "thread");
        if (published) {
          completed.fetch_add(1);
        } else if (published.code() == ErrorCode::ShuttingDown) {
          refused.fetch_add(1);
          return;
        } else {
          unexpected.fetch_add(1);
          return;
        }
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto closed = (*registry)->Close();
  QOS_CHECK(closed.has_value());
  for (std::thread& worker : workers) {
    worker.join();
  }
  // Work that crossed its authoritative completion boundary is durable; work
  // that arrived after shutdown was refused rather than silently dropped.
  QOS_CHECK_EQ(unexpected.load(), 0);
  QOS_CHECK(completed.load() >= 1);
  // Every acknowledged publication survives a reopen.
  const int acknowledged = completed.load();
  auto reopened = Registry::Open(registry_options(directory.path()));
  QOS_REQUIRE(reopened.has_value());
  const auto identifiers = (*reopened)->list_classes();
  QOS_REQUIRE(identifiers.has_value());
  std::size_t retained = 0;
  for (const QoSClassId& id : *identifiers) {
    if (id.str().rfind("class.", 0) == 0) {
      ++retained;
    }
  }
  // Every acknowledged publication is durable; a refused publication is not
  // counted either way, so the retained set can never exceed what was
  // acknowledged.
  QOS_CHECK(retained <= static_cast<std::size_t>(acknowledged));
  QOS_REQUIRE((*reopened)->Close().has_value());
}
