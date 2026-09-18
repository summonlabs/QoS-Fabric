// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Canonical binary encoding. The encoding is the authority for content
// digests, durable records and wire frames, so it is fixed, versioned,
// little-endian, length-prefixed and fully bounds-checked in both directions:
// a reader can never be driven past the end of its buffer, and a writer can
// never exceed its declared budget.

#ifndef QOSFABRIC_SERIALIZE_HPP
#define QOSFABRIC_SERIALIZE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "qosfabric/limits.hpp"

namespace qosfabric {

class ByteWriter {
 public:
  explicit ByteWriter(std::size_t limit = limits::kMaxCanonicalRecordBytes)
      : limit_(limit) {
    buffer_.reserve(256);
  }

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void raw(const void* data, std::size_t size);
  void bytes(const std::vector<std::uint8_t>& value);
  void str(std::string_view value, std::size_t max_len = limits::kMaxCanonicalRecordBytes);
  void digest_bytes(const void* data, std::size_t size) { raw(data, size); }

  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  void fail() noexcept { failed_ = true; }

  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

  // Writers for optional and counted containers keep the presence/count byte
  // layout in exactly one place so encode and decode cannot drift apart.
  void presence(bool present) { u8(present ? 1u : 0u); }

  template <class Fn>
  void optional(bool present, Fn&& emit) {
    presence(present);
    if (present && ok()) {
      emit(*this);
    }
  }

  template <class Fn>
  void counted(std::size_t count, Fn&& emit) {
    if (count > 0xFFFFFFFFull) {
      fail();
      return;
    }
    u32(static_cast<std::uint32_t>(count));
    if (!ok()) {
      return;
    }
    emit(*this);
  }

 private:
  std::vector<std::uint8_t> buffer_{};
  std::size_t limit_{limits::kMaxCanonicalRecordBytes};
  bool failed_{false};
};

class ByteReader {
 public:
  explicit ByteReader(const std::vector<std::uint8_t>& buffer) noexcept
      : data_(buffer.data()), size_(buffer.size()) {}
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
  explicit ByteReader(std::string_view text) noexcept
      : data_(reinterpret_cast<const std::uint8_t*>(text.data())), size_(text.size()) {}

  [[nodiscard]] std::uint8_t u8();
  [[nodiscard]] std::uint16_t u16();
  [[nodiscard]] std::uint32_t u32();
  [[nodiscard]] std::uint64_t u64();
  [[nodiscard]] std::string_view raw(std::size_t size);
  [[nodiscard]] std::vector<std::uint8_t> bytes(std::size_t max_len = limits::kMaxCanonicalRecordBytes);
  [[nodiscard]] std::string str(std::size_t max_len = limits::kMaxCanonicalRecordBytes);

  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  void fail() noexcept { failed_ = true; }

  [[nodiscard]] bool at_end() const noexcept { return pos_ == size_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - pos_; }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }

  [[nodiscard]] bool presence();

  template <class Fn>
  [[nodiscard]] bool optional(bool& present, Fn&& consume) {
    present = presence();
    if (failed_) {
      return false;
    }
    if (present) {
      consume(*this);
    }
    return !failed_;
  }

  // Reads a count and refuses it outright when it exceeds the caller's
  // structural budget, before any per-element work happens.
  [[nodiscard]] bool count(std::size_t max_count, std::size_t& out);

 private:
  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::size_t pos_{0};
  bool failed_{false};
};

// Digests the canonical encoding produced by a writer callback. Used to bind
// decisions and records to the exact bytes that justified them.
template <class Fn>
[[nodiscard]] std::vector<std::uint8_t> encode_canonical(Fn&& emit,
                                                         std::size_t limit = limits::kMaxCanonicalRecordBytes) {
  ByteWriter writer(limit);
  emit(writer);
  if (!writer.ok()) {
    return {};
  }
  return writer.data();
}

}  // namespace qosfabric

#endif  // QOSFABRIC_SERIALIZE_HPP
