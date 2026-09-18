# QoS Fabric

Open-source, vendor-neutral C++20 runtime for authoritative end-to-end network
service classes, obligations, compatibility, generations, and policy-bound QoS
contracts across distributed fabric paths.

Version 1.0.0. C++20. Apache License 2.0.

## The question this runtime answers

Given a traffic or workload service requirement, a policy, the exact path and
resource capabilities, reservations, a priority class and the current
generations, **what end-to-end service class is authoritative, which
obligations must every participating resource satisfy, and when is the contract
incompatible, degraded, stale, or no longer enforceable?**

QoS Fabric answers that question deterministically and explains the answer. A
class label alone is not proof of delivery, so the runtime never treats one as
proof.

## Boundary

QoS Fabric **owns** service-class semantics and the end-to-end obligations
those semantics imply: class definitions and generations, contract evaluation,
capability compatibility, policy binding, freshness and generation authority,
degradation accounting, and the explanation of a verdict.

QoS Fabric **does not** admit traffic, reserve bandwidth, arbitrate capacity,
schedule flows, place paths, enforce rates, configure queues, or collect
telemetry. It consumes declarations made by the systems that do those things,
verifies them against each other, and produces an authoritative verdict bound
to the exact evidence that justified it. It verifies reservations; it never
creates one.

## What is implemented

### Authoritative decision

`evaluate()` is a pure function of explicit inputs. The same inputs always
produce a byte-identical explanation and an identical decision digest. Two
independent runs, two threads, and a replay from durable state all produce the
same digest for the same evidence.

### Outcomes and precedence

Outcomes are totally ordered. The dominant outcome of any set of findings is
the one with the highest precedence:

| Precedence | Outcome | Meaning |
| --- | --- | --- |
| 1 | `SUPPORTED` | Every required obligation is satisfied by every required component with live, current evidence, and nothing was relaxed. |
| 2 | `SUPPORTED_DEGRADED` | Everything is enforceable, but an explicit, policy-permitted relaxation or a resource-declared degradation is in force. Never silent. |
| 3 | `UNKNOWN` | At least one required component offers no evidence for an attribute an obligation needs. |
| 4 | `UNSUPPORTED` | At least one required component is definitively unable to satisfy a binding obligation, or declared itself unavailable. |
| 5 | `STALE` | Applicability is invalidated: superseded or expired authority, path, capability, policy or epoch. |
| 6 | `CONFLICT` | Contradiction: an equivocated definition, a policy that denies the referenced class, or a path and resource that disagree about a safety-relevant property. |
| 7 | `REJECTED` | The contract cannot be formed at all: structurally invalid input, or a referenced authority definition that does not exist. |

Absent definitions are `REJECTED`; superseded definitions are `STALE`; absent
*resource evidence* is `UNKNOWN`. Those three cases are deliberately distinct.

### End-to-end composition

Obligations are not evaluated per resource in isolation and then waived. They
compose across the whole path:

| Obligation | Direction | Composition |
| --- | --- | --- |
| `rate.min_bps`, `rate.max_bps`, `rate.peak_bps` | at least | weakest link over the path |
| `latency.max_us`, `jitter.max_us` | at most | checked sum, plus path fixed overhead |
| `loss.max_ppm` | at most | independent loss composition, `1 - product(1 - loss_i)` in parts per million |
| `burst.bytes` | at least | weakest link |
| `burst.interval_us` | at most | worst case |
| `mtu.min_bytes` | at least | weakest link |
| `isolation.level`, `treatment.mode`, `priority.min_rank` | at least | weakest link on an ordered ladder |
| `reservation.min_rate_bps` | at least | weakest link over committed reservations |
| `redundancy.disjoint_paths` | at least | distinct failure domains among components that independently satisfy every other obligation |
| `capability.predicate.*` | at least | conjunctive: every component must declare the predicate |
| `resource.state` | at least | worst declared liveness state |

Structural obligations (isolation, treatment, priority, predicates,
reservations) are **never** relaxable: they cannot be silently dropped.

### Degradation is explicit and bounded

A relaxation is applied only when **all** of the following hold:

1. the class declares `allow_degrade` with a non-zero window;
2. the policy permits degradation with a non-zero ceiling;
3. the class violation policy is not `Reject`;
4. the shortfall is inside `min(class window, policy ceiling)`.

The applied relaxation, the affected obligation, the resource it binds to, and
the resulting effective limit are always reported. The `ReportOnly` violation
policy requires both a class window and policy permission, forces
`SUPPORTED_DEGRADED`, and can never produce a clean `SUPPORTED`.

### Genuine end-to-end semantics

The runtime composes real numbers, not labels. Latency is summed with checked
arithmetic; loss is composed as independent probabilities; rates resolve to the
weakest link; diversity counts distinct failure domains among components that
qualify on their own evidence. The output names the **binding obligation** and
the **binding resource** for every verdict.

### Generations, epochs and fencing

Every durable definition carries a generation. Every assertion carries the
fabric epoch it was made in. Every publisher carries a monotonic incarnation.

* A path binds the exact `ResourceCapabilityGeneration` it was computed
  against; evidence at any other generation is staleness, not an upgrade.
* Publication at an existing generation with different content is
  **equivocation**. It is refused, recorded durably, and every later contract
  that depends on that definition is `CONFLICT`.
* Publisher incarnations must strictly increase. Reusing one is `FENCED`,
  including across a real process restart.
* Every authoritative open of a registry advances the fabric epoch durably and
  discards every lease. Durable state never restores process liveness,
  publisher authority, lease validity or evidence freshness.
* Content digests address the **definition**, not the assertion: epoch,
  timestamps and provenance are normalized away, so byte-identical content is
  idempotent and genuinely different content at the same generation is
  detectable as equivocation.

### Durability

State lives in one directory:

```
MANIFEST.qfm          format version, store identity, epoch, snapshot reference, CRC
SNAPSHOT-<seq>.qsn    checksummed full state at a committed sequence
WAL.qwl               framed, checksummed append-only records since the snapshot
```

Every mutation follows *validate -> bind authority -> plan -> stage intent
(fsync) -> apply -> commit (fsync)*. A mutation is never acknowledged before
its commit frame is durable. Recovery distinguishes a torn tail (truncated and
reported) from genuine mid-log corruption (refused, never silently discarded),
reports unfinished attempts, and rebuilds a missing manifest from the surviving
checksummed artifacts.

### Explanation

Every decision can be rendered as bounded, human-readable text containing the
class definition, subject, obligations, per-resource status, the binding
obligation and resource, the authority vector, and the degradation reasons.

## Building

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

Options: `QOSFABRIC_BUILD_TESTS`, `QOSFABRIC_BUILD_TOOLS`,
`QOSFABRIC_WARNINGS_AS_ERRORS`, `QOSFABRIC_ENABLE_ASAN`,
`QOSFABRIC_ENABLE_UBSAN`, `QOSFABRIC_ENABLE_STATIC_ANALYSIS`.

No test declares a timeout. A test that hangs is a defect to diagnose, not
something to kill.

## Installing and consuming

```
cmake --install build --config Release --prefix /your/prefix
```

A downstream project then writes:

```cmake
find_package(QoSFabric 1.0 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE qosfabric::qosfabric)
```

`tests/consumer/` is exactly such a project and is deliberately not part of the
QoS Fabric build.

## Command line

```
qosfabric_cli init      --store DIR
qosfabric_cli status    --store DIR
qosfabric_cli verify    --store DIR
qosfabric_cli compact   --store DIR
qosfabric_cli scenario  --store DIR --file FILE
qosfabric_cli serve     --store DIR [--port N] [--port-file PATH]
qosfabric_cli selftest
qosfabric_publisher     --port N --boot-file PATH --publisher-id ID --resource ID ...
```

`status` and `verify` are observational: they claim no authority and therefore
advance no epoch and invalidate no lease. `examples/voice-toll.scenario` is a
complete worked scenario.

## Benchmarks

Measured with `qosfabric_bench` on Windows 10.0.26200, MSVC 19.44.35222, x64,
Release with `/W4 /WX`. Every figure counts **completed** work, never enqueued
work. The workload is SYNTHETIC and in-memory on a single host: no physical
network, switch, NIC or fabric hardware is involved, and none of these numbers
is a physical-network measurement. Ranges cover two runs on a shared machine.

| Measurement | Completed work per run | Observed rate |
| --- | --- | --- |
| Evaluate a 4-component contract, 1 thread | 20,000 decisions | 24k - 65k decisions/s |
| Evaluate a 4-component contract, 8 threads | 160,000 decisions | 93k - 315k decisions/s |
| Durable class publication (intent fsync + commit fsync) | 2,000 publications | 67 - 98 publications/s |
| Reopen and replay a compacted store | 1 replayed record | 5 - 28 ms |
| Verify manifest, snapshot and log integrity | 1 verification | 4 ms |

The durable publication rate is orders of magnitude below the evaluation rate
on purpose: each publication crosses two fsync barriers before it is
acknowledged. A faster number there would mean an acknowledgement that does not
mean what it claims.

## Validation status

Labeled precisely, because the difference matters.

**REAL.** Real operating-system processes, a real TCP loopback socket, real
length-prefixed and checksummed framing, real abrupt process termination, real
durable files, real process restarts, real incarnation and epoch fencing across
process boundaries, real checksummed corruption injected into logs and
snapshots. SHA-256 and CRC-32C are checked against published test vectors.

**SYNTHETIC.** Property, adversarial, concurrency and boundary suites use a
seeded deterministic generator. Populations are synthetic and are never
presented as physical-network measurements. The benchmark numbers below are
synthetic input throughput, not fabric throughput.

**UNSUPPORTED.** No multi-node, multi-switch, RDMA, NVLink, optical, NIC, DPU
or physical-network validation was performed, and none is claimed. Everything
distributed runs on one host over loopback. No telemetry is collected or
transmitted by anything in this repository.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
