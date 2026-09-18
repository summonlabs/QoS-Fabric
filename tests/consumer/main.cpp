// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Downstream consumer smoke test: link the installed QoS Fabric package, open
// a durable registry, declare a service class, a policy, a path and resource
// capabilities, evaluate a contract end to end and print the authoritative
// verdict. Exits non-zero if the package does not behave as documented.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "qosfabric/qosfabric.hpp"

using namespace qosfabric;

int main(int argc, char** argv) {
  std::error_code ec;
  std::filesystem::path directory;
  if (argc > 1) {
    directory = std::filesystem::path(argv[1]);
  } else {
    directory = std::filesystem::temp_directory_path(ec) / "qosfabric-consumer-store";
  }
  std::filesystem::remove_all(directory, ec);

  RegistryOptions options;
  options.directory = directory.string();
  options.create_if_missing = true;
  auto registry = Registry::Open(options);
  if (!registry) {
    std::fprintf(stderr, "open failed: %s\n", registry.error().message().c_str());
    return 1;
  }
  const std::uint64_t now = system_now_millis();
  (*registry)->set_now_millis(now);

  Policy policy;
  policy.id = PolicyId{"consumer.policy"};
  policy.generation = Generation{1};
  policy.capability_max_age_ms = 600'000;
  if (!(*registry)->PublishPolicy(policy, "consumer")) {
    std::fprintf(stderr, "policy publish failed\n");
    return 1;
  }

  QoSClass klass;
  klass.id = QoSClassId{"consumer.voice"};
  klass.generation = Generation{1};
  klass.min_rate_bps = 1'000'000;
  klass.max_latency_us = 1'000;
  klass.isolation = IsolationLevel::QueueIsolated;
  klass.treatment = TreatmentMode::RateLimited;
  if (!(*registry)->PublishClass(klass, "consumer")) {
    std::fprintf(stderr, "class publish failed\n");
    return 1;
  }

  Path path;
  path.id = PathId{"consumer.path"};
  path.generation = Generation{1};
  PathComponent component;
  component.resource = ResourceId{"consumer.nic.0"};
  component.capability_generation = Generation{1};
  component.diversity_domain = DiversityDomain{"consumer.domain.0"};
  component.role = PathRole::Primary;
  path.components.push_back(component);
  if (!(*registry)->PublishPath(path, "consumer")) {
    std::fprintf(stderr, "path publish failed\n");
    return 1;
  }

  const auto lease = (*registry)->RegisterPublisher(PublisherId{"consumer.agent"},
                                                    PublisherIncarnation{1}, BootId::generate(),
                                                    600'000, "consumer");
  if (!lease) {
    std::fprintf(stderr, "lease failed: %s\n", lease.error().message().c_str());
    return 1;
  }

  ResourceCapability capability;
  capability.resource = ResourceId{"consumer.nic.0"};
  capability.generation = Generation{1};
  capability.epoch = (*registry)->current_epoch();
  capability.publisher = PublisherId{"consumer.agent"};
  capability.incarnation = PublisherIncarnation{1};
  capability.published_at_ms = now;
  capability.expires_at_ms = now + 600'000;
  capability.state = CapabilityState::Available;
  capability.supported_rate_bps = 4'000'000;
  capability.latency_bound_us = 700;
  capability.max_isolation = IsolationLevel::QueueIsolated;
  capability.max_treatment = TreatmentMode::RateLimited;
  capability.diversity_domain = DiversityDomain{"consumer.domain.0"};
  if (!(*registry)->PublishCapability(capability)) {
    std::fprintf(stderr, "capability publish failed\n");
    return 1;
  }

  ContractRequest request;
  request.contract = QoSContractId{"consumer.contract"};
  request.contract_generation = Generation{1};
  request.subject = SubjectId{"consumer.workload"};
  request.subject_generation = Generation{1};
  request.class_id = klass.id;
  request.path = path.id;
  request.policy = policy.id;
  request.priority = PriorityClassId{"consumer.gold"};
  request.priority_generation = Generation{1};
  request.observed_epoch = (*registry)->current_epoch();

  const auto decision = (*registry)->Evaluate(request);
  if (!decision) {
    std::fprintf(stderr, "evaluation failed: %s\n", decision.error().message().c_str());
    return 1;
  }
  std::printf("outcome=%s reason=%s\n", to_string(decision->outcome),
              decision->primary_reason.c_str());
  std::printf("version=%s store=%s\n", version_string().data(),
              (*registry)->store_id().c_str());
  if (!(*registry)->Close()) {
    std::fprintf(stderr, "close failed\n");
    return 1;
  }
  if (decision->outcome != Outcome::Supported) {
    std::fprintf(stderr, "expected SUPPORTED, got %s\n", to_string(decision->outcome));
    return 2;
  }
  std::printf("consumer OK\n");
  return 0;
}
