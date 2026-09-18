// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounded resources. Every externally influenced size, count, depth, or
// duration that QoS Fabric admits is capped by a constant declared here so
// that admission, serialization, persistence, explanation rendering and wire
// decoding all share one auditable budget.

#ifndef QOSFABRIC_LIMITS_HPP
#define QOSFABRIC_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace qosfabric {
namespace limits {

// --- Identifier and text budgets -------------------------------------------
inline constexpr std::size_t kMaxIdentifierLen = 96;
inline constexpr std::size_t kMaxLabelLen = 128;
inline constexpr std::size_t kMaxDetailLen = 192;
inline constexpr std::size_t kMaxReasonLen = 256;
inline constexpr std::size_t kMaxOriginLen = 128;
inline constexpr std::size_t kMaxPredicateKeyLen = 64;

// --- Structural budgets ----------------------------------------------------
inline constexpr std::size_t kMaxPathComponents = 256;
inline constexpr std::size_t kMaxRequiredPredicates = 64;
inline constexpr std::size_t kMaxCapabilityPredicates = 64;
inline constexpr std::size_t kMaxObligations = 128;
inline constexpr std::size_t kMaxDegradations = 64;
// The authority vector carries one entry per referenced authority dimension
// plus one per resolved capability and reservation, so its budget is derived
// from the path budget rather than guessed.
inline constexpr std::size_t kMaxAuthorityEntries = 8 + 2 * kMaxPathComponents;
inline constexpr std::size_t kMaxComponentAssessments = 256;

// --- Serialized and in-flight budgets --------------------------------------
inline constexpr std::size_t kMaxCanonicalRecordBytes = 1u << 20;   // 1 MiB
inline constexpr std::size_t kMaxWireFrameBytes = 256u << 10;       // 256 KiB
inline constexpr std::size_t kMaxWalRecordBytes = 4u << 20;         // 4 MiB
inline constexpr std::size_t kMaxSnapshotBytes = 64u << 20;         // 64 MiB
inline constexpr std::size_t kMaxExplainBytes = 64u << 10;          // 64 KiB
inline constexpr std::size_t kMaxExplainObligations = 64;
inline constexpr std::size_t kMaxExplainComponents = 256;
inline constexpr std::size_t kMaxErrorDetailBytes = 512;

// --- Durable retention budgets (bounded growth) ----------------------------
inline constexpr std::size_t kMaxGenerationsPerId = 8;
inline constexpr std::size_t kMaxClassesRetained = 512;
inline constexpr std::size_t kMaxPoliciesRetained = 256;
inline constexpr std::size_t kMaxPathsRetained = 1024;
inline constexpr std::size_t kMaxCapabilitiesRetained = 4096;
inline constexpr std::size_t kMaxPublishersRetained = 512;
inline constexpr std::size_t kMaxDecisionsRetained = 4096;
inline constexpr std::size_t kMaxJournalIntentsInFlight = 256;

// --- Numeric domains -------------------------------------------------------
inline constexpr std::uint64_t kMaxRateBps = 1'000'000'000'000ull;      // 1 Tbps
inline constexpr std::uint64_t kMaxLatencyUs = 3'600'000'000ull;        // 1 hour
inline constexpr std::uint64_t kMaxJitterUs = 3'600'000'000ull;
inline constexpr std::uint64_t kMaxBurstBytes = 1ull << 40;             // 1 TiB
inline constexpr std::uint64_t kMaxBurstIntervalUs = 3'600'000'000ull;
inline constexpr std::uint64_t kMaxMtuBytes = 1ull << 24;
inline constexpr std::uint32_t kMaxLossPpm = 1'000'000u;                // 100 percent
inline constexpr std::uint32_t kMaxRelaxationPpm = 1'000'000u;          // 100 percent
inline constexpr std::uint32_t kMaxPriorityRank = 64;
inline constexpr std::uint32_t kMaxDisjointPaths = 64;
inline constexpr std::uint32_t kMaxStaleEpochs = 4096;
inline constexpr std::uint64_t kMaxLeaseMs = 3'600'000ull;              // 1 hour

// --- Transport budgets -----------------------------------------------------
inline constexpr std::uint32_t kDefaultListenBacklog = 16;
inline constexpr std::uint32_t kMaxServerConnections = 64;
inline constexpr std::uint64_t kMaxConnectionIdleMs = 600'000ull;

}  // namespace limits
}  // namespace qosfabric

#endif  // QOSFABRIC_LIMITS_HPP
