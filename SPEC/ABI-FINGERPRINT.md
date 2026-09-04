# ABI Fingerprint — Implementer's Reference

**MMUKO ABI revision 1** · companion to `MMUKO-ABI-SPEC.md` §4

This document is what you implement from if you are writing a third
implementation of the MMUKO fingerprint — a Go binding, a Zig module, a
verifier in Python. It is written to be sufficient on its own: an implementation
built from this text, without reading `src/mmuko_abi.c`, must produce
bit-identical output.

That property is the point. Two implementations that were transliterated from
one another prove nothing when they agree; two built independently from a
specification prove that the specification is unambiguous.

---

## 1. Canonical encoding

### 1.1 Grammar

```
export-encoding := "SYM=" symbol "|" signature-encoding

signature-encoding :=
      "MMUKOABI/1"
      "|A="  arch-digits          ; "32" or "64"
      "|CC=" convention-token     ; see 1.3
      "|R="  type-encoding
      "|N="  decimal-arity
      { "|P" decimal-index "=" type-encoding }
      "|V="  ( "0" | "1" )        ; variadic

type-encoding := scalar-token | aggregate-encoding

aggregate-encoding :=
      kind-open                   ; "S{" | "U{" | "E{"
      "sz=" decimal "," "al=" decimal ":"
      [ type-encoding { "," type-encoding } ]
      "}"
```

Every byte is ASCII. There is no whitespace anywhere. Decimal numbers have no
leading zeros and no sign; zero is written `0`.

### 1.2 Scalar tokens

| code   | token  |
|-------:|--------|
| `0x00` | `v`    |
| `0x01` | `b8`   |
| `0x10` | `i8`   |
| `0x11` | `u8`   |
| `0x12` | `i16`  |
| `0x13` | `u16`  |
| `0x14` | `i32`  |
| `0x15` | `u32`  |
| `0x16` | `i64`  |
| `0x17` | `u64`  |
| `0x20` | `f32`  |
| `0x21` | `f64`  |
| `0x30` | `ptr`  |
| `0x31` | `cstr` |

Tokens are frozen. `b8` rather than `bool` and `v` rather than `void` are
arbitrary choices, fixed now, and changing either would change every fingerprint
in existence.

### 1.3 Convention tokens

| value | token      |
|------:|------------|
| `1`   | `cdecl`    |
| `2`   | `stdcall`  |
| `3`   | `fastcall` |
| `4`   | `syscall`  |

Value `0` and any unrecognised value MUST be **refused**, not encoded. If an
unknown convention were encoded as some placeholder token, two different unknown
values would share a fingerprint — and the failure would be a wrong bind, not a
diagnostic.

### 1.4 Refusals

The encoder MUST refuse, returning an error rather than any output:

| condition | reason |
|---|---|
| profile is neither 32 nor 64 | the profile tag is an unconditional input |
| convention not in §1.3 | see above |
| return type descriptor is null | nothing to encode |
| a scalar descriptor carries fields | malformed; must not be normalised to the bare token |
| `void` appears as a parameter | `f(void)` is arity zero; conflating them shares a fingerprint across different contracts |
| arity exceeds 32 | bounded so the encoder needs no heap |
| aggregate nesting exceeds 8 | likewise, and so a hostile descriptor cannot drive unbounded recursion |
| aggregate has `size == 0` or `align == 0` | layout is contract; an unmeasured aggregate has none |
| the output buffer would overflow (4096 bytes) | a truncated encoding is not an encoding |

Every one of these is a refusal rather than a normalisation. An approximate
identity is worse than no identity: it compares equal to things it does not
describe.

### 1.5 Worked examples

`int32 add(int32, int32)`, cdecl, mmuko64:

```
MMUKOABI/1|A=64|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0
```

The same signature on mmuko32 — note that nothing but the profile tag changes,
and that this is sufficient to make the fingerprints differ:

```
MMUKOABI/1|A=32|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0
```

`double add(double, double)`, cdecl, mmuko64 — the transcript's break:

```
MMUKOABI/1|A=64|CC=cdecl|R=f64|N=2|P0=f64|P1=f64|V=0
```

`void f(struct { int32; double })` naturally aligned, mmuko64:

```
MMUKOABI/1|A=64|CC=cdecl|R=v|N=1|P0=S{sz=16,al=8:i32,f64}|V=0
```

The same struct packed — a different contract, and a different encoding:

```
MMUKOABI/1|A=64|CC=cdecl|R=v|N=1|P0=S{sz=12,al=1:i32,f64}|V=0
```

`int32 printf_like(cstr, ...)`, variadic, mmuko64:

```
MMUKOABI/1|A=64|CC=cdecl|R=i32|N=1|P0=cstr|V=1
```

As a named export:

```
SYM=add|MMUKOABI/1|A=64|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0
```

---

## 2. The hash

### 2.1 Constants

```
LANE_A_OFFSET = 0xcbf29ce484222325
LANE_A_PRIME  = 0x00000100000001B3
LANE_B_OFFSET = 0x9ae16a3b2f90404f
LANE_B_PRIME  = 0x00000100000001C9
```

Lane A is standard FNV-1a 64. Lane B uses a different odd offset and a different
odd prime, so the two lanes are not affine transforms of one another and a
collision in one carries no information about the other.

### 2.2 Algorithm

All arithmetic is on unsigned 64-bit integers with wrapping multiplication.

```
function fingerprint(bytes):
    a = LANE_A_OFFSET
    b = LANE_B_OFFSET

    for byte in bytes:                  # forward
        a = (a XOR byte) * LANE_A_PRIME

    for byte in reverse(bytes):         # backward
        b = (b XOR byte) * LANE_B_PRIME

    a = (a XOR length(bytes)) * LANE_A_PRIME
    b = (b XOR length(bytes)) * LANE_B_PRIME

    return { lo: a, hi: b }
```

Three details, each load-bearing:

- **Lane B runs backward.** A transposition of two bytes that happens to leave
  lane A unchanged still moves lane B. This is what makes argument *order*
  reliably significant.
- **The length is mixed in at the end.** Without it, a prefix could collide with
  its own extension in a padded encoding.
- **`lo` is lane A and `hi` is lane B.** The field names are historical; the
  assignment is fixed and an implementation that swapped them would produce
  different formatted output for the same input.

### 2.3 Text form

`hi` then `:` then `lo`, both as 16 lowercase hexadecimal digits, zero-padded:

```
cede175a87d95827:64442899dfdb396b
```

Exactly 33 characters.

### 2.4 Test vectors

Raw byte hashes, checked by `differential.rs: raw_byte_hash_agrees`:

| input | must differ from |
|---|---|
| `""` | anything else |
| `"abc"` | `"cba"` (lane B catches the reversal) |
| `"ab"` | `"ab\0"` (length mixing catches the extension) |

Signature fingerprints on mmuko64, cdecl, as produced by the reference
implementation and printed by `mmuko-abi-dump`:

| export | fingerprint |
|---|---|
| `add` : `i32(i32,i32)` | `cede175a87d95827:64442899dfdb396b` |
| `mul` : `i32(i32,i32)` | `36008af78159144a:100d4bee77f69f92` |
| `add` : `f64(f64,f64)` | `50b66bfe67b08663:124b22dca50f727d` |

The first two differ only in the symbol name, which is the check that the name
is genuinely bound into the identity. The first and third differ only in the
types, which is the transcript's break.

---

## 3. Module fingerprints

A module's identity mixes its header with an **order-independent** fold of its
export fingerprints.

```
header_bytes := "MOD=" module-name "|A=" arch "|V=" semverx-encoding
header       := fingerprint(header_bytes)

fold := { lo: 0, hi: 0 }
for each export e:
    f = export_fingerprint(e)
    fold.lo ^= f.lo
    fold.hi ^= f.hi

module_fingerprint := { lo: header.lo XOR fold.lo,
                        hi: header.hi XOR fold.hi }
```

where `semverx-encoding` is `major.state.minor.state.patch.state` with the state
tokens spelled out in full (`experimental`, `beta`, `stable`, `legacy`, `lts`).

XOR is chosen precisely *because* it is order-independent. Reordering a
declaration list changes nothing any caller can observe, so it must not read as
a breaking change — a sequential hash would make a cosmetic reshuffle look like
one and train everybody to ignore the signal.

XOR's known weakness is that duplicates cancel to zero. That is handled
upstream: duplicate symbol names are rejected by table validation
(`MMUKO-ABI-SPEC.md` §5.2) before this function is reachable on any table the
system acts upon. An implementation that computes module fingerprints without
first validating for duplicates has a hole here.

---

## 4. Conformance checklist

An implementation is conformant when, for every input in the cross-product of
{both profiles} × {every scalar as return} × {every scalar as each of 0–32
parameters} × {every convention} × {variadic, fixed}:

1. it produces the canonical encoding of §1 byte for byte;
2. it produces the fingerprint of §2 bit for bit;
3. it refuses exactly the inputs listed in §1.4, and no others;
4. it produces the module fingerprint of §3.

`rust/mmuko-kernel/tests/differential.rs` is the reference harness for this. It
runs 4 732 encoding comparisons and the full satisfaction matrix against the C
implementation, plus a layout check of the `#[repr(C)]` mirrors against the C
compiler's measured offsets.

To test a new implementation against the reference, link
`tools/mmuko_layout_probe.c` and the four core translation units, and compare
against `mmuko_encode_signature` and `mmuko_fp_export` directly — the same way
the Rust suite does.
