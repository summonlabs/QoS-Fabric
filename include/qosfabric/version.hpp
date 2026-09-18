// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef QOSFABRIC_VERSION_HPP
#define QOSFABRIC_VERSION_HPP

#include <cstdint>
#include <string_view>

#define QOSFABRIC_VERSION_MAJOR 1
#define QOSFABRIC_VERSION_MINOR 0
#define QOSFABRIC_VERSION_PATCH 0

namespace qosfabric {

inline constexpr std::uint32_t kVersionMajor = QOSFABRIC_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = QOSFABRIC_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = QOSFABRIC_VERSION_PATCH;

// Durable-store format version. Independent of the library version: a store
// written by a format the runtime does not understand is refused outright
// rather than being partially interpreted.
inline constexpr std::uint16_t kStoreFormatVersion = 1;
// Publisher wire protocol version.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

[[nodiscard]] std::string_view version_string() noexcept;
[[nodiscard]] std::string_view build_configuration() noexcept;

}  // namespace qosfabric

#endif  // QOSFABRIC_VERSION_HPP
