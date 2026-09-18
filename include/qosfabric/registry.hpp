// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The registry is the durable owner of service-class definitions, policies,
// paths, capability declarations, reservations, conflict evidence, publisher
// fencing watermarks and recorded contract decisions. It is also the facade
// that binds a contract request to the exact generations and evidence that
// are current, and hands them to the pure evaluation engine.
//
// Liveness is never restored from durable state. A lease, a publisher
// authority or a capability freshness window that existed before a restart is
// gone afterwards: the fabric epoch advances on every open, and every lease
// is discarded, so a restarted process must re-establish authority.

#ifndef QOSFABRIC_REGISTRY_HPP
#define QOSFABRIC_REGISTRY_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "qosfabric/decision.hpp"
#include "qosfabric/engine.hpp"
#include "qosfabric/model.hpp"
#include "qosfabric/store.hpp"

namespace qosfabric {

// Millisecond wall clock used for freshness accounting. Exposed so that tests
// and replay tools can drive the runtime from an explicit clock.
[[nodiscard]] std::uint64_t system_now_millis() noexcept;

class PublisherLease {
 public:
  PublisherId publisher{};
  PublisherIncarnation incarnation{};
  FabricEpoch epoch{};
  LeaseId lease{};
  BootId boot{};
  std::uint64_t granted_at_ms{0};
  std::uint64_t expires_at_ms{0};
};

class RegistryOptions {
 public:
  std::string directory{};
  bool create_if_missing{true};
  bool repair_truncate_corrupt_tail{false};
  // Claiming authority advances the fabric epoch durably and discards every
  // lease issued by the previous incarnation. A purely observational open
  // (status, integrity verification) claims nothing and therefore changes
  // nothing, so inspection can never fence a running fabric by accident.
  bool claim_authority{true};
  std::uint64_t max_wal_bytes{limits::kMaxSnapshotBytes};
  std::size_t max_record_bytes{limits::kMaxWalRecordBytes};
  std::size_t max_generations_per_id{limits::kMaxGenerationsPerId};
};

class RegistryCounts {
 public:
  std::size_t classes{0};
  std::size_t policies{0};
  std::size_t paths{0};
  std::size_t capabilities{0};
  std::size_t reservations{0};
  std::size_t publishers{0};
  std::size_t decisions{0};
  std::size_t conflicts{0};
  std::size_t live_leases{0};
};

class PublicationResult {
 public:
  Generation generation{};
  Sequence sequence{};
  Digest256 digest{};
};

class Registry {
 public:
  static Result<std::unique_ptr<Registry>> Open(const RegistryOptions& options);
  ~Registry();
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  Result<void> Close();

  // --- Authority ----------------------------------------------------------
  [[nodiscard]] FabricEpoch current_epoch() const;
  [[nodiscard]] FabricEpoch durable_epoch() const;
  // Advances the fabric epoch durably and monotonically. Every authoritative
  // open advances it once, which is what fences a previous incarnation.
  Result<FabricEpoch> AdvanceEpoch(std::string_view reason);

  // --- Publication --------------------------------------------------------
  Result<PublicationResult> PublishClass(const QoSClass& value, std::string_view origin);
  Result<PublicationResult> PublishPolicy(const Policy& value, std::string_view origin);
  Result<PublicationResult> PublishPath(const Path& value, std::string_view origin);
  Result<PublicationResult> PublishCapability(const ResourceCapability& value);
  Result<PublicationResult> PublishReservation(const Reservation& value);

  // --- Reads --------------------------------------------------------------
  Result<QoSClass> GetClass(const QoSClassId& id, Generation generation) const;
  Result<QoSClass> LatestClass(const QoSClassId& id) const;
  Result<Policy> GetPolicy(const PolicyId& id, Generation generation) const;
  Result<Policy> LatestPolicy(const PolicyId& id) const;
  Result<Path> GetPath(const PathId& id, Generation generation) const;
  Result<Path> LatestPath(const PathId& id) const;
  Result<ResourceCapability> GetCapability(const ResourceId& id, Generation generation) const;
  Result<ResourceCapability> LatestCapability(const ResourceId& id) const;
  Result<Reservation> GetReservation(const ResourceId& id, Generation generation) const;
  Result<Reservation> LatestReservation(const ResourceId& id) const;

  [[nodiscard]] Result<std::vector<QoSClassId>> list_classes() const;
  [[nodiscard]] Result<std::vector<PolicyId>> list_policies() const;
  [[nodiscard]] Result<std::vector<PathId>> list_paths() const;
  [[nodiscard]] Result<std::vector<ResourceId>> list_resources() const;
  [[nodiscard]] std::vector<ConflictMarker> conflicts() const;

  // --- Publishers ---------------------------------------------------------
  // Accepts a strictly newer publisher incarnation only. An incarnation at or
  // below the durable watermark is fenced: the caller has lost authority and
  // must not be able to publish under an identity that was already used.
  Result<PublisherLease> RegisterPublisher(const PublisherId& publisher,
                                           PublisherIncarnation incarnation, const BootId& boot,
                                           std::uint64_t ttl_ms, std::string_view origin);
  Result<void> RevokePublisher(const PublisherId& publisher, PublisherIncarnation incarnation);
  Result<void> ExpireLeases();
  [[nodiscard]] std::optional<PublisherLease> FindLease(const PublisherId& publisher) const;
  [[nodiscard]] PublisherIncarnation highest_incarnation(const PublisherId& publisher) const;

  // --- Contracts ----------------------------------------------------------
  // Resolves the request against current authority, evaluates end to end and
  // records the decision durably. The decision is durable when this returns.
  Result<ContractDecision> Evaluate(const ContractRequest& request);
  Result<ContractDecision> GetDecision(const QoSContractId& id, Generation generation) const;
  [[nodiscard]] Result<std::vector<QoSContractId>> list_contracts() const;

  // --- Housekeeping -------------------------------------------------------
  Result<void> Compact();
  Result<void> Verify();
  [[nodiscard]] RegistryCounts counts() const;
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return store_->recovery(); }
  [[nodiscard]] const std::string& store_id() const noexcept { return store_->store_id(); }

  // Pins the clock used for freshness accounting. Test and replay hook: while
  // pinned, the runtime never reads the system clock.
  void set_now_millis(std::uint64_t value);
  [[nodiscard]] std::uint64_t now_millis() const;

 private:
  Registry() = default;

  // Single lock for all registry state. Public methods take it once at entry;
  // every helper below assumes it is held, so no path re-enters it. The only
  // permitted nesting is registry state -> store state, never the reverse, and
  // no callback is ever invoked while the lock is held.
  mutable std::mutex mutex_{};

  [[nodiscard]] Result<QoSClass> class_for(const QoSClassId& id, Generation generation) const;
  [[nodiscard]] Result<Policy> policy_for(const PolicyId& id, Generation generation) const;
  [[nodiscard]] Result<Path> path_for(const PathId& id, Generation generation) const;
  [[nodiscard]] std::optional<PublisherLease> find_lease(const PublisherId& publisher) const;
  Result<FabricEpoch> advance_epoch_locked(std::string_view reason);
  Result<void> compact_locked();

  Result<void> open_impl(const RegistryOptions& options);
  Result<void> load_snapshot_state();
  Result<void> apply_record(const LogRecord& record);
  Result<void> rebuild_from_log();
  Result<std::vector<std::uint8_t>> encode_snapshot_state() const;
  Result<Sequence> stage_with_retry(RecordType type, const std::vector<std::uint8_t>& payload);
  Result<void> record_decision(const ContractDecision& decision);
  template <class T, class Table>
  Result<PublicationResult> publish_entity(Table& table, RecordType type, T value,
                                           std::size_t max_identifiers);
  void prune();
  void prune_decisions();
  Result<ContractDecision> rejection(const ContractRequest& request, std::string reason) const;
  Result<ContractDecision> evaluate_internal(const ContractRequest& request,
                                             const std::string& reject_reason);

  RegistryOptions options_{};
  std::unique_ptr<Store> store_{};

  std::unordered_map<std::string, std::vector<QoSClass>> classes_{};
  std::unordered_map<std::string, std::vector<Policy>> policies_{};
  std::unordered_map<std::string, std::vector<Path>> paths_{};
  std::unordered_map<std::string, std::vector<ResourceCapability>> capabilities_{};
  std::unordered_map<std::string, std::vector<Reservation>> reservations_{};
  std::unordered_map<std::string, PublisherIncarnation> publisher_watermark_{};
  std::unordered_map<std::string, PublisherLease> leases_{};
  std::vector<ConflictMarker> conflicts_{};
  std::vector<QoSContractId> decision_order_{};
  std::unordered_map<std::string, ContractDecision> decisions_{};

  FabricEpoch durable_epoch_{};
  FabricEpoch current_epoch_{};
  void tick() noexcept {
    if (!clock_pinned_) {
      now_ms_ = system_now_millis();
    }
  }

  std::uint64_t now_ms_{0};
  bool clock_pinned_{false};
};

}  // namespace qosfabric

#endif  // QOSFABRIC_REGISTRY_HPP
