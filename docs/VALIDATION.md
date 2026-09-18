# Validation

## What is proven, and how

| Suite | What it establishes |
| --- | --- |
| `test_core` | SHA-256 against published vectors including the streamed 1,000,000-byte case; CRC-32C against its published check vector; checked arithmetic; identifier alphabet; serialization bounds in both directions; model validation; canonical record round-trips and rejection of every truncated prefix; content digests ignoring assertion metadata; decision record round-trip; bounded explanations; total outcome precedence. |
| `test_engine` | End-to-end composition arithmetic (weakest link, additive, loss composition, diversity); the exact degradation window; no silent downgrade under any combination of class and policy permission; UNKNOWN never becoming a positive verdict; stale generation, epoch, expiry, age and currency; conflict precedence; structural rejection; unavailable components; resource-declared degradation; report-only policy; reservation verification; predicate conjunction; deterministic and canonically ordered decisions; the authority vector. |
| `test_registry` | Generation assignment and advancement, idempotent re-publication, equivocation recorded durably and surviving restart, publisher incarnation fencing across restarts, capability publication requiring live authority, monotonic epoch advance invalidating leases, durable decision records, compaction round-tripping the entire registry, observation-only opens changing nothing, retention bounds, and refusal of malformed publications. |
| `test_store` | Frame round-trip and ordering; unfinished intents never applied; aborted intents discarded; torn tails truncated and reported; mid-log corruption refused even with repair requested; tail checksum failure requiring explicit repair; manifest rebuilt from artifacts; unsupported format version refused; record and log budgets enforced; compaction preserving state and shrinking the log; verification detecting a damaged snapshot. |
| `test_wire` | Frame round-trip; every single-bit corruption of a frame either refused or decoded as the same message (no corrupted frame is ever accepted as a *different* well-formed message); every truncation prefix refused; oversized and trailing frames refused; protocol version mismatch refused; unknown message types refused; error-to-status mapping never leaking internal detail. |
| `test_property` | Seeded randomized properties over 300-odd cases each: determinism of digest and explanation; missing evidence never producing a positive verdict; worsening a component never improving a verdict; component permutation preserving composed values; the degradation boundary exact at `limit`, `effective`, and one unit either side; random byte strings never accepted as records; wide paths staying bounded and explainable. |
| `test_concurrency` | Eight threads evaluating the same contract agree on the digest exactly; concurrent distinct publications all land; concurrent equivocation produces exactly one winner; concurrent epoch advance and reads stay consistent; shutdown during in-flight work refuses rather than drops, and everything acknowledged survives a reopen. |
| `test_multiprocess` | Real processes over real loopback TCP with real framing: a publisher claims a lease and publishes; a restart takes the next incarnation and is accepted; a rewound durable boot counter is fenced across a process boundary; an abruptly terminated registry causes later publishers to fail rather than silently succeed; the durable store is recovered in a brand new process; generations continue across a real restart. |

No test declares a timeout. A hanging test is treated as a defect to diagnose.

## Toolchain coverage

* **Release, MSVC 19.44 x64, `/W4 /WX /permissive- /Zc:__cplusplus /utf-8`** —
  clean, 8/8 suites pass.
* **Debug, same flags** — clean, 8/8 suites pass.
* **AddressSanitizer** (`/fsanitize=address` with debug information) — clean,
  8/8 suites pass. Confirmed active: the built test binaries import
  `clang_rt.asan_dynamic-x86_64.dll`.
* **MSVC static analysis** (`/analyze`) — clean. One first-party finding was
  reported and fixed (an unproven bound in the CRC-32C table construction).
* **UndefinedBehaviorSanitizer** — not available with this toolchain on this
  platform. It is offered as a CMake option for GCC and Clang builds and is
  stated here as unavailable rather than silently skipped.
* **ThreadSanitizer** — not available with this toolchain. Race coverage is by
  construction (two locks, one acquisition order, no callbacks under a lock)
  and by the concurrency suite, not by a race detector. That limitation is
  stated rather than papered over.

## REAL, SYNTHETIC, UNSUPPORTED

**REAL.** Real operating-system processes, real TCP loopback sockets, real
length-prefixed and checksummed framing, real abrupt process termination, real
files with real fsync barriers, real process restarts, real incarnation and
epoch fencing across process boundaries, real corruption injected into logs and
snapshots, real published cryptographic test vectors.

**SYNTHETIC.** Property, adversarial, boundary, concurrency and benchmark
workloads are generated in memory from a seeded generator. They are labelled
synthetic everywhere they appear and are never presented as physical-network
measurements.

**UNSUPPORTED.** Multi-node, multi-switch, RDMA, NVLink, optical, NIC, DPU and
physical-network validation. Nothing here was run against real fabric
hardware, and no such claim is made anywhere in this repository.
