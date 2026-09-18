// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/registry.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>

#include "qosfabric/checked.hpp"
#include "qosfabric/serialize.hpp"

namespace qosfabric {
namespace {

constexpr std::uint16_t kSnapshotTag = 0x0301;
constexpr std::uint16_t kSnapshotVersion = 1;
constexpr std::size_t kMaxStoredConflicts = 256;

std::string key_of(std::string_view id, Generation generation) {
  std::string key(id);
  key.push_back('#');
  key += std::to_string(generation.value());
  return key;
}

// Uniform access to the identifier a durable entity is keyed by, so the
// snapshot tables share one decoder.
[[nodiscard]] std::string_view entity_id(const QoSClass& value) noexcept { return value.id.view(); }
[[nodiscard]] std::string_view entity_id(const Policy& value) noexcept { return value.id.view(); }
[[nodiscard]] std::string_view entity_id(const Path& value) noexcept { return value.id.view(); }
[[nodiscard]] std::string_view entity_id(const ResourceCapability& value) noexcept {
  return value.resource.view();
}
[[nodiscard]] std::string_view entity_id(const Reservation& value) noexcept {
  return value.resource.view();
}

template <class T>
void write_blob(ByteWriter& writer, const T& value) {
  ByteWriter inner(limits::kMaxCanonicalRecordBytes);
  canonical_encode(value, inner);
  if (!inner.ok()) {
    writer.fail();
    return;
  }
  writer.bytes(inner.data());
}

template <class T>
Result<void> read_blob(ByteReader& reader, T& out) {
  const std::vector<std::uint8_t> raw = reader.bytes(limits::kMaxCanonicalRecordBytes);
  if (reader.failed()) {
    return make_error(ErrorCode::Malformed, "record blob is malformed or over budget");
  }
  ByteReader inner(raw);
  const auto decoded = canonical_decode(inner, out);
  if (!decoded) {
    return decoded.error();
  }
  return {};
}

}  // namespace

std::uint64_t system_now_millis() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

Registry::~Registry() = default;

Result<std::unique_ptr<Registry>> Registry::Open(const RegistryOptions& options) {
  if (options.directory.empty()) {
    return make_error(ErrorCode::InvalidArgument, "registry directory is required");
  }
  if (options.max_generations_per_id == 0 || options.max_generations_per_id > 1024) {
    return make_error(ErrorCode::InvalidArgument, "generation retention outside the supported range");
  }
  std::unique_ptr<Registry> registry(new Registry());
  const auto opened = registry->open_impl(options);
  if (!opened) {
    return opened.error();
  }
  return registry;
}

Result<void> Registry::open_impl(const RegistryOptions& options) {
  options_ = options;
  StoreOptions store_options;
  store_options.directory = options.directory;
  store_options.create_if_missing = options.create_if_missing;
  store_options.max_wal_bytes = options.max_wal_bytes;
  store_options.max_record_bytes = options.max_record_bytes;
  store_options.repair_truncate_corrupt_tail = options.repair_truncate_corrupt_tail;

  auto store = Store::Open(store_options);
  if (!store) {
    return store.error();
  }
  store_ = std::move(*store);

  now_ms_ = system_now_millis();

  const auto loaded = load_snapshot_state();
  if (!loaded) {
    return loaded.error();
  }
  const auto rebuilt = rebuild_from_log();
  if (!rebuilt) {
    return rebuilt.error();
  }

  if (!options.claim_authority) {
    // Observation only: no epoch movement, no lease invalidation.
    return {};
  }
  // Fresh incarnation: advance the epoch durably, then discard every lease.
  // Durable state never restores liveness, so nothing that was live before
  // this open may be treated as live now.
  const auto advanced = advance_epoch_locked("fabric-authority-restart");
  if (!advanced) {
    return advanced.error();
  }
  leases_.clear();
  return {};
}

Result<void> Registry::load_snapshot_state() {
  const std::vector<std::uint8_t>& raw = store_->snapshot_bytes();
  if (raw.empty()) {
    return {};
  }
  ByteReader reader(raw);
  const std::uint16_t tag = reader.u16();
  const std::uint16_t version = reader.u16();
  if (reader.failed() || tag != kSnapshotTag || version != kSnapshotVersion) {
    return make_error(ErrorCode::VersionMismatch, "registry snapshot schema is not supported");
  }
  durable_epoch_ = FabricEpoch{reader.u64()};
  current_epoch_ = FabricEpoch{reader.u64()};

  std::size_t publisher_count = 0;
  if (!reader.count(limits::kMaxPublishersRetained, publisher_count)) {
    return make_error(ErrorCode::TooLarge, "publisher watermark table exceeds its budget");
  }
  for (std::size_t i = 0; i < publisher_count; ++i) {
    const std::string id = reader.str(limits::kMaxIdentifierLen);
    const std::uint64_t incarnation = reader.u64();
    if (reader.failed() || !is_valid_identifier(id)) {
      return make_error(ErrorCode::Corrupt, "publisher watermark entry is malformed");
    }
    publisher_watermark_[id] = PublisherIncarnation{incarnation};
  }

  auto read_generation_table = [&reader](auto& table, std::size_t max_ids,
                                         std::size_t max_generations,
                                         auto&& decode_one) -> Result<void> {
    std::size_t id_count = 0;
    if (!reader.count(max_ids, id_count)) {
      return make_error(ErrorCode::TooLarge, "snapshot identifier table exceeds its budget");
    }
    for (std::size_t i = 0; i < id_count; ++i) {
      std::size_t generation_count = 0;
      if (!reader.count(max_generations, generation_count)) {
        return make_error(ErrorCode::TooLarge, "snapshot generation table exceeds its budget");
      }
      for (std::size_t g = 0; g < generation_count; ++g) {
        typename std::decay_t<decltype(table)>::mapped_type::value_type value;
        const auto decoded = decode_one(reader, value);
        if (!decoded) {
          return decoded.error();
        }
        table[std::string(entity_id(value))].push_back(std::move(value));
      }
    }
    return {};
  };

  auto decode_class = [](ByteReader& r, QoSClass& out) { return read_blob(r, out); };
  auto decode_policy = [](ByteReader& r, Policy& out) { return read_blob(r, out); };
  auto decode_path = [](ByteReader& r, Path& out) { return read_blob(r, out); };
  auto decode_capability = [](ByteReader& r, ResourceCapability& out) { return read_blob(r, out); };
  auto decode_reservation = [](ByteReader& r, Reservation& out) { return read_blob(r, out); };

  if (auto ok = read_generation_table(classes_, limits::kMaxClassesRetained,
                                      limits::kMaxGenerationsPerId, decode_class);
      !ok) {
    return ok.error();
  }
  if (auto ok = read_generation_table(policies_, limits::kMaxPoliciesRetained,
                                      limits::kMaxGenerationsPerId, decode_policy);
      !ok) {
    return ok.error();
  }
  if (auto ok = read_generation_table(paths_, limits::kMaxPathsRetained,
                                      limits::kMaxGenerationsPerId, decode_path);
      !ok) {
    return ok.error();
  }
  if (auto ok = read_generation_table(capabilities_, limits::kMaxCapabilitiesRetained,
                                      limits::kMaxGenerationsPerId, decode_capability);
      !ok) {
    return ok.error();
  }
  if (auto ok = read_generation_table(reservations_, limits::kMaxCapabilitiesRetained,
                                      limits::kMaxGenerationsPerId, decode_reservation);
      !ok) {
    return ok.error();
  }

  std::size_t conflict_count = 0;
  if (!reader.count(kMaxStoredConflicts, conflict_count)) {
    return make_error(ErrorCode::TooLarge, "conflict table exceeds its budget");
  }
  for (std::size_t i = 0; i < conflict_count; ++i) {
    ConflictMarker marker;
    const std::uint8_t scope = reader.u8();
    marker.id = reader.str(limits::kMaxIdentifierLen);
    marker.generation = Generation{reader.u64()};
    const std::string_view incumbent = reader.raw(Digest256::kBytes);
    const std::string_view challenger = reader.raw(Digest256::kBytes);
    marker.epoch = FabricEpoch{reader.u64()};
    marker.detail = reader.str(limits::kMaxReasonLen);
    if (reader.failed() || scope > static_cast<std::uint8_t>(ConflictMarker::Scope::Count)) {
      return make_error(ErrorCode::Corrupt, "conflict marker is malformed");
    }
    marker.scope = static_cast<ConflictMarker::Scope>(scope);
    std::array<std::uint8_t, Digest256::kBytes> a{};
    std::array<std::uint8_t, Digest256::kBytes> b{};
    std::copy(incumbent.begin(), incumbent.end(), a.begin());
    std::copy(challenger.begin(), challenger.end(), b.begin());
    marker.incumbent_digest = Digest256{a};
    marker.challenger_digest = Digest256{b};
    conflicts_.push_back(std::move(marker));
  }

  std::size_t decision_count = 0;
  if (!reader.count(limits::kMaxDecisionsRetained, decision_count)) {
    return make_error(ErrorCode::TooLarge, "decision audit table exceeds its budget");
  }
  for (std::size_t i = 0; i < decision_count; ++i) {
    if (!reader.presence()) {
      continue;
    }
    ContractDecision value;
    const auto decoded = read_blob(reader, value);
    if (!decoded) {
      return decoded.error();
    }
    value.decision_digest = compute_decision_digest(value);
    decision_order_.push_back(value.contract);
    decisions_[key_of(value.contract.view(), value.contract_generation)] = std::move(value);
  }

  if (reader.failed()) {
    return make_error(ErrorCode::Corrupt, "registry snapshot is truncated");
  }
  if (!reader.at_end()) {
    return make_error(ErrorCode::Corrupt, "registry snapshot carries trailing bytes");
  }
  return {};
}

Result<std::vector<std::uint8_t>> Registry::encode_snapshot_state() const {
  ByteWriter writer(limits::kMaxSnapshotBytes);
  writer.u16(kSnapshotTag);
  writer.u16(kSnapshotVersion);
  writer.u64(durable_epoch_.value());
  writer.u64(current_epoch_.value());

  std::vector<std::string> ordered;
  ordered.reserve(publisher_watermark_.size());
  for (const auto& entry : publisher_watermark_) {
    ordered.push_back(entry.first);
  }
  std::sort(ordered.begin(), ordered.end());
  writer.counted(ordered.size(), [&](ByteWriter& w) {
    for (const std::string& id : ordered) {
      w.str(id, limits::kMaxIdentifierLen);
      w.u64(publisher_watermark_.at(id).value());
    }
  });

  std::vector<std::string> class_ids;
  class_ids.reserve(classes_.size());
  for (const auto& entry : classes_) {
    class_ids.push_back(entry.first);
  }
  std::sort(class_ids.begin(), class_ids.end());
  writer.counted(class_ids.size(), [&](ByteWriter& w) {
    for (const std::string& id : class_ids) {
      const auto& generation_list = classes_.at(id);
      w.counted(generation_list.size(), [&](ByteWriter& inner) {
        for (const QoSClass& value : generation_list) {
          write_blob(inner, value);
        }
      });
    }
  });

  std::vector<std::string> policy_ids;
  policy_ids.reserve(policies_.size());
  for (const auto& entry : policies_) {
    policy_ids.push_back(entry.first);
  }
  std::sort(policy_ids.begin(), policy_ids.end());
  writer.counted(policy_ids.size(), [&](ByteWriter& w) {
    for (const std::string& id : policy_ids) {
      const auto& generation_list = policies_.at(id);
      w.counted(generation_list.size(), [&](ByteWriter& inner) {
        for (const Policy& value : generation_list) {
          write_blob(inner, value);
        }
      });
    }
  });

  std::vector<std::string> path_ids;
  path_ids.reserve(paths_.size());
  for (const auto& entry : paths_) {
    path_ids.push_back(entry.first);
  }
  std::sort(path_ids.begin(), path_ids.end());
  writer.counted(path_ids.size(), [&](ByteWriter& w) {
    for (const std::string& id : path_ids) {
      const auto& generation_list = paths_.at(id);
      w.counted(generation_list.size(), [&](ByteWriter& inner) {
        for (const Path& value : generation_list) {
          write_blob(inner, value);
        }
      });
    }
  });

  std::vector<std::string> resource_ids;
  resource_ids.reserve(capabilities_.size());
  for (const auto& entry : capabilities_) {
    resource_ids.push_back(entry.first);
  }
  std::sort(resource_ids.begin(), resource_ids.end());
  writer.counted(resource_ids.size(), [&](ByteWriter& w) {
    for (const std::string& id : resource_ids) {
      const auto& generation_list = capabilities_.at(id);
      w.counted(generation_list.size(), [&](ByteWriter& inner) {
        for (const ResourceCapability& value : generation_list) {
          write_blob(inner, value);
        }
      });
    }
  });

  std::vector<std::string> reservation_ids;
  reservation_ids.reserve(reservations_.size());
  for (const auto& entry : reservations_) {
    reservation_ids.push_back(entry.first);
  }
  std::sort(reservation_ids.begin(), reservation_ids.end());
  writer.counted(reservation_ids.size(), [&](ByteWriter& w) {
    for (const std::string& id : reservation_ids) {
      const auto& generation_list = reservations_.at(id);
      w.counted(generation_list.size(), [&](ByteWriter& inner) {
        for (const Reservation& value : generation_list) {
          write_blob(inner, value);
        }
      });
    }
  });

  writer.counted(conflicts_.size(), [&](ByteWriter& w) {
    for (const ConflictMarker& marker : conflicts_) {
      w.u8(static_cast<std::uint8_t>(marker.scope));
      w.str(marker.id, limits::kMaxIdentifierLen);
      w.u64(marker.generation.value());
      w.raw(marker.incumbent_digest.bytes().data(), Digest256::kBytes);
      w.raw(marker.challenger_digest.bytes().data(), Digest256::kBytes);
      w.u64(marker.epoch.value());
      w.str(marker.detail, limits::kMaxReasonLen);
    }
  });

  // Decisions are emitted in a canonical key order rather than in arrival
  // order, so a snapshot of the same state is byte-identical regardless of how
  // the state was reached.
  std::vector<std::string> decision_keys;
  decision_keys.reserve(decisions_.size());
  for (const auto& entry : decisions_) {
    decision_keys.push_back(entry.first);
  }
  std::sort(decision_keys.begin(), decision_keys.end());
  writer.counted(decision_keys.size(), [&](ByteWriter& w) {
    for (const std::string& key : decision_keys) {
      const auto found = decisions_.find(key);
      if (found == decisions_.end()) {
        w.presence(false);
        continue;
      }
      w.presence(true);
      write_blob(w, found->second);
    }
  });

  if (!writer.ok()) {
    return make_error(ErrorCode::TooLarge, "registry snapshot exceeds its budget");
  }
  return writer.data();
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------
Result<void> Registry::apply_record(const LogRecord& record) {
  switch (record.type) {
    case RecordType::EpochAdvance: {
      if (record.payload.size() < 8) {
        return make_error(ErrorCode::Corrupt, "epoch record is too small");
      }
      const std::uint64_t value =
          static_cast<std::uint64_t>(record.payload[0]) |
          (static_cast<std::uint64_t>(record.payload[1]) << 8) |
          (static_cast<std::uint64_t>(record.payload[2]) << 16) |
          (static_cast<std::uint64_t>(record.payload[3]) << 24) |
          (static_cast<std::uint64_t>(record.payload[4]) << 32) |
          (static_cast<std::uint64_t>(record.payload[5]) << 40) |
          (static_cast<std::uint64_t>(record.payload[6]) << 48) |
          (static_cast<std::uint64_t>(record.payload[7]) << 56);
      durable_epoch_ = FabricEpoch{value};
      if (current_epoch_ < durable_epoch_) {
        current_epoch_ = durable_epoch_;
      }
      return {};
    }
    case RecordType::PublishClass: {
      ByteReader reader(record.payload);
      QoSClass value;
      const auto decoded = canonical_decode(reader, value);
      if (!decoded) {
        return decoded.error();
      }
      classes_[std::string(entity_id(value))].push_back(std::move(value));
      return {};
    }
    case RecordType::PublishPolicy: {
      ByteReader reader(record.payload);
      Policy value;
      const auto decoded = canonical_decode(reader, value);
      if (!decoded) {
        return decoded.error();
      }
      policies_[std::string(entity_id(value))].push_back(std::move(value));
      return {};
    }
    case RecordType::PublishPath: {
      ByteReader reader(record.payload);
      Path value;
      const auto decoded = canonical_decode(reader, value);
      if (!decoded) {
        return decoded.error();
      }
      paths_[std::string(entity_id(value))].push_back(std::move(value));
      return {};
    }
    case RecordType::PublishCapability: {
      ByteReader reader(record.payload);
      ResourceCapability value;
      const auto decoded = canonical_decode(reader, value);
      if (!decoded) {
        return decoded.error();
      }
      capabilities_[std::string(entity_id(value))].push_back(std::move(value));
      return {};
    }
    case RecordType::PublishReservation: {
      ByteReader reader(record.payload);
      Reservation value;
      const auto decoded = canonical_decode(reader, value);
      if (!decoded) {
        return decoded.error();
      }
      reservations_[std::string(entity_id(value))].push_back(std::move(value));
      return {};
    }
    case RecordType::RegisterPublisher: {
      ByteReader reader(record.payload);
      const std::string id = reader.str(limits::kMaxIdentifierLen);
      const std::uint64_t incarnation = reader.u64();
      if (reader.failed() || !is_valid_identifier(id) || incarnation == 0) {
        return make_error(ErrorCode::Corrupt, "publisher registration record is malformed");
      }
      const PublisherIncarnation parsed{incarnation};
      auto found = publisher_watermark_.find(id);
      if (found == publisher_watermark_.end() || found->second < parsed) {
        publisher_watermark_[id] = parsed;
      }
      return {};
    }
    case RecordType::ConflictRecord: {
      ByteReader reader(record.payload);
      const std::uint8_t scope = reader.u8();
      ConflictMarker marker;
      marker.id = reader.str(limits::kMaxIdentifierLen);
      marker.generation = Generation{reader.u64()};
      const std::string_view incumbent = reader.raw(Digest256::kBytes);
      const std::string_view challenger = reader.raw(Digest256::kBytes);
      marker.epoch = FabricEpoch{reader.u64()};
      marker.detail = reader.str(limits::kMaxReasonLen);
      if (reader.failed() || scope > static_cast<std::uint8_t>(ConflictMarker::Scope::Count)) {
        return make_error(ErrorCode::Corrupt, "conflict record is malformed");
      }
      marker.scope = static_cast<ConflictMarker::Scope>(scope);
      std::array<std::uint8_t, Digest256::kBytes> a{};
      std::array<std::uint8_t, Digest256::kBytes> b{};
      std::copy(incumbent.begin(), incumbent.end(), a.begin());
      std::copy(challenger.begin(), challenger.end(), b.begin());
      marker.incumbent_digest = Digest256{a};
      marker.challenger_digest = Digest256{b};
      conflicts_.push_back(std::move(marker));
      if (conflicts_.size() > kMaxStoredConflicts) {
        conflicts_.erase(conflicts_.begin());
      }
      return {};
    }
    case RecordType::RecordDecision: {
      ByteReader reader(record.payload);
      ContractDecision value;
      const auto decoded = canonical_decode(reader, value);
      if (!decoded) {
        return decoded.error();
      }
      value.decision_digest = compute_decision_digest(value);
      const std::string key = key_of(value.contract.view(), value.contract_generation);
      if (decisions_.find(key) == decisions_.end()) {
        decision_order_.push_back(value.contract);
      }
      decisions_[key] = std::move(value);
      prune_decisions();
      return {};
    }
    case RecordType::StoreHeader:
    case RecordType::RevokePublisher:
    case RecordType::PublisherLeaseCleared:
    case RecordType::CleanShutdown:
    case RecordType::MutationIntent:
    case RecordType::MutationCommit:
    case RecordType::MutationAbort:
    case RecordType::Unknown:
      return {};
  }
  return {};
}

Result<void> Registry::rebuild_from_log() {
  for (const LogRecord& record : store_->replayed()) {
    const auto applied = apply_record(record);
    if (!applied) {
      return applied.error();
    }
  }
  return {};
}

// ---------------------------------------------------------------------------
// Epoch
// ---------------------------------------------------------------------------
Result<FabricEpoch> Registry::advance_epoch_locked(std::string_view reason) {
  FabricEpoch next = durable_epoch_;
  if (!next.try_increment()) {
    return make_error(ErrorCode::Overflow, "fabric epoch space is exhausted");
  }
  ByteWriter writer(512);
  writer.u64(next.value());
  writer.str(reason.substr(0, limits::kMaxReasonLen), limits::kMaxReasonLen);
  if (!writer.ok()) {
    return make_error(ErrorCode::TooLarge, "epoch record exceeds its budget");
  }
  const auto appended = store_->Append(RecordType::EpochAdvance, writer.data());
  if (!appended) {
    return appended.error();
  }
  durable_epoch_ = next;
  current_epoch_ = next;
  // An epoch advance invalidates every lease issued in the previous epoch.
  leases_.clear();
  return durable_epoch_;
}

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------
Result<Sequence> Registry::stage_with_retry(RecordType type,
                                            const std::vector<std::uint8_t>& payload) {
  auto attempt = store_->Stage(type, payload);
  if (!attempt && attempt.error().code() == ErrorCode::CapacityExhausted) {
    const auto compacted = compact_locked();
    if (!compacted) {
      return compacted.error();
    }
    attempt = store_->Stage(type, payload);
  }
  if (!attempt) {
    return attempt.error();
  }
  return Sequence{attempt->value()};
}

namespace {

// The assertion epoch and provenance live outside the content digest; these
// overloads let one publication path serve every durable entity type.
FabricEpoch& assertion_epoch(QoSClass& value) noexcept { return value.published_epoch; }
FabricEpoch& assertion_epoch(Policy& value) noexcept { return value.published_epoch; }
FabricEpoch& assertion_epoch(Path& value) noexcept { return value.epoch; }
FabricEpoch& assertion_epoch(ResourceCapability& value) noexcept { return value.epoch; }
FabricEpoch& assertion_epoch(Reservation& value) noexcept { return value.epoch; }

Provenance& assertion_provenance(QoSClass& value) noexcept { return value.provenance; }
Provenance& assertion_provenance(Policy& value) noexcept { return value.provenance; }
Provenance& assertion_provenance(Path& value) noexcept { return value.provenance; }
Provenance& assertion_provenance(ResourceCapability& value) noexcept { return value.provenance; }
Provenance& assertion_provenance(Reservation& value) noexcept { return value.provenance; }

ConflictMarker::Scope conflict_scope(RecordType type) noexcept {
  switch (type) {
    case RecordType::PublishClass: return ConflictMarker::Scope::Class;
    case RecordType::PublishPolicy: return ConflictMarker::Scope::Policy;
    case RecordType::PublishPath: return ConflictMarker::Scope::Path;
    case RecordType::PublishCapability: return ConflictMarker::Scope::Capability;
    case RecordType::PublishReservation: return ConflictMarker::Scope::Reservation;
    default: return ConflictMarker::Scope::Class;
  }
}

}  // namespace

template <class T, class Table>
Result<PublicationResult> Registry::publish_entity(Table& table, RecordType type, T value,
                                                   std::size_t max_identifiers) {
  const auto valid = validate(value);
  if (!valid) {
    return valid.error();
  }
  const std::string identifier(entity_id(value));
  if (value.generation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "generation must be non-zero");
  }

  // Keep the retained-generation window bounded before accepting more.
  auto found = table.find(identifier);
  if (found != table.end() && found->second.size() >= options_.max_generations_per_id) {
    const auto compacted = compact_locked();
    if (!compacted) {
      return compacted.error();
    }
    found = table.find(identifier);
  }

  // The digest addresses the definition only: epoch, timestamps and
  // provenance are assertion metadata and are normalized away here.
  assertion_epoch(value) = FabricEpoch{};
  assertion_provenance(value) = Provenance{};
  value.digest = Digest256{};
  const Digest256 digest = compute_digest(value);

  if (found == table.end()) {
    if (table.size() >= max_identifiers) {
      return make_error(ErrorCode::CapacityExhausted,
                        "identifier budget exhausted; retire identifiers or raise the budget");
    }
    if (value.generation.value() != 1) {
      return make_error(ErrorCode::Stale,
                        "the first published generation of an identifier must be generation 1");
    }
  } else {
    const Generation latest = found->second.back().generation;
    if (value.generation < latest) {
      return make_error(ErrorCode::Stale, "generation is older than the retained generation");
    }
    if (value.generation == latest) {
      if (found->second.back().digest == digest) {
        // Idempotent re-publication of byte-identical content.
        return PublicationResult{latest, Sequence{0}, digest};
      }
      ConflictMarker marker;
      marker.scope = conflict_scope(type);
      marker.id = identifier;
      marker.generation = value.generation;
      marker.incumbent_digest = found->second.back().digest;
      marker.challenger_digest = digest;
      marker.epoch = current_epoch_;
      marker.detail = "two different definitions were asserted for the same identifier "
                      "and generation";
      ByteWriter writer(512);
      writer.u8(static_cast<std::uint8_t>(marker.scope));
      writer.str(marker.id, limits::kMaxIdentifierLen);
      writer.u64(marker.generation.value());
      writer.raw(marker.incumbent_digest.bytes().data(), Digest256::kBytes);
      writer.raw(marker.challenger_digest.bytes().data(), Digest256::kBytes);
      writer.u64(marker.epoch.value());
      writer.str(marker.detail, limits::kMaxReasonLen);
      if (!writer.ok()) {
        return make_error(ErrorCode::TooLarge, "conflict record exceeds its budget");
      }
      const auto staged = stage_with_retry(RecordType::ConflictRecord, writer.data());
      if (!staged) {
        return staged.error();
      }
      conflicts_.push_back(marker);
      if (conflicts_.size() > kMaxStoredConflicts) {
        conflicts_.erase(conflicts_.begin());
      }
      const auto committed = store_->Commit(AttemptId{staged->value()}, sha256(marker.detail));
      if (!committed) {
        conflicts_.pop_back();
        return committed.error();
      }
      return make_error(ErrorCode::Conflict,
                        "equivocated definition: the identifier and generation already "
                        "carry different content");
    }
    Generation expected = latest;
    if (!expected.try_increment() || value.generation != expected) {
      return make_error(ErrorCode::InvalidArgument, "generation must advance by exactly one");
    }
  }

  assertion_epoch(value) = current_epoch_;
  value.digest = digest;

  ByteWriter payload_writer(limits::kMaxCanonicalRecordBytes);
  canonical_encode(value, payload_writer);
  if (!payload_writer.ok()) {
    return make_error(ErrorCode::TooLarge, "definition exceeds the canonical record budget");
  }

  const auto staged = stage_with_retry(type, payload_writer.data());
  if (!staged) {
    return staged.error();
  }
  assertion_provenance(value) =
      Provenance{std::string("registry"), current_epoch_, value.generation, *staged};

  const Generation published_generation = value.generation;
  auto& list = table[identifier];
  const bool created = list.empty();
  list.push_back(std::move(value));
  const auto committed = store_->Commit(AttemptId{staged->value()}, digest);
  if (!committed) {
    list.pop_back();
    if (created) {
      table.erase(identifier);
    }
    return committed.error();
  }
  return PublicationResult{published_generation, *staged, digest};
}

Result<PublicationResult> Registry::PublishClass(const QoSClass& value, std::string_view origin) {
  std::lock_guard<std::mutex> guard(mutex_);
  QoSClass copy = value;
  copy.provenance = Provenance{std::string(origin.substr(0, limits::kMaxOriginLen)), current_epoch_,
                               copy.generation, Sequence{0}};
  return publish_entity(classes_, RecordType::PublishClass, std::move(copy),
                        limits::kMaxClassesRetained);
}

Result<PublicationResult> Registry::PublishPolicy(const Policy& value, std::string_view origin) {
  std::lock_guard<std::mutex> guard(mutex_);
  Policy copy = value;
  copy.provenance = Provenance{std::string(origin.substr(0, limits::kMaxOriginLen)), current_epoch_,
                               copy.generation, Sequence{0}};
  return publish_entity(policies_, RecordType::PublishPolicy, std::move(copy),
                        limits::kMaxPoliciesRetained);
}

Result<PublicationResult> Registry::PublishPath(const Path& value, std::string_view origin) {
  std::lock_guard<std::mutex> guard(mutex_);
  Path copy = value;
  copy.provenance = Provenance{std::string(origin.substr(0, limits::kMaxOriginLen)), current_epoch_,
                               copy.generation, Sequence{0}};
  return publish_entity(paths_, RecordType::PublishPath, std::move(copy), limits::kMaxPathsRetained);
}

Result<PublicationResult> Registry::PublishCapability(const ResourceCapability& value) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto valid = validate(value);
  if (!valid) {
    return valid.error();
  }
  const auto lease = find_lease(value.publisher);
  if (!lease.has_value()) {
    return make_error(ErrorCode::Unauthorized,
                      "publisher holds no live lease in the current fabric epoch");
  }
  if (lease->incarnation != value.incarnation) {
    return make_error(ErrorCode::Fenced,
                      "publisher incarnation does not match the live lease");
  }
  if (value.epoch != lease->epoch || value.epoch != current_epoch_) {
    return make_error(ErrorCode::Stale, "capability asserts an epoch that is not current");
  }
  ResourceCapability copy = value;
  copy.provenance = Provenance{value.publisher.str(), current_epoch_, value.generation, Sequence{0}};
  return publish_entity(capabilities_, RecordType::PublishCapability, std::move(copy),
                        limits::kMaxCapabilitiesRetained);
}

Result<PublicationResult> Registry::PublishReservation(const Reservation& value) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto valid = validate(value);
  if (!valid) {
    return valid.error();
  }
  const auto lease = find_lease(value.publisher);
  if (!lease.has_value()) {
    return make_error(ErrorCode::Unauthorized,
                      "reservation publisher holds no live lease in the current fabric epoch");
  }
  if (lease->incarnation != value.incarnation) {
    return make_error(ErrorCode::Fenced, "reservation publisher incarnation is not live");
  }
  if (value.epoch != lease->epoch || value.epoch != current_epoch_) {
    return make_error(ErrorCode::Stale, "reservation asserts an epoch that is not current");
  }
  Reservation copy = value;
  copy.provenance = Provenance{value.publisher.str(), current_epoch_, value.generation, Sequence{0}};
  return publish_entity(reservations_, RecordType::PublishReservation, std::move(copy),
                        limits::kMaxCapabilitiesRetained);
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------
namespace {

// Entities are stored under their plain identifier, with one vector holding
// every retained generation in ascending order. A generation lookup is a
// search inside that vector, never a second keying scheme.
template <class Table>
Result<const typename Table::mapped_type::value_type*> find_entity(const Table& table,
                                                                  std::string_view identifier,
                                                                  Generation generation) {
  const auto found = table.find(std::string(identifier));
  if (found == table.end()) {
    return make_error(ErrorCode::NotFound,
                      "no definition is held for that identifier and generation");
  }
  for (const auto& item : found->second) {
    if (item.generation == generation) {
      return &item;
    }
  }
  return make_error(ErrorCode::NotFound,
                    "no definition is held for that identifier and generation");
}

template <class Table>
Result<const typename Table::mapped_type::value_type*> find_latest(const Table& table,
                                                                  std::string_view identifier) {
  const auto found = table.find(std::string(identifier));
  if (found == table.end() || found->second.empty()) {
    return make_error(ErrorCode::NotFound, "no definition is held for that identifier");
  }
  const typename Table::mapped_type::value_type* best = &found->second.front();
  for (const auto& item : found->second) {
    if (best->generation < item.generation) {
      best = &item;
    }
  }
  return best;
}

}  // namespace

// Resolution helpers assume the registry lock is already held, so a public
// accessor can delegate without ever re-entering it. A zero generation means
// "whatever is newest"; an explicit generation is looked up exactly and is
// never silently upgraded to a newer one.
Result<QoSClass> Registry::class_for(const QoSClassId& id, Generation generation) const {
  if (generation.is_zero()) {
    const auto found = find_latest(classes_, id.view());
    if (!found) {
      return found.error();
    }
    return **found;
  }
  const auto found = find_entity(classes_, id.view(), generation);
  if (!found) {
    return found.error();
  }
  return **found;
}

Result<Policy> Registry::policy_for(const PolicyId& id, Generation generation) const {
  if (generation.is_zero()) {
    const auto found = find_latest(policies_, id.view());
    if (!found) {
      return found.error();
    }
    return **found;
  }
  const auto found = find_entity(policies_, id.view(), generation);
  if (!found) {
    return found.error();
  }
  return **found;
}

Result<Path> Registry::path_for(const PathId& id, Generation generation) const {
  if (generation.is_zero()) {
    const auto found = find_latest(paths_, id.view());
    if (!found) {
      return found.error();
    }
    return **found;
  }
  const auto found = find_entity(paths_, id.view(), generation);
  if (!found) {
    return found.error();
  }
  return **found;
}

Result<QoSClass> Registry::GetClass(const QoSClassId& id, Generation generation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return class_for(id, generation);
}

Result<QoSClass> Registry::LatestClass(const QoSClassId& id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return class_for(id, Generation{0});
}

Result<Policy> Registry::GetPolicy(const PolicyId& id, Generation generation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return policy_for(id, generation);
}

Result<Policy> Registry::LatestPolicy(const PolicyId& id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return policy_for(id, Generation{0});
}

Result<Path> Registry::GetPath(const PathId& id, Generation generation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return path_for(id, generation);
}

Result<Path> Registry::LatestPath(const PathId& id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return path_for(id, Generation{0});
}

Result<ResourceCapability> Registry::GetCapability(const ResourceId& id,
                                                   Generation generation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = find_entity(capabilities_, id.view(), generation);
  if (!found) {
    return found.error();
  }
  return **found;
}

Result<ResourceCapability> Registry::LatestCapability(const ResourceId& id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = find_latest(capabilities_, id.view());
  if (!found) {
    return found.error();
  }
  return **found;
}

Result<Reservation> Registry::GetReservation(const ResourceId& id, Generation generation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = find_entity(reservations_, id.view(), generation);
  if (!found) {
    return found.error();
  }
  return **found;
}

Result<Reservation> Registry::LatestReservation(const ResourceId& id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = find_latest(reservations_, id.view());
  if (!found) {
    return found.error();
  }
  return **found;
}

namespace {

template <class Table>
Result<std::vector<std::string>> list_identifiers(const Table& table) {
  std::vector<std::string> ids;
  ids.reserve(table.size());
  for (const auto& entry : table) {
    if (entry.second.empty()) {
      continue;
    }
    ids.emplace_back(entity_id(entry.second.back()));
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

}  // namespace

Result<std::vector<QoSClassId>> Registry::list_classes() const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto ids = list_identifiers(classes_);
  if (!ids) {
    return ids.error();
  }
  std::vector<QoSClassId> out;
  out.reserve(ids->size());
  for (const std::string& id : *ids) {
    out.emplace_back(id);
  }
  return out;
}

Result<std::vector<PolicyId>> Registry::list_policies() const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto ids = list_identifiers(policies_);
  if (!ids) {
    return ids.error();
  }
  std::vector<PolicyId> out;
  out.reserve(ids->size());
  for (const std::string& id : *ids) {
    out.emplace_back(id);
  }
  return out;
}

Result<std::vector<PathId>> Registry::list_paths() const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto ids = list_identifiers(paths_);
  if (!ids) {
    return ids.error();
  }
  std::vector<PathId> out;
  out.reserve(ids->size());
  for (const std::string& id : *ids) {
    out.emplace_back(id);
  }
  return out;
}

Result<std::vector<ResourceId>> Registry::list_resources() const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto ids = list_identifiers(capabilities_);
  if (!ids) {
    return ids.error();
  }
  std::vector<ResourceId> out;
  out.reserve(ids->size());
  for (const std::string& id : *ids) {
    out.emplace_back(id);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Publisher authority
// ---------------------------------------------------------------------------
Result<PublisherLease> Registry::RegisterPublisher(const PublisherId& publisher,
                                                   PublisherIncarnation incarnation,
                                                   const BootId& boot, std::uint64_t ttl_ms,
                                                   std::string_view origin) {
  std::lock_guard<std::mutex> guard(mutex_);
  tick();
  if (!is_valid(publisher)) {
    return make_error(ErrorCode::InvalidArgument, "publisher identifier is not canonical");
  }
  if (incarnation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "publisher incarnation must be non-zero");
  }
  if (ttl_ms == 0 || ttl_ms > limits::kMaxLeaseMs) {
    return make_error(ErrorCode::InvalidArgument, "lease duration is outside the supported range");
  }
  if (boot.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "publisher boot identity must be non-zero");
  }

  const auto watermark = publisher_watermark_.find(publisher.str());
  const PublisherIncarnation highest =
      (watermark == publisher_watermark_.end()) ? PublisherIncarnation{0} : watermark->second;
  if (incarnation <= highest) {
    // Incarnation numbers are never reused. A publisher that comes back with a
    // stale incarnation has either lost its durable boot counter or is an old
    // process that must not be able to speak for the identity again.
    return make_error(ErrorCode::Fenced,
                      "publisher incarnation is not newer than the durable watermark");
  }
  if (watermark == publisher_watermark_.end() &&
      publisher_watermark_.size() >= limits::kMaxPublishersRetained) {
    return make_error(ErrorCode::CapacityExhausted, "publisher table is at its configured budget");
  }

  PublisherLease lease;
  lease.publisher = publisher;
  lease.incarnation = incarnation;
  lease.epoch = current_epoch_;
  lease.boot = boot;
  lease.granted_at_ms = now_ms_;
  std::uint64_t expires = 0;
  if (!checked_add<std::uint64_t>(now_ms_, ttl_ms, expires)) {
    return make_error(ErrorCode::Overflow, "lease expiry overflowed the representable domain");
  }
  lease.expires_at_ms = expires;

  ByteWriter writer(512);
  writer.str(publisher.view(), limits::kMaxIdentifierLen);
  writer.u64(incarnation.value());
  writer.raw(boot.bytes().data(), BootId::kBytes);
  writer.u64(lease.granted_at_ms);
  writer.u64(lease.expires_at_ms);
  writer.str(origin.substr(0, limits::kMaxOriginLen), limits::kMaxOriginLen);
  if (!writer.ok()) {
    return make_error(ErrorCode::TooLarge, "lease record exceeds its budget");
  }

  const auto staged = stage_with_retry(RecordType::RegisterPublisher, writer.data());
  if (!staged) {
    return staged.error();
  }
  lease.lease = LeaseId{staged->value()};

  const bool created = watermark == publisher_watermark_.end();
  publisher_watermark_[publisher.str()] = incarnation;
  leases_[publisher.str()] = lease;
  const auto committed = store_->Commit(AttemptId{staged->value()}, sha256(publisher.view()));
  if (!committed) {
    leases_.erase(publisher.str());
    if (created) {
      publisher_watermark_.erase(publisher.str());
    } else {
      publisher_watermark_[publisher.str()] = highest;
    }
    return committed.error();
  }
  return lease;
}

Result<void> Registry::RevokePublisher(const PublisherId& publisher,
                                       PublisherIncarnation incarnation) {
  std::lock_guard<std::mutex> guard(mutex_);
  tick();
  if (!is_valid(publisher) || incarnation.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "revocation names an invalid publisher");
  }
  const auto found = leases_.find(publisher.str());
  if (found == leases_.end() || found->second.incarnation != incarnation) {
    return make_error(ErrorCode::Fenced, "revocation does not match the live lease");
  }
  ByteWriter writer(256);
  writer.str(publisher.view(), limits::kMaxIdentifierLen);
  writer.u64(incarnation.value());
  if (!writer.ok()) {
    return make_error(ErrorCode::TooLarge, "revocation record exceeds its budget");
  }
  const auto staged = stage_with_retry(RecordType::RevokePublisher, writer.data());
  if (!staged) {
    return staged.error();
  }
  const PublisherLease removed = found->second;
  leases_.erase(found);
  const auto committed = store_->Commit(AttemptId{staged->value()}, sha256(publisher.view()));
  if (!committed) {
    leases_[publisher.str()] = removed;
    return committed.error();
  }
  return {};
}

Result<void> Registry::ExpireLeases() {
  std::lock_guard<std::mutex> guard(mutex_);
  tick();
  for (auto it = leases_.begin(); it != leases_.end();) {
    if (it->second.expires_at_ms <= now_ms_ || it->second.epoch != current_epoch_) {
      it = leases_.erase(it);
    } else {
      ++it;
    }
  }
  return {};
}

std::optional<PublisherLease> Registry::find_lease(const PublisherId& publisher) const {
  const auto found = leases_.find(publisher.str());
  if (found == leases_.end()) {
    return std::nullopt;
  }
  if (found->second.epoch != current_epoch_) {
    return std::nullopt;
  }
  if (found->second.expires_at_ms <= now_ms_) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<PublisherLease> Registry::FindLease(const PublisherId& publisher) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return find_lease(publisher);
}

std::vector<ConflictMarker> Registry::conflicts() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return conflicts_;
}

FabricEpoch Registry::current_epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return current_epoch_;
}

FabricEpoch Registry::durable_epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return durable_epoch_;
}

void Registry::set_now_millis(std::uint64_t value) {
  std::lock_guard<std::mutex> guard(mutex_);
  now_ms_ = value;
  clock_pinned_ = true;
}

std::uint64_t Registry::now_millis() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return now_ms_;
}

Result<FabricEpoch> Registry::AdvanceEpoch(std::string_view reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  return advance_epoch_locked(reason);
}

Result<void> Registry::Compact() {
  std::lock_guard<std::mutex> guard(mutex_);
  return compact_locked();
}

PublisherIncarnation Registry::highest_incarnation(const PublisherId& publisher) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = publisher_watermark_.find(publisher.str());
  if (found == publisher_watermark_.end()) {
    return PublisherIncarnation{0};
  }
  return found->second;
}

// ---------------------------------------------------------------------------
// Contracts
// ---------------------------------------------------------------------------
Result<ContractDecision> Registry::rejection(const ContractRequest& request,
                                             std::string reason) const {
  ContractDecision decision;
  decision.contract = request.contract;
  decision.contract_generation = request.contract_generation;
  decision.outcome = Outcome::Rejected;
  decision.class_id = request.class_id;
  decision.class_generation = request.class_generation;
  decision.subject = request.subject;
  decision.subject_generation = request.subject_generation;
  decision.path = request.path;
  decision.path_generation = request.path_generation;
  decision.policy = request.policy;
  decision.policy_generation = request.policy_generation;
  decision.priority = request.priority;
  decision.priority_generation = request.priority_generation;
  decision.fabric_epoch = current_epoch_;
  decision.observed_epoch = request.observed_epoch;
  decision.decided_at_ms = now_ms_;
  decision.request_digest = compute_digest(request);
  decision.primary_reason = reason.substr(0, limits::kMaxReasonLen);
  decision.provenance = Provenance{std::string("registry"), current_epoch_, Generation{1},
                                   Sequence{1}};
  decision.authority.push_back(AuthorityEntry{AuthorityEntry::Dimension::FabricEpoch, "fabric",
                                              Generation{current_epoch_.value()}, std::string{},
                                              current_epoch_, std::string("registry")});
  decision.decision_digest = compute_decision_digest(decision);
  return decision;
}

Result<void> Registry::record_decision(const ContractDecision& decision) {
  ByteWriter writer(limits::kMaxCanonicalRecordBytes);
  canonical_encode(decision, writer);
  if (!writer.ok()) {
    return make_error(ErrorCode::TooLarge, "decision exceeds the canonical record budget");
  }
  const auto staged = stage_with_retry(RecordType::RecordDecision, writer.data());
  if (!staged) {
    return staged.error();
  }
  const std::string key = key_of(decision.contract.view(), decision.contract_generation);
  const bool created = decisions_.find(key) == decisions_.end();
  if (created) {
    decision_order_.push_back(decision.contract);
  }
  decisions_[key] = decision;
  const auto committed = store_->Commit(AttemptId{staged->value()}, decision.decision_digest);
  if (!committed) {
    decisions_.erase(key);
    if (created) {
      decision_order_.pop_back();
    }
    return committed.error();
  }
  prune_decisions();
  return {};
}

Result<ContractDecision> Registry::Evaluate(const ContractRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  tick();
  const auto request_valid = validate(request);
  if (!request_valid) {
    auto decision = rejection(request, std::string("request rejected at intake: ") +
                                           request_valid.error().detail());
    if (!decision) {
      return decision.error();
    }
    const auto recorded = record_decision(*decision);
    if (!recorded) {
      return recorded.error();
    }
    return decision;
  }

  const auto policy = policy_for(request.policy, request.policy_generation);
  if (!policy) {
    auto decision = rejection(request, "policy definition is absent from the registry");
    if (!decision) {
      return decision.error();
    }
    const auto recorded = record_decision(*decision);
    if (!recorded) {
      return recorded.error();
    }
    return decision;
  }
  const auto path = path_for(request.path, request.path_generation);
  if (!path) {
    auto decision = rejection(request, "path definition is absent from the registry");
    if (!decision) {
      return decision.error();
    }
    const auto recorded = record_decision(*decision);
    if (!recorded) {
      return recorded.error();
    }
    return decision;
  }
  const auto klass = class_for(request.class_id, request.class_generation);
  if (!klass) {
    auto decision = rejection(request, "service class definition is absent from the registry");
    if (!decision) {
      return decision.error();
    }
    const auto recorded = record_decision(*decision);
    if (!recorded) {
      return recorded.error();
    }
    return decision;
  }

  EvaluationInputs inputs;
  inputs.request = request;
  inputs.klass = *klass;
  inputs.policy = *policy;
  inputs.path = *path;
  inputs.current_epoch = current_epoch_;
  inputs.now_ms = now_ms_;
  inputs.evaluator = "registry:" + store_->store_id().substr(0, 16);
  inputs.conflicts = conflicts_;

  // Evidence is supplied at the newest generation the registry holds, never at
  // the generation the path bound. The engine is then the single place that
  // decides whether a generation difference is acceptable, so a stale binding
  // can never be silently satisfied by fresh evidence or vice versa.
  inputs.capabilities.reserve(inputs.path.components.size());
  inputs.reservations.reserve(inputs.path.components.size());
  for (const PathComponent& component : inputs.path.components) {
    std::optional<ResourceCapability> capability;
    const auto held = find_latest(capabilities_, component.resource.view());
    if (held) {
      capability = **held;
    }
    inputs.capabilities.push_back(std::move(capability));

    std::optional<Reservation> reservation;
    const auto reserved = find_latest(reservations_, component.resource.view());
    if (reserved) {
      reservation = **reserved;
    }
    inputs.reservations.push_back(std::move(reservation));
  }

  const auto currency_class = class_for(request.class_id, Generation{0});
  if (currency_class) {
    inputs.currency.latest_class_generation = currency_class->generation;
  }
  const auto currency_policy = policy_for(request.policy, Generation{0});
  if (currency_policy) {
    inputs.currency.latest_policy_generation = currency_policy->generation;
  }
  const auto currency_path = path_for(request.path, Generation{0});
  if (currency_path) {
    inputs.currency.latest_path_generation = currency_path->generation;
  }

  auto evaluated = evaluate(inputs);
  if (!evaluated) {
    return evaluated.error();
  }
  const auto recorded = record_decision(*evaluated);
  if (!recorded) {
    return recorded.error();
  }
  return evaluated;
}

Result<ContractDecision> Registry::GetDecision(const QoSContractId& id,
                                               Generation generation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = decisions_.find(key_of(id.view(), generation));
  if (found == decisions_.end()) {
    return make_error(ErrorCode::NotFound, "no decision is recorded for that contract");
  }
  return found->second;
}

Result<std::vector<QoSContractId>> Registry::list_contracts() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return decision_order_;
}

// ---------------------------------------------------------------------------
// Housekeeping
// ---------------------------------------------------------------------------
void Registry::prune_decisions() {
  while (decision_order_.size() > limits::kMaxDecisionsRetained) {
    const QoSContractId oldest = decision_order_.front();
    decision_order_.erase(decision_order_.begin());
    // Several generations of one contract may be retained; only drop the
    // stored entry once no retained generation still references it.
    bool still_referenced = false;
    for (const QoSContractId& id : decision_order_) {
      if (id == oldest) {
        still_referenced = true;
        break;
      }
    }
    if (still_referenced) {
      continue;
    }
    for (auto it = decisions_.begin(); it != decisions_.end();) {
      if (it->second.contract == oldest) {
        it = decisions_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void Registry::prune() {
  auto prune_table = [this](auto& table) {
    for (auto& entry : table) {
      auto& list = entry.second;
      if (list.size() <= options_.max_generations_per_id) {
        continue;
      }
      std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.generation < b.generation;
      });
      list.erase(list.begin(), list.end() - static_cast<std::ptrdiff_t>(
                                                 options_.max_generations_per_id));
    }
  };
  prune_table(classes_);
  prune_table(policies_);
  prune_table(paths_);
  prune_table(capabilities_);
  prune_table(reservations_);
  prune_decisions();
  if (conflicts_.size() > kMaxStoredConflicts) {
    conflicts_.erase(conflicts_.begin(),
                     conflicts_.begin() + static_cast<std::ptrdiff_t>(conflicts_.size() -
                                                                     kMaxStoredConflicts));
  }
}

Result<void> Registry::compact_locked() {
  prune();
  const auto bytes = encode_snapshot_state();
  if (!bytes) {
    return bytes.error();
  }
  return store_->WriteSnapshot(*bytes);
}

Result<void> Registry::Verify() {
  std::lock_guard<std::mutex> guard(mutex_);
  return store_->Verify();
}

RegistryCounts Registry::counts() const {
  std::lock_guard<std::mutex> guard(mutex_);
  RegistryCounts out;
  auto count_generations = [](const auto& table) {
    std::size_t total = 0;
    for (const auto& entry : table) {
      total += entry.second.size();
    }
    return total;
  };
  {
    std::vector<std::string> ids;
    for (const auto& entry : classes_) {
      if (!entry.second.empty()) {
        ids.push_back(entry.second.back().id.str());
      }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    out.classes = ids.size();
  }
  {
    std::vector<std::string> ids;
    for (const auto& entry : policies_) {
      if (!entry.second.empty()) {
        ids.push_back(entry.second.back().id.str());
      }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    out.policies = ids.size();
  }
  {
    std::vector<std::string> ids;
    for (const auto& entry : paths_) {
      if (!entry.second.empty()) {
        ids.push_back(entry.second.back().id.str());
      }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    out.paths = ids.size();
  }
  out.capabilities = count_generations(capabilities_);
  out.reservations = count_generations(reservations_);
  out.publishers = publisher_watermark_.size();
  out.decisions = decisions_.size();
  out.conflicts = conflicts_.size();
  out.live_leases = leases_.size();
  return out;
}

Result<void> Registry::Close() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_ == nullptr) {
    return {};
  }
  return store_->Close();
}

}  // namespace qosfabric




