// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Fallible-result plumbing. QoS Fabric does not use exceptions across its
// public boundary: every operation that can fail on hostile, stale, or
// missing input returns a Result carrying a typed ErrorCode.

#ifndef QOSFABRIC_ERROR_HPP
#define QOSFABRIC_ERROR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "qosfabric/limits.hpp"

namespace qosfabric {

enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument,
  Malformed,
  TooLarge,
  NotFound,
  Duplicate,
  Conflict,
  Stale,
  Fenced,
  Unauthorized,
  Corrupt,
  IoError,
  VersionMismatch,
  Overflow,
  NotSupported,
  CapacityExhausted,
  ShuttingDown,
  Cancelled,
  Internal,
};

[[nodiscard]] const char* to_string(ErrorCode code) noexcept;

class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string detail) : code_(code) {
    if (detail.size() > limits::kMaxErrorDetailBytes) {
      detail.resize(limits::kMaxErrorDetailBytes);
    }
    detail_ = std::move(detail);
  }

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }

  [[nodiscard]] std::string message() const {
    std::string out = to_string(code_);
    if (!detail_.empty()) {
      out += ": ";
      out += detail_;
    }
    return out;
  }

 private:
  ErrorCode code_{ErrorCode::Ok};
  std::string detail_{};
};

// A Result is either a value or an Error. Result<void> carries only the error
// channel. Both are cheap to move and never throw on the success path.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] T& operator*() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& operator*() const& { return std::get<0>(storage_); }
  [[nodiscard]] T* operator->() { return &std::get<0>(storage_); }
  [[nodiscard]] const T* operator->() const { return &std::get<0>(storage_); }

  [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }

  [[nodiscard]] ErrorCode code() const noexcept {
    return has_value() ? ErrorCode::Ok : std::get<1>(storage_).code();
  }

 private:
  std::variant<T, Error> storage_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ErrorCode code() const noexcept { return error_.code(); }

 private:
  Error error_{};
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string_view detail) {
  return Error{code, std::string(detail)};
}

}  // namespace qosfabric

#endif  // QOSFABRIC_ERROR_HPP
