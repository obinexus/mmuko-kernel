# MMUKO Kernel Dynamic C ABI — Normative Specification

**Revision 1** · OBINexus Computing · MMUKO kernel
Profiles: `mmuko32`, `mmuko64`

---

## 0. Status of this document

This is the normative specification for revision 1 of the MMUKO ABI. Where the
implementation in this repository disagrees with this document, one of them is
wrong and the disagreement is a defect to be filed, not a judgement call.

The key words MUST, MUST NOT, SHALL, SHOULD and MAY are used in the sense of
RFC 2119.

---

## 1. The problem, stated exactly

A shared object exports:

```c
int add(int a, int b);
```

A later release of the same object exports:

```c
double add(double a, double b);
```

The soname does not change. The symbol name does not change. The arity does not
change. The dynamic linker resolves the symbol **by name**, finds it, and binds
it. Nothing traps.

The machine contract, however, has changed completely:

| profile | v1 `add`                                    | v2 `add`                                        |
|---------|---------------------------------------------|-------------------------------------------------|
| mmuko64 | `a` in EDI, `b` in ESI, result in EAX       | `a` in XMM0, `b` in XMM1, result in XMM0        |
| mmuko32 | two 4-byte integers on the stack, result EAX| two 8-byte doubles on the stack, result on st(0)|

An already-compiled caller pushes integers and reads the integer return
register. The callee reads and writes the floating-point ones. The program keeps
running and returns a number that is not the sum of anything.

`demo/run_demo.sh` reproduces this exactly. On the reference machine it prints
`add(2, 3) = 1299980649`.

**This specification exists to make that outcome unreachable.**

Note carefully what is and is not wrong in that scenario. The v2 library is
correct code. Its author did nothing careless. The fault is that the caller's
understanding of the contract was consumed by the compiler and then discarded,
so at the moment of binding there was nothing to compare the library's contract
against. MMUKO's contribution is not better checking — it is making the caller's
contract survive compilation so that a comparison is possible at all.

---

## 2. Architecture profiles

### 2.1 Definitions

- **`mmuko32`** — ILP32. `void *` is 4 bytes. The standard C calling convention
  is SysV i386 cdecl: all arguments on the stack, caller cleans up, integers
  return in EAX (EAX:EDX for 64-bit), floating point returns on the x87 stack.

- **`mmuko64`** — LP64. `void *` is 8 bytes. The standard C calling convention
  is SysV AMD64: integer and pointer arguments in RDI, RSI, RDX, RCX, R8, R9;
  floating-point arguments in XMM0–XMM7; integer return in RAX (RAX:RDX);
  floating-point return in XMM0 (XMM0:XMM1); a variadic call sets AL to the
  number of vector registers used.

### 2.2 Disjointness

The profiles are disjoint. An `mmuko32` descriptor MUST NOT satisfy an
`mmuko64` call site, or the reverse.

This is enforced by comparison rather than by convention: the profile tag is an
unconditional input to every fingerprint (§4.3), so a cross-profile descriptor
produces a different fingerprint even for a signature containing no pointers.
That is deliberate — register assignment, stack alignment and return conventions
all differ between the profiles even where the argument types do not.

---

## 3. The type vocabulary

### 3.1 Codes

| code | token | width | meaning |
|-----:|-------|-------|---------|
| 0x00 | `v`    | —          | `void`; legal as a return type only |
| 0x01 | `b8`   | 1          | boolean, 0 or 1 |
| 0x10 | `i8`   | 1          | signed 8-bit |
| 0x11 | `u8`   | 1          | unsigned 8-bit |
| 0x12 | `i16`  | 2          | signed 16-bit |
| 0x13 | `u16`  | 2          | unsigned 16-bit |
| 0x14 | `i32`  | 4          | signed 32-bit |
| 0x15 | `u32`  | 4          | unsigned 32-bit |
| 0x16 | `i64`  | 8          | signed 64-bit |
| 0x17 | `u64`  | 8          | unsigned 64-bit |
| 0x20 | `f32`  | 4          | IEEE-754 binary32 |
| 0x21 | `f64`  | 8          | IEEE-754 binary64 |
| 0x30 | `ptr`  | *profile*  | opaque `void *` |
| 0x31 | `cstr` | *profile*  | `const char *`, NUL terminated |
| 0x40 | `S{…}` | *measured* | struct, passed by value |
| 0x41 | `U{…}` | *measured* | union, passed by value |
| 0x42 | `E{…}` | *measured* | enum |

The numeric codes are **frozen**. Appending new codes is permitted; renumbering
an existing one is a breaking change to the encoding itself and MUST be
accompanied by a change to `MMUKO_ABI_TAG` (§4.1).

### 3.2 What is deliberately absent

There is no `int` and no `long`.

`long` is 32 bits under Windows LLP64 and 64 bits under SysV LP64. A vocabulary
that could name it would permit a single fingerprint to describe two different
machine contracts, and every guarantee in §5 would be void on any system that
mixed them. Removing the width-ambiguous types from the vocabulary removes that
entire fault class structurally, rather than delegating it to review.

Bindings that map a source language's `int` onto `i32` do so at the point of
declaration, where the mapping is visible and can be wrong in an obvious way.

### 3.3 Aggregates

A struct, union or enum descriptor MUST carry:

- `size` — the size in bytes as measured by the **providing** compiler on the
  **providing** profile;
- `align` — the alignment in bytes, likewise measured;
- the ordered list of field descriptors.

Both `size` and `align` MUST be non-zero. A descriptor with either at zero is
malformed and MUST be refused, not defaulted.

Size and alignment are part of the contract because two structs with identical
field types can have different padding under different packing pragmas, and a
struct passed by value is classified for register assignment by its size and its
field classes **together**. Encoding the fields alone would let a packed and an
unpacked struct share a fingerprint.

Aggregate nesting MUST NOT exceed `MMUKO_TYPE_MAX_DEPTH` (8). A deeper
descriptor is refused rather than truncated.

### 3.4 Arity

- `void` MUST NOT appear as a parameter type. `f(void)` is arity zero; treating
  the two as the same would let them share a fingerprint on any platform where
  they do not share a calling contract.
- Arity MUST NOT exceed `MMUKO_MAX_ARGS` (32).

---

## 4. ABI fingerprints

### 4.1 Canonical encoding

A signature encodes to a deterministic ASCII string:

```
MMUKOABI/1|A=<profile>|CC=<convention>|R=<type>|N=<arity>|P0=<type>|…|V=<0|1>
```

For `int32 add(int32, int32)` on `mmuko64` with the C convention:

```
MMUKOABI/1|A=64|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0
```

For the breaking replacement `double add(double, double)`:

```
MMUKOABI/1|A=64|CC=cdecl|R=f64|N=2|P0=f64|P1=f64|V=0
```

Different bytes, therefore different fingerprint, therefore no bind.

An aggregate encodes structurally:

```
S{sz=16,al=8:i32,f64}
```

A named export prefixes the symbol:

```
SYM=add|MMUKOABI/1|A=64|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0
```

`MMUKOABI/1` is a domain-separation tag. Changing it changes every fingerprint
in existence, which is exactly what a breaking change to the encoding itself
must do.

### 4.2 The hash

The fingerprint is 128 bits, produced by two independent FNV-1a passes over the
canonical bytes:

- **lane A** — forward, offset `0xcbf29ce484222325`, prime `0x100000001B3`;
- **lane B** — backward, offset `0x9ae16a3b2f90404f`, prime `0x100000001C9`;
- both lanes then mix in the byte length.

Two lanes with different bases means a collision in one carries no information
about the other. The backward pass means a transposition that leaves lane A
unchanged still moves lane B. Mixing the length means a prefix cannot collide
with its own extension.

A valid encoding never produces the all-zero fingerprint, so zero is available
as a sentinel meaning "not computed".

### 4.3 Required properties

An implementation MUST satisfy all of the following. Each is checked by
`tests/test_fingerprint.c` and, across implementations, by
`rust/mmuko-kernel/tests/differential.rs`.

1. **Determinism.** The same signature under the same profile produces the same
   fingerprint, on every compiler and at every optimisation level. (Guaranteed
   by hashing the canonical ASCII, never raw struct memory.)
2. **Profile separation.** The same signature under different profiles produces
   different fingerprints — including signatures containing no pointers.
3. **Type distinctness.** No two distinct type codes share a fingerprint in any
   position. In particular `i32` ≠ `u32`, `i64` ≠ `f64`, and `ptr` ≠ `cstr`,
   despite equal widths: signedness changes the widening and comparison
   instructions the caller emits, and integer versus floating point changes the
   register class entirely.
4. **Order sensitivity.** `f(i32, f64)` ≠ `f(f64, i32)`.
5. **Arity sensitivity.** `f(i32)` ≠ `f(i32, i32)`.
6. **Convention sensitivity.** cdecl ≠ stdcall.
7. **Variadic sensitivity.** A variadic signature differs from the fixed one
   with the same prefix.
8. **Name binding.** For export fingerprints, the same shape under two names
   produces different fingerprints.
9. **Refusal, not normalisation.** Any descriptor that cannot be encoded exactly
   MUST be refused. An approximate identity is worse than no identity, because
   it compares equal to things it does not describe.

---

## 5. Self-describing modules

### 5.1 The query symbol

Every conformant object MUST export exactly one symbol:

```c
const mmuko_module_desc_t *mmuko_abi_query_v1(void);
```

An object without it MUST be refused with `MMUKO_LOAD_E_NO_TABLE`.

This is a real cost and worth naming plainly: MMUKO cannot make an arbitrary
pre-existing `.so` safe. What it can do is refuse to pretend. An object that has
never stated its contract offers nothing to compare against, and binding to it
would be a guess wearing the costume of a check. Migration means giving each
object a table — one macro block per library, emitted by the same compiler pass
that emits the code, so it cannot drift from it the way a hand-maintained header
can.

### 5.2 Validation

Before any pointer inside a foreign table is dereferenced for a binding
decision, the table MUST be validated:

1. `magic == MMUKO_ABI_MAGIC` (0x4F4B4D4D);
2. `abi_rev == MMUKO_ABI_REV` (1) — a future revision is refused, never
   partially read;
3. `arch` is one of 32 or 64, and matches the loading site's profile;
4. `module` is non-NULL and non-empty;
5. `version` is a structurally valid SemVerX (§6.1);
6. every export has a non-empty symbol, a non-NULL signature, arity within
   bounds, a valid `since`, and a signature the encoder accepts;
7. no two exports share a symbol name.

Duplicate symbols are rejected because they would make lookup order-dependent,
and an order-dependent resolver can bind differently on two machines from the
same inputs.

### 5.3 `since` semantics

An export's `since` is **the version at which that symbol's shape first
appeared** — not the version of the module carrying it.

A module at 1.stable.7 whose `add` has not changed shape since 1.stable.0
declares `since = 1.stable.0.stable.0.stable` for it. This is what allows an old
caller to bind to a new object, and getting it wrong is the most common way to
make a correct release look like a breaking one.

---

## 6. SemVerX

### 6.1 Form

```
major.state.minor.state.patch.state
```

for example `4.stable.17.beta.2.stable`.

States: `experimental`, `beta`, `stable`, `legacy`, `lts`.

All six fields are REQUIRED. A parser MUST refuse a shorter form rather than
supplying a default state — an omitted state is precisely the ambiguity SemVerX
exists to remove, and guessing one at the parser reintroduces it at the cheapest
possible place to have caught it. Downstream, a guessed state is
indistinguishable from a declared one.

### 6.2 State is identity

`4.17.15-stable` and `4.17.15-legacy` are **different versions**. This is what
stops a cached legacy build masquerading as the stable one that CI actually
tested.

### 6.3 Satisfaction

`provided` satisfies a call site requiring `required` under mask `M` when:

1. both are structurally valid;
2. every state field of `provided` is admitted by `M`;
3. `provided.major == required.major` **and**
   `provided.major_state == required.major_state`;
4. `(provided.minor, provided.patch) >= (required.minor, required.patch)`.

Rule 2 applies to every field independently: a package whose patch line is beta
is a beta package however stable its major line claims to be.

### 6.4 What satisfaction is not

Version satisfaction is **never sufficient for binding**. It is a policy claim
about intent; ABI identity is a fact about machine code. §7.3 fixes the order in
which they are consulted, and that order is not negotiable.

---

## 7. Trident consensus binding

### 7.1 Topology

```
      u1      u2        u1 = REQUIRED contract, frozen at the caller's
        \    /               compile time
         \  /           u2 = PROVIDED contract, read from the loaded
          v                  object's self-describing table
          |              v = convergence: the binding decision
          w              w = propagation: the callable slot
```

Two incoming hooks, one outgoing hook. Constant degree: exactly three edges per
node, so a graph of them costs O(n) memory and resolves in time linear in the
node count.

### 7.2 The rule

> A trident node binds **if and only if** its two incoming contracts are
> identical. Otherwise it stays unbound and blocks propagation.

For a C ABI, *identical* means identical 128-bit export fingerprint.

### 7.3 Order of evaluation — normative

A conformant resolver MUST evaluate in exactly this order:

1. **Containment.** If any dependency is not bound, fault `UPSTREAM` and stop.
   Checked first so containment is transitive rather than merely local.
2. **Hook presence.** Missing `u1` → `MALFORMED`. Missing `u2` → `MISSING`.
   `u2` without an address → `MALFORMED`.
3. **Profile.** Fault `ARCH`.
4. **Calling convention.** Fault `CC`.
5. **Fingerprint.** Fault `FINGERPRINT`.
6. **SemVerX.** Fault `STATE` or `VERSION`.

Steps 3 and 4 are redundant with step 5 — a profile or convention mismatch
already changes the fingerprint — and are performed anyway so the diagnostic
names the real cause instead of reporting every structural disagreement as a
shape mismatch.

**Step 5 MUST precede step 6.** This is the single most important requirement in
this document. ABI identity is a machine fact; version compatibility is a policy
claim; and a policy claim MUST NOT be able to admit a contract the machine has
refused. If the order were reversed, or if the version check could override the
fingerprint, then promoting a release from experimental to stable would "repair"
an ABI break — and under deployment pressure, someone eventually would.

`tests/fixtures/mathlib_v2_stable.c` exists solely to pin this down: it is
byte-identical to the breaking v2 except for its state, and it MUST still fault
with `FINGERPRINT`, never `VERSION` and never `STATE`.

### 7.4 Publication — normative

The decision MUST be computed entirely into local storage, and `w` MUST be
written **exactly once**, at the end, to either the validated provider address
or the trap.

A concurrent reader of `w` therefore observes either the previous value or the
new one, and both passed consensus at the moment they were written. There is no
instant at which `w` holds an unvalidated address. This is the whole of the
hot-swap safety argument (§8.4), and it is why a fix can land under a running
caller with no restart.

### 7.5 The trap

`w` MUST NOT be null at any point after initialisation, including before the
first resolve. An unbound slot points at a trap thunk.

A null slot would move the failure to the caller's dereference, at an arbitrary
later moment, with no record of which contract disagreed. A trap slot fails at
the call, once, with both fingerprints in hand. Neither is as good as the caller
checking the state first — the trap is defence in depth, not the strategy.

The trap takes no arguments and returns `int32`. Under both profiles the
**caller** cleans the argument area, so entering it through a pointer of any
non-variadic signature does not unbalance the stack. Its return value is
meaningful only for slots whose declared return class is integer; the observable
that supervisors read is the trap **counter**, not the returned value.

### 7.6 Generation counter

Each node carries a monotonic counter that increments **only** on a transition
into the bound state.

- A fault MUST NOT advance it — a fault is not a new binding.
- Re-resolving an already-bound node with unchanged hooks MUST NOT advance it,
  or the "has this changed?" test becomes useless.

A caller that cached a function pointer compares the generation it saw against
the current one to learn it must re-read the slot. It is never told, and it never
restarts.

---

## 8. Guarantees

Each guarantee below names the tests that check it. A guarantee with no test is
a wish.

### 8.1 Shape containment

*No caller can obtain a callable address whose machine contract differs from the
contract it was compiled against.*

`test_trident.c: the_transcript_fault`, `test_loader.c: v2_is_refused`,
`resolver.rs: the_transcript_break_faults_and_exposes_nothing`,
`qemu/kmain.c: THE BREAK`.

### 8.2 Fault containment

*If any trident fails to converge, no node downstream of it can bind, at any
depth.*

A node whose dependency is not bound faults `UPSTREAM` regardless of its own
hooks, transitively.

`test_trident.c: fault_containment`,
`resolver.rs: fault_containment_is_transitive_and_reversible`,
`qemu/kmain.c: fault containment across a three-node chain`.

### 8.3 Linear resolution

*Three edges per node; the fixpoint loop is bounded by the node count and exits
as soon as a pass adds no bindings.*

The bound count is non-decreasing across passes within a run and bounded above
by the node count, so the loop terminates. Worst case O(n²) for a chain
presented in reverse order; linear in practice because loaders emit nodes in
dependency order.

`test_trident.c: containment_is_ordering_independent`,
`resolver.rs: resolution_reaches_the_same_fixpoint_whatever_the_registration_order`.

### 8.4 Hot-swap safety

*Replacing a provider cannot create a transient inconsistency, and a fix can
land under a running caller with no restart.*

Follows from §7.4: `w` is written once per resolve, always to a validated value.

`test_trident.c: hot_swap_heals`,
`test_loader.c: hot_swap_under_a_running_caller`,
`resolver.rs: hot_swap_heals_without_a_restart`,
`qemu/kmain.c: hot-swap heals with no restart`.

### 8.5 Cross-implementation agreement

*The Rust kernel and the C modules it loads compute identical identities.*

Two independent implementations of §4 exist. `differential.rs` asserts that both
the canonical encodings and the fingerprints agree across 4 732 signature
comparisons spanning both profiles, and that the `#[repr(C)]` mirrors match the
C compiler's measured layout.

### 8.6 Freestanding operation

*The resolver runs at ring 0 with no libc, no allocator, no FPU and no dynamic
loader.*

`make freestanding-check` asserts that the four core translation units reference
no symbol outside the `mmuko_` namespace. `qemu/run_qemu.sh` boots them on bare
metal under both profiles.

### 8.7 What is NOT guaranteed

Stated plainly, because a specification that only lists its strengths is not one
anyone should rely on:

- **Objects without an ABI table cannot be made safe.** They are refused (§5.1).
- **A correct fingerprint does not imply correct behaviour.** MMUKO checks the
  calling contract, not semantics. A library that keeps `int add(int, int)` and
  starts returning the difference will bind, and should.
- **The table is trusted once validated.** A hostile module can declare a
  truthful table and behave otherwise. Defending against that requires the
  loader to be outside the module's trust boundary — a different mechanism from
  this one, and one MMUKO does not claim.
- **Concurrency is single-writer.** §7.4 makes a concurrent *reader* safe.
  Two threads resolving the same node simultaneously is undefined; the kernel
  serialises resolution.
- **The fingerprint is not a security primitive.** FNV-1a is not
  collision-resistant against an adversary who is choosing the input. It is
  chosen for determinism, zero dependencies and freestanding operation against
  *accidental* collision. A deployment that needs adversarial resistance should
  sign the module, which is an orthogonal mechanism.

---

## 9. Conformance

An implementation is conformant when it:

1. produces the canonical encodings of §4.1 byte for byte;
2. produces the fingerprints of §4.2 bit for bit;
3. satisfies all nine properties of §4.3;
4. validates tables per §5.2 before acting on them;
5. evaluates in the order of §7.3, with the fingerprint strictly before the
   version;
6. publishes `w` per §7.4;
7. maintains the generation counter per §7.6.

The suites in `tests/` and `rust/mmuko-kernel/tests/` constitute the reference
conformance set. As of revision 1 they comprise 326 C assertions across both
profiles, 45 Rust tests, and 42 ring-0 assertions per profile under QEMU.
