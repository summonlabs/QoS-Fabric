// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "harness.hpp"

#include <array>
#include <string>
#include <vector>

#include "qosfabric/qosfabric.hpp"

using namespace qosfabric;

namespace {

QoSClass make_class(const char* id, std::uint64_t generation) {
  QoSClass value;
  value.id = QoSClassId{id};
  value.generation = Generation{generation};
  value.min_rate_bps = 1'000'000;
  value.max_latency_us = 1500;
  value.isolation = IsolationLevel::QueueIsolated;
  value.treatment = TreatmentMode::RateLimited;
  return value;
}

}  // namespace

QOS_TEST(sha256_matches_published_vectors) {
  QOS_CHECK_EQ(sha256("").hex(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  QOS_CHECK_EQ(sha256("abc").hex(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  QOS_CHECK_EQ(
      sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // The 1,000,000 'a' vector, streamed in uneven chunks.
  Sha256 streaming;
  const std::string chunk(9973, 'a');
  std::size_t remaining = 1'000'000;
  while (remaining > 0) {
    const std::size_t take = remaining < chunk.size() ? remaining : chunk.size();
    streaming.update(chunk.data(), take);
    remaining -= take;
  }
  QOS_CHECK_EQ(streaming.finish().hex(),
               std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

QOS_TEST(crc32c_matches_published_vector) {
  const std::string check = "123456789";
  QOS_CHECK_EQ(crc32c(check), 0xE3069283u);
  QOS_CHECK_EQ(crc32c(""), 0u);
}

QOS_TEST(digest_round_trips_through_hex) {
  const Digest256 original = sha256("qos-fabric");
  Digest256 parsed;
  QOS_REQUIRE(Digest256::parse_hex(original.hex(), parsed));
  QOS_CHECK(parsed == original);
  Digest256 rejected;
  QOS_CHECK(!Digest256::parse_hex("not-hex", rejected));
  QOS_CHECK(!Digest256::parse_hex(std::string(63, 'a'), rejected));
  QOS_CHECK(!Digest256::parse_hex(std::string(65, 'a'), rejected));
  Digest256 uppercase;
  std::string upper = original.hex();
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  QOS_CHECK(Digest256::parse_hex(upper, uppercase));
  QOS_CHECK(uppercase == original);
}

QOS_TEST(checked_arithmetic_refuses_overflow) {
  std::uint64_t out = 0;
  QOS_CHECK(checked_add<std::uint64_t>(1, 2, out));
  QOS_CHECK_EQ(out, 3ull);
  const std::uint64_t ceiling = UINT64_MAX;
  const std::uint64_t one = 1;
  const std::uint64_t two = 2;
  QOS_CHECK(!checked_add<std::uint64_t>(ceiling, one, out));
  QOS_CHECK(!checked_mul<std::uint64_t>(ceiling, two, out));
  QOS_CHECK(checked_mul<std::uint64_t>(0, UINT64_MAX, out));
  QOS_CHECK_EQ(out, 0ull);
  QOS_CHECK(checked_ceil_div<std::uint64_t>(10, 3, out));
  QOS_CHECK_EQ(out, 4ull);
  QOS_CHECK(!checked_ceil_div<std::uint64_t>(1, 0, out));
  QOS_CHECK_EQ(saturating_add<std::uint64_t>(UINT64_MAX, 5), UINT64_MAX);
  QOS_CHECK_EQ(saturating_sub<std::uint64_t>(1, 5), 0ull);
}

QOS_TEST(relaxation_windows_are_exact) {
  std::uint64_t relaxed = 0;
  QOS_REQUIRE(apply_ppm_relaxation(1500, 100000, relaxed));
  QOS_CHECK_EQ(relaxed, 1650ull);
  QOS_REQUIRE(apply_ppm_relaxation(1500, 0, relaxed));
  QOS_CHECK_EQ(relaxed, 1500ull);
  QOS_CHECK(!apply_ppm_relaxation(1500, 1000001, relaxed));
  std::uint32_t ppm = 0;
  QOS_REQUIRE(relative_ppm(1500, 0, ppm));
  QOS_CHECK_EQ(ppm, 0u);
  QOS_REQUIRE(relative_ppm(1500, 1, ppm));
  QOS_CHECK_EQ(ppm, 667u);
  QOS_REQUIRE(relative_ppm(1500, 1500, ppm));
  QOS_CHECK_EQ(ppm, 1000000u);
  QOS_REQUIRE(relative_ppm(0, 1, ppm));
  QOS_CHECK_EQ(ppm, 1000000u);
  QOS_REQUIRE(relative_ppm(0, 0, ppm));
  QOS_CHECK_EQ(ppm, 0u);
}

QOS_TEST(identifier_alphabet_is_enforced) {
  QOS_CHECK(is_valid_identifier("voice.toll"));
  QOS_CHECK(is_valid_identifier("a/b-c_d:e"));
  QOS_CHECK(!is_valid_identifier(""));
  QOS_CHECK(!is_valid_identifier("has space"));
  QOS_CHECK(!is_valid_identifier("has\ttab"));
  QOS_CHECK(!is_valid_identifier(std::string(97, 'a')));
  QOS_CHECK(!is_valid_identifier("semi;colon"));
  QOS_CHECK(!is_valid_identifier("brace{}"));
  const auto parsed = parse_string_id<QoSClassIdTag>("ok");
  QOS_REQUIRE(parsed.has_value());
  QOS_CHECK_EQ(parsed->str(), std::string("ok"));
  QOS_CHECK(!parse_string_id<QoSClassIdTag>("bad id").has_value());
}

QOS_TEST(boot_identities_are_unique_and_nonzero) {
  BootId first = BootId::generate();
  BootId second = BootId::generate();
  QOS_CHECK(!first.is_zero());
  QOS_CHECK(!(first == second));
  QOS_CHECK(BootId{}.is_zero());
}

QOS_TEST(generations_advance_and_saturate) {
  Generation generation{1};
  QOS_REQUIRE(generation.try_increment());
  QOS_CHECK_EQ(generation.value(), 2ull);
  Generation ceiling{UINT64_MAX};
  QOS_CHECK(!ceiling.try_increment());
  QOS_CHECK_EQ(ceiling.value(), UINT64_MAX);
}

QOS_TEST(serialization_reader_never_over_reads) {
  ByteWriter writer(64);
  writer.u32(0xDEADBEEFu);
  writer.str("hello", 16);
  QOS_REQUIRE(writer.ok());
  ByteReader reader(writer.data());
  QOS_CHECK_EQ(reader.u32(), 0xDEADBEEFu);
  QOS_CHECK_EQ(reader.str(16), std::string("hello"));
  QOS_CHECK(reader.ok());
  QOS_CHECK(reader.at_end());
  QOS_CHECK_EQ(reader.u8(), 0);
  QOS_CHECK(reader.failed());

  // A declared length far beyond the buffer is refused, not partially read.
  ByteWriter hostile(16);
  hostile.u32(0xFFFFFFFFu);
  ByteReader hostile_reader(hostile.data());
  QOS_CHECK_EQ(hostile_reader.str(1024), std::string());
  QOS_CHECK(hostile_reader.failed());

  // A writer honours its budget and stops producing bytes past it.
  ByteWriter bounded(8);
  bounded.u64(1);
  bounded.u64(2);
  QOS_CHECK(bounded.failed());
  QOS_CHECK(bounded.size() <= 8);
}

QOS_TEST(model_validation_rejects_contradictions) {
  QoSClass value = make_class("voice.toll", 1);
  QOS_CHECK(validate(value).has_value());

  QoSClass contradictory = value;
  contradictory.min_rate_bps = 2'000'000;
  contradictory.max_rate_bps = 1'000'000;
  QOS_CHECK_EQ(validate(contradictory).code(), ErrorCode::Conflict);

  QoSClass no_generation = value;
  no_generation.generation = Generation{0};
  QOS_CHECK_EQ(validate(no_generation).code(), ErrorCode::InvalidArgument);

  QoSClass half_burst = value;
  half_burst.burst_bytes = 1024;
  QOS_CHECK_EQ(validate(half_burst).code(), ErrorCode::Conflict);

  QoSClass bad_id = value;
  bad_id.id = QoSClassId{"bad id"};
  QOS_CHECK_EQ(validate(bad_id).code(), ErrorCode::InvalidArgument);

  QoSClass zero_diversity = value;
  zero_diversity.min_disjoint_paths = 0;
  QOS_CHECK_EQ(validate(zero_diversity).code(), ErrorCode::InvalidArgument);

  QoSClass degrade_without_permission = value;
  degrade_without_permission.degrade_max_relaxation_ppm = 1000;
  QOS_CHECK_EQ(validate(degrade_without_permission).code(), ErrorCode::Conflict);

  QoSClass report_only_without_window = value;
  report_only_without_window.violation_policy = ViolationPolicy::ReportOnly;
  QOS_CHECK_EQ(validate(report_only_without_window).code(), ErrorCode::Conflict);

  QoSClass reservation_without_rate = value;
  reservation_without_rate.min_rate_bps.reset();
  reservation_without_rate.requires_reservation = true;
  QOS_CHECK_EQ(validate(reservation_without_rate).code(), ErrorCode::Conflict);

  QoSClass duplicate_predicate = value;
  duplicate_predicate.required_predicates = {CapabilityKey{"p"}, CapabilityKey{"p"}};
  QOS_CHECK_EQ(validate(duplicate_predicate).code(), ErrorCode::Duplicate);

  QoSClass oversized = value;
  oversized.max_latency_us = limits::kMaxLatencyUs + 1;
  QOS_CHECK_EQ(validate(oversized).code(), ErrorCode::TooLarge);

  Policy policy;
  policy.id = PolicyId{"strict"};
  policy.generation = Generation{1};
  QOS_CHECK(validate(policy).has_value());
  Policy policy_conflict = policy;
  policy_conflict.max_relaxation_ppm = 1000;
  QOS_CHECK_EQ(validate(policy_conflict).code(), ErrorCode::Conflict);
}

QOS_TEST(path_validation_rejects_duplicates_and_gaps) {
  Path path;
  path.id = PathId{"p1"};
  path.generation = Generation{1};
  PathComponent first;
  first.resource = ResourceId{"r1"};
  first.capability_generation = Generation{1};
  first.diversity_domain = DiversityDomain{"d1"};
  path.components.push_back(first);
  QOS_CHECK(validate(path).has_value());

  Path duplicate = path;
  duplicate.components.push_back(first);
  QOS_CHECK_EQ(validate(duplicate).code(), ErrorCode::Duplicate);

  Path no_generation = path;
  no_generation.components[0].capability_generation = Generation{0};
  QOS_CHECK_EQ(validate(no_generation).code(), ErrorCode::InvalidArgument);

  Path empty = path;
  empty.components.clear();
  QOS_CHECK_EQ(validate(empty).code(), ErrorCode::InvalidArgument);

  Path too_many = path;
  too_many.components.clear();
  for (std::size_t i = 0; i <= limits::kMaxPathComponents; ++i) {
    PathComponent component = first;
    component.resource = ResourceId{"r" + std::to_string(i)};
    too_many.components.push_back(component);
  }
  QOS_CHECK_EQ(validate(too_many).code(), ErrorCode::TooLarge);
}

QOS_TEST(canonical_records_round_trip_losslessly) {
  QoSClass klass = make_class("voice.toll", 3);
  klass.required_predicates = {CapabilityKey{"tsn.tas"}, CapabilityKey{"detnet.aggregate"}};
  klass.allow_degrade = true;
  klass.degrade_max_relaxation_ppm = 250000;
  klass.violation_policy = ViolationPolicy::DegradeThenReject;
  klass.published_epoch = FabricEpoch{7};
  klass.provenance = Provenance{"unit-test", FabricEpoch{7}, Generation{3}, Sequence{11}};
  klass.digest = compute_digest(klass);
  ByteWriter writer;
  canonical_encode(klass, writer);
  QOS_REQUIRE(writer.ok());
  ByteReader reader(writer.data());
  QoSClass decoded;
  QOS_REQUIRE(canonical_decode(reader, decoded).has_value());
  QOS_CHECK(reader.at_end());
  QOS_CHECK(decoded.id == klass.id);
  QOS_CHECK(decoded.generation == klass.generation);
  QOS_CHECK_EQ(decoded.degrade_max_relaxation_ppm, klass.degrade_max_relaxation_ppm);
  QOS_CHECK_EQ(decoded.required_predicates.size(), klass.required_predicates.size());
  QOS_CHECK(decoded.digest == klass.digest);
  QOS_CHECK_EQ(decoded.provenance.origin(), klass.provenance.origin());
  QOS_CHECK(decoded.published_epoch == klass.published_epoch);

  // Truncation at every prefix must be refused, never partially accepted.
  const std::vector<std::uint8_t>& bytes = writer.data();
  for (std::size_t cut = 0; cut < bytes.size(); cut += 7) {
    ByteReader partial(bytes.data(), cut);
    QoSClass discarded;
    QOS_CHECK(!canonical_decode(partial, discarded).has_value());
  }
}

QOS_TEST(content_digest_ignores_assertion_metadata) {
  QoSClass first = make_class("voice.toll", 1);
  first.published_epoch = FabricEpoch{1};
  first.provenance = Provenance{"a", FabricEpoch{1}, Generation{1}, Sequence{1}};
  QoSClass second = first;
  second.published_epoch = FabricEpoch{99};
  second.provenance = Provenance{"b", FabricEpoch{99}, Generation{1}, Sequence{42}};
  QOS_CHECK(compute_digest(first) == compute_digest(second));
  second.max_latency_us = 1501;
  QOS_CHECK(!(compute_digest(first) == compute_digest(second)));
}

QOS_TEST(class_content_digest_is_order_independent_for_predicates) {
  QoSClass first = make_class("voice.toll", 1);
  first.required_predicates = {CapabilityKey{"zzz.last"}, CapabilityKey{"aaa.first"}};
  QoSClass second = first;
  second.required_predicates = {CapabilityKey{"aaa.first"}, CapabilityKey{"zzz.last"}};
  // Predicates are a set: listing them in a different order is the same class,
  // so it must carry the same authority and the same digest.
  QOS_CHECK(compute_digest(first) == compute_digest(second));

  ByteWriter first_writer;
  canonical_encode(first, first_writer);
  ByteWriter second_writer;
  canonical_encode(second, second_writer);
  QOS_REQUIRE(first_writer.ok());
  QOS_REQUIRE(second_writer.ok());
  QOS_CHECK(first_writer.data() == second_writer.data());

  first.required_predicates.push_back(CapabilityKey{"mmm.middle"});
  QOS_CHECK(!(compute_digest(first) == compute_digest(second)));
}

QOS_TEST(decision_record_round_trips) {
  ContractDecision decision;
  decision.contract = QoSContractId{"c1"};
  decision.contract_generation = Generation{2};
  decision.outcome = Outcome::SupportedDegraded;
  decision.class_id = QoSClassId{"voice.toll"};
  decision.class_generation = Generation{3};
  decision.class_digest = sha256("class");
  decision.subject = SubjectId{"svc"};
  decision.subject_generation = Generation{4};
  decision.path = PathId{"p1"};
  decision.path_generation = Generation{5};
  decision.path_digest = sha256("path");
  decision.policy = PolicyId{"strict"};
  decision.policy_generation = Generation{6};
  decision.policy_digest = sha256("policy");
  decision.priority = PriorityClassId{"gold"};
  decision.priority_generation = Generation{7};
  decision.fabric_epoch = FabricEpoch{8};
  decision.observed_epoch = FabricEpoch{8};
  decision.decided_at_ms = 123456;
  decision.unknown_components = 0;
  decision.violated_obligations = 0;
  decision.primary_reason = "latency relaxed";
  decision.request_digest = sha256("request");
  decision.binding_obligation = ObligationKind::Latency;
  decision.binding_resource = ResourceId{"r1"};

  ObligationAssessment assessment;
  assessment.kind = ObligationKind::Latency;
  assessment.id = ObligationId{"latency.max_us"};
  assessment.direction = CompareDirection::AtMost;
  assessment.aggregate = AggregateKind::Additive;
  assessment.required_value = 1500;
  assessment.effective_limit = 1650;
  assessment.composed_value = 1600;
  assessment.relaxation_applied_ppm = 66666;
  assessment.status = ObligationStatus::Degraded;
  assessment.binding_resource = ResourceId{"r1"};
  assessment.weakest_resource = ResourceId{"r2"};
  assessment.headroom_ppm = 0;
  assessment.detail = "inside window";
  decision.obligations.push_back(assessment);

  ResourceAssessment resource;
  resource.resource = ResourceId{"r1"};
  resource.diversity_domain = DiversityDomain{"d1"};
  resource.role = PathRole::Primary;
  resource.bound_capability_generation = Generation{9};
  resource.observed_capability_generation = Generation{9};
  resource.observed_incarnation = PublisherIncarnation{4};
  resource.status = ComponentStatus::Degraded;
  resource.self_declared_degraded = true;
  resource.reason = "declared degraded";
  decision.resources.push_back(resource);

  DegradationReason degradation;
  degradation.kind = ObligationKind::Latency;
  degradation.id = ObligationId{"latency.max_us"};
  degradation.resource = ResourceId{"r1"};
  degradation.relaxation_ppm = 66666;
  degradation.detail = "inside window";
  decision.degradations.push_back(degradation);

  decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::Class, "voice.toll",
                                              Generation{3}, decision.class_digest.hex(),
                                              FabricEpoch{8}, "unit-test"});

  decision.decision_digest = compute_decision_digest(decision);
  ByteWriter writer;
  canonical_encode(decision, writer);
  QOS_REQUIRE(writer.ok());
  ByteReader reader(writer.data());
  ContractDecision decoded;
  QOS_REQUIRE(canonical_decode(reader, decoded).has_value());
  QOS_CHECK(reader.at_end());
  QOS_CHECK_EQ(decoded.obligations.size(), decision.obligations.size());
  QOS_CHECK_EQ(decoded.resources.size(), decision.resources.size());
  QOS_CHECK_EQ(decoded.degradations.size(), decision.degradations.size());
  QOS_CHECK_EQ(decoded.authority.size(), decision.authority.size());
  QOS_CHECK(decoded.outcome == decision.outcome);
  QOS_CHECK(decoded.binding_obligation.has_value());
  QOS_CHECK(*decoded.binding_obligation == ObligationKind::Latency);
  QOS_CHECK(compute_decision_digest(decoded) == decision.decision_digest);
}

QOS_TEST(explanation_is_bounded_and_informative) {
  ContractDecision decision;
  decision.contract = QoSContractId{"c1"};
  decision.contract_generation = Generation{1};
  decision.outcome = Outcome::Unsupported;
  decision.class_id = QoSClassId{"voice.toll"};
  decision.class_generation = Generation{1};
  decision.path = PathId{"p1"};
  decision.path_generation = Generation{1};
  decision.policy = PolicyId{"strict"};
  decision.policy_generation = Generation{1};
  decision.priority = PriorityClassId{"gold"};
  decision.priority_generation = Generation{1};
  decision.primary_reason = "rate.min_bps is the binding obligation";
  for (std::size_t i = 0; i < 200; ++i) {
    ObligationAssessment item;
    item.kind = ObligationKind::Predicate;
    item.id = ObligationId{"capability.predicate.p" + std::to_string(i)};
    item.status = ObligationStatus::Satisfied;
    decision.obligations.push_back(item);
  }
  decision.decision_digest = compute_decision_digest(decision);

  ExplainOptions options;
  options.max_bytes = 4096;
  const std::string text = explain(decision, options);
  QOS_CHECK(text.size() <= 4096);
  QOS_CHECK(text.find("outcome: UNSUPPORTED") != std::string::npos);
  QOS_CHECK(text.find("truncated") != std::string::npos);

  ExplainOptions generous;
  const std::string full = explain(decision, generous);
  QOS_CHECK(full.size() <= limits::kMaxExplainBytes);
  QOS_CHECK(full.find("outcome: UNSUPPORTED") != std::string::npos);
  QOS_CHECK(full.find("decision-digest:") != std::string::npos);
}

QOS_TEST(outcome_precedence_is_total) {
  QOS_CHECK(dominant(Outcome::Supported, Outcome::Unknown) == Outcome::Unknown);
  QOS_CHECK(dominant(Outcome::Unknown, Outcome::Unsupported) == Outcome::Unsupported);
  QOS_CHECK(dominant(Outcome::Unsupported, Outcome::Stale) == Outcome::Stale);
  QOS_CHECK(dominant(Outcome::Stale, Outcome::Conflict) == Outcome::Conflict);
  QOS_CHECK(dominant(Outcome::Conflict, Outcome::Rejected) == Outcome::Rejected);
  QOS_CHECK(dominant(Outcome::SupportedDegraded, Outcome::Supported) ==
            Outcome::SupportedDegraded);
  QOS_CHECK(is_positive(Outcome::Supported));
  QOS_CHECK(is_positive(Outcome::SupportedDegraded));
  QOS_CHECK(!is_positive(Outcome::Unknown));
  Outcome parsed = Outcome::Rejected;
  QOS_CHECK(parse_outcome("SUPPORTED_DEGRADED", parsed));
  QOS_CHECK(parsed == Outcome::SupportedDegraded);
  QOS_CHECK(!parse_outcome("MAYBE", parsed));
}
