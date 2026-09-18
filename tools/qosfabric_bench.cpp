// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// qosfabric_bench: measures *completed* work, not enqueue latency.
//
// Every number here is SYNTHETIC: the workload is generated in memory, and no
// physical network, switch, NIC or fabric hardware is involved. Nothing in
// this tool models multi-node behaviour and nothing it prints may be quoted as
// a physical-network measurement. What it does measure honestly is the count
// of finished, authoritative operations per second on this host, including the
// durability barrier for every durable mutation.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "qosfabric/qosfabric.hpp"

using namespace qosfabric;

namespace {

constexpr std::uint64_t kNow = 1'800'000'000'000ull;

double seconds_since(const std::chrono::steady_clock::time_point& start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

QoSClass make_class(const std::string& id, std::uint64_t generation) {
  QoSClass value;
  value.id = QoSClassId{id};
  value.generation = Generation{generation};
  value.min_rate_bps = 1'000'000;
  value.max_latency_us = 1'500;
  value.isolation = IsolationLevel::QueueIsolated;
  value.treatment = TreatmentMode::RateLimited;
  return value;
}

EvaluationInputs make_inputs(std::size_t components) {
  EvaluationInputs inputs;
  inputs.klass.id = QoSClassId{"bench.class"};
  inputs.klass.generation = Generation{1};
  inputs.klass.min_rate_bps = 1'000'000;
  inputs.klass.max_latency_us = 5'000;
  inputs.klass.max_jitter_us = 500;
  inputs.klass.max_loss_ppm = 1'000;
  inputs.klass.isolation = IsolationLevel::QueueIsolated;
  inputs.klass.treatment = TreatmentMode::RateLimited;
  inputs.klass.min_disjoint_paths = 2;
  inputs.klass.digest = compute_digest(inputs.klass);

  inputs.policy.id = PolicyId{"bench.policy"};
  inputs.policy.generation = Generation{1};
  inputs.policy.capability_max_age_ms = 600'000;
  inputs.policy.digest = compute_digest(inputs.policy);

  inputs.path.id = PathId{"bench.path"};
  inputs.path.generation = Generation{1};
  inputs.path.fixed_latency_us = 50;
  inputs.path.epoch = FabricEpoch{1};
  for (std::size_t i = 0; i < components; ++i) {
    PathComponent component;
    component.resource = ResourceId{"bench.res." + std::to_string(i)};
    component.capability_generation = Generation{1};
    component.diversity_domain = DiversityDomain{"bench.domain." + std::to_string(i % 4)};
    component.role = PathRole::Member;
    inputs.path.components.push_back(component);

    ResourceCapability capability;
    capability.resource = component.resource;
    capability.generation = Generation{1};
    capability.epoch = FabricEpoch{1};
    capability.publisher = PublisherId{"bench.agent"};
    capability.incarnation = PublisherIncarnation{1};
    capability.published_at_ms = kNow;
    capability.expires_at_ms = kNow + 600'000;
    capability.state = CapabilityState::Available;
    capability.supported_rate_bps = 10'000'000;
    capability.latency_bound_us = 400;
    capability.jitter_bound_us = 40;
    capability.loss_ppm = 10;
    capability.max_isolation = IsolationLevel::QueueIsolated;
    capability.max_treatment = TreatmentMode::RateLimited;
    capability.max_priority_rank = 4;
    capability.diversity_domain = component.diversity_domain;
    capability.digest = compute_digest(capability);
    inputs.capabilities.push_back(capability);
    inputs.reservations.push_back(std::nullopt);
  }
  inputs.path.digest = compute_digest(inputs.path);

  inputs.request.contract = QoSContractId{"bench.contract"};
  inputs.request.contract_generation = Generation{1};
  inputs.request.subject = SubjectId{"bench.subject"};
  inputs.request.subject_generation = Generation{1};
  inputs.request.class_id = inputs.klass.id;
  inputs.request.class_generation = inputs.klass.generation;
  inputs.request.path = inputs.path.id;
  inputs.request.path_generation = inputs.path.generation;
  inputs.request.policy = inputs.policy.id;
  inputs.request.policy_generation = inputs.policy.generation;
  inputs.request.priority = PriorityClassId{"bench.gold"};
  inputs.request.priority_generation = Generation{1};
  inputs.request.observed_epoch = FabricEpoch{1};
  inputs.request.request_ms = kNow;
  inputs.current_epoch = FabricEpoch{1};
  inputs.now_ms = kNow;
  inputs.evaluator = "bench";
  return inputs;
}

void report(const char* label, std::uint64_t completed, double seconds) {
  const double per_second = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
  std::printf("SYNTHETIC %-42s completed=%llu seconds=%.4f per_second=%.1f\n", label,
              static_cast<unsigned long long>(completed), seconds, per_second);
}

}  // namespace

int main(int argc, char** argv) {
  std::string store_directory;
  std::uint64_t iterations = 20'000;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--store" && i + 1 < argc) {
      store_directory = argv[++i];
    } else if (flag == "--iterations" && i + 1 < argc) {
      iterations = std::strtoull(argv[++i], nullptr, 10);
    } else {
      std::fprintf(stderr, "usage: qosfabric_bench --store DIR [--iterations N]\n");
      return 1;
    }
  }
  if (store_directory.empty()) {
    std::fprintf(stderr, "usage: qosfabric_bench --store DIR [--iterations N]\n");
    return 1;
  }
  std::printf("qosfabric %s (%s) benchmark\n", version_string().data(),
              build_configuration().data());
  std::printf("All figures are SYNTHETIC in-memory workloads on one host.\n");
  std::printf("No physical network, switch, NIC or fabric hardware is exercised.\n\n");

  // --- Pure evaluation, single thread, completed decisions ---
  {
    const EvaluationInputs inputs = make_inputs(4);
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t completed = 0;
    Digest256 last{};
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto decision = evaluate(inputs);
      if (!decision || !is_positive(decision->outcome)) {
        std::fprintf(stderr, "benchmark evaluation did not produce a positive verdict\n");
        return 1;
      }
      last = decision->decision_digest;
      ++completed;
    }
    report("evaluate:4-components:1-thread", completed, seconds_since(start));
    std::printf("           decision digest: %s\n", last.hex().c_str());
  }

  // --- Pure evaluation, eight threads ---
  {
    const EvaluationInputs inputs = make_inputs(4);
    const unsigned hardware = std::thread::hardware_concurrency();
    const unsigned threads = hardware == 0 ? 4u : (hardware > 8u ? 8u : hardware);
    std::vector<std::thread> workers;
    std::vector<std::uint64_t> per_thread(threads, 0);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned t = 0; t < threads; ++t) {
      workers.emplace_back([&inputs, &per_thread, t, iterations] {
        std::uint64_t completed = 0;
        for (std::uint64_t i = 0; i < iterations; ++i) {
          const auto decision = evaluate(inputs);
          if (decision && is_positive(decision->outcome)) {
            ++completed;
          }
        }
        per_thread[t] = completed;
      });
    }
    for (std::thread& worker : workers) {
      worker.join();
    }
    std::uint64_t completed = 0;
    for (const std::uint64_t value : per_thread) {
      completed += value;
    }
    char label[96];
    std::snprintf(label, sizeof(label), "evaluate:4-components:%u-threads", threads);
    report(label, completed, seconds_since(start));
  }

  // --- Durable publication: every completion crosses an fsync barrier ---
  {
    auto registry = Registry::Open([&] {
      RegistryOptions options;
      options.directory = store_directory;
      options.create_if_missing = true;
      return options;
    }());
    if (!registry) {
      std::fprintf(stderr, "registry open failed: %s\n", registry.error().message().c_str());
      return 1;
    }
    (*registry)->set_now_millis(kNow);
    // A bounded identifier population with advancing generations: this also
    // exercises the retention window and the automatic compaction that keeps
    // durable growth bounded, which a stream of fresh identifiers never would.
    constexpr std::uint64_t kIdentifiers = 16;
    const std::uint64_t publications = iterations < 2'000 ? iterations : 2'000;
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t completed = 0;
    for (std::uint64_t index = 0; index < publications; ++index) {
      const std::uint64_t slot = index % kIdentifiers;
      const std::uint64_t generation = (index / kIdentifiers) + 1;
      QoSClass value = make_class("bench.durable." + std::to_string(slot), generation);
      value.max_latency_us = 1'000 + generation;
      const auto published = (*registry)->PublishClass(value, "bench");
      if (!published) {
        std::fprintf(stderr, "publication failed: %s\n", published.error().message().c_str());
        return 1;
      }
      ++completed;
    }
    report("durable-publish:distinct-classes", completed, seconds_since(start));
    if (!(*registry)->Close()) {
      std::fprintf(stderr, "close failed\n");
      return 1;
    }
  }

  // --- Recovery: reopen the durable store and replay it ---
  {
    const auto start = std::chrono::steady_clock::now();
    auto registry = Registry::Open([&] {
      RegistryOptions options;
      options.directory = store_directory;
      options.create_if_missing = true;
      return options;
    }());
    if (!registry) {
      std::fprintf(stderr, "reopen failed: %s\n", registry.error().message().c_str());
      return 1;
    }
    const double seconds = seconds_since(start);
    std::printf("SYNTHETIC %-42s records=%llu seconds=%.4f\n", "recovery:reopen-and-replay",
                static_cast<unsigned long long>((*registry)->recovery().replayed_records),
                seconds);
    const auto verify_start = std::chrono::steady_clock::now();
    const auto verified = (*registry)->Verify();
    if (!verified) {
      std::fprintf(stderr, "verify failed: %s\n", verified.error().message().c_str());
      return 1;
    }
    std::printf("SYNTHETIC %-42s seconds=%.4f\n", "verify:manifest+snapshot+log",
                seconds_since(verify_start));
    if (!(*registry)->Close()) {
      std::fprintf(stderr, "close failed\n");
      return 1;
    }
  }

  std::printf("\nbenchmark complete\n");
  return 0;
}
