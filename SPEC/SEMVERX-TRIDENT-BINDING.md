# SemVerX Trident Binding — Rationale and Proofs

**MMUKO ABI revision 1** · companion to `MMUKO-ABI-SPEC.md` §6–§8

This document argues *why* the resolver is shaped the way it is, and proves the
four properties the specification claims. It is the document to read before
proposing a change to the ordering in §7.3 of the specification, because most
proposed changes to it break one of these proofs.

---

## 1. Two layers, and why they must not be merged

There are two distinct questions in dependency resolution, and conflating them
is the root of most package-manager failure modes.

**Selection:** given several candidates that could fill a slot, which should we
try? This is a judgement. It weighs endorsement, provenance, maturity and
recency, and reasonable people tune it differently.

**Consensus:** may *this* candidate fill *this* slot? This is not a judgement.
Either the machine contracts match or they do not.

```
   candidates ──▶ [ CONSENSUS GATE ] ──▶ survivors ──▶ [ SELECT ] ──▶ winner
                    binary, factual                     weighted, tunable
```

**The gate runs first.** A candidate with unanimous endorsement, perfect
provenance and the highest version in the registry scores exactly nothing if its
fingerprint disagrees with the call site, because it never enters the scored set
at all.

Reversing the order — score everything, then check the winner — has two failure
modes, and the second is worse than the first. The obvious one: a high-scoring
broken candidate beats a low-scoring correct one, and the resolver reports "no
viable provider" while a working provider sits unexamined. The insidious one: a
weighted scheme with a "compatibility" term *in* it can be tuned until a broken
candidate wins. Under deployment pressure, on a Friday, someone will tune it.

So compatibility is a **filter**, never a **term**. `select.rs` implements the
scoring layer and calls the real resolver to filter, rather than reimplementing
the comparison — there is exactly one place in this system that decides whether
two contracts agree, and selection is not permitted to become a second one.

---

## 2. Why the fingerprint precedes the version

This is the same argument one level down, and it is the single most important
ordering in the system.

**ABI identity is a machine fact.** Two pieces of machine code either agree
about which register holds the second argument, or they do not. Nobody's
intention enters into it.

**Version compatibility is a policy claim.** `2.0.0` means "the author believes
this breaks things"; `1.4.2` means "the author believes it does not". Both are
assertions by a human being about their own work, made under time pressure,
often about code they wrote months ago.

Every existing package manager ultimately trusts the second kind of claim. That
is the actual root cause of the transcript's failure: the resolver had nothing
but a name and a number to work with, and the number was not wrong — v2 *was* a
new major version — but nothing in the system connected that number to the
caller's compiled expectations.

MMUKO consults the fact first and never lets the claim override it. Concretely:

- A module that bumps only its patch number and changes a parameter width is
  refused. The version says "safe"; the fingerprint says otherwise; the
  fingerprint wins.
- A module promoted from `2.experimental` to `2.stable` without changing its
  code is *still* refused against a caller compiled for `1.stable`, and the
  fault is reported as `FINGERPRINT`, never `VERSION` or `STATE`.

That second case has a fixture of its own —
`tests/fixtures/mathlib_v2_stable.c` — whose only difference from the breaking
v2 is its SemVerX state. If the resolver ever reported `VERSION` there, it would
mean the version machinery had been consulted about a contract the machine had
already refused. And a check that can be *consulted* can eventually be
*relaxed*.

---

## 3. Why state is part of identity

SemVerX writes `major.state.minor.state.patch.state`, and treats
`4.17.15-stable` and `4.17.15-legacy` as **different versions**.

The failure this prevents is specific and common. A CI cache holds a legacy
build under a version number a developer tested against as stable. The resolver
sees matching numbers, binds the legacy build, and the difference surfaces as a
"works on my machine" report that nobody can reproduce because the two machines
genuinely have different code under the same name.

Making the state part of the identity means the resolver never considered them
the same version, so the substitution cannot happen silently.

Three consequences worth stating:

**All six fields are required.** A parser that supplied `stable` for an omitted
state would reintroduce the ambiguity at the cheapest possible place to have
caught it, and downstream a guessed state is indistinguishable from a declared
one.

**Every field is masked independently.** A package whose *patch* line is beta is
a beta package however stable its major line claims to be. Gating only the major
state would let an unreviewed patch ride into a stable-only deployment behind a
stable major number.

**The state ordering is not a compatibility ordering.** `experimental < beta <
stable < legacy < lts` exists so a policy can be expressed as a mask test. It
does not mean a `stable` provider satisfies a `beta` requirement.

---

## 4. Why `since` is the shape's version, not the module's

An export's `since` field records the version at which **that symbol's shape**
first appeared, not the version of the object carrying it.

A module at `1.stable.7` whose `add` has not changed shape since `1.stable.0`
declares `since = 1.stable.0.stable.0.stable` for `add`.

This is what lets an old caller bind to a new object, and getting it wrong is
the most common way to make a correct release look like a breaking one. If every
export inherited the module's version, then every release outside the caller's
major line would fault on version — even one that changed nothing the caller
touches — and the version gate would become an obstacle people learn to disable
rather than a signal they read.

`tests/fixtures/mathlib_v1_1.c` demonstrates the intended pattern: a module at
`1.stable.1` where `add` and `mul` keep `since = 1.stable.0` and a genuinely new
symbol `addf` carries `since = 1.stable.1`.

---

## 5. The four properties

### 5.1 Shape containment

> No caller can obtain a callable address whose machine contract differs from
> the contract it was compiled against.

**Argument.** The callable slot `w` is written in exactly one place in the
resolver, at the end of `resolve`, to one of two values: the provider's address,
or the trap. The provider branch is reachable only when `fault == NONE`, which
requires the fingerprint comparison to have passed. The fingerprint is a
function of the complete calling contract (`ABI-FINGERPRINT.md` §1), so equal
fingerprints imply equal encodings imply — by construction of the grammar —
equal profile, convention, return type, arity and argument types.

The residual risk is a hash collision. Two lanes over 128 bits with independent
bases makes accidental collision negligible; deliberate collision is out of
scope and stated as such in `MMUKO-ABI-SPEC.md` §8.7.

**Where it is checked.** `test_trident.c: the_transcript_fault` asserts
`t.w_slot != add_f64` directly — not merely that the state is faulted, but that
the wrongly-shaped address is *not present in the slot*. The Rust and ring-0
suites assert the same.

### 5.2 Fault containment

> If any trident fails to converge, no node downstream of it can bind, at any
> depth.

**Argument.** Let `D(n)` be the set of nodes reachable from `n` by dependency
edges. `resolve(n)` checks every direct dependency's state *before* examining
`n`'s own hooks, and faults `UPSTREAM` if any is not `BOUND`.

By induction on the length of the longest path from `n` to a faulted node: for
length 1, the direct check faults `n`. For length `k+1`, the intermediate node
was faulted by the induction hypothesis, so `n`'s direct check sees a
non-`BOUND` dependency and faults `n`.

Therefore a faulted contract cannot reach code at any distance downstream.

**Reversibility matters as much as strictness.** A containment rule that could
not be undone would be a one-way ratchet: the first fault in a long-running
system would permanently disable everything downstream and force exactly the
restart the design exists to avoid. Because `resolve` recomputes from current
hook state on every call, healing the root and re-resolving restores the whole
chain.

**Where it is checked.** `test_trident.c: fault_containment` builds A → B → C,
breaks only A, asserts all three fault with B and C reporting `UPSTREAM`, then
heals A and asserts all three bind again with C's generation advanced.

### 5.3 Linear resolution

> Three edges per node; the fixpoint loop terminates.

**Argument.** A trident has exactly two incoming hooks and one outgoing hook, so
the structure is constant-degree and a graph of `n` nodes occupies O(n).

For termination: let `B(p)` be the number of bound nodes after pass `p`. Within
a run, hooks do not change, so a node that bound in pass `p` binds again in pass
`p+1` (its dependencies were bound, and by the same argument they remain bound).
Hence `B` is non-decreasing, and it is bounded above by `n`. The loop exits when
`B(p) == B(p-1)`, which must occur within `n+1` passes.

Worst case is O(n²) for a chain presented in exactly reverse dependency order,
where each pass binds one more node. In practice loaders emit nodes in
dependency order and the loop converges in two passes — one to bind, one to
observe the fixpoint.

**Where it is checked.**
`test_trident.c: containment_is_ordering_independent` and
`resolver.rs: resolution_reaches_the_same_fixpoint_whatever_the_registration_order`
present a chain backwards and assert it still binds completely.

### 5.4 Hot-swap safety

> Replacing a provider cannot create a transient inconsistency.

**Argument.** `resolve` computes its verdict entirely into local storage —
`fault`, `next_state`, `next_slot` — and writes `w` exactly once, as the last
action.

Therefore at every instant `w` holds either the value written by the previous
resolve or the value written by this one, and each of those passed consensus at
the moment it was written. There is no instant at which `w` holds an address
that has not been validated.

A concurrent *reader* — a caller loading the slot to make a call — is therefore
safe without a lock: it observes one valid address or the other. This is the
property that makes "the fix lands under a running caller" true rather than
aspirational.

The generation counter completes the picture. It advances only on a transition
*into* bound. A caller that cached a function pointer compares the generation it
saw against the current one to learn it must re-read; it is never told, and it
never restarts. A fault must not advance it (a fault is not a new binding), and
re-resolving an unchanged bound node must not advance it (or the "has this
changed?" test becomes useless) — both are asserted.

**The limit of this claim.** Only the reader is safe. Two threads calling
`resolve` on the same node concurrently is undefined; the kernel serialises
resolution. Stated in `MMUKO-ABI-SPEC.md` §8.7.

---

## 6. Why the unbound slot is a trap and not null

A conforming caller checks the binding state before calling and never reaches
the trap. So why have one?

Because the alternative is worse in a specific way. A null slot moves the
failure from the *binding* to the caller's *dereference* — at an arbitrary later
moment, in unrelated code, with no record of which contract disagreed. The trap
fails at the call, once, at a known address, with both fingerprints still in
hand.

The trap is defence in depth. It is not the strategy, and the design does not
depend on callers hitting it.

Its shape is constrained by what must remain true when it is entered through a
pointer of the *wrong* type. It takes no arguments and returns `int32`. Under
both MMUKO profiles the **caller** cleans the argument area, so entering it
through any non-variadic signature does not unbalance the stack. Its return
value is meaningful only for integer-returning slots — a caller expecting a
`double` reads XMM0, which the trap does not write — which is precisely why the
observable that tests and supervisors read is the trap **counter**, not the
returned value.

---

## 7. Scoring, when there is a choice to make

Once the gate has produced a set of survivors, `select.rs` ranks them:

```
score = 0.5 × consensus + 0.3 × artifact_confidence + 0.2 × version_weight
```

with

```
consensus      = (yes + 0.5 × abstain) / total,  or 0.5 when no votes cast
version_weight = base/(base + 100) × state_multiplier,
                 base = 100×major + 10×minor + patch
```

Four details, each chosen against a specific failure:

**An empty vote tally scores neutral, not zero.** "Nobody has looked at this" is
not the claim "everybody who looked rejected it". Scoring them identically would
make a brand-new correct release indistinguishable from a condemned one.

**Abstentions are counted, not dropped.** An abstention is information — someone
looked and would not commit. Dropping them would let one endorsement among nine
abstentions read as unanimous.

**The version term is compressed by `base/(base+100)`.** Without it a package at
major 400 would swamp every other term combined, and the ranking would degenerate
into "whoever bumped their major number most often".

**Ties break by version, then by identifier.** The identifier tie-break is what
makes the ordering *total*. Without it, two candidates equal on every measured
axis would be separated by whatever order they happened to be enumerated in, and
the same registry would resolve differently on two machines from identical
inputs — which is the class of bug that takes a week to find.

### 7.1 Fixed point, not floating point

Scores are `u32` in millionths, not `f64`. Three reasons, all specific to
running at ring 0:

1. The FPU/SSE register file is usually not saved on kernel entry. A resolver
   that touches XMM registers either corrupts userspace state or forces an
   expensive save on every call into it.
2. `f64` arithmetic is not bit-reproducible across compilers, optimisation
   levels and FMA availability. Two nodes that must agree on a ranking would be
   comparing numbers that could differ in the last place.
3. Ordering `f64` requires handling NaN. A NaN artifact score in a `partial_cmp`
   chain silently loses every comparison, so a corrupt input would quietly rank
   last instead of being rejected.

The arithmetic uses `u64` intermediates and saturating operations throughout, so
it cannot overflow or panic on any input — asserted by
`selection.rs: scoring_never_overflows_or_panics` against the extremes of every
field.

### 7.2 The compact version dialect

`select.rs::parse_compact` reads `major.minor.patch-channel`, because that is
how versions arrive from npm-shaped registries and from human hands, and
refusing to read them would only mean somebody writes a lossy converter
elsewhere.

It is an **input dialect**, not the canonical form. The single channel applies to
all three state fields, and — this is the one place it differs from an ordinary
SemVer parser — **a missing channel is an error, not a default of `stable`**.
`SemVerX::parse` remains the canonical six-field reader and does not accept the
dialect, so the two never blur.
