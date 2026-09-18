# Design

## The boundary, precisely

QoS Fabric is a decision authority, not a data plane.

**Owned.** Service-class definitions and their generations; the end-to-end
obligations a class implies; capability compatibility across a whole path;
policy binding; generation, epoch and incarnation authority; freshness
accounting; degradation accounting; equivocation evidence; the authority vector
and the explanation; the durable record of all of the above.

**Not owned.** Traffic admission, bandwidth reservation, capacity arbitration,
flow scheduling, path placement, rate enforcement, queue configuration and
telemetry collection. Each of those is performed by a different system that
*declares* what it has done; QoS Fabric consumes those declarations, checks
them against each other, and answers a question about the composite.

A reservation is the clearest example. QoS Fabric verifies that a reservation
exists, that it names the same class generation, that it is not expired and
that it commits at least the class minimum. It never creates, sizes or renews
one.

## Why the outcomes are ordered

The precedence order is not a convenience; it encodes which finding is
*conclusive*:

* A request that cannot be formed has no meaning, so `REJECTED` dominates.
* A definition that disagrees with itself cannot be resolved by evidence, so
  `CONFLICT` dominates everything except `REJECTED`.
* Stale evidence cannot be used to assert anything at all, not even a
  negative, so `STALE` dominates `UNSUPPORTED`.
* A definitive negative from one required component is conclusive, so
  `UNSUPPORTED` dominates `UNKNOWN`.
* Missing evidence is never a licence, so `UNKNOWN` dominates the positive
  outcomes.
* An explicit, permitted relaxation is still a contract, but never a clean
  one, so `SUPPORTED_DEGRADED` sits above `SUPPORTED`.

The invariant "UNKNOWN cannot become SUPPORTED" is therefore structural: there
is no code path that removes `UNKNOWN` from a merged outcome. The policy field
`max_unknown_components` is a *reporting tolerance*; it does not change a
verdict.

## Determinism

Everything that varies with time or with registry state is an explicit input
to `evaluate()`: the current epoch, the wall clock, the currency vector, the
conflict markers. The obligation set is built in canonical order
`(kind, identifier)`, predicates are treated as a set and emitted sorted, and
content digests address the definition rather than the assertion. Two equal
inputs therefore yield byte-identical explanations and identical decision
digests, which is what makes a decision auditable after the fact.

## The degradation window, exactly

For a requirement `R` and a composed end-to-end value `V`:

```
window = min(class.degrade_max_relaxation_ppm, policy.max_relaxation_ppm)
delta  = floor(R * window / 1e6)
limit  = (direction == at-least) ? R - delta : R + delta
```

The obligation is `Degraded` when `V` still satisfies `limit`, otherwise
`Violated`. The applied relaxation is reported as
`ceil(shortfall * 1e6 / R)`, so a non-zero shortfall can never be reported as
zero relaxation.

Structural obligations are excluded from this arithmetic entirely. Isolation,
treatment, priority rank, capability predicates and reservations cannot be
relaxed at any window width, because a class that could silently lose its
isolation or its reservation would not be a class.

## Unfinished work and shutdown

Cancelled or failed work never reports success and never mutates authoritative
state: a staged mutation whose commit frame never becomes durable is discarded
on recovery and reported as an unfinished attempt. Shutdown stops accepting
work first and only then commits the clean-shutdown marker, so recovery can
distinguish a tidy stop from an abrupt one.

## Locking

There are exactly two locks: registry state and store state. A public registry
method takes the registry lock once at entry; every helper assumes it is held
and never re-acquires it. The only permitted nesting is registry state then
store state. No callback, no I/O completion and no user code is invoked while a
lock is held. This is checked by the concurrency suite, which drives real
thread interleaving against shared authority.
