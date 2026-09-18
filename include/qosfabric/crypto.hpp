// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Content addressing and frame integrity. SHA-256 gives canonical records a
// stable digest used for equivocation detection and authority binding;
// CRC-32C gives durable log frames and wire frames a cheap integrity check.

#ifndef QOSFABRIC_CRYPTO_HPP
#define QOSFABRIC_CRYPTO_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace qosfabric {

class Digest256 {
 public:
  static constexpr std::size_t kBytes = 32;

  Digest256() noexcept = default;
  explicit Digest256(const std::array<std::uint8_t, kBytes>& bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool is_zero() const noexcept;

  [[nodiscard]] std::string hex() const;
  [[nodiscard]] static bool parse_hex(std::string_view text, Digest256& out) noexcept;

  friend bool operator==(const Digest256& a, const Digest256& b) noexcept {
    return a.bytes_ == b.bytes_;
  }
  friend bool operator!=(const Digest256& a, const Digest256& b) noexcept {
    return a.bytes_ != b.bytes_;
  }

 private:
  std::array<std::uint8_t, kBytes> bytes_{};
};

// Incremental SHA-256. Streaming so that large canonical records never need a
// second full copy in memory.
class Sha256 {
 public:
  Sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  [[nodiscard]] Digest256 finish() noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_bytes_{0};
};

[[nodiscard]] Digest256 sha256(const void* data, std::size_t size) noexcept;
[[nodiscard]] Digest256 sha256(std::string_view text) noexcept;

// CRC-32C (Castagnoli), reflected, polynomial 0x1EDC6F41.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
[[nodiscard]] inline std::uint32_t crc32c(std::string_view text) noexcept {
  return crc32c(text.data(), text.size());
}

}  // namespace qosfabric

namespace std {
template <>
struct hash<qosfabric::Digest256> {
  std::size_t operator()(const qosfabric::Digest256& value) const noexcept {
    std::size_t h = 1469598103934665603ull;
    const auto& bytes = value.bytes();
    for (std::size_t i = 0; i < 8; ++i) {
      h ^= static_cast<std::size_t>(bytes[i]);
      h *= 1099511628211ull;
    }
    return h;
  }
};
}  // namespace std

#endif  // QOSFABRIC_CRYPTO_HPP
