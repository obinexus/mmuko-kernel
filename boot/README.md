# PHASE 8 — ABI Namespace Bring-Up

Integration of the MMUKO dynamic C ABI into the `mmuko-boot` sequence.

```
make boot        # phases 0-8, then a program that computes through a bound slot
make boot-neg    # a mismatched contract must FAIL the boot
```

---

## Where phase 8 sits, and why there

```
PHASE 0   vacuum medium init          ─┐
PHASE 1   cubit ring initialisation    │
PHASE 2   compass alignment            │  the MACHINE MODEL comes up
PHASE 3   superposition entanglement   │
PHASE 4   frame centering              │
PHASE 5   nonlinear resolution         │
PHASE 6   rotation verification       ─┘

PHASE 8   ABI NAMESPACE BRING-UP      ←  the MODULE NAMESPACE comes up

PHASE 7   BOOT COMPLETE
          └── program runs, calling through a trident-bound slot
```

Phases 0–6 establish that the machine is coherent: the medium is initialised,
every cubit has a direction, entangled pairs have resolved, the frame is
centred, and every cubit can still rotate.

Phase 8 establishes something different — that this kernel can bind a module's
exports to a call site and be certain the two agree about the machine contract.

**It runs before phase 7 declares the boot complete, and that ordering is the
whole argument.** A program is a thing that calls into modules. A kernel that
has not established it can verify module contracts has no business launching
one, because the first thing that program does is make a call it cannot check.
So a failure here is a *boot* failure — `BOOT_ABI_UNBOUND` — not a warning the
system carries forward and hopes nothing trips over.

Phase 8 is numbered 8 rather than inserted as a new 7 so that "PHASE 7 = BOOT
COMPLETE" keeps the meaning it already had in your sequence and in `DESIGN.md`.

---

## Phase 8 does not get a free pass

Phase 8 needs to print, and printing is a call across a module boundary like
any other. Rather than let that one boundary be the exception, the kernel
describes what it provides as an ordinary MMUKO module:

```c
MMUKO_SIG(ksvc_sig_puts, MMUKO_CC_CDECL, MMUKO_VOID, MMUKO_CSTR);
MMUKO_SIG(ksvc_sig_hex,  MMUKO_CC_CDECL, MMUKO_VOID, MMUKO_U32);

static const mmuko_export_desc_t ksvc_exports[] = {
    MMUKO_EXPORT("kputs",   ksvc_sig_puts, ..., kputs),
    MMUKO_EXPORT("kputhex", ksvc_sig_hex,  ..., kputhex)
};
MMUKO_MODULE(ksvc_desc, MMUKO_KSVC_MODULE, ..., ksvc_exports);
```

and phase 8 binds it through tridents before it prints its first character.

This has an ordering consequence worth naming: **phase 8 cannot report a
failure to bind the print functions, because printing is the thing it is
binding.** That is not an inconvenience to route around — it is the honest
situation of every module bring-up. You do not get diagnostics from a facility
until you have established you may use it. So a failure there returns a code
and stays silent, and the kernel, which still has its own unbound print
functions, reports it:

```
[PHASE 8] ABI namespace bring-up FAILED: kernel-services-unbound

=== BOOT FAILED ===
Status code: 4  (BOOT_ABI_UNBOUND: no verified module namespace)
```

---

## What a successful boot prints

```
[PHASE 6] All cubits rotate freely

[PHASE 8] ABI namespace bring-up
  [PHASE 8] profile mmuko32, descriptor revision 1
  [PHASE 8] kernel services bound through tridents:
           kputs    BOUND  none  gen=1
           kputhex  BOUND  none  gen=1
  [PHASE 8] arithmetic namespace bound:
           add  BOUND  none  gen=1
             u1 required de7fd3077f1d5fac:9f44125d214936f0
             u2 provided de7fd3077f1d5fac:9f44125d214936f0
           mul  BOUND  none  gen=1
  [PHASE 8] self-test: the shape break, at ring 0
           add  UNBOUND_FAULT  abi-fingerprint-mismatch  gen=1
             u1 required de7fd3077f1d5fac:9f44125d214936f0
             u2 provided 6951741f063a80a8:dc7986f26390ecbe
           mul  UNBOUND_FAULT  upstream-fault-contained  gen=1
  [PHASE 8] self-test: the heal, with nothing restarted
           add  BOUND  none  gen=2
           mul  BOUND  none  gen=2
  [PHASE 8] namespace fingerprint f56ea814d0f6a50d:d957347814493be5
[PHASE 8] ABI namespace ready: every call site has a verified contract

[PHASE 7] MMUKO BOOT COMPLETE

=== MMUKO PROGRAM START ===
[PROGRAM] add(2, 3) via bound slot = 5
[PROGRAM] Memory checksum: 0xF82D4FAE
[PROGRAM] ABI namespace:  0x25980D19:0xCD1E0F9D
=== MMUKO PROGRAM END ===
```

Two things in that transcript are load-bearing rather than cosmetic.

**The self-test runs every boot.** The hosted suites already prove the resolver
is correct on a developer's machine. This proves it is behaving on *this*
machine, in *this* image, with whatever compiler and flags actually produced
the running binary — a different claim, and the only one that matters to a
system about to launch a program. It costs microseconds.

Watch what the self-test shows: `add` is swapped to the `double`-shaped
provider and faults with `abi-fingerprint-mismatch`, and `mul` — whose own
hooks are perfectly good — faults with `upstream-fault-contained`. Then the
correct provider is republished and both heal, with `add`'s generation
advancing 1 → 2 and nothing restarted.

**The program computes through the bound slot.** `mmuko_program_main` calls
`mmuko_abi_bound_add()`, not `add()` directly. If the resolver ever stopped
working, the program would stop computing. A self-test the rest of the system
does not depend on is a self-test people eventually delete.

---

## The negative test

`make boot-neg` compiles the kernel with `kputhex` declared as taking a 64-bit
value while the function — and phase 8's expectation of it — still take 32.

That is the boot-time form of the transcript's break: one honest declaration,
one honest implementation, and a disagreement between them that no linker would
notice, because the symbol name is unchanged.

The run **passes only if the boot fails.** A gate that has never been observed
to refuse anything is not a gate; it is a hope with a function call attached.

---

## Changes to your files

`kernel.c` is your file with seven edits, each marked `MMUKO-ABI`:

| # | Change | Why |
|---|--------|-----|
| 1 | `#include "mmuko_abi_phase8.h"` | — |
| 2 | `BOOT_ABI_UNBOUND` added to `BootStatus` | a namespace that will not converge is a boot failure, not a warning |
| 3 | `kputs` / `kputhex` wrappers + `mmuko_ksvc_table()` | the kernel describes its own services as an MMUKO module |
| 4 | phase 8 call in `mmuko_boot()`, after 6 and before 7 | see above |
| 5 | `mmuko_program_main` calls through the bound slot | makes the guarantee load-bearing |
| 6 | namespace fingerprint printed beside the memory checksum | one says what the machine holds, the other what the contracts agreed to |
| 7 | `BOOT_ABI_UNBOUND` named in the failure path | a bare status code is not a diagnosis |

`boot.asm`, `linker.ld` and `grub.cfg` are unmodified.

New files: `mmuko_abi_phase8.h`, `mmuko_abi_phase8.c`, `run_boot.sh`.

---

## Toolchain

`run_boot.sh` prefers `i686-elf-gcc` and falls back to host `gcc -m32`.

The fallback is legitimate here: nothing links against the host libc, the
linker script places every section itself, and `-ffreestanding` tells the
compiler not to assume hosted semantics. What a cross compiler buys is that
mistakes in those flags fail loudly instead of quietly picking up a host
header — so use one when you have one. The fallback exists so that not having
one is not a reason to skip the boot test.

`nasm` is required for `boot.asm`; the script says so and stops rather than
silently substituting a different entry point. `tools/bootstrap.sh` installs it.

This path needs no GRUB, no `xorriso` and no ISO: QEMU's `-kernel` loads a
multiboot ELF directly. The GRUB route in your original `Makefile` still works
and is unaffected.

`mmuko_loader.c` is deliberately absent from the boot build. It needs `dlopen`,
and there is no dynamic loader at ring 0. The resolver it feeds has no such
dependency — which is exactly why they are separate translation units.

---

## A structural note

Phase 3 resolves cubit entanglement by requiring paired cubits (0↔7, 1↔6, 2↔5)
to agree, leaving indices 3 and 4 unpaired and therefore unresolved. The
trident's rule has the same shape one level up: two incoming hooks must agree
or the node stays unbound.

Offered as an observation, not a claim of identity — the two mechanisms are
independent and neither implements the other. It is worth noticing because it
is the same discipline applied to a different substrate, which is usually a
sign the discipline is the real idea.
