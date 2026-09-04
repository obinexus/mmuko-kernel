# MMUKO Kernel Dynamic C ABI

**SemVerX trident consensus binding for `mmuko32` and `mmuko64`.**
OBINexus Computing · MMUKO kernel · ABI revision 1

---

## The problem

A shared object exports `int add(int, int)`. A later release changes it to
`double add(double, double)` and keeps the same soname and the same symbol
name. The dynamic linker resolves the symbol **by name**, finds it, and binds
it. The caller's already-compiled machine code passes two integers and reads the
integer return register; the callee reads and writes the floating-point ones.

Nothing traps. The process keeps running and returns a number that is not the
sum of anything.

```
$ ./demo/run_demo.sh

libmath.so -> libmath_v1.so    [int add(int,int)]
naive_caller: add(2, 3) = 5
   correct.

Now repoint the soname at v2.  The caller is NOT recompiled.
libmath.so -> libmath_v2.so    [double add(double,double)]
naive_caller: add(2, 3) = 1299980649
   ^ same binary, same source, same symbol name.
     No crash.  No linker error.  No warning.  A wrong number, silently.
```

Note what is *not* wrong here. The v2 library is correct code and its author was
not careless. The fault is that the caller's understanding of the contract was
consumed by the compiler and then thrown away, so at the moment of binding there
was nothing to compare the library's contract against.

## The answer

A symbol is not identified by its name. It is identified by the **128-bit
fingerprint of its complete calling contract** — profile, calling convention,
return class and width, arity, and every argument class and width — together
with its SemVerX identity.

Binding is gated by a **trident consensus**:

```
      u1      u2        u1 = the contract the caller was COMPILED AGAINST
        \    /               (frozen into the caller's binary)
         \  /           u2 = the contract the object ACTUALLY PROVIDES
          v                  (read from its self-describing table)
          |              v = convergence: they must be identical
          w              w = the callable slot, populated only if they are
```

> A trident node binds **if and only if** its two incoming contracts are
> identical. Otherwise it stays unbound and blocks propagation.

Same demo, with the trident in the path:

```
against v2  [f64 add(f64,f64) @ 2.experimental]:
module mathlib@2.experimental.0.stable.0.stable  arch=mmuko64  0/1 bound  [DEGRADED]
  add                  UNBOUND_FAULT  abi-fingerprint-mismatch     gen=0
      u1 required cede175a87d95827:64442899dfdb396b
      u2 provided 50b66bfe67b08663:124b22dca50f727d
guarded_caller: REFUSED to call add -- abi-fingerprint-mismatch
guarded_caller: the process is alive and no wrong answer was produced.
```

---

## Quick start

```bash
./tools/bootstrap.sh     # install what verification needs (optional)
make check               # freestanding link check + every suite
make demo                # reproduce the transcript, unguarded then guarded
make boot                # the mmuko-boot sequence, phases 0-8
make boot-neg            # a mismatched contract must FAIL the boot
```

**On Windows**, `make` from PowerShell or `cmd` will not work: these recipes are
POSIX shell, and `cmd.exe` is not one. The Makefile detects that and says so
rather than emitting a cascade of unrelated errors. Two working paths:

```powershell
wsl make check                                        # everything
powershell -ExecutionPolicy Bypass -File tools\build.ps1   # C suites, natively
```

The PowerShell path builds with MinGW gcc and runs the four C suites on
Windows, which is the only thing that exercises the
`LoadLibrary`/`GetProcAddress` branch of the loader — the POSIX build never
compiles it. It does not cover the Rust, QEMU or mmuko-boot suites; those need
the Unix shell. `make win-check` cross-compiles the Windows build from Linux
when MinGW-w64 is installed, so that branch is at least compile-checked in a
normal run.

Expected output of `make check` on a fully equipped machine:

```
mmuko64:  39 + 27 + 52 + 45 = 163 assertions passed
mmuko32:  39 + 27 + 52 + 45 = 163 assertions passed
kernel.rs: 45 tests passed  (13 differential, 14 resolver, 18 selection)
ring 0:   mmuko32 PASS (21 assertions), mmuko64 PASS (21 assertions)
mmuko-boot: phases 0-8 complete; negative gate test refuses as required
```

## Booting into the kernel

`boot/` splices the resolver into the `mmuko-boot` sequence as **PHASE 8: ABI
namespace bring-up**, between rotation verification and BOOT COMPLETE.

Phases 0-6 bring the machine model up. Phase 8 brings the module namespace up,
and it runs *before* phase 7 declares the boot complete because a program is a
thing that calls into modules — a kernel that cannot verify a module contract
has no business launching one. A failure there is `BOOT_ABI_UNBOUND`, a boot
failure rather than a warning.

Phase 8 does not get a free pass: it binds the kernel's own `kputs`/`kputhex`
through tridents before printing its first character, so the very first
inter-module call in the system goes through the same resolver every later
module will. `boot/README.md` has the full transcript and the seven marked
edits to `kernel.c`.

Everything degrades gracefully. A machine with no 32-bit toolchain, no Rust and
no QEMU still builds and runs the 64-bit C suites and says clearly what it
skipped. A missing tool must weaken the evidence, never break the build.

---

## Layout

```
SPEC/
  MMUKO-ABI-SPEC.md      normative specification: encoding, ordering, guarantees
include/mmuko/
  mmuko_abi.h            types, profiles, canonical encoding, fingerprints
  mmuko_semverx.h        major.state.minor.state.patch.state
  mmuko_desc.h           self-describing module tables + declaration macros
  mmuko_trident.h        consensus binding and fault containment
  mmuko_loader.h         hosted dynamic loader (the only non-freestanding part)
src/
  mmuko_abi.c            encoder + FNV-1a-128        ─┐
  mmuko_semverx.c        version identity             │ freestanding.
  mmuko_desc.c           table validation             │ links at ring 0.
  mmuko_trident.c        the resolver                ─┘
  mmuko_loader.c         dlopen / LoadLibrary front end
rust/mmuko-kernel/
  src/kernel.rs          the registry the kernel calls + extern "C" entry points
  src/fingerprint.rs     a SECOND, independent implementation of the encoder
  src/trident.rs         the resolver, in Rust
  src/select.rs          candidate scoring, gated by the resolver
  tests/differential.rs  asserts the two implementations agree, byte for byte
tests/                   C suites + the mathlib v1/v2 fixtures
qemu/                    minimal multiboot ring-0 harness, both profiles
boot/                    PHASE 8: the resolver spliced into the mmuko-boot
                         sequence, gating BOOT COMPLETE
demo/                    the transcript, reproduced and then prevented
tools/
  mmuko_abi_dump.c       inspect and diff two objects' ABI tables
  bootstrap.sh           install the verification toolchain
  build.ps1              native Windows build; exercises the Win32 loader
```

---

## Declaring a module

Provider side, in the shared object:

```c
#include "mmuko/mmuko_desc.h"

MMUKO_PUBLIC int32_t add(int32_t a, int32_t b);
MMUKO_PUBLIC int32_t add(int32_t a, int32_t b) { return a + b; }

MMUKO_SIG(sig_add, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);

static const mmuko_export_desc_t exports[] = {
    MMUKO_EXPORT("add", sig_add, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), add)
};

MMUKO_MODULE(desc, "mathlib", MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), exports);
MMUKO_QUERY_IMPL(desc)
```

Consumer side:

```c
MMUKO_SIG(req_add, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);
static const mmuko_export_desc_t required[] = {
    MMUKO_REQUIRE("add", req_add, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE))
};

mmuko_load_request_t req;
mmuko_module_t mod;
mmuko_load_request_init(&req, "libmathlib.so", required, 1);

if (mmuko_load(&req, &mod) == MMUKO_LOAD_OK) {
    add_fn_t add = (add_fn_t)mmuko_module_sym(&mod, "add");
    printf("%d\n", add(2, 3));
}
```

Both sides go through the same encoder. If the shapes differ by one argument's
width, the fingerprints differ and the slot never binds.

---

## Before you publish

```
$ mmuko-abi-dump libmathlib_v1.so libmathlib_v2.so

=== can a caller compiled against the first bind to the second? ===

  add              BREAKING         abi-fingerprint-mismatch
  mul              compatible

VERDICT: BREAKING.  This is a new major line, or the changed
         symbols need new names beside the old ones.
```

It answers the question with the real resolver, not a heuristic, so the tool
cannot disagree with the loader that will run in production.

---

## Four design decisions worth knowing about

**The type vocabulary has no `int` and no `long`.** `long` is 32 bits under
Windows LLP64 and 64 under SysV LP64. A vocabulary that could name it would let
one fingerprint describe two machine contracts. Removing width-ambiguous types
removes that fault class structurally rather than delegating it to review.

**The fingerprint is checked before the version, always.** ABI identity is a
machine fact; version compatibility is a policy claim. A policy claim must never
admit a contract the machine refused. `tests/fixtures/mathlib_v2_stable.c` is
byte-identical to the breaking v2 except for its SemVerX state, and it still
faults with `FINGERPRINT` — promoting a release cannot repair an ABI break. If
that ordering were negotiable, someone under deployment pressure would
eventually negotiate it.

**An object with no ABI table cannot be loaded at all.** This is a real cost:
MMUKO cannot make an arbitrary pre-existing `.so` safe. What it can do is refuse
to pretend. An object that has never stated its contract offers nothing to
compare against, and binding to it would be a guess wearing the costume of a
check.

**There are two implementations of the fingerprint, and they are tested against
each other.** The MMUKO kernel is Rust; the modules it loads are C. If the two
computed identity differently, the resolver would refuse correct bindings or —
far worse — accept incorrect ones, in a way no single-implementation test could
show. `differential.rs` asserts byte-for-byte agreement across 4 732 signature
comparisons spanning both profiles.

---

## What this does not do

Stated plainly, because a README that only lists strengths is not one anyone
should rely on. The full list is §8.7 of the specification.

- It checks the **calling contract**, not semantics. A library that keeps
  `int add(int, int)` and starts returning the difference will bind, and should.
- It cannot retrofit safety onto objects that carry no ABI table.
- Once validated, the table is trusted. A hostile module can declare a truthful
  table and behave otherwise; defending against that needs a different mechanism.
- FNV-1a is not collision-resistant against an adversary choosing the input. It
  is chosen for determinism, zero dependencies and freestanding operation
  against *accidental* collision. Adversarial resistance means signing the
  module, which is orthogonal to this.
- Resolution is single-writer. A concurrent *reader* of a slot is safe by
  construction (§7.4); two threads resolving the same node is not.

---

## Toolchain

`cargo` · `rustc` · `rustup` · `make` · `cmake` · `qemu` · `nasm` · `gcc`/`clang`
· `mingw-w64` (optional, for the Windows cross-check)

The Rust crate has **zero external dependencies** by design, so it builds on a
machine with no network and no registry cache — the normal condition for kernel
work and for the OBINexus deployment target. `build.rs` invokes the C compiler
directly rather than using the `cc` crate, and degrades to skipping the
differential suite if no compiler is found.

---

*For what is yet to be, I became.*
