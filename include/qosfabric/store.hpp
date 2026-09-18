// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Crash-safe durable log store.
//
// Layout, all inside one directory:
//   MANIFEST.qfm           small, atomically replaced, carries format version,
//                          store identity, epoch, snapshot reference and CRC
//   SNAPSHOT-<seq>.qsn     full state at a committed sequence
//   WAL.qwl                framed append-only records since the snapshot
//
// Durability protocol for every mutation:
//   validate -> bind authority -> plan -> reserve -> journal intent (fsync)
//   -> apply -> verify -> journal commit (fsync) -> retire
// A mutation is never acknowledged before its commit frame is durable, and an
// intent without a commit is reported as an unfinished attempt on recovery
// rather than being silently applied.

#ifndef QOSFABRIC_STORE_HPP
#define QOSFABRIC_STORE_HPP

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "qosfabric/crypto.hpp"
#include "qosfabric/error.hpp"
#include "qosfabric/ids.hpp"
#include "qosfabric/limits.hpp"
#include "qosfabric/version.hpp"

namespace qosfabric {

enum class RecordType : std::uint16_t {
  Unknown = 0,
  StoreHeader = 1,
  EpochAdvance = 2,
  PublishClass = 3,
  PublishPolicy = 4,
  PublishPath = 5,
  PublishCapability = 6,
  PublishReservation = 7,
  RegisterPublisher = 8,
  RevokePublisher = 9,
  RecordDecision = 10,
  MutationIntent = 11,
  MutationCommit = 12,
  ConflictRecord = 13,
  PublisherLeaseCleared = 14,
  MutationAbort = 15,
  CleanShutdown = 16,
};

[[nodiscard]] const char* to_string(RecordType value) noexcept;

class LogRecord {
 public:
  RecordType type{RecordType::Unknown};
  Sequence sequence{};
  std::vector<std::uint8_t> payload{};
};

class RecoveryReport {
 public:
  bool store_created{false};
  bool opened_clean{false};
  bool manifest_rebuilt{false};
  bool snapshot_loaded{false};
  std::uint64_t snapshot_sequence{0};
  std::uint64_t replayed_records{0};
  std::uint64_t truncated_tail_bytes{0};
  std::uint64_t unfinished_intents{0};
  std::uint64_t discarded_intents{0};
  // Set when a checksum failure is not a torn tail write but genuine mid-log
  // corruption. The store then refuses to open rather than discard data.
  bool mid_log_corruption{false};
  std::uint64_t epoch_advances{0};
  std::uint64_t last_sequence{0};
  std::string detail{};
};

class StoreOptions {
 public:
  std::string directory{};
  std::uint16_t format_version{kStoreFormatVersion};
  bool create_if_missing{true};
  // Refuse appends once the write-ahead log exceeds this budget, forcing the
  // owner to compact. Bounds durable growth unconditionally.
  std::uint64_t max_wal_bytes{limits::kMaxSnapshotBytes};
  std::size_t max_record_bytes{limits::kMaxWalRecordBytes};
  // When false (the default) a complete frame whose checksum does not match
  // is a hard error: the store refuses to open and reports the corruption
  // instead of silently discarding data.
  bool repair_truncate_corrupt_tail{false};
};

class Store {
 public:
  static Result<std::unique_ptr<Store>> Open(const StoreOptions& options);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Records replayed from the durable log during Open, in commit order. An
  // unfinished intent never appears here.
  [[nodiscard]] const std::vector<LogRecord>& replayed() const noexcept { return replayed_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const std::string& store_id() const noexcept { return store_id_; }
  [[nodiscard]] Sequence last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] FabricEpoch durable_epoch() const noexcept { return durable_epoch_; }
  [[nodiscard]] std::uint64_t wal_bytes() const noexcept { return wal_bytes_; }
  // The verified snapshot payload recovered during Open, if the manifest
  // referenced one. Empty when the store has no snapshot.
  [[nodiscard]] const std::vector<std::uint8_t>& snapshot_bytes() const noexcept {
    return snapshot_bytes_;
  }

  // --- Single-phase append ------------------------------------------------
  // Used for records whose presence *is* the state (epoch advance, clean
  // shutdown, conflict markers). Durable when this returns.
  Result<Sequence> Append(RecordType type, const std::vector<std::uint8_t>& payload);

  // --- Two-phase journalled mutation --------------------------------------
  // Stage writes and fsyncs the intent. The caller then applies the change in
  // memory, and only after that calls Commit, which fsyncs the commit frame.
  Result<AttemptId> Stage(RecordType type, const std::vector<std::uint8_t>& payload);
  Result<void> Commit(AttemptId attempt, const Digest256& applied_digest);
  Result<void> Abort(AttemptId attempt, std::string_view reason);

  // Persists the snapshot atomically and retires the log prefix it covers.
  Result<void> WriteSnapshot(const std::vector<std::uint8_t>& snapshot);

  // Journals a clean-shutdown record, fsyncs, and stops accepting appends.
  Result<void> Close();

  // Adopts a freshly generated store identity and publishes it durably.
  Result<void> AdoptStoreIdentity(const std::string& store_id);

  // Re-reads every durable artifact and verifies its integrity.
  Result<void> Verify() const;

 private:
  Store() = default;

  // Every public operation takes this lock. Internal helpers assume it is
  // already held, so no path re-enters it and there is exactly one lock in the
  // process-wide acquisition order: registry state, then store state.
  mutable std::mutex mutex_{};

  Result<void> open_impl(const StoreOptions& options);
  Result<void> load_manifest(const StoreOptions& options);
  Result<void> rebuild_manifest(const StoreOptions& options);
  Result<void> load_snapshot();
  Result<void> replay_log(const StoreOptions& options);
  Result<void> write_manifest(const StoreOptions& options);
  Result<void> reopen_wal();
  Result<void> fsync_wal();
  Result<void> append_frame(RecordType type, std::uint8_t flags, AttemptId attempt,
                            const std::vector<std::uint8_t>& payload, Sequence& out_sequence);
  Result<void> close_locked();

  StoreOptions options_{};
  std::string store_id_{};
  std::string manifest_path_{};
  std::string wal_path_{};
  std::string snapshot_path_{};
  std::FILE* wal_handle_{nullptr};
  Sequence last_sequence_{};
  FabricEpoch durable_epoch_{};
  std::uint64_t wal_bytes_{0};
  std::uint64_t snapshot_sequence_{0};
  std::uint32_t staged_count_{0};
  std::vector<LogRecord> replayed_{};
  std::vector<std::uint8_t> snapshot_bytes_{};
  RecoveryReport recovery_{};
  bool read_only_{false};
};

// Filesystem helpers shared with the rest of the runtime.
[[nodiscard]] Result<bool> path_exists(const std::string& path);
[[nodiscard]] Result<void> ensure_directory(const std::string& path);
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::string& path,
                                                          std::size_t max_bytes);
[[nodiscard]] Result<void> write_file_atomic(const std::string& path,
                                             const std::vector<std::uint8_t>& bytes);
[[nodiscard]] Result<void> remove_file(const std::string& path);
[[nodiscard]] Result<std::vector<std::string>> list_directory(const std::string& path);

}  // namespace qosfabric

#endif  // QOSFABRIC_STORE_HPP
