// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Checked integer arithmetic for every externally influenced size, capacity,
// duration or rate. Overflow is a typed failure, never silent wraparound.

#ifndef QOSFABRIC_CHECKED_HPP
#define QOSFABRIC_CHECKED_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace qosfabric {

template <class T>
[[nodiscard]] constexpr bool checked_add(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked arithmetic is defined for unsigned types");
  if (b > static_cast<T>(std::numeric_limits<T>::max() - a)) {
    return false;
  }
  out = static_cast<T>(a + b);
  return true;
}

template <class T>
[[nodiscard]] constexpr bool checked_mul(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked arithmetic is defined for unsigned types");
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) {
    return false;
  }
  out = static_cast<T>(a * b);
  return true;
}

template <class T>
[[nodiscard]] constexpr bool checked_ceil_div(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked arithmetic is defined for unsigned types");
  if (b == 0) {
    return false;
  }
  T q = static_cast<T>(a / b);
  if (a % b != 0) {
    if (q == std::numeric_limits<T>::max()) {
      return false;
    }
    q = static_cast<T>(q + 1);
  }
  out = q;
  return true;
}

// Saturating multiply-add used by bounded-growth accounting where saturation
// (rather than failure) is the correct response.
template <class T>
[[nodiscard]] constexpr T saturating_add(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "saturating arithmetic is defined for unsigned types");
  T out = 0;
  if (!checked_add(a, b, out)) {
    return std::numeric_limits<T>::max();
  }
  return out;
}

template <class T>
[[nodiscard]] constexpr T saturating_sub(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "saturating arithmetic is defined for unsigned types");
  return (a < b) ? static_cast<T>(0) : static_cast<T>(a - b);
}

// Applies a parts-per-million relaxation to a value, rounding toward zero and
// returning false on overflow. Used exclusively for the explicit, policy-bound
// degradation windows; it is never used to invent authority.
[[nodiscard]] inline bool apply_ppm_relaxation(std::uint64_t value, std::uint32_t ppm,
                                               std::uint64_t& out) noexcept {
  if (ppm > 1000000u) {
    return false;
  }
  std::uint64_t delta = 0;
  if (!checked_mul<std::uint64_t>(value, static_cast<std::uint64_t>(ppm), delta)) {
    return false;
  }
  delta = static_cast<std::uint64_t>(delta / 1000000ull);
  return checked_add<std::uint64_t>(value, delta, out);
}

// Expresses an absolute shortfall as parts per million of a reference value,
// rounding up so that a nonzero shortfall is never reported as zero
// relaxation. A zero reference with a nonzero shortfall is total loss of the
// requirement, reported as one million parts per million.
[[nodiscard]] inline bool relative_ppm(std::uint64_t reference, std::uint64_t shortfall,
                                       std::uint32_t& out_ppm) noexcept {
  if (shortfall == 0) {
    out_ppm = 0u;
    return true;
  }
  if (reference == 0) {
    out_ppm = 1000000u;
    return true;
  }
  std::uint64_t scaled = 0;
  if (!checked_mul<std::uint64_t>(shortfall, 1000000ull, scaled)) {
    out_ppm = 1000000u;
    return true;
  }
  std::uint64_t ppm = 0;
  if (!checked_ceil_div<std::uint64_t>(scaled, reference, ppm)) {
    return false;
  }
  out_ppm = static_cast<std::uint32_t>(ppm > 1000000ull ? 1000000ull : ppm);
  return true;
}

}  // namespace qosfabric

#endif  // QOSFABRIC_CHECKED_HPP
