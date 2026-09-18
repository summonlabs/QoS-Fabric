// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/decision.hpp"

#include <algorithm>

#include "qosfabric/checked.hpp"
#include "qosfabric/serialize.hpp"

namespace qosfabric {
namespace {

constexpr std::uint16_t kTagDecision = 0x0201;
constexpr std::uint16_t kSchemaVersion = 1;

void encode_gen(ByteWriter& writer, Generation value) { writer.u64(value.value()); }
void encode_epoch(ByteWriter& writer, FabricEpoch value) { writer.u64(value.value()); }

void encode_digest(ByteWriter& writer, const Digest256& value) {
  writer.raw(value.bytes().data(), Digest256::kBytes);
}

void encode_optional_u64(ByteWriter& writer, const std::optional<std::uint64_t>& value) {
  writer.presence(value.has_value());
  if (value.has_value()) {
    writer.u64(*value);
  }
}

void encode_optional_gen(ByteWriter& writer, const std::optional<Generation>& value) {
  writer.presence(value.has_value());
  if (value.has_value()) {
    encode_gen(writer, *value);
  }
}

void encode_optional_resource(ByteWriter& writer, const std::optional<ResourceId>& value) {
  writer.presence(value.has_value());
  if (value.has_value()) {
    writer.str(value->view(), limits::kMaxIdentifierLen);
  }
}

void encode_optional_incarnation(ByteWriter& writer,
                                 const std::optional<PublisherIncarnation>& value) {
  writer.presence(value.has_value());
  if (value.has_value()) {
    writer.u64(value->value());
  }
}

// Appends text while respecting a hard byte budget. Truncation is explicit:
// the renderer marks the point at which output was clipped.
class BoundedText {
 public:
  explicit BoundedText(std::size_t budget) : budget_(budget) { out_.reserve(budget < 4096 ? budget : 4096); }

  void line(std::string_view text) {
    if (clipped_) {
      return;
    }
    if (out_.size() + text.size() + 1 > budget_) {
      clipped_ = true;
      out_ += "\n[explanation truncated at ";
      out_ += std::to_string(budget_);
      out_ += " bytes]\n";
      return;
    }
    out_ += text;
    out_ += '\n';
  }

  [[nodiscard]] bool clipped() const noexcept { return clipped_; }
  [[nodiscard]] std::string take() { return std::move(out_); }

 private:
  std::string out_{};
  std::size_t budget_;
  bool clipped_{false};
};

std::string u64_text(std::uint64_t value) { return std::to_string(value); }

std::string obligation_line(const ObligationAssessment& assessment, bool include_timings) {
  std::string line = "  ";
  line += assessment.id.view();
  while (line.size() < 24) {
    line += ' ';
  }
  line += (assessment.direction == CompareDirection::AtLeast) ? "req>=" : "req<=";
  line += u64_text(assessment.required_value);
  line += " composed=";
  if (assessment.composed_value.has_value()) {
    line += u64_text(*assessment.composed_value);
  } else {
    line += "UNKNOWN";
  }
  line += " status=";
  line += to_string(assessment.status);
  if (assessment.relaxation_applied_ppm != 0) {
    line += " relax=";
    line += u64_text(assessment.relaxation_applied_ppm);
    line += "ppm";
    if (assessment.relaxation_reported_only) {
      line += "(reported)";
    }
    line += " limit=";
    line += u64_text(assessment.effective_limit);
  }
  if (assessment.binding_resource.has_value()) {
    line += " binding=";
    line += assessment.binding_resource->view();
  }
  if (include_timings) {
    line += " headroom=";
    line += u64_text(assessment.headroom_ppm);
    line += "ppm aggregate=";
    line += to_string(assessment.aggregate);
  }
  if (!assessment.detail.empty()) {
    line += " (";
    line += assessment.detail;
    line += ")";
  }
  return line;
}

}  // namespace

const char* to_string(Outcome value) noexcept {
  switch (value) {
    case Outcome::Supported: return "SUPPORTED";
    case Outcome::SupportedDegraded: return "SUPPORTED_DEGRADED";
    case Outcome::Unknown: return "UNKNOWN";
    case Outcome::Unsupported: return "UNSUPPORTED";
    case Outcome::Stale: return "STALE";
    case Outcome::Conflict: return "CONFLICT";
    case Outcome::Rejected: return "REJECTED";
  }
  return "INVALID";
}

bool parse_outcome(std::string_view text, Outcome& out) noexcept {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(Outcome::Rejected); ++raw) {
    const auto candidate = static_cast<Outcome>(raw);
    if (text == to_string(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

const char* to_string(ObligationStatus value) noexcept {
  switch (value) {
    case ObligationStatus::Satisfied: return "satisfied";
    case ObligationStatus::Degraded: return "degraded";
    case ObligationStatus::Violated: return "violated";
    case ObligationStatus::Unknown: return "unknown";
    case ObligationStatus::Unavailable: return "unavailable";
    case ObligationStatus::Stale: return "stale";
    case ObligationStatus::NotApplicable: return "not-applicable";
  }
  return "invalid";
}

const char* to_string(ComponentStatus value) noexcept {
  switch (value) {
    case ComponentStatus::Satisfied: return "satisfied";
    case ComponentStatus::Degraded: return "degraded";
    case ComponentStatus::Unknown: return "unknown";
    case ComponentStatus::Unavailable: return "unavailable";
    case ComponentStatus::Violated: return "violated";
    case ComponentStatus::Stale: return "stale";
  }
  return "invalid";
}

const char* to_string(AuthorityEntry::Dimension value) noexcept {
  switch (value) {
    case AuthorityEntry::Dimension::Class: return "class";
    case AuthorityEntry::Dimension::Subject: return "subject";
    case AuthorityEntry::Dimension::Path: return "path";
    case AuthorityEntry::Dimension::Policy: return "policy";
    case AuthorityEntry::Dimension::Priority: return "priority";
    case AuthorityEntry::Dimension::Capability: return "capability";
    case AuthorityEntry::Dimension::Reservation: return "reservation";
    case AuthorityEntry::Dimension::FabricEpoch: return "fabric-epoch";
    case AuthorityEntry::Dimension::Count: break;
  }
  return "invalid";
}

const char* to_string(ConflictMarker::Scope value) noexcept {
  switch (value) {
    case ConflictMarker::Scope::Class: return "class";
    case ConflictMarker::Scope::Policy: return "policy";
    case ConflictMarker::Scope::Path: return "path";
    case ConflictMarker::Scope::Capability: return "capability";
    case ConflictMarker::Scope::Reservation: return "reservation";
    case ConflictMarker::Scope::Count: break;
  }
  return "invalid";
}

const ObligationAssessment* ContractDecision::find(ObligationKind kind) const noexcept {
  for (const ObligationAssessment& assessment : obligations) {
    if (assessment.kind == kind) {
      return &assessment;
    }
  }
  return nullptr;
}

void canonical_encode(const ContractDecision& value, ByteWriter& writer) {
  writer.u16(kTagDecision);
  writer.u16(kSchemaVersion);
  writer.str(value.contract.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.contract_generation);
  writer.u8(static_cast<std::uint8_t>(value.outcome));

  writer.str(value.class_id.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.class_generation);
  encode_digest(writer, value.class_digest);

  writer.str(value.subject.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.subject_generation);

  writer.str(value.path.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.path_generation);
  encode_digest(writer, value.path_digest);

  writer.str(value.policy.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.policy_generation);
  encode_digest(writer, value.policy_digest);

  writer.str(value.priority.view(), limits::kMaxIdentifierLen);
  encode_gen(writer, value.priority_generation);

  encode_epoch(writer, value.fabric_epoch);
  encode_epoch(writer, value.observed_epoch);
  writer.u64(value.decided_at_ms);

  writer.counted(value.obligations.size(), [&](ByteWriter& w) {
    for (const ObligationAssessment& item : value.obligations) {
      w.u8(static_cast<std::uint8_t>(item.kind));
      w.str(item.id.view(), limits::kMaxIdentifierLen);
      w.presence(item.binding);
      w.presence(item.relaxable);
      w.u8(static_cast<std::uint8_t>(item.direction));
      w.u8(static_cast<std::uint8_t>(item.aggregate));
      w.presence(item.path_level);
      w.u64(item.required_value);
      w.u64(item.effective_limit);
      encode_optional_u64(w, item.composed_value);
      w.u32(item.relaxation_applied_ppm);
      w.presence(item.relaxation_reported_only);
      w.u8(static_cast<std::uint8_t>(item.status));
      encode_optional_resource(w, item.binding_resource);
      encode_optional_resource(w, item.weakest_resource);
      w.u32(item.headroom_ppm);
      w.str(item.detail, limits::kMaxDetailLen);
    }
  });

  writer.counted(value.resources.size(), [&](ByteWriter& w) {
    for (const ResourceAssessment& item : value.resources) {
      w.str(item.resource.view(), limits::kMaxIdentifierLen);
      w.str(item.diversity_domain.view(), limits::kMaxIdentifierLen);
      w.u8(static_cast<std::uint8_t>(item.role));
      encode_gen(w, item.bound_capability_generation);
      encode_optional_gen(w, item.observed_capability_generation);
      encode_optional_incarnation(w, item.observed_incarnation);
      w.u8(static_cast<std::uint8_t>(item.status));
      w.presence(item.self_declared_degraded);
      w.str(item.reason, limits::kMaxReasonLen);
    }
  });

  writer.counted(value.degradations.size(), [&](ByteWriter& w) {
    for (const DegradationReason& item : value.degradations) {
      w.u8(static_cast<std::uint8_t>(item.kind));
      w.str(item.id.view(), limits::kMaxIdentifierLen);
      encode_optional_resource(w, item.resource);
      w.u32(item.relaxation_ppm);
      w.presence(item.reported_only);
      w.str(item.detail, limits::kMaxReasonLen);
    }
  });

  writer.counted(value.authority.size(), [&](ByteWriter& w) {
    for (const AuthorityEntry& item : value.authority) {
      w.u8(static_cast<std::uint8_t>(item.dimension));
      w.str(item.id, limits::kMaxIdentifierLen);
      encode_gen(w, item.generation);
      w.str(item.digest_hex, 64);
      encode_epoch(w, item.epoch);
      w.str(item.source, limits::kMaxOriginLen);
    }
  });

  writer.presence(value.binding_obligation.has_value());
  if (value.binding_obligation.has_value()) {
    writer.u8(static_cast<std::uint8_t>(*value.binding_obligation));
  }
  encode_optional_resource(writer, value.binding_resource);
  writer.u32(value.unknown_components);
  writer.u32(value.violated_obligations);
  writer.u32(value.unsatisfied_diversity_domains);
  writer.str(value.primary_reason, limits::kMaxReasonLen);
  encode_digest(writer, value.request_digest);
  encode_epoch(writer, value.provenance.epoch());
  writer.str(value.provenance.origin(), limits::kMaxOriginLen);
}

namespace {

bool read_optional_u64(ByteReader& reader, std::optional<std::uint64_t>& out) {
  bool present = false;
  if (!reader.optional(present, [&](ByteReader& r) { out = r.u64(); })) {
    return false;
  }
  if (!present) {
    out.reset();
  }
  return true;
}

bool read_optional_generation(ByteReader& reader, std::optional<Generation>& out) {
  bool present = false;
  if (!reader.optional(present, [&](ByteReader& r) { out = Generation{r.u64()}; })) {
    return false;
  }
  if (!present) {
    out.reset();
  }
  return true;
}

bool read_optional_incarnation(ByteReader& reader, std::optional<PublisherIncarnation>& out) {
  bool present = false;
  if (!reader.optional(present, [&](ByteReader& r) { out = PublisherIncarnation{r.u64()}; })) {
    return false;
  }
  if (!present) {
    out.reset();
  }
  return true;
}

bool read_optional_resource(ByteReader& reader, std::optional<ResourceId>& out) {
  bool present = false;
  if (!reader.optional(present, [&](ByteReader& r) {
        out = ResourceId{r.str(limits::kMaxIdentifierLen)};
      })) {
    return false;
  }
  if (!present) {
    out.reset();
  }
  return true;
}

bool read_digest(ByteReader& reader, Digest256& out) {
  const std::string_view raw = reader.raw(Digest256::kBytes);
  if (reader.failed()) {
    return false;
  }
  std::array<std::uint8_t, Digest256::kBytes> bytes{};
  std::copy(raw.begin(), raw.end(), bytes.begin());
  out = Digest256{bytes};
  return true;
}

}  // namespace

Result<void> canonical_decode(ByteReader& reader, ContractDecision& out) {
  const std::uint16_t tag = reader.u16();
  const std::uint16_t version = reader.u16();
  if (reader.failed() || tag != kTagDecision || version != kSchemaVersion) {
    return make_error(ErrorCode::VersionMismatch, "decision record schema is not supported");
  }
  ContractDecision value;
  value.contract = QoSContractId{reader.str(limits::kMaxIdentifierLen)};
  value.contract_generation = Generation{reader.u64()};
  const std::uint8_t raw_outcome = reader.u8();
  if (reader.failed() || raw_outcome > static_cast<std::uint8_t>(Outcome::Rejected)) {
    return make_error(ErrorCode::Malformed, "decision outcome is out of range");
  }
  value.outcome = static_cast<Outcome>(raw_outcome);

  value.class_id = QoSClassId{reader.str(limits::kMaxIdentifierLen)};
  value.class_generation = Generation{reader.u64()};
  if (!read_digest(reader, value.class_digest)) {
    return make_error(ErrorCode::Malformed, "decision class digest is malformed");
  }
  value.subject = SubjectId{reader.str(limits::kMaxIdentifierLen)};
  value.subject_generation = Generation{reader.u64()};
  value.path = PathId{reader.str(limits::kMaxIdentifierLen)};
  value.path_generation = Generation{reader.u64()};
  if (!read_digest(reader, value.path_digest)) {
    return make_error(ErrorCode::Malformed, "decision path digest is malformed");
  }
  value.policy = PolicyId{reader.str(limits::kMaxIdentifierLen)};
  value.policy_generation = Generation{reader.u64()};
  if (!read_digest(reader, value.policy_digest)) {
    return make_error(ErrorCode::Malformed, "decision policy digest is malformed");
  }
  value.priority = PriorityClassId{reader.str(limits::kMaxIdentifierLen)};
  value.priority_generation = Generation{reader.u64()};
  value.fabric_epoch = FabricEpoch{reader.u64()};
  value.observed_epoch = FabricEpoch{reader.u64()};
  value.decided_at_ms = reader.u64();
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "decision header is truncated");
  }

  std::size_t obligation_count = 0;
  if (!reader.count(limits::kMaxObligations, obligation_count)) {
    return make_error(ErrorCode::TooLarge, "decision obligation table exceeds its budget");
  }
  for (std::size_t i = 0; i < obligation_count; ++i) {
    ObligationAssessment item;
    const std::uint8_t kind = reader.u8();
    item.id = ObligationId{reader.str(limits::kMaxIdentifierLen)};
    item.binding = reader.presence();
    item.relaxable = reader.presence();
    const std::uint8_t direction = reader.u8();
    const std::uint8_t aggregate = reader.u8();
    item.path_level = reader.presence();
    item.required_value = reader.u64();
    item.effective_limit = reader.u64();
    if (!read_optional_u64(reader, item.composed_value)) {
      return make_error(ErrorCode::Malformed, "decision obligation value is malformed");
    }
    item.relaxation_applied_ppm = reader.u32();
    item.relaxation_reported_only = reader.presence();
    const std::uint8_t status = reader.u8();
    if (reader.failed() || kind >= static_cast<std::uint8_t>(ObligationKind::Count) ||
        direction > static_cast<std::uint8_t>(CompareDirection::AtMost) ||
        aggregate > static_cast<std::uint8_t>(AggregateKind::PathDiversity) ||
        status > static_cast<std::uint8_t>(ObligationStatus::NotApplicable)) {
      return make_error(ErrorCode::Malformed, "decision obligation fields are out of range");
    }
    item.kind = static_cast<ObligationKind>(kind);
    item.direction = static_cast<CompareDirection>(direction);
    item.aggregate = static_cast<AggregateKind>(aggregate);
    item.status = static_cast<ObligationStatus>(status);
    if (!read_optional_resource(reader, item.binding_resource) ||
        !read_optional_resource(reader, item.weakest_resource)) {
      return make_error(ErrorCode::Malformed, "decision obligation resource is malformed");
    }
    item.headroom_ppm = reader.u32();
    item.detail = reader.str(limits::kMaxDetailLen);
    if (reader.failed()) {
      return make_error(ErrorCode::Malformed, "decision obligation record is truncated");
    }
    value.obligations.push_back(std::move(item));
  }

  std::size_t resource_count = 0;
  if (!reader.count(limits::kMaxPathComponents, resource_count)) {
    return make_error(ErrorCode::TooLarge, "decision resource table exceeds its budget");
  }
  for (std::size_t i = 0; i < resource_count; ++i) {
    ResourceAssessment item;
    item.resource = ResourceId{reader.str(limits::kMaxIdentifierLen)};
    item.diversity_domain = DiversityDomain{reader.str(limits::kMaxIdentifierLen)};
    const std::uint8_t role = reader.u8();
    item.bound_capability_generation = Generation{reader.u64()};
    if (!read_optional_generation(reader, item.observed_capability_generation) ||
        !read_optional_incarnation(reader, item.observed_incarnation)) {
      return make_error(ErrorCode::Malformed, "decision resource fields are malformed");
    }
    const std::uint8_t status = reader.u8();
    item.self_declared_degraded = reader.presence();
    item.reason = reader.str(limits::kMaxReasonLen);
    if (reader.failed() || role > static_cast<std::uint8_t>(PathRole::Protection) ||
        status > static_cast<std::uint8_t>(ComponentStatus::Stale)) {
      return make_error(ErrorCode::Malformed, "decision resource fields are out of range");
    }
    item.role = static_cast<PathRole>(role);
    item.status = static_cast<ComponentStatus>(status);
    value.resources.push_back(std::move(item));
  }

  std::size_t degradation_count = 0;
  if (!reader.count(limits::kMaxDegradations, degradation_count)) {
    return make_error(ErrorCode::TooLarge, "decision degradation table exceeds its budget");
  }
  for (std::size_t i = 0; i < degradation_count; ++i) {
    DegradationReason item;
    const std::uint8_t kind = reader.u8();
    item.id = ObligationId{reader.str(limits::kMaxIdentifierLen)};
    if (!read_optional_resource(reader, item.resource)) {
      return make_error(ErrorCode::Malformed, "decision degradation resource is malformed");
    }
    item.relaxation_ppm = reader.u32();
    item.reported_only = reader.presence();
    item.detail = reader.str(limits::kMaxReasonLen);
    if (reader.failed() || kind >= static_cast<std::uint8_t>(ObligationKind::Count)) {
      return make_error(ErrorCode::Malformed, "decision degradation fields are out of range");
    }
    item.kind = static_cast<ObligationKind>(kind);
    value.degradations.push_back(std::move(item));
  }

  std::size_t authority_count = 0;
  if (!reader.count(limits::kMaxAuthorityEntries, authority_count)) {
    return make_error(ErrorCode::TooLarge, "decision authority table exceeds its budget");
  }
  for (std::size_t i = 0; i < authority_count; ++i) {
    AuthorityEntry item;
    const std::uint8_t dimension = reader.u8();
    item.id = reader.str(limits::kMaxIdentifierLen);
    item.generation = Generation{reader.u64()};
    item.digest_hex = reader.str(64);
    item.epoch = FabricEpoch{reader.u64()};
    item.source = reader.str(limits::kMaxOriginLen);
    if (reader.failed() ||
        dimension > static_cast<std::uint8_t>(AuthorityEntry::Dimension::Count)) {
      return make_error(ErrorCode::Malformed, "decision authority entry is malformed");
    }
    item.dimension = static_cast<AuthorityEntry::Dimension>(dimension);
    value.authority.push_back(std::move(item));
  }

  bool has_binding = false;
  if (!reader.optional(has_binding, [&](ByteReader& r) {
        const std::uint8_t kind = r.u8();
        if (kind >= static_cast<std::uint8_t>(ObligationKind::Count)) {
          r.fail();
          return;
        }
        value.binding_obligation = static_cast<ObligationKind>(kind);
      })) {
    return make_error(ErrorCode::Malformed, "decision binding obligation is malformed");
  }
  if (!read_optional_resource(reader, value.binding_resource)) {
    return make_error(ErrorCode::Malformed, "decision binding resource is malformed");
  }
  value.unknown_components = reader.u32();
  value.violated_obligations = reader.u32();
  value.unsatisfied_diversity_domains = reader.u32();
  value.primary_reason = reader.str(limits::kMaxReasonLen);
  if (!read_digest(reader, value.request_digest)) {
    return make_error(ErrorCode::Malformed, "decision request digest is malformed");
  }
  const FabricEpoch provenance_epoch = FabricEpoch{reader.u64()};
  const std::string provenance_origin = reader.str(limits::kMaxOriginLen);
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "decision record is truncated");
  }
  value.provenance = Provenance{provenance_origin, provenance_epoch, Generation{1}, Sequence{1}};
  if (!reader.at_end()) {
    return make_error(ErrorCode::Malformed, "decision record carries trailing bytes");
  }
  out = std::move(value);
  return {};
}

Digest256 compute_decision_digest(const ContractDecision& value) {
  ByteWriter writer;
  canonical_encode(value, writer);
  if (!writer.ok()) {
    return Digest256{};
  }
  return sha256(writer.data().data(), writer.data().size());
}

std::string explain(const ContractDecision& decision, const ExplainOptions& options) {
  const std::size_t budget = (options.max_bytes == 0)
                                 ? limits::kMaxExplainBytes
                                 : std::min(options.max_bytes, limits::kMaxExplainBytes);
  BoundedText text(budget);

  std::string head = "outcome: ";
  head += to_string(decision.outcome);
  text.line(head);

  text.line("contract: " + decision.contract.str() + "@" +
            u64_text(decision.contract_generation.value()) + " subject: " +
            decision.subject.str() + "@" + u64_text(decision.subject_generation.value()));
  text.line("class: " + decision.class_id.str() + "@" + u64_text(decision.class_generation.value()) +
            " digest=" + decision.class_digest.hex());
  text.line("path: " + decision.path.str() + "@" + u64_text(decision.path_generation.value()) +
            " digest=" + decision.path_digest.hex());
  text.line("policy: " + decision.policy.str() + "@" + u64_text(decision.policy_generation.value()) +
            " digest=" + decision.policy_digest.hex());
  text.line("priority: " + decision.priority.str() + "@" +
            u64_text(decision.priority_generation.value()));
  text.line("fabric-epoch: " + u64_text(decision.fabric_epoch.value()) + " observed-epoch: " +
            u64_text(decision.observed_epoch.value()));
  text.line("reason: " + decision.primary_reason);
  if (decision.binding_obligation.has_value()) {
    std::string binding = "binding-obligation: ";
    binding += to_string(*decision.binding_obligation);
    if (decision.binding_resource.has_value()) {
      binding += " resource=";
      binding += decision.binding_resource->view();
    }
    text.line(binding);
  }
  text.line("counts: obligations=" + u64_text(decision.obligations.size()) + " resources=" +
            u64_text(decision.resources.size()) + " degradations=" +
            u64_text(decision.degradations.size()) + " unknown-components=" +
            u64_text(decision.unknown_components) + " violated=" +
            u64_text(decision.violated_obligations));

  text.line("obligations:");
  const std::size_t obligation_limit =
      std::min(options.max_obligations, limits::kMaxExplainObligations);
  std::size_t shown = 0;
  for (const ObligationAssessment& assessment : decision.obligations) {
    if (shown >= obligation_limit) {
      text.line("  ... " + u64_text(decision.obligations.size() - shown) + " more");
      break;
    }
    text.line(obligation_line(assessment, options.include_timings));
    ++shown;
  }

  text.line("resources:");
  const std::size_t resource_limit = std::min(options.max_resources, limits::kMaxExplainComponents);
  std::size_t resource_shown = 0;
  for (const ResourceAssessment& assessment : decision.resources) {
    if (resource_shown >= resource_limit) {
      text.line("  ... " + u64_text(decision.resources.size() - resource_shown) + " more");
      break;
    }
    std::string line = "  ";
    line += assessment.resource.view();
    line += " domain=";
    line += assessment.diversity_domain.view();
    line += " role=";
    line += to_string(assessment.role);
    line += " bound-cap=";
    line += u64_text(assessment.bound_capability_generation.value());
    line += " observed-cap=";
    line += assessment.observed_capability_generation.has_value()
                ? u64_text(assessment.observed_capability_generation->value())
                : std::string("none");
    line += " status=";
    line += to_string(assessment.status);
    if (!assessment.reason.empty()) {
      line += " (";
      line += assessment.reason;
      line += ")";
    }
    text.line(line);
    ++resource_shown;
  }

  if (!decision.degradations.empty()) {
    text.line("degradations:");
    for (const DegradationReason& reason : decision.degradations) {
      std::string line = "  ";
      line += reason.id.view();
      if (reason.resource.has_value()) {
        line += " resource=";
        line += reason.resource->view();
      }
      line += " relax=";
      line += u64_text(reason.relaxation_ppm);
      line += "ppm";
      if (reason.reported_only) {
        line += " reported-only";
      }
      if (!reason.detail.empty()) {
        line += " (";
        line += reason.detail;
        line += ")";
      }
      text.line(line);
    }
  }

  if (options.include_authority && !decision.authority.empty()) {
    text.line("authority:");
    for (const AuthorityEntry& entry : decision.authority) {
      std::string line = "  ";
      line += to_string(entry.dimension);
      line += " ";
      line += entry.id;
      line += "@";
      line += u64_text(entry.generation.value());
      line += " epoch=";
      line += u64_text(entry.epoch.value());
      if (!entry.digest_hex.empty()) {
        line += " digest=";
        line += entry.digest_hex;
      }
      if (!entry.source.empty()) {
        line += " source=";
        line += entry.source;
      }
      text.line(line);
    }
  }

  text.line("decision-digest: " + decision.decision_digest.hex());
  text.line("request-digest: " + decision.request_digest.hex());
  return text.take();
}

}  // namespace qosfabric
