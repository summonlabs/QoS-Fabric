// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "qosfabric/checked.hpp"
#include "qosfabric/serialize.hpp"
#include "qosfabric/version.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace qosfabric {
namespace {

constexpr char kManifestMagic[8] = {'Q', 'O', 'S', 'F', 'M', 'N', 'F', '1'};
constexpr char kSnapshotMagic[8] = {'Q', 'O', 'S', 'F', 'S', 'N', 'P', '1'};
constexpr std::uint16_t kFrameVersion = 1;
constexpr std::size_t kFrameHeaderBytes = 8;    // length + checksum
constexpr std::size_t kRecordHeaderBytes = 25;  // version, type, flags, seq, attempt, body length
constexpr std::size_t kMaxWalScanBytes = limits::kMaxSnapshotBytes + (1u << 20);

enum FrameFlags : std::uint8_t {
  kFlagNone = 0,
  kFlagHasAttempt = 1u << 0,
};

std::string manifest_file_name() { return "MANIFEST.qfm"; }
std::string wal_file_name() { return "WAL.qwl"; }
std::string snapshot_prefix() { return "SNAPSHOT-"; }
std::string snapshot_suffix() { return ".qsn"; }

std::string snapshot_file_name(std::uint64_t sequence) {
  return snapshot_prefix() + std::to_string(sequence) + snapshot_suffix();
}

#ifdef _WIN32
std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
  return out;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
  if (length <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
  return out;
}
#endif

// Normalizes a UTF-8 path to the platform-native separator form.
std::string native_path(const std::string& utf8) {
#ifdef _WIN32
  std::wstring wide = widen(utf8);
  for (wchar_t& ch : wide) {
    if (ch == L'/') {
      ch = L'\\';
    }
  }
  return narrow(wide);
#else
  return utf8;
#endif
}

// Builds a native filesystem path from a UTF-8 string. Using the platform
// path type avoids the deprecated std::filesystem::u8path overloads while
// still round-tripping non-ASCII store directories on Windows.
std::filesystem::path fs_path(const std::string& utf8) {
#ifdef _WIN32
  return std::filesystem::path(widen(utf8));
#else
  return std::filesystem::path(utf8);
#endif
}

std::FILE* open_file(const std::string& path, const char* mode) {
#ifdef _WIN32
  const std::wstring wide = widen(path);
  std::wstring wide_mode;
  for (const char* p = mode; *p != 0; ++p) {
    wide_mode.push_back(static_cast<wchar_t>(*p));
  }
  // Share both ways: the store keeps the write-ahead log open for appending
  // for the whole lifetime of the process, and verification must still be able
  // to read it back without a second handle being denied.
  return ::_wfsopen(wide.c_str(), wide_mode.c_str(), _SH_DENYNO);
#else
  return std::fopen(path.c_str(), mode);
#endif
}

int file_descriptor(std::FILE* handle) {
#ifdef _WIN32
  return ::_fileno(handle);
#else
  return ::fileno(handle);
#endif
}

std::string join_path(const std::string& directory, const std::string& leaf) {
  if (directory.empty()) {
    return leaf;
  }
  const char last = directory.back();
  if (last == '/' || last == '\\') {
    return directory + leaf;
  }
  return directory + "/" + leaf;
}

void store_le(std::uint64_t value, std::uint8_t* out, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
  }
}

std::uint64_t load_le(const std::uint8_t* in, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (i * 8);
  }
  return value;
}

}  // namespace
}  // namespace qosfabric

namespace qosfabric {
namespace {

Result<void> sync_handle(std::FILE* handle) {
  if (handle == nullptr) {
    return make_error(ErrorCode::Internal, "no file handle to synchronize");
  }
  if (std::fflush(handle) != 0) {
    return make_error(ErrorCode::IoError, "flush failed");
  }
#ifdef _WIN32
  if (::_commit(file_descriptor(handle)) != 0) {
    return make_error(ErrorCode::IoError, "commit failed");
  }
#else
  if (::fsync(file_descriptor(handle)) != 0) {
    return make_error(ErrorCode::IoError, "fsync failed");
  }
#endif
  return {};
}

// Directory metadata durability. Windows has no equivalent primitive: every
// metadata file is published with an atomic replace, and that replace is the
// durability boundary there. This is therefore a documented no-op on Windows.
Result<void> sync_directory(const std::string& directory) {
#ifdef _WIN32
  (void)directory;
  return {};
#else
  const int fd = ::open(directory.c_str(), O_RDONLY);
  if (fd < 0) {
    return make_error(ErrorCode::IoError, "cannot open directory for sync");
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) {
    return make_error(ErrorCode::IoError, "directory fsync failed");
  }
  return {};
#endif
}

Result<void> atomic_replace(const std::string& from, const std::string& to) {
#ifdef _WIN32
  if (::MoveFileExW(widen(from).c_str(), widen(to).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return make_error(ErrorCode::IoError, "atomic replace failed");
  }
  return {};
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return make_error(ErrorCode::IoError, "atomic replace failed");
  }
  return {};
#endif
}

class FrameView {
 public:
  bool ok{false};
  bool incomplete{false};
  std::size_t next_offset{0};
  RecordType type{RecordType::Unknown};
  std::uint8_t flags{0};
  Sequence sequence{};
  AttemptId attempt{};
  std::string_view body{};
};

// Parses one frame starting at an offset. A frame that runs past the end of
// the file is reported as incomplete, which distinguishes a torn tail write
// from a complete frame whose checksum does not match.
FrameView parse_frame(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                      std::size_t max_record_bytes) {
  FrameView view;
  if (offset + kFrameHeaderBytes > bytes.size()) {
    view.incomplete = true;
    return view;
  }
  const std::uint32_t length = static_cast<std::uint32_t>(load_le(bytes.data() + offset, 4));
  const std::uint32_t expected_crc =
      static_cast<std::uint32_t>(load_le(bytes.data() + offset + 4, 4));
  if (length > max_record_bytes) {
    return view;
  }
  if (offset + kFrameHeaderBytes + length > bytes.size()) {
    view.incomplete = true;
    return view;
  }
  const auto* payload = bytes.data() + offset + kFrameHeaderBytes;
  if (crc32c(payload, length) != expected_crc) {
    return view;
  }
  if (length < kRecordHeaderBytes) {
    return view;
  }
  ByteReader reader(payload, length);
  const std::uint16_t version = reader.u16();
  const std::uint16_t raw_type = reader.u16();
  const std::uint8_t flags = reader.u8();
  const std::uint64_t sequence = reader.u64();
  const std::uint64_t attempt = reader.u64();
  const std::uint32_t body_length = reader.u32();
  if (reader.failed() || version != kFrameVersion) {
    return view;
  }
  if (raw_type > static_cast<std::uint16_t>(RecordType::CleanShutdown)) {
    return view;
  }
  if (body_length > length - kRecordHeaderBytes) {
    return view;
  }
  const std::string_view body_view(
      reinterpret_cast<const char*>(payload) + kRecordHeaderBytes, body_length);
  view.ok = true;
  view.next_offset = offset + kFrameHeaderBytes + length;
  view.type = static_cast<RecordType>(raw_type);
  view.flags = flags;
  view.sequence = Sequence{sequence};
  view.attempt = AttemptId{attempt};
  view.body = body_view;
  return view;
}

// Looks for any well-formed frame strictly after an offset. Its presence means
// a checksum failure is not a torn tail but mid-log corruption, which must
// never be silently discarded.
bool any_valid_frame_after(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                           std::size_t max_record_bytes) {
  for (std::size_t probe = offset + 1; probe + kFrameHeaderBytes <= bytes.size(); ++probe) {
    if (parse_frame(bytes, probe, max_record_bytes).ok) {
      return true;
    }
  }
  return false;
}

std::vector<std::uint8_t> encode_snapshot(std::uint64_t sequence,
                                          const std::vector<std::uint8_t>& payload) {
  ByteWriter writer(limits::kMaxSnapshotBytes + 64);
  writer.raw(kSnapshotMagic, sizeof(kSnapshotMagic));
  writer.u16(kFrameVersion);
  writer.u16(0);
  writer.u64(sequence);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.raw(payload.data(), payload.size());
  writer.u32(crc32c(payload.data(), payload.size()));
  if (!writer.ok()) {
    return {};
  }
  return writer.data();
}

Result<std::vector<std::uint8_t>> decode_snapshot(const std::vector<std::uint8_t>& bytes,
                                                  std::uint64_t& sequence) {
  constexpr std::size_t kMinimum = sizeof(kSnapshotMagic) + 2 + 2 + 8 + 4 + 4;
  if (bytes.size() < kMinimum) {
    return make_error(ErrorCode::Corrupt, "snapshot is too small to be valid");
  }
  ByteReader reader(bytes);
  const std::string_view magic = reader.raw(sizeof(kSnapshotMagic));
  if (reader.failed() || std::memcmp(magic.data(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
    return make_error(ErrorCode::Corrupt, "snapshot magic mismatch");
  }
  const std::uint16_t version = reader.u16();
  (void)reader.u16();
  sequence = reader.u64();
  const std::uint32_t length = reader.u32();
  if (reader.failed() || version != kFrameVersion) {
    return make_error(ErrorCode::VersionMismatch, "snapshot format version is not supported");
  }
  if (length > limits::kMaxSnapshotBytes) {
    return make_error(ErrorCode::TooLarge, "snapshot payload exceeds budget");
  }
  const std::string_view payload = reader.raw(length);
  const std::uint32_t expected_crc = reader.u32();
  if (reader.failed()) {
    return make_error(ErrorCode::Corrupt, "snapshot is truncated");
  }
  if (crc32c(payload.data(), payload.size()) != expected_crc) {
    return make_error(ErrorCode::Corrupt, "snapshot checksum mismatch");
  }
  if (!reader.at_end()) {
    return make_error(ErrorCode::Corrupt, "snapshot carries trailing bytes");
  }
  return std::vector<std::uint8_t>(payload.begin(), payload.end());
}

}  // namespace

const char* to_string(RecordType value) noexcept {
  switch (value) {
    case RecordType::Unknown: return "unknown";
    case RecordType::StoreHeader: return "store-header";
    case RecordType::EpochAdvance: return "epoch-advance";
    case RecordType::PublishClass: return "publish-class";
    case RecordType::PublishPolicy: return "publish-policy";
    case RecordType::PublishPath: return "publish-path";
    case RecordType::PublishCapability: return "publish-capability";
    case RecordType::PublishReservation: return "publish-reservation";
    case RecordType::RegisterPublisher: return "register-publisher";
    case RecordType::RevokePublisher: return "revoke-publisher";
    case RecordType::RecordDecision: return "record-decision";
    case RecordType::MutationIntent: return "mutation-intent";
    case RecordType::MutationCommit: return "mutation-commit";
    case RecordType::ConflictRecord: return "conflict-record";
    case RecordType::PublisherLeaseCleared: return "publisher-lease-cleared";
    case RecordType::MutationAbort: return "mutation-abort";
    case RecordType::CleanShutdown: return "clean-shutdown";
  }
  return "invalid";
}

}  // namespace qosfabric

namespace qosfabric {

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------
Result<bool> path_exists(const std::string& path) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(fs_path(path), ec);
  if (ec) {
    return make_error(ErrorCode::IoError, "existence check failed");
  }
  return exists;
}

Result<void> ensure_directory(const std::string& path) {
  std::error_code ec;
  std::filesystem::create_directories(fs_path(path), ec);
  if (ec) {
    std::error_code probe;
    if (!std::filesystem::is_directory(fs_path(path), probe) || probe) {
      return make_error(ErrorCode::IoError, "cannot create store directory");
    }
  }
  return {};
}

Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::size_t max_bytes) {
  std::FILE* handle = open_file(path, "rb");
  if (handle == nullptr) {
    std::string detail = "cannot open file for reading: ";
    detail += path;
    if (detail.size() > limits::kMaxErrorDetailBytes) {
      detail.resize(limits::kMaxErrorDetailBytes);
    }
    return make_error(ErrorCode::NotFound, detail);
  }
  if (std::fseek(handle, 0, SEEK_END) != 0) {
    std::fclose(handle);
    return make_error(ErrorCode::IoError, "seek failed");
  }
  const long end = std::ftell(handle);
  if (end < 0) {
    std::fclose(handle);
    return make_error(ErrorCode::IoError, "tell failed");
  }
  const std::uint64_t size = static_cast<std::uint64_t>(end);
  if (size > max_bytes) {
    std::fclose(handle);
    return make_error(ErrorCode::TooLarge, "file exceeds the configured read budget");
  }
  if (std::fseek(handle, 0, SEEK_SET) != 0) {
    std::fclose(handle);
    return make_error(ErrorCode::IoError, "seek failed");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0 && std::fread(bytes.data(), 1, bytes.size(), handle) != bytes.size()) {
    std::fclose(handle);
    return make_error(ErrorCode::IoError, "short read");
  }
  std::fclose(handle);
  return bytes;
}

Result<void> write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const std::string temp = path + ".tmp";
  std::FILE* handle = open_file(temp, "wb");
  if (handle == nullptr) {
    return make_error(ErrorCode::IoError, "cannot open temporary file for writing");
  }
  if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), handle) != bytes.size()) {
    std::fclose(handle);
    (void)remove_file(temp);
    return make_error(ErrorCode::IoError, "short write");
  }
  const auto synced = sync_handle(handle);
  std::fclose(handle);
  if (!synced) {
    (void)remove_file(temp);
    return synced.error();
  }
  return atomic_replace(temp, path);
}

Result<void> remove_file(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(fs_path(path), ec);
  if (ec) {
    return make_error(ErrorCode::IoError, "cannot remove file");
  }
  return {};
}

Result<std::vector<std::string>> list_directory(const std::string& path) {
  std::vector<std::string> entries;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(fs_path(path), ec)) {
    if (ec) {
      return make_error(ErrorCode::IoError, "directory iteration failed");
    }
    entries.push_back(entry.path().filename().string());
    if (entries.size() > 4096) {
      return make_error(ErrorCode::TooLarge, "directory holds more entries than the budget allows");
    }
  }
  if (ec) {
    return make_error(ErrorCode::IoError, "directory iteration failed");
  }
  std::sort(entries.begin(), entries.end());
  return entries;
}

// ---------------------------------------------------------------------------
// Store lifecycle
// ---------------------------------------------------------------------------
Store::~Store() {
  if (wal_handle_ != nullptr) {
    std::fclose(wal_handle_);
    wal_handle_ = nullptr;
  }
}

Result<std::unique_ptr<Store>> Store::Open(const StoreOptions& options) {
  if (options.directory.empty()) {
    return make_error(ErrorCode::InvalidArgument, "store directory is required");
  }
  if (options.format_version != kStoreFormatVersion) {
    return make_error(ErrorCode::VersionMismatch, "requested store format version is not supported");
  }
  if (options.max_record_bytes == 0 || options.max_record_bytes > limits::kMaxWalRecordBytes) {
    return make_error(ErrorCode::InvalidArgument, "record budget outside the supported range");
  }
  std::unique_ptr<Store> store(new Store());
  const auto opened = store->open_impl(options);
  if (!opened) {
    return opened.error();
  }
  return store;
}

Result<void> Store::open_impl(const StoreOptions& options) {
  options_ = options;
  options_.directory = native_path(options.directory);
  manifest_path_ = join_path(options_.directory, manifest_file_name());
  wal_path_ = join_path(options_.directory, wal_file_name());

  const auto exists = path_exists(options_.directory);
  if (!exists) {
    return exists.error();
  }
  if (!*exists) {
    if (!options.create_if_missing) {
      return make_error(ErrorCode::NotFound, "store directory does not exist");
    }
    const auto created = ensure_directory(options_.directory);
    if (!created) {
      return created.error();
    }
    recovery_.store_created = true;
  }

  const auto manifest = load_manifest(options);
  if (!manifest) {
    if (manifest.error().code() == ErrorCode::VersionMismatch) {
      return manifest.error();
    }
    const auto rebuilt = rebuild_manifest(options);
    if (!rebuilt) {
      return rebuilt.error();
    }
    recovery_.manifest_rebuilt = true;
  }

  const auto entries = list_directory(options_.directory);
  if (entries) {
    for (const std::string& name : *entries) {
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
        (void)remove_file(join_path(options_.directory, name));
      }
    }
  }

  const auto snapshot = load_snapshot();
  if (!snapshot) {
    return snapshot.error();
  }

  const auto log = replay_log(options);
  if (!log) {
    return log.error();
  }

  // Retire snapshot files the manifest does not reference. They can only be
  // orphans left by a crash between writing a snapshot and publishing it.
  if (entries) {
    for (const std::string& name : *entries) {
      if (name.size() <= snapshot_prefix().size() + snapshot_suffix().size()) {
        continue;
      }
      if (name.compare(0, snapshot_prefix().size(), snapshot_prefix()) != 0) {
        continue;
      }
      if (name.compare(name.size() - snapshot_suffix().size(), snapshot_suffix().size(),
                       snapshot_suffix()) != 0) {
        continue;
      }
      const std::string full = join_path(options_.directory, name);
      if (!snapshot_path_.empty() && full == snapshot_path_) {
        continue;
      }
      (void)remove_file(full);
    }
  }

  const auto reopened = reopen_wal();
  if (!reopened) {
    return reopened.error();
  }

  // A store that has never been opened has no durable identity. Mint one and
  // publish it as the first durable record so later boots can recognise it.
  Digest256 parsed_identity;
  if (!Digest256::parse_hex(store_id_, parsed_identity) || parsed_identity.is_zero()) {
    const BootId fresh = BootId::generate();
    const Digest256 minted = sha256(fresh.bytes().data(), fresh.bytes().size());
    const auto identity = minted.bytes();
    store_id_ = minted.hex();
    std::vector<std::uint8_t> header(identity.begin(), identity.end());
    const auto appended = Append(RecordType::StoreHeader, header);
    if (!appended) {
      return appended.error();
    }
    const auto published = write_manifest(options);
    if (!published) {
      return published.error();
    }
  }

  recovery_.last_sequence = last_sequence_.value();
  return {};
}

Result<void> Store::load_manifest(const StoreOptions& options) {
  const auto exists = path_exists(manifest_path_);
  if (!exists) {
    return exists.error();
  }
  if (!*exists) {
    return make_error(ErrorCode::NotFound, "manifest is absent");
  }
  const auto bytes = read_file(manifest_path_, 64u << 10);
  if (!bytes) {
    return bytes.error();
  }
  const std::vector<std::uint8_t>& raw = *bytes;
  constexpr std::size_t kMinimum = 8 + 2 + 2 + Digest256::kBytes + 8 + 8 + 8 + 4 + 4;
  if (raw.size() < kMinimum) {
    return make_error(ErrorCode::Corrupt, "manifest is too small");
  }
  const std::uint32_t expected_crc =
      static_cast<std::uint32_t>(load_le(raw.data() + raw.size() - 4, 4));
  if (crc32c(raw.data(), raw.size() - 4) != expected_crc) {
    return make_error(ErrorCode::Corrupt, "manifest checksum mismatch");
  }
  if (std::memcmp(raw.data(), kManifestMagic, sizeof(kManifestMagic)) != 0) {
    return make_error(ErrorCode::Corrupt, "manifest magic mismatch");
  }
  std::size_t offset = sizeof(kManifestMagic);
  const std::uint16_t version = static_cast<std::uint16_t>(load_le(raw.data() + offset, 2));
  offset += 4;  // version and reserved
  if (version != kStoreFormatVersion || options.format_version != version) {
    return make_error(ErrorCode::VersionMismatch, "store format version is not supported");
  }
  std::array<std::uint8_t, Digest256::kBytes> id_bytes{};
  std::memcpy(id_bytes.data(), raw.data() + offset, Digest256::kBytes);
  offset += Digest256::kBytes;
  store_id_ = Digest256{id_bytes}.hex();
  durable_epoch_ = FabricEpoch{load_le(raw.data() + offset, 8)};
  offset += 8;
  snapshot_sequence_ = load_le(raw.data() + offset, 8);
  offset += 8;
  last_sequence_ = Sequence{load_le(raw.data() + offset, 8)};
  offset += 8;
  const std::uint32_t name_length = static_cast<std::uint32_t>(load_le(raw.data() + offset, 4));
  offset += 4;
  if (name_length > 64 || offset + name_length + 4 > raw.size()) {
    return make_error(ErrorCode::Corrupt, "manifest snapshot name is malformed");
  }
  const std::string name(reinterpret_cast<const char*>(raw.data() + offset), name_length);
  offset += name_length;
  if (offset + 4 != raw.size()) {
    return make_error(ErrorCode::Corrupt, "manifest length is inconsistent");
  }
  if (name.empty()) {
    snapshot_path_.clear();
    snapshot_sequence_ = 0;
    return {};
  }
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
      name.find("..") != std::string::npos) {
    return make_error(ErrorCode::Corrupt, "manifest snapshot name escapes the store directory");
  }
  snapshot_path_ = join_path(options_.directory, name);
  const auto snapshot_exists = path_exists(snapshot_path_);
  if (!snapshot_exists) {
    return snapshot_exists.error();
  }
  if (!*snapshot_exists) {
    return make_error(ErrorCode::Corrupt, "manifest references a snapshot that is absent");
  }
  return {};
}

Result<void> Store::rebuild_manifest(const StoreOptions& options) {
  // Reconstruct the manifest from whatever durable artifacts exist. The
  // highest fully valid snapshot wins, and the log is then replayed over it.
  const auto entries = list_directory(options_.directory);
  if (!entries) {
    return entries.error();
  }
  std::uint64_t best_sequence = 0;
  std::string best_name;
  bool found_any = false;
  for (const std::string& name : *entries) {
    if (name.size() <= snapshot_prefix().size() + snapshot_suffix().size()) {
      continue;
    }
    if (name.compare(0, snapshot_prefix().size(), snapshot_prefix()) != 0) {
      continue;
    }
    if (name.compare(name.size() - snapshot_suffix().size(), snapshot_suffix().size(),
                     snapshot_suffix()) != 0) {
      continue;
    }
    const std::string digits =
        name.substr(snapshot_prefix().size(),
                    name.size() - snapshot_prefix().size() - snapshot_suffix().size());
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    const std::uint64_t sequence = std::strtoull(digits.c_str(), nullptr, 10);
    const std::string full = join_path(options_.directory, name);
    const auto bytes = read_file(full, limits::kMaxSnapshotBytes);
    if (!bytes) {
      continue;
    }
    std::uint64_t decoded_sequence = 0;
    if (!decode_snapshot(*bytes, decoded_sequence)) {
      continue;
    }
    if (decoded_sequence != sequence) {
      continue;
    }
    if (!found_any || sequence > best_sequence) {
      found_any = true;
      best_sequence = sequence;
      best_name = name;
    }
  }

  // A missing manifest is not a reason to trust anything: every field below is
  // rebuilt from checksummed artifacts, and a store identity is only adopted
  // once a StoreHeader record has been read back or freshly minted.
  store_id_ = Digest256{}.hex();
  durable_epoch_ = FabricEpoch{0};
  last_sequence_ = Sequence{0};
  snapshot_sequence_ = 0;
  snapshot_path_.clear();
  if (found_any) {
    snapshot_sequence_ = best_sequence;
    snapshot_path_ = join_path(options_.directory, best_name);
    last_sequence_ = Sequence{best_sequence};
  }

  const auto wal_exists = path_exists(wal_path_);
  if (wal_exists && *wal_exists) {
    const auto bytes = read_file(wal_path_, kMaxWalScanBytes);
    if (bytes) {
      std::size_t offset = 0;
      while (offset + kFrameHeaderBytes <= bytes->size()) {
        const FrameView frame = parse_frame(*bytes, offset, options_.max_record_bytes);
        if (!frame.ok) {
          break;
        }
        if (frame.sequence.value() > last_sequence_.value()) {
          last_sequence_ = frame.sequence;
        }
        if (frame.type == RecordType::EpochAdvance && frame.body.size() >= 8) {
          durable_epoch_ = FabricEpoch{load_le(
              reinterpret_cast<const std::uint8_t*>(frame.body.data()), 8)};
        }
        if (frame.type == RecordType::StoreHeader &&
            frame.body.size() >= Digest256::kBytes) {
          std::array<std::uint8_t, Digest256::kBytes> id_bytes{};
          std::memcpy(id_bytes.data(), frame.body.data(), Digest256::kBytes);
          store_id_ = Digest256{id_bytes}.hex();
        }
        offset = frame.next_offset;
      }
    }
  }

  const auto written = write_manifest(options);
  if (!written) {
    return written.error();
  }
  recovery_.detail += "manifest rebuilt from durable artifacts; ";
  return {};
}

Result<void> Store::write_manifest(const StoreOptions& options) {
  ByteWriter writer(64u << 10);
  writer.raw(kManifestMagic, sizeof(kManifestMagic));
  writer.u16(kStoreFormatVersion);
  writer.u16(0);
  std::array<std::uint8_t, Digest256::kBytes> id_bytes{};
  Digest256 parsed;
  if (Digest256::parse_hex(store_id_, parsed)) {
    id_bytes = parsed.bytes();
  }
  writer.raw(id_bytes.data(), id_bytes.size());
  writer.u64(durable_epoch_.value());
  writer.u64(snapshot_sequence_);
  writer.u64(last_sequence_.value());
  const std::string name =
      snapshot_path_.empty() ? std::string{} : snapshot_file_name(snapshot_sequence_);
  writer.u32(static_cast<std::uint32_t>(name.size()));
  writer.raw(name.data(), name.size());
  if (!writer.ok()) {
    return make_error(ErrorCode::TooLarge, "manifest exceeds its budget");
  }
  std::vector<std::uint8_t> bytes = writer.data();
  const std::uint32_t crc = crc32c(bytes.data(), bytes.size());
  std::uint8_t crc_bytes[4];
  store_le(crc, crc_bytes, 4);
  bytes.insert(bytes.end(), crc_bytes, crc_bytes + 4);
  const auto written = write_file_atomic(manifest_path_, bytes);
  if (!written) {
    return written.error();
  }
  return sync_directory(options.directory);
}

Result<void> Store::load_snapshot() {
  if (snapshot_path_.empty()) {
    return {};
  }
  const auto bytes = read_file(snapshot_path_, limits::kMaxSnapshotBytes);
  if (!bytes) {
    return bytes.error();
  }
  std::uint64_t sequence = 0;
  auto payload = decode_snapshot(*bytes, sequence);
  if (!payload) {
    return payload.error();
  }
  if (sequence != snapshot_sequence_) {
    return make_error(ErrorCode::Corrupt, "snapshot sequence does not match the manifest");
  }
  snapshot_bytes_ = std::move(*payload);
  recovery_.snapshot_loaded = true;
  recovery_.snapshot_sequence = sequence;
  return {};
}

Result<void> Store::replay_log(const StoreOptions& options) {
  const auto exists = path_exists(wal_path_);
  if (!exists) {
    return exists.error();
  }
  if (!*exists) {
    wal_bytes_ = 0;
    return {};
  }
  const auto bytes = read_file(wal_path_, kMaxWalScanBytes);
  if (!bytes) {
    return bytes.error();
  }
  wal_bytes_ = bytes->size();

  std::vector<std::pair<AttemptId, LogRecord>> staged;
  std::size_t offset = 0;
  std::size_t good_end = 0;
  std::size_t frame_start = 0;
  std::size_t bad_offset = 0;
  bool saw_bad = false;
  bool saw_clean_shutdown = false;

  while (offset + kFrameHeaderBytes <= bytes->size()) {
    const FrameView frame = parse_frame(*bytes, offset, options.max_record_bytes);
    if (!frame.ok) {
      saw_bad = true;
      bad_offset = offset;
      break;
    }
    frame_start = offset;
    offset = frame.next_offset;
    good_end = offset;
    saw_clean_shutdown = false;

    if (frame.sequence.value() <= snapshot_sequence_) {
      continue;
    }
    if (frame.sequence.value() > last_sequence_.value()) {
      last_sequence_ = frame.sequence;
    }

    bool body_malformed = false;
    switch (frame.type) {
      case RecordType::MutationIntent: {
        ByteReader reader(frame.body);
        const std::uint16_t raw_type = reader.u16();
        const std::uint64_t attempt = reader.u64();
        const std::uint32_t body_length = reader.u32();
        if (reader.failed() || body_length > reader.remaining() ||
            raw_type > static_cast<std::uint16_t>(RecordType::CleanShutdown)) {
          body_malformed = true;
          break;
        }
        const std::string_view body = reader.raw(body_length);
        if (reader.failed()) {
          body_malformed = true;
          break;
        }
        LogRecord record;
        record.type = static_cast<RecordType>(raw_type);
        record.payload.assign(body.begin(), body.end());
        staged.emplace_back(AttemptId{attempt}, std::move(record));
        break;
      }
      case RecordType::MutationCommit: {
        ByteReader reader(frame.body);
        const std::uint64_t attempt = reader.u64();
        if (reader.failed()) {
          body_malformed = true;
          break;
        }
        const auto found =
            std::find_if(staged.begin(), staged.end(), [attempt](const auto& item) {
              return item.first.value() == attempt;
            });
        if (found == staged.end()) {
          break;
        }
        LogRecord record = found->second;
        record.sequence = frame.sequence;
        replayed_.push_back(std::move(record));
        staged.erase(found);
        break;
      }
      case RecordType::MutationAbort: {
        ByteReader reader(frame.body);
        const std::uint64_t attempt = reader.u64();
        if (reader.failed()) {
          body_malformed = true;
          break;
        }
        const auto found =
            std::find_if(staged.begin(), staged.end(), [attempt](const auto& item) {
              return item.first.value() == attempt;
            });
        if (found != staged.end()) {
          staged.erase(found);
          ++recovery_.discarded_intents;
        }
        break;
      }
      case RecordType::EpochAdvance: {
        if (frame.body.size() < 8) {
          body_malformed = true;
          break;
        }
        durable_epoch_ =
            FabricEpoch{load_le(reinterpret_cast<const std::uint8_t*>(frame.body.data()), 8)};
        ++recovery_.epoch_advances;
        LogRecord record;
        record.type = frame.type;
        record.sequence = frame.sequence;
        record.payload.assign(frame.body.begin(), frame.body.end());
        replayed_.push_back(std::move(record));
        break;
      }
      case RecordType::CleanShutdown:
        saw_clean_shutdown = true;
        break;
      case RecordType::StoreHeader:
        // Store identity is durable metadata, not application state: it is
        // reported through the store itself and never replayed as work.
        if (frame.body.size() < Digest256::kBytes) {
          body_malformed = true;
        }
        break;
      case RecordType::Unknown:
        body_malformed = true;
        break;
      default: {
        LogRecord record;
        record.type = frame.type;
        record.sequence = frame.sequence;
        record.payload.assign(frame.body.begin(), frame.body.end());
        replayed_.push_back(std::move(record));
        break;
      }
    }
    if (body_malformed) {
      saw_bad = true;
      bad_offset = frame_start;
      break;
    }
  }

  recovery_.unfinished_intents += static_cast<std::uint64_t>(staged.size());

  const bool trailing_partial = !saw_bad && offset < bytes->size();
  if (saw_bad || trailing_partial) {
    const std::size_t cutoff = saw_bad ? bad_offset : offset;
    bool complete_frame_bad = false;
    if (saw_bad && cutoff + kFrameHeaderBytes <= bytes->size()) {
      const std::uint64_t declared = load_le(bytes->data() + cutoff, 4);
      if (declared <= options.max_record_bytes &&
          cutoff + kFrameHeaderBytes + static_cast<std::size_t>(declared) <= bytes->size()) {
        complete_frame_bad = true;
      }
    }
    const bool valid_frame_follows =
        saw_bad && any_valid_frame_after(*bytes, cutoff, options.max_record_bytes);
    if (complete_frame_bad || valid_frame_follows) {
      recovery_.mid_log_corruption = true;
      if (!options.repair_truncate_corrupt_tail || valid_frame_follows) {
        return make_error(ErrorCode::Corrupt,
                          "write-ahead log holds a corrupted frame; the store refuses to "
                          "interpret or discard it silently");
      }
    }
    recovery_.truncated_tail_bytes = static_cast<std::uint64_t>(bytes->size() - good_end);
    std::FILE* handle = open_file(wal_path_, "r+b");
    if (handle != nullptr) {
#ifdef _WIN32
      (void)::_chsize_s(file_descriptor(handle), static_cast<__int64>(good_end));
#else
      (void)::ftruncate(file_descriptor(handle), static_cast<off_t>(good_end));
#endif
      std::fclose(handle);
    }
    wal_bytes_ = good_end;
    recovery_.detail += "truncated a torn write-ahead log tail; ";
  }

  recovery_.opened_clean = saw_clean_shutdown && !saw_bad && !trailing_partial &&
                           recovery_.truncated_tail_bytes == 0;
  recovery_.replayed_records = replayed_.size();
  return {};
}

Result<void> Store::reopen_wal() {
  if (wal_handle_ != nullptr) {
    std::fclose(wal_handle_);
    wal_handle_ = nullptr;
  }
  wal_handle_ = open_file(wal_path_, "ab");
  if (wal_handle_ == nullptr) {
    return make_error(ErrorCode::IoError, "cannot open the write-ahead log for appending");
  }
  return {};
}

Result<void> Store::fsync_wal() { return sync_handle(wal_handle_); }

Result<void> Store::append_frame(RecordType type, std::uint8_t flags, AttemptId attempt,
                                 const std::vector<std::uint8_t>& payload,
                                 Sequence& out_sequence) {
  if (read_only_ || wal_handle_ == nullptr) {
    return make_error(ErrorCode::ShuttingDown, "store is closed");
  }
  Sequence next = last_sequence_;
  if (!next.try_increment()) {
    return make_error(ErrorCode::Overflow, "durable sequence space is exhausted");
  }
  if (payload.size() > options_.max_record_bytes) {
    return make_error(ErrorCode::TooLarge, "record body exceeds the configured budget");
  }

  ByteWriter body_writer(options_.max_record_bytes);
  body_writer.u16(kFrameVersion);
  body_writer.u16(static_cast<std::uint16_t>(type));
  body_writer.u8(flags);
  body_writer.u64(next.value());
  body_writer.u64(attempt.value());
  body_writer.u32(static_cast<std::uint32_t>(payload.size()));
  body_writer.raw(payload.data(), payload.size());
  if (!body_writer.ok()) {
    return make_error(ErrorCode::TooLarge, "record exceeds the configured budget");
  }
  const std::vector<std::uint8_t>& body = body_writer.data();

  std::uint64_t frame_bytes = 0;
  if (!checked_add<std::uint64_t>(static_cast<std::uint64_t>(kFrameHeaderBytes + body.size()),
                                  wal_bytes_, frame_bytes)) {
    return make_error(ErrorCode::Overflow, "write-ahead log size accounting overflowed");
  }
  if (frame_bytes > options_.max_wal_bytes) {
    return make_error(ErrorCode::CapacityExhausted,
                      "write-ahead log budget exhausted; compaction is required");
  }

  std::uint8_t header[kFrameHeaderBytes];
  store_le(static_cast<std::uint64_t>(body.size()), header, 4);
  store_le(static_cast<std::uint64_t>(crc32c(body.data(), body.size())), header + 4, 4);

  if (std::fwrite(header, 1, kFrameHeaderBytes, wal_handle_) != kFrameHeaderBytes ||
      (!body.empty() && std::fwrite(body.data(), 1, body.size(), wal_handle_) != body.size())) {
    return make_error(ErrorCode::IoError, "failed to append a frame to the write-ahead log");
  }
  const auto synced = fsync_wal();
  if (!synced) {
    return synced.error();
  }
  wal_bytes_ = frame_bytes;
  last_sequence_ = next;
  out_sequence = next;
  return {};
}

Result<Sequence> Store::Append(RecordType type, const std::vector<std::uint8_t>& payload) {
  std::lock_guard<std::mutex> guard(mutex_);
  Sequence sequence{};
  const auto appended = append_frame(type, kFlagNone, AttemptId{}, payload, sequence);
  if (!appended) {
    return appended.error();
  }
  return sequence;
}

Result<AttemptId> Store::Stage(RecordType type, const std::vector<std::uint8_t>& payload) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (staged_count_ >= limits::kMaxJournalIntentsInFlight) {
    return make_error(ErrorCode::CapacityExhausted, "too many journalled intents are in flight");
  }
  const AttemptId attempt{last_sequence_.value() + 1};
  ByteWriter writer(options_.max_record_bytes);
  writer.u16(static_cast<std::uint16_t>(type));
  writer.u64(attempt.value());
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.raw(payload.data(), payload.size());
  if (!writer.ok()) {
    return make_error(ErrorCode::TooLarge, "journalled intent exceeds the configured budget");
  }
  Sequence sequence{};
  const auto appended =
      append_frame(RecordType::MutationIntent, kFlagHasAttempt, attempt, writer.data(), sequence);
  if (!appended) {
    return appended.error();
  }
  ++staged_count_;
  return attempt;
}

Result<void> Store::Commit(AttemptId attempt, const Digest256& applied_digest) {
  std::lock_guard<std::mutex> guard(mutex_);
  ByteWriter writer(256);
  writer.u64(attempt.value());
  writer.raw(applied_digest.bytes().data(), Digest256::kBytes);
  if (!writer.ok()) {
    return make_error(ErrorCode::TooLarge, "commit record exceeds the configured budget");
  }
  Sequence sequence{};
  const auto appended = append_frame(RecordType::MutationCommit, kFlagHasAttempt, attempt,
                                     writer.data(), sequence);
  if (!appended) {
    return appended.error();
  }
  if (staged_count_ > 0) {
    --staged_count_;
  }
  return {};
}

Result<void> Store::Abort(AttemptId attempt, std::string_view reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  ByteWriter writer(512);
  writer.u64(attempt.value());
  writer.str(reason.substr(0, limits::kMaxReasonLen), limits::kMaxReasonLen);
  if (!writer.ok()) {
    return make_error(ErrorCode::TooLarge, "abort record exceeds the configured budget");
  }
  Sequence sequence{};
  const auto appended = append_frame(RecordType::MutationAbort, kFlagHasAttempt, attempt,
                                     writer.data(), sequence);
  if (!appended) {
    return appended.error();
  }
  if (staged_count_ > 0) {
    --staged_count_;
  }
  return {};
}

Result<void> Store::AdoptStoreIdentity(const std::string& store_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  store_id_ = store_id;
  return write_manifest(options_);
}

Result<void> Store::Close() {
  std::lock_guard<std::mutex> guard(mutex_);
  return close_locked();
}

Result<void> Store::close_locked() {
  if (read_only_) {
    return {};
  }
  if (wal_handle_ != nullptr) {
    Sequence ignored{};
    const auto closed = append_frame(RecordType::CleanShutdown, kFlagNone, AttemptId{}, {},
                                     ignored);
    if (!closed) {
      std::fclose(wal_handle_);
      wal_handle_ = nullptr;
      read_only_ = true;
      return closed.error();
    }
    std::fclose(wal_handle_);
    wal_handle_ = nullptr;
  }
  read_only_ = true;
  return {};
}

Result<void> Store::WriteSnapshot(const std::vector<std::uint8_t>& snapshot) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (snapshot.size() > limits::kMaxSnapshotBytes) {
    return make_error(ErrorCode::TooLarge, "snapshot exceeds the configured budget");
  }
  const std::uint64_t sequence = last_sequence_.value();
  const std::vector<std::uint8_t> encoded = encode_snapshot(sequence, snapshot);
  if (encoded.empty()) {
    return make_error(ErrorCode::TooLarge, "snapshot encoding exceeded its budget");
  }
  const std::string target = join_path(options_.directory, snapshot_file_name(sequence));
  const std::string previous_path = snapshot_path_;

  const auto written = write_file_atomic(target, encoded);
  if (!written) {
    return written.error();
  }

  snapshot_path_ = target;
  snapshot_sequence_ = sequence;
  const auto manifest = write_manifest(options_);
  if (!manifest) {
    return manifest.error();
  }

  // Retire the log prefix the snapshot now covers. A crash before this point
  // is harmless: replay skips every record at or below the snapshot sequence,
  // and the snapshot file is checksummed and referenced by the manifest.
  if (wal_handle_ != nullptr) {
    std::fclose(wal_handle_);
    wal_handle_ = nullptr;
  }
  const auto truncated = write_file_atomic(wal_path_, {});
  if (!truncated) {
    const auto reopened = reopen_wal();
    (void)reopened;
    return truncated.error();
  }
  wal_bytes_ = 0;
  const auto reopened = reopen_wal();
  if (!reopened) {
    return reopened.error();
  }

  if (!previous_path.empty() && previous_path != target) {
    (void)remove_file(previous_path);
  }
  return {};
}

Result<void> Store::Verify() const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto manifest = read_file(manifest_path_, 64u << 10);
  if (!manifest) {
    return manifest.error();
  }
  if (manifest->size() < 12) {
    return make_error(ErrorCode::Corrupt, "manifest is too small");
  }
  const std::uint32_t expected_crc =
      static_cast<std::uint32_t>(load_le(manifest->data() + manifest->size() - 4, 4));
  if (crc32c(manifest->data(), manifest->size() - 4) != expected_crc) {
    return make_error(ErrorCode::Corrupt, "manifest checksum mismatch");
  }
  if (!snapshot_path_.empty()) {
    const auto bytes = read_file(snapshot_path_, limits::kMaxSnapshotBytes);
    if (!bytes) {
      return bytes.error();
    }
    std::uint64_t sequence = 0;
    const auto payload = decode_snapshot(*bytes, sequence);
    if (!payload) {
      return payload.error();
    }
    if (sequence != snapshot_sequence_) {
      return make_error(ErrorCode::Corrupt, "snapshot sequence mismatch during verification");
    }
  }
  const auto wal_exists = path_exists(wal_path_);
  if (!wal_exists) {
    return wal_exists.error();
  }
  if (*wal_exists) {
    const auto bytes = read_file(wal_path_, kMaxWalScanBytes);
    if (!bytes) {
      return bytes.error();
    }
    std::size_t offset = 0;
    while (offset + kFrameHeaderBytes <= bytes->size()) {
      const FrameView frame = parse_frame(*bytes, offset, options_.max_record_bytes);
      if (!frame.ok) {
        return make_error(ErrorCode::Corrupt, "write-ahead log frame failed verification");
      }
      offset = frame.next_offset;
    }
    if (offset != bytes->size()) {
      return make_error(ErrorCode::Corrupt, "write-ahead log has a torn tail");
    }
  }
  return {};
}

}  // namespace qosfabric





