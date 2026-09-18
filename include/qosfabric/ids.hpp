// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strongly typed identities. Numeric identities that name different concepts
// (generation, fabric epoch, publisher incarnation, boot, attempt, lease,
// sequence) are distinct types and cannot be interchanged by accident.
// Nominal identities are validated, bounded, canonically comparable strings.

#ifndef QOSFABRIC_IDS_HPP
#define QOSFABRIC_IDS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <compare>

#include "qosfabric/error.hpp"
#include "qosfabric/limits.hpp"

namespace qosfabric {

// ---------------------------------------------------------------------------
// Numeric strong identity
// ---------------------------------------------------------------------------
template <class Tag>
class U64Id {
 public:
  using value_type = std::uint64_t;

  constexpr U64Id() noexcept = default;
  explicit constexpr U64Id(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  // Monotonic advance with overflow rejection. Returns false and leaves the
  // value untouched at the ceiling.
  [[nodiscard]] bool try_increment() noexcept {
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }
    ++value_;
    return true;
  }

  void reset() noexcept { value_ = 0; }
  void assign(std::uint64_t value) noexcept { value_ = value; }

  friend constexpr bool operator==(U64Id a, U64Id b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(U64Id a, U64Id b) noexcept { return a.value_ != b.value_; }
  friend constexpr std::strong_ordering operator<=>(U64Id a, U64Id b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  std::uint64_t value_{0};
};

struct GenerationTag;
struct EpochTag;
struct IncarnationTag;
struct AttemptTag;
struct LeaseTag;
struct SequenceTag;

using Generation = U64Id<GenerationTag>;
using FabricEpoch = U64Id<EpochTag>;
using PublisherIncarnation = U64Id<IncarnationTag>;
using AttemptId = U64Id<AttemptTag>;
using LeaseId = U64Id<LeaseTag>;
using Sequence = U64Id<SequenceTag>;

[[nodiscard]] inline Generation next_generation(Generation current) {
  Generation out = current;
  (void)out.try_increment();
  return out;
}

// ---------------------------------------------------------------------------
// Nominal strong identity
// ---------------------------------------------------------------------------
struct QoSClassIdTag;
struct QoSContractIdTag;
struct SubjectIdTag;
struct PathIdTag;
struct ResourceIdTag;
struct PolicyIdTag;
struct PriorityClassIdTag;
struct PublisherIdTag;
struct ObligationIdTag;
struct CapabilityKeyTag;

template <class Tag>
class StringId {
 public:
  using tag_type = Tag;

  constexpr StringId() noexcept = default;
  explicit StringId(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return value_.size(); }

  friend bool operator==(const StringId& a, const StringId& b) noexcept {
    return a.value_ == b.value_;
  }
  friend bool operator!=(const StringId& a, const StringId& b) noexcept {
    return a.value_ != b.value_;
  }
  friend std::strong_ordering operator<=>(const StringId& a, const StringId& b) noexcept {
    return a.value_.compare(b.value_) <=> 0;
  }

  // Binding hook used exclusively by the canonical decoder, which has already
  // validated the identifier against the canonical alphabet.
  [[nodiscard]] std::string& bind_unchecked() noexcept { return value_; }

 private:
  std::string value_{};
};

using QoSClassId = StringId<QoSClassIdTag>;
using QoSContractId = StringId<QoSContractIdTag>;
using SubjectId = StringId<SubjectIdTag>;
using PathId = StringId<PathIdTag>;
using ResourceId = StringId<ResourceIdTag>;
using PolicyId = StringId<PolicyIdTag>;
using PriorityClassId = StringId<PriorityClassIdTag>;
using PublisherId = StringId<PublisherIdTag>;
using ObligationId = StringId<ObligationIdTag>;
using CapabilityKey = StringId<CapabilityKeyTag>;

// Identifier alphabet: letters, digits and a small punctuation set. Anything
// outside this set is refused so identifiers cannot carry whitespace, control
// characters, path separators, shell metacharacters or non-ASCII bytes into
// logs, file names or stream frames.
[[nodiscard]] bool is_valid_identifier_char(char c) noexcept;

// Validates length and alphabet. Rejects empty, over-long, and
// non-canonical identifiers.
[[nodiscard]] bool is_valid_identifier(std::string_view value,
                                      std::size_t max_len = limits::kMaxIdentifierLen) noexcept;

template <class Tag>
[[nodiscard]] Result<StringId<Tag>> parse_string_id(std::string_view value,
                                                    std::size_t max_len = limits::kMaxIdentifierLen) {
  if (!is_valid_identifier(value, max_len)) {
    return make_error(ErrorCode::InvalidArgument, "identifier rejected by canonical alphabet or length");
  }
  return StringId<Tag>{std::string(value)};
}

template <class Tag>
[[nodiscard]] bool is_valid(const StringId<Tag>& id,
                            std::size_t max_len = limits::kMaxIdentifierLen) noexcept {
  return is_valid_identifier(id.view(), max_len);
}

// ---------------------------------------------------------------------------
// Boot identity: a fresh, unpredictable per-process-incarnation token.
// ---------------------------------------------------------------------------
class BootId {
 public:
  static constexpr std::size_t kBytes = 16;

  BootId() noexcept = default;
  explicit BootId(const std::array<std::uint8_t, kBytes>& bytes) noexcept : bytes_(bytes) {}

  // Generates a fresh boot identity from the OS entropy source mixed with a
  // process-local monotonic counter and the current wall clock. Never returns
  // the all-zero identity.
  [[nodiscard]] static BootId generate() noexcept;

  [[nodiscard]] const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const BootId& a, const BootId& b) noexcept { return a.bytes_ == b.bytes_; }
  friend bool operator!=(const BootId& a, const BootId& b) noexcept { return a.bytes_ != b.bytes_; }

 private:
  std::array<std::uint8_t, kBytes> bytes_{};
};

// ---------------------------------------------------------------------------
// Provenance: who asserted a fact, under which generation/epoch, and in what
// order. Provenance is descriptive; it never proves liveness.
// ---------------------------------------------------------------------------
class Provenance {
 public:
  Provenance() = default;
  Provenance(std::string origin, FabricEpoch epoch, Generation generation, Sequence sequence)
      : origin_(std::move(origin)), epoch_(epoch), generation_(generation), sequence_(sequence) {
    if (origin_.size() > limits::kMaxOriginLen) {
      origin_.resize(limits::kMaxOriginLen);
    }
  }

  [[nodiscard]] const std::string& origin() const noexcept { return origin_; }
  [[nodiscard]] FabricEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] Generation generation() const noexcept { return generation_; }
  [[nodiscard]] Sequence sequence() const noexcept { return sequence_; }

  friend bool operator==(const Provenance& a, const Provenance& b) noexcept {
    return a.origin_ == b.origin_ && a.epoch_ == b.epoch_ && a.generation_ == b.generation_ &&
           a.sequence_ == b.sequence_;
  }

 private:
  std::string origin_{};
  FabricEpoch epoch_{};
  Generation generation_{};
  Sequence sequence_{};
};

}  // namespace qosfabric

namespace std {
template <class Tag>
struct hash<qosfabric::U64Id<Tag>> {
  std::size_t operator()(const qosfabric::U64Id<Tag>& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};

template <class Tag>
struct hash<qosfabric::StringId<Tag>> {
  std::size_t operator()(const qosfabric::StringId<Tag>& value) const noexcept {
    return std::hash<std::string_view>{}(value.view());
  }
};

template <>
struct hash<qosfabric::BootId> {
  std::size_t operator()(const qosfabric::BootId& value) const noexcept {
    std::size_t h = 1469598103934665603ull;
    for (std::uint8_t b : value.bytes()) {
      h ^= static_cast<std::size_t>(b);
      h *= 1099511628211ull;
    }
    return h;
  }
};
}  // namespace std

#endif  // QOSFABRIC_IDS_HPP
