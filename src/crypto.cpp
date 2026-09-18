// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/crypto.hpp"

#include <cstring>

namespace qosfabric {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kSha256Init = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                                      0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                                      0x1f83d9abu, 0x5be0cd19u};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
  return static_cast<std::uint32_t>((value >> bits) | (value << (32u - bits)));
}

// CRC-32C lookup table, built once at compile time. The table is filled by
// iterating its own elements rather than by indexing it, so the access is
// provably in range without any assumption about the table length.
[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  std::uint32_t ordinal = 0;
  for (std::uint32_t& slot : table) {
    std::uint32_t crc = ordinal++;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? static_cast<std::uint32_t>((crc >> 1) ^ 0x82F63B78u)
                       : static_cast<std::uint32_t>(crc >> 1);
    }
    slot = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

bool Digest256::is_zero() const noexcept {
  for (std::uint8_t b : bytes_) {
    if (b != 0) {
      return false;
    }
  }
  return true;
}

std::string Digest256::hex() const {
  std::string out;
  out.resize(kBytes * 2);
  for (std::size_t i = 0; i < kBytes; ++i) {
    out[i * 2] = kHexDigits[(bytes_[i] >> 4) & 0x0Fu];
    out[i * 2 + 1] = kHexDigits[bytes_[i] & 0x0Fu];
  }
  return out;
}

bool Digest256::parse_hex(std::string_view text, Digest256& out) noexcept {
  if (text.size() != kBytes * 2) {
    return false;
  }
  std::array<std::uint8_t, kBytes> bytes{};
  for (std::size_t i = 0; i < kBytes; ++i) {
    unsigned value = 0;
    for (std::size_t half = 0; half < 2; ++half) {
      const char c = text[i * 2 + half];
      unsigned nibble = 0;
      if (c >= '0' && c <= '9') {
        nibble = static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        nibble = static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        nibble = static_cast<unsigned>(c - 'A' + 10);
      } else {
        return false;
      }
      value = (value << 4) | nibble;
    }
    bytes[i] = static_cast<std::uint8_t>(value);
  }
  out = Digest256{bytes};
  return true;
}

void Sha256::reset() noexcept {
  state_ = kSha256Init;
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
}

void Sha256::compress(const std::uint8_t block[64]) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = static_cast<std::uint32_t>(w[i - 16] + s0 + w[i - 7] + s1);
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = static_cast<std::uint32_t>(h + s1 + ch + kSha256K[i] + w[i]);
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = static_cast<std::uint32_t>(s0 + maj);

    h = g;
    g = f;
    f = e;
    e = static_cast<std::uint32_t>(d + temp1);
    d = c;
    c = b;
    b = a;
    a = static_cast<std::uint32_t>(temp1 + temp2);
  }

  state_[0] = static_cast<std::uint32_t>(state_[0] + a);
  state_[1] = static_cast<std::uint32_t>(state_[1] + b);
  state_[2] = static_cast<std::uint32_t>(state_[2] + c);
  state_[3] = static_cast<std::uint32_t>(state_[3] + d);
  state_[4] = static_cast<std::uint32_t>(state_[4] + e);
  state_[5] = static_cast<std::uint32_t>(state_[5] + f);
  state_[6] = static_cast<std::uint32_t>(state_[6] + g);
  state_[7] = static_cast<std::uint32_t>(state_[7] + h);
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  if (data == nullptr || size == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += static_cast<std::uint64_t>(size);

  if (buffered_ > 0) {
    const std::size_t need = 64 - buffered_;
    const std::size_t take = (size < need) ? size : need;
    std::memcpy(buffer_.data() + buffered_, bytes, take);
    buffered_ += take;
    bytes += take;
    size -= take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  while (size >= 64) {
    compress(bytes);
    bytes += 64;
    size -= 64;
  }

  if (size > 0) {
    std::memcpy(buffer_.data(), bytes, size);
    buffered_ = size;
  }
}

Digest256 Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8ull;
  const std::uint8_t pad = 0x80;
  update(&pad, 1);
  const std::uint8_t zero = 0x00;
  while (buffered_ != 56) {
    update(&zero, 1);
  }
  std::uint8_t length_bytes[8];
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((bit_length >> ((7 - i) * 8)) & 0xFFu);
  }
  // Bypass the length accounting so that the appended length does not perturb
  // the already-captured bit length.
  std::memcpy(buffer_.data() + 56, length_bytes, 8);
  compress(buffer_.data());
  buffered_ = 0;

  std::array<std::uint8_t, Digest256::kBytes> out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
  }
  return Digest256{out};
}

Digest256 sha256(const void* data, std::size_t size) noexcept {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

Digest256 sha256(std::string_view text) noexcept { return sha256(text.data(), text.size()); }

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    crc = kCrc32cTable[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  }
  return static_cast<std::uint32_t>(crc ^ 0xFFFFFFFFu);
}

}  // namespace qosfabric
