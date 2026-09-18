// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Umbrella header for the QoS Fabric runtime.
//
// QoS Fabric is a vendor-neutral C++20 runtime that owns service-class
// semantics and the end-to-end obligations those semantics imply across
// distributed fabric paths. It answers one question authoritatively:
//
//   Given a traffic or workload service requirement, a policy, the exact path
//   and resource capabilities, reservations, a priority class and the current
//   generations, what end-to-end service class is authoritative, which
//   obligations must every participating resource satisfy, and when is the
//   contract incompatible, degraded, stale, or no longer enforceable?
//
// Boundary: it does not admit traffic, reserve bandwidth, arbitrate capacity,
// schedule flows, place paths, enforce rates, configure queues, or collect
// telemetry. It consumes declarations made by those systems and produces an
// authoritative, explainable verdict bound to the exact evidence that
// justified it.

#ifndef QOSFABRIC_QOSFABRIC_HPP
#define QOSFABRIC_QOSFABRIC_HPP

#include "qosfabric/checked.hpp"
#include "qosfabric/crypto.hpp"
#include "qosfabric/decision.hpp"
#include "qosfabric/engine.hpp"
#include "qosfabric/error.hpp"
#include "qosfabric/ids.hpp"
#include "qosfabric/limits.hpp"
#include "qosfabric/model.hpp"
#include "qosfabric/net.hpp"
#include "qosfabric/registry.hpp"
#include "qosfabric/serialize.hpp"
#include "qosfabric/store.hpp"
#include "qosfabric/version.hpp"
#include "qosfabric/wire.hpp"

#endif  // QOSFABRIC_QOSFABRIC_HPP
