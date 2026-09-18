// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/version.hpp"

#include "qosfabric/error.hpp"

namespace qosfabric {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid-argument";
    case ErrorCode::Malformed: return "malformed";
    case ErrorCode::TooLarge: return "too-large";
    case ErrorCode::NotFound: return "not-found";
    case ErrorCode::Duplicate: return "duplicate";
    case ErrorCode::Conflict: return "conflict";
    case ErrorCode::Stale: return "stale";
    case ErrorCode::Fenced: return "fenced";
    case ErrorCode::Unauthorized: return "unauthorized";
    case ErrorCode::Corrupt: return "corrupt";
    case ErrorCode::IoError: return "io-error";
    case ErrorCode::VersionMismatch: return "version-mismatch";
    case ErrorCode::Overflow: return "overflow";
    case ErrorCode::NotSupported: return "not-supported";
    case ErrorCode::CapacityExhausted: return "capacity-exhausted";
    case ErrorCode::ShuttingDown: return "shutting-down";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::Internal: return "internal";
  }
  return "unknown";
}

std::string_view version_string() noexcept { return "1.0.0"; }

std::string_view build_configuration() noexcept {
#if defined(NDEBUG)
  return "release";
#else
  return "debug";
#endif
}

}  // namespace qosfabric
