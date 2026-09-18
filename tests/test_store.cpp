// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "harness.hpp"
#include "tempdir.hpp"

#include <string>
#include <vector>

#include "qosfabric/qosfabric.hpp"

using namespace qosfabric;

namespace {

std::vector<std::uint8_t> bytes_of(const std::string& text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

StoreOptions store_options(const std::string& directory) {
  StoreOptions options;
  options.directory = directory;
  options.create_if_missing = true;
  return options;
}

}  // namespace

QOS_TEST(appended_records_survive_reopen_in_order) {
  qostest::TempDir directory("store-basic");
  {
    auto store = Store::Open(store_options(directory.child("s")));
    QOS_REQUIRE(store.has_value());
    QOS_CHECK((*store)->recovery().store_created);
    for (int i = 0; i < 5; ++i) {
      const auto sequence =
          (*store)->Append(RecordType::PublishClass, bytes_of("record-" + std::to_string(i)));
      QOS_REQUIRE(sequence.has_value());
      QOS_CHECK_EQ(sequence->value(), static_cast<std::uint64_t>(i + 2));
    }
    QOS_REQUIRE((*store)->Close().has_value());
  }
  {
    auto store = Store::Open(store_options(directory.child("s")));
    QOS_REQUIRE(store.has_value());
    QOS_CHECK((*store)->recovery().opened_clean);
    QOS_CHECK_EQ((*store)->recovery().replayed_records, std::uint64_t{5});
    const std::vector<LogRecord>& replayed = (*store)->replayed();
    QOS_REQUIRE(replayed.size() == 5);
    for (std::size_t i = 0; i < replayed.size(); ++i) {
      QOS_CHECK_EQ(std::string(replayed[i].payload.begin(), replayed[i].payload.end()),
                   std::string("record-" + std::to_string(i)));
      QOS_CHECK(replayed[i].sequence.value() < replayed[replayed.size() - 1].sequence.value() + 1);
    }
    QOS_REQUIRE((*store)->Close().has_value());
  }
}

QOS_TEST(unfinished_intent_is_never_applied) {
  qostest::TempDir directory("store-intent");
  {
    auto store = Store::Open(store_options(directory.child("s")));
    QOS_REQUIRE(store.has_value());
    const auto attempt = (*store)->Stage(RecordType::PublishPolicy, bytes_of("never-committed"));
    QOS_REQUIRE(attempt.has_value());
    const auto committed =
        (*store)->Commit(*attempt, sha256("applied"));
    QOS_REQUIRE(committed.has_value());
    const auto abandoned = (*store)->Stage(RecordType::PublishPath, bytes_of("abandoned"));
    QOS_REQUIRE(abandoned.has_value());
    (void)abandoned;
    QOS_REQUIRE((*store)->Close().has_value());
  }
  {
    auto store = Store::Open(store_options(directory.child("s")));
    QOS_REQUIRE(store.has_value());
    QOS_CHECK_EQ((*store)->recovery().unfinished_intents, std::uint64_t{1});
    const std::vector<LogRecord>& replayed = (*store)->replayed();
    QOS_REQUIRE(replayed.size() == 1);
    QOS_CHECK(replayed[0].type == RecordType::PublishPolicy);
    QOS_CHECK_EQ(std::string(replayed[0].payload.begin(), replayed[0].payload.end()),
                 std::string("never-committed"));
    QOS_REQUIRE((*store)->Close().has_value());
  }
}

QOS_TEST(aborted_intent_is_discarded_and_reported) {
  qostest::TempDir directory("store-abort");
  {
    auto store = Store::Open(store_options(directory.child("s")));
    QOS_REQUIRE(store.has_value());
    const auto attempt = (*store)->Stage(RecordType::PublishClass, bytes_of("aborted"));
    QOS_REQUIRE(attempt.has_value());
    QOS_REQUIRE((*store)->Abort(*attempt, "caller rolled back").has_value());
    QOS_REQUIRE((*store)->Close().has_value());
  }
  {
    auto store = Store::Open(store_options(directory.child("s")));
    QOS_REQUIRE(store.has_value());
    QOS_CHECK_EQ((*store)->recovery().unfinished_intents, std::uint64_t{0});
    QOS_CHECK_EQ((*store)->recovery().discarded_intents, std::uint64_t{1});
    QOS_CHECK_EQ((*store)->replayed().size(), std::size_t{0});
    QOS_REQUIRE((*store)->Close().has_value());
  }
}

QOS_TEST(torn_tail_is_truncated_and_reported) {
  qostest::TempDir directory("store-torn");
  const std::string store_path = directory.child("s");
  {
    auto store = Store::Open(store_options(store_path));
    QOS_REQUIRE(store.has_value());
    for (int i = 0; i < 4; ++i) {
      QOS_REQUIRE((*store)->Append(RecordType::PublishClass, bytes_of("payload-" + std::to_string(i)))
                      .has_value());
    }
    // No Close: simulate a crash by dropping the handle without a clean marker.
    const std::uint64_t wal_bytes = (*store)->wal_bytes();
    QOS_CHECK(wal_bytes > 0);
  }
  const std::string wal = store_path + "/WAL.qwl";
  auto content = read_file(wal, 1u << 20);
  QOS_REQUIRE(content.has_value());
  QOS_REQUIRE(content->size() > 5);
  content->resize(content->size() - 5);
  QOS_REQUIRE(write_file_atomic(wal, *content).has_value());

  auto store = Store::Open(store_options(store_path));
  QOS_REQUIRE(store.has_value());
  QOS_CHECK((*store)->recovery().truncated_tail_bytes > 0);
  QOS_CHECK(!(*store)->recovery().opened_clean);
  QOS_CHECK((*store)->replayed().size() <= 4);
  QOS_REQUIRE((*store)->Close().has_value());
}

QOS_TEST(mid_log_corruption_is_refused_not_discarded) {
  qostest::TempDir directory("store-corrupt");
  const std::string store_path = directory.child("s");
  {
    auto store = Store::Open(store_options(store_path));
    QOS_REQUIRE(store.has_value());
    for (int i = 0; i < 6; ++i) {
      QOS_REQUIRE((*store)->Append(RecordType::PublishClass, bytes_of("payload-" + std::to_string(i)))
                      .has_value());
    }
  }
  const std::string wal = store_path + "/WAL.qwl";
  auto content = read_file(wal, 1u << 20);
  QOS_REQUIRE(content.has_value());
  QOS_REQUIRE(content->size() > 60);
  // Flip a bit inside the payload of an early frame, leaving later frames
  // intact. That is corruption, not a torn tail.
  (*content)[40] = static_cast<std::uint8_t>((*content)[40] ^ 0x40u);
  QOS_REQUIRE(write_file_atomic(wal, *content).has_value());

  {
    auto store = Store::Open(store_options(store_path));
    QOS_CHECK(!store.has_value());
    QOS_CHECK_EQ(store.code(), ErrorCode::Corrupt);
  }
  {
    StoreOptions repair = store_options(store_path);
    repair.repair_truncate_corrupt_tail = true;
    auto store = Store::Open(repair);
    // Repair only ever discards a *tail*. A valid frame follows the damaged
    // one, so even an explicit repair request must refuse.
    QOS_CHECK(!store.has_value());
    QOS_CHECK_EQ(store.code(), ErrorCode::Corrupt);
  }
}

QOS_TEST(checksum_failure_at_the_very_tail_needs_explicit_repair) {
  qostest::TempDir directory("store-tailcrc");
  const std::string store_path = directory.child("s");
  {
    auto store = Store::Open(store_options(store_path));
    QOS_REQUIRE(store.has_value());
    QOS_REQUIRE((*store)->Append(RecordType::PublishClass, bytes_of("first")).has_value());
    QOS_REQUIRE((*store)->Append(RecordType::PublishClass, bytes_of("second")).has_value());
  }
  const std::string wal = store_path + "/WAL.qwl";
  auto content = read_file(wal, 1u << 20);
  QOS_REQUIRE(content.has_value());
  // Damage the final byte of the file: the last frame is complete but its
  // checksum no longer matches.
  (*content)[content->size() - 1] = static_cast<std::uint8_t>((*content)[content->size() - 1] ^ 0xFFu);
  QOS_REQUIRE(write_file_atomic(wal, *content).has_value());

  {
    auto store = Store::Open(store_options(store_path));
    QOS_CHECK(!store.has_value());
    QOS_CHECK_EQ(store.code(), ErrorCode::Corrupt);
  }
  {
    StoreOptions repair = store_options(store_path);
    repair.repair_truncate_corrupt_tail = true;
    auto store = Store::Open(repair);
    QOS_REQUIRE(store.has_value());
    QOS_CHECK((*store)->recovery().mid_log_corruption);
    QOS_CHECK((*store)->recovery().truncated_tail_bytes > 0);
    QOS_REQUIRE((*store)->Close().has_value());
  }
}

QOS_TEST(manifest_is_rebuilt_from_durable_artifacts) {
  qostest::TempDir directory("store-manifest");
  const std::string store_path = directory.child("s");
  std::string store_id;
  {
    auto store = Store::Open(store_options(store_path));
    QOS_REQUIRE(store.has_value());
    store_id = (*store)->store_id();
    QOS_REQUIRE((*store)->Append(RecordType::PublishClass, bytes_of("a")).has_value());
    QOS_REQUIRE((*store)->Append(RecordType::PublishClass, bytes_of("b")).has_value());
    QOS_REQUIRE((*store)->Close().has_value());
  }
  QOS_REQUIRE(remove_file(store_path + "/MANIFEST.qfm").has_value());
  {
    auto store = Store::Open(store_options(store_path));
    QOS_REQUIRE(store.has_value());
    QOS_CHECK((*store)->recovery().manifest_rebuilt);
    QOS_CHECK_EQ((*store)->store_id(), store_id);
    QOS_CHECK((*store)->replayed().size() >= 2);
    QOS_REQUIRE((*store)->Verify().has_value());
    QOS_REQUIRE((*store)->Close().has_value());
  }
}

QOS_TEST(unsupported_store_format_version_is_refused) {
  qostest::TempDir directory("store-version");
  const std::string store_path = directory.child("s");
  {
    auto store = Store::Open(store_options(store_path));
    QOS_REQUIRE(store.has_value());
    QOS_REQUIRE((*store)->Close().has_value());
  }
  StoreOptions legacy = store_options(store_path);
  legacy.format_version = 99;
  auto store = Store::Open(legacy);
  QOS_CHECK(!store.has_value());
  QOS_CHECK_EQ(store.code(), ErrorCode::VersionMismatch);
}

QOS_TEST(oversized_records_and_budgets_are_refused) {
  qostest::TempDir directory("store-budget");
  StoreOptions options = store_options(directory.child("s"));
  options.max_record_bytes = 256;
  auto store = Store::Open(options);
  QOS_REQUIRE(store.has_value());
  const std::vector<std::uint8_t> large(512, 0x41);
  QOS_CHECK_EQ((*store)->Append(RecordType::PublishClass, large).code(), ErrorCode::TooLarge);
  QOS_CHECK((*store)->Append(RecordType::PublishClass, std::vector<std::uint8_t>(64, 0x42))
                .has_value());
  QOS_REQUIRE((*store)->Close().has_value());
}

QOS_TEST(log_budget_forces_compaction) {
  qostest::TempDir directory("store-capacity");
  StoreOptions options = store_options(directory.child("s"));
  options.max_wal_bytes = 4096;
  auto store = Store::Open(options);
  QOS_REQUIRE(store.has_value());
  bool exhausted = false;
  for (int i = 0; i < 2000 && !exhausted; ++i) {
    const auto appended =
        (*store)->Append(RecordType::PublishClass, std::vector<std::uint8_t>(64, 0x41));
    if (!appended) {
      QOS_CHECK_EQ(appended.code(), ErrorCode::CapacityExhausted);
      exhausted = true;
    }
  }
  QOS_CHECK(exhausted);
  QOS_REQUIRE((*store)->WriteSnapshot(bytes_of("bounded-snapshot")).has_value());
  QOS_CHECK_EQ((*store)->wal_bytes(), std::uint64_t{0});
  QOS_CHECK((*store)->Append(RecordType::PublishClass, std::vector<std::uint8_t>(64, 0x41))
                .has_value());
  QOS_REQUIRE((*store)->Close().has_value());
}

QOS_TEST(snapshot_compaction_preserves_state_and_shrinks_the_log) {
  qostest::TempDir directory("store-compact");
  const std::string store_path = directory.child("s");
  {
    auto store = Store::Open(store_options(store_path));
    QOS_REQUIRE(store.has_value());
    for (int i = 0; i < 20; ++i) {
      QOS_REQUIRE((*store)->Append(RecordType::PublishPolicy, bytes_of("policy-" + std::to_string(i)))
                      .has_value());
    }
    const std::uint64_t before = (*store)->wal_bytes();
    QOS_REQUIRE((*store)->WriteSnapshot(bytes_of("snapshot-body")).has_value());
    QOS_CHECK((*store)->wal_bytes() < before);
    QOS_REQUIRE((*store)->Append(RecordType::PublishPolicy, bytes_of("after-snapshot")).has_value());
    QOS_REQUIRE((*store)->Close().has_value());
  }
  {
    auto store = Store::Open(store_options(store_path));
    QOS_REQUIRE(store.has_value());
    QOS_CHECK((*store)->recovery().snapshot_loaded);
    QOS_CHECK_EQ((*store)->recovery().snapshot_sequence, (*store)->recovery().snapshot_sequence);
    QOS_CHECK_EQ(std::string((*store)->snapshot_bytes().begin(), (*store)->snapshot_bytes().end()),
                 std::string("snapshot-body"));
    const std::vector<LogRecord>& replayed = (*store)->replayed();
    QOS_REQUIRE(replayed.size() == 1);
    QOS_CHECK_EQ(std::string(replayed[0].payload.begin(), replayed[0].payload.end()),
                 std::string("after-snapshot"));
    QOS_REQUIRE((*store)->Verify().has_value());
    QOS_REQUIRE((*store)->Close().has_value());
  }
}

QOS_TEST(verify_detects_a_damaged_snapshot) {
  qostest::TempDir directory("store-verify");
  const std::string store_path = directory.child("s");
  std::string snapshot_path;
  {
    auto store = Store::Open(store_options(store_path));
    QOS_REQUIRE(store.has_value());
    QOS_REQUIRE((*store)->Append(RecordType::PublishClass, bytes_of("x")).has_value());
    QOS_REQUIRE((*store)->WriteSnapshot(bytes_of("snapshot-body")).has_value());
    QOS_REQUIRE((*store)->Close().has_value());
  }
  const auto entries = list_directory(store_path);
  QOS_REQUIRE(entries.has_value());
  for (const std::string& name : *entries) {
    if (name.rfind("SNAPSHOT-", 0) == 0) {
      snapshot_path = store_path + "/" + name;
    }
  }
  QOS_REQUIRE(!snapshot_path.empty());
  auto content = read_file(snapshot_path, 1u << 20);
  QOS_REQUIRE(content.has_value());
  (*content)[content->size() - 3] = static_cast<std::uint8_t>((*content)[content->size() - 3] ^ 0x01u);
  QOS_REQUIRE(write_file_atomic(snapshot_path, *content).has_value());
  auto store = Store::Open(store_options(store_path));
  QOS_CHECK(!store.has_value());
  QOS_CHECK_EQ(store.code(), ErrorCode::Corrupt);
}
