// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/ids.hpp"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

namespace qosfabric {
namespace {

bool is_ascii_alnum(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

}  // namespace

bool is_valid_identifier_char(char c) noexcept {
  if (is_ascii_alnum(c)) {
    return true;
  }
  switch (c) {
    case '.':
    case '_':
    case '-':
    case ':':
    case '/':
      return true;
    default:
      return false;
  }
}

bool is_valid_identifier(std::string_view value, std::size_t max_len) noexcept {
  if (value.empty() || value.size() > max_len) {
    return false;
  }
  for (const char c : value) {
    if (!is_valid_identifier_char(c)) {
      return false;
    }
  }
  return true;
}

BootId BootId::generate() noexcept {
  // The boot identity only has to be unique across process incarnations on
  // this fabric; it is not a secret and is never used as an authentication
  // token. std::random_device draws from the operating system entropy source.
  static std::atomic<std::uint64_t> counter{0};
  std::random_device device;
  const std::uint64_t tick = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t wall = static_cast<std::uint64_t>(
      std::chrono::system_clock::now().time_since_epoch().count());
  const std::uint64_t ordinal = counter.fetch_add(1, std::memory_order_relaxed);

  std::array<std::uint8_t, BootId::kBytes> bytes{};
  for (std::size_t i = 0; i < 4; ++i) {
    const std::uint32_t word = device();
    for (std::size_t b = 0; b < 4; ++b) {
      bytes[i * 4 + b] = static_cast<std::uint8_t>((word >> (b * 8)) & 0xFFu);
    }
  }
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[i] ^= static_cast<std::uint8_t>((tick >> (i * 8)) & 0xFFu);
    bytes[8 + i] ^= static_cast<std::uint8_t>((wall >> (i * 8)) & 0xFFu);
  }
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[i] ^= static_cast<std::uint8_t>((ordinal >> (i * 8)) & 0xFFu);
  }
  return BootId{bytes};
}

bool BootId::is_zero() const noexcept {
  for (const std::uint8_t b : bytes_) {
    if (b != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace qosfabric
