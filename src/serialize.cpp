// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/serialize.hpp"

namespace qosfabric {
namespace {

void store_le(std::uint64_t value, std::uint8_t* out, std::size_t width) noexcept {
  for (std::size_t i = 0; i < width; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
  }
}

[[nodiscard]] std::uint64_t load_le(const std::uint8_t* in, std::size_t width) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (i * 8);
  }
  return value;
}

}  // namespace

void ByteWriter::u8(std::uint8_t value) { raw(&value, 1); }

void ByteWriter::u16(std::uint16_t value) {
  std::uint8_t tmp[2];
  store_le(value, tmp, 2);
  raw(tmp, 2);
}

void ByteWriter::u32(std::uint32_t value) {
  std::uint8_t tmp[4];
  store_le(value, tmp, 4);
  raw(tmp, 4);
}

void ByteWriter::u64(std::uint64_t value) {
  std::uint8_t tmp[8];
  store_le(value, tmp, 8);
  raw(tmp, 8);
}

void ByteWriter::raw(const void* data, std::size_t size) {
  if (failed_) {
    return;
  }
  if (size > limit_ || buffer_.size() > limit_ - size) {
    failed_ = true;
    return;
  }
  if (size == 0) {
    return;
  }
  if (data == nullptr) {
    failed_ = true;
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void ByteWriter::bytes(const std::vector<std::uint8_t>& value) {
  if (value.size() > 0xFFFFFFFFull) {
    failed_ = true;
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value.data(), value.size());
}

void ByteWriter::str(std::string_view value, std::size_t max_len) {
  if (value.size() > max_len) {
    failed_ = true;
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value.data(), value.size());
}

std::uint8_t ByteReader::u8() {
  const std::string_view view = raw(1);
  if (failed_) {
    return 0;
  }
  return static_cast<std::uint8_t>(view[0]);
}

std::uint16_t ByteReader::u16() {
  const std::string_view view = raw(2);
  if (failed_) {
    return 0;
  }
  return static_cast<std::uint16_t>(load_le(reinterpret_cast<const std::uint8_t*>(view.data()), 2));
}

std::uint32_t ByteReader::u32() {
  const std::string_view view = raw(4);
  if (failed_) {
    return 0;
  }
  return static_cast<std::uint32_t>(load_le(reinterpret_cast<const std::uint8_t*>(view.data()), 4));
}

std::uint64_t ByteReader::u64() {
  const std::string_view view = raw(8);
  if (failed_) {
    return 0;
  }
  return load_le(reinterpret_cast<const std::uint8_t*>(view.data()), 8);
}

std::string_view ByteReader::raw(std::size_t size) {
  if (failed_) {
    return {};
  }
  if (size > remaining()) {
    failed_ = true;
    return {};
  }
  const char* begin = reinterpret_cast<const char*>(data_ + pos_);
  pos_ += size;
  return std::string_view(begin, size);
}

std::vector<std::uint8_t> ByteReader::bytes(std::size_t max_len) {
  const std::uint32_t length = u32();
  if (failed_) {
    return {};
  }
  if (length > max_len) {
    failed_ = true;
    return {};
  }
  const std::string_view view = raw(length);
  if (failed_) {
    return {};
  }
  return std::vector<std::uint8_t>(view.begin(), view.end());
}

std::string ByteReader::str(std::size_t max_len) {
  const std::uint32_t length = u32();
  if (failed_) {
    return {};
  }
  if (length > max_len) {
    failed_ = true;
    return {};
  }
  const std::string_view view = raw(length);
  if (failed_) {
    return {};
  }
  return std::string(view);
}

bool ByteReader::presence() {
  const std::uint8_t value = u8();
  if (failed_) {
    return false;
  }
  if (value > 1u) {
    failed_ = true;
    return false;
  }
  return value == 1u;
}

bool ByteReader::count(std::size_t max_count, std::size_t& out) {
  const std::uint32_t value = u32();
  if (failed_) {
    return false;
  }
  if (value > max_count) {
    failed_ = true;
    return false;
  }
  out = value;
  return true;
}

}  // namespace qosfabric
