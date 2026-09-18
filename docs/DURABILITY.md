# Durability

## Layout

```
<store>/
  MANIFEST.qfm          magic, format version, store identity, epoch,
                        snapshot reference, last sequence, CRC-32C
  SNAPSHOT-<seq>.qsn    full registry state at a committed sequence, CRC-32C
  WAL.qwl               framed append-only records since the snapshot
```

A frame is `[u32 payload length][u32 CRC-32C][payload]`, and the payload is
`[u16 frame version][u16 record type][u8 flags][u64 sequence][u64 attempt]
[u32 body length][body]`. Every length is checked against a hard budget before
any allocation, and every checksum is verified before any interpretation.

## The mutation protocol

```
validate  ->  bind authority  ->  plan  ->  reserve
   ->  journal intent (fsync)  ->  apply  ->  verify
   ->  journal commit (fsync)  ->  retire
```

A durable mutation is never acknowledged before its commit frame is durable.
The intent frame carries the full payload, so a crash between intent and commit
loses nothing that was promised and applies nothing that was not committed.

## Recovery

Recovery distinguishes five distinct situations, and never conflates them:

| Situation | Treatment |
| --- | --- |
| Torn tail: the final frame runs past the end of the file | Truncated at the last complete frame. The byte count is reported. |
| Complete frame with a bad checksum, nothing valid after it | Refused. `repair_truncate_corrupt_tail` must be set explicitly to discard it. |
| Complete frame with a bad checksum and a valid frame after it | Always refused, even with repair requested. This is mid-log corruption, not a torn write. |
| Intent without commit | Discarded and reported as an unfinished attempt. |
| Intent explicitly aborted | Discarded and reported. |

A missing or damaged manifest is rebuilt from the surviving checksummed
artifacts: the highest fully valid snapshot wins and the log is replayed over
it. A store written by a format version the runtime does not understand is
refused outright rather than partially interpreted.

## What durable state does not restore

Durable state does **not** restore process liveness, publisher authority, lease
validity, or evidence freshness. Every authoritative open:

1. advances the fabric epoch durably and monotonically;
2. discards every lease issued by the previous incarnation.

Capability declarations survive as *data* but are stamped with the epoch in
which they were asserted. After an epoch advance they are only usable if the
policy's staleness window covers the gap, which by default it does not. The
publisher incarnation watermark *is* durable, so an old process cannot speak
for an identity it already used.

A purely observational open (`claim_authority = false`, used by `status` and
`verify`) claims nothing: it advances no epoch and invalidates no lease, so
inspecting a running fabric can never fence it.

## Bounded growth

Every budget is declared in `include/qosfabric/limits.hpp`: record size, log
size, snapshot size, retained generations per identifier, retained identifiers,
retained decisions, retained conflict markers, in-flight intents, explanation
size. When the log budget is exhausted, the registry compacts automatically —
pruning retained generations, rewriting a snapshot and retiring the log prefix.
When the identifier budget is exhausted, publication is refused with
`capacity-exhausted` rather than growing without bound.

All externally influenced sizes and capacities use checked arithmetic.
Aggregation overflows are reported, never wrapped.

## Concurrency

Registry state and store state each have exactly one lock, and the acquisition
order is always registry then store. Public registry methods take the lock once
and delegate to helpers that assume it is held, so no path re-enters it.
`Compact()` is reachable from inside a publication (the automatic compaction
path) only through its unlocked internal form.
