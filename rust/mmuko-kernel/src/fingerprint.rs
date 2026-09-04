// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! Canonical signature encoding and the 128-bit ABI fingerprint.
//!
//! This is a second implementation of the algorithm in `src/mmuko_abi.c`,
//! written independently against the specification in
//! `SPEC/ABI-FINGERPRINT.md` rather than transliterated from the C.
//!
//! Two implementations that must agree is not redundancy here, it is the
//! mechanism. The MMUKO kernel is Rust and the modules it binds are C; if the
//! two sides computed identity differently, the resolver would refuse correct
//! bindings or -- far worse -- accept incorrect ones, and it would do so in a
//! way no single-implementation test could detect.
//! `tests/differential.rs` asserts byte-for-byte agreement.
//!
//! No allocation. The encoder writes into a fixed [`Canon`] buffer, so it runs
//! before any memory manager exists.

use crate::abi::{Arch, CallConv, FnSig, TypeCode, TypeDesc};

/// Maximum length of a canonical encoding. Matches `MMUKO_CANON_MAX`.
pub const CANON_MAX: usize = 4096;

/// Maximum aggregate nesting depth. Matches `MMUKO_TYPE_MAX_DEPTH`.
pub const TYPE_MAX_DEPTH: u32 = 8;

/// Maximum described arity. Matches `MMUKO_MAX_ARGS`.
pub const MAX_ARGS: u32 = 32;

/// Why an encoding could not be produced.
///
/// Every variant is a refusal, never a normalisation. A descriptor the encoder
/// cannot represent exactly must not be given an approximate fingerprint: an
/// approximate identity is worse than no identity, because it compares equal
/// to things it does not describe.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EncodeError {
    /// The output buffer filled. A truncated encoding is not an encoding.
    Overflow,
    /// Aggregate nesting exceeded [`TYPE_MAX_DEPTH`].
    TooDeep,
    /// Arity exceeded [`MAX_ARGS`].
    TooManyArgs,
    /// An aggregate carried no measured size or alignment.
    UnsizedAggregate,
    /// A scalar descriptor also carried aggregate fields.
    MalformedType,
    /// `void` appeared as a parameter type. `f(void)` is arity zero.
    VoidParameter,
    /// The architecture profile was unspecified.
    NoArch,
    /// The calling convention was not one of the recognised set.
    BadCallConv,
    /// An export was encoded with an empty symbol name.
    EmptySymbol,
}

/// A fixed-capacity ASCII buffer. The encoder's only output surface.
pub struct Canon {
    buf: [u8; CANON_MAX],
    len: usize,
    overflow: bool,
}

impl Default for Canon {
    fn default() -> Self {
        Self::new()
    }
}

impl Canon {
    /// An empty buffer.
    pub const fn new() -> Self {
        Canon {
            buf: [0u8; CANON_MAX],
            len: 0,
            overflow: false,
        }
    }

    /// The bytes written so far.
    pub fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }

    /// The encoding as a string. Always valid UTF-8: the encoder emits ASCII.
    pub fn as_str(&self) -> &str {
        // SAFETY-free: every byte the encoder writes is ASCII by construction,
        // and symbol names are validated as ASCII before being appended.
        core::str::from_utf8(self.as_bytes()).unwrap_or("")
    }

    /// Whether the buffer overflowed. Sticky once set.
    pub fn overflowed(&self) -> bool {
        self.overflow
    }

    /// Append an ASCII string. Sets the overflow flag if it will not fit.
    pub fn put(&mut self, s: &str) {
        if self.overflow {
            return;
        }
        let b = s.as_bytes();
        if self.len + b.len() > CANON_MAX {
            self.overflow = true;
            return;
        }
        self.buf[self.len..self.len + b.len()].copy_from_slice(b);
        self.len += b.len();
    }

    /// Append one byte.
    pub fn put_u8(&mut self, c: u8) {
        if self.overflow {
            return;
        }
        if self.len + 1 > CANON_MAX {
            self.overflow = true;
            return;
        }
        self.buf[self.len] = c;
        self.len += 1;
    }

    /// Append a decimal integer.
    pub fn put_u64(&mut self, mut v: u64) {
        if v == 0 {
            self.put_u8(b'0');
            return;
        }
        let mut tmp = [0u8; 20];
        let mut i = 0usize;
        while v > 0 {
            tmp[i] = b'0' + (v % 10) as u8;
            v /= 10;
            i += 1;
        }
        while i > 0 {
            i -= 1;
            self.put_u8(tmp[i]);
        }
    }
}

// --------------------------------------------------------------------------
// The hash
// --------------------------------------------------------------------------

const FNV1A_OFFSET_A: u64 = 0xcbf2_9ce4_8422_2325;
const FNV1A_PRIME_A: u64 = 0x0000_0100_0000_01B3;
const FNV1A_OFFSET_B: u64 = 0x9ae1_6a3b_2f90_404f;
const FNV1A_PRIME_B: u64 = 0x0000_0100_0000_01C9;

/// A 128-bit ABI fingerprint.
///
/// Two independent FNV-1a lanes over the same canonical bytes: lane A forward
/// with the standard basis, lane B backward with a second basis. The lanes are
/// not affine transforms of one another, so a collision in one carries no
/// information about the other, and the backward pass means a transposition
/// that leaves lane A unchanged still moves lane B.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(C)]
pub struct Fingerprint {
    /// Lane A (forward).
    pub lo: u64,
    /// Lane B (reverse).
    pub hi: u64,
}

impl Fingerprint {
    /// The all-zero fingerprint. Never produced by a valid encoding.
    pub const ZERO: Fingerprint = Fingerprint { lo: 0, hi: 0 };

    /// Hash arbitrary bytes.
    pub fn of_bytes(data: &[u8]) -> Fingerprint {
        let mut a = FNV1A_OFFSET_A;
        let mut b = FNV1A_OFFSET_B;
        for &byte in data {
            a ^= byte as u64;
            a = a.wrapping_mul(FNV1A_PRIME_A);
        }
        for &byte in data.iter().rev() {
            b ^= byte as u64;
            b = b.wrapping_mul(FNV1A_PRIME_B);
        }
        // Mix the length so a prefix cannot collide with its extension.
        a ^= data.len() as u64;
        a = a.wrapping_mul(FNV1A_PRIME_A);
        b ^= data.len() as u64;
        b = b.wrapping_mul(FNV1A_PRIME_B);
        Fingerprint { lo: a, hi: b }
    }

    /// True when both lanes are zero.
    pub fn is_zero(&self) -> bool {
        self.lo == 0 && self.hi == 0
    }

    /// Render as `hi:lo` in lowercase hex, 33 bytes plus a NUL slot.
    pub fn format(&self, out: &mut [u8; 34]) {
        fn hex(mut v: u64, dst: &mut [u8]) {
            const D: &[u8; 16] = b"0123456789abcdef";
            for i in (0..16).rev() {
                dst[i] = D[(v & 0xF) as usize];
                v >>= 4;
            }
        }
        let (hi, rest) = out.split_at_mut(16);
        hex(self.hi, hi);
        rest[0] = b':';
        let mut lo = [0u8; 16];
        hex(self.lo, &mut lo);
        rest[1..17].copy_from_slice(&lo);
        rest[17] = 0;
    }
}

// --------------------------------------------------------------------------
// Type encoding
// --------------------------------------------------------------------------

/// The frozen canonical token for a scalar type code.
///
/// `None` for aggregates, which have no fixed-width token and encode
/// structurally instead.
pub fn scalar_token(code: TypeCode) -> Option<&'static str> {
    Some(match code {
        TypeCode::Void => "v",
        TypeCode::Bool => "b8",
        TypeCode::I8 => "i8",
        TypeCode::U8 => "u8",
        TypeCode::I16 => "i16",
        TypeCode::U16 => "u16",
        TypeCode::I32 => "i32",
        TypeCode::U32 => "u32",
        TypeCode::I64 => "i64",
        TypeCode::U64 => "u64",
        TypeCode::F32 => "f32",
        TypeCode::F64 => "f64",
        TypeCode::Ptr => "ptr",
        TypeCode::CStr => "cstr",
        TypeCode::Struct | TypeCode::Union | TypeCode::Enum => return None,
    })
}

fn encode_type(t: &TypeDesc, out: &mut Canon, depth: u32) -> Result<(), EncodeError> {
    if depth > TYPE_MAX_DEPTH {
        return Err(EncodeError::TooDeep);
    }

    if let Some(tok) = scalar_token(t.code) {
        // A scalar that also carries fields is malformed, and must be refused
        // rather than silently normalised to the bare token.
        if !t.fields.is_empty() {
            return Err(EncodeError::MalformedType);
        }
        out.put(tok);
        return if out.overflowed() {
            Err(EncodeError::Overflow)
        } else {
            Ok(())
        };
    }

    out.put(match t.code {
        TypeCode::Struct => "S{",
        TypeCode::Union => "U{",
        TypeCode::Enum => "E{",
        _ => return Err(EncodeError::MalformedType),
    });

    // Size and alignment are part of the contract: two structs with identical
    // field types but different packing are classified differently for
    // register assignment, so encoding the fields alone would let them share
    // a fingerprint.
    if t.size == 0 || t.align == 0 {
        return Err(EncodeError::UnsizedAggregate);
    }
    out.put("sz=");
    out.put_u64(t.size as u64);
    out.put(",al=");
    out.put_u64(t.align as u64);
    out.put_u8(b':');

    for (i, f) in t.fields.iter().enumerate() {
        if i > 0 {
            out.put_u8(b',');
        }
        encode_type(f, out, depth + 1)?;
    }
    out.put_u8(b'}');

    if out.overflowed() {
        Err(EncodeError::Overflow)
    } else {
        Ok(())
    }
}

// --------------------------------------------------------------------------
// Signature encoding
// --------------------------------------------------------------------------

/// Encode a signature into its canonical form.
///
/// For `int32 add(int32, int32)` on `mmuko64` with the C calling convention:
///
/// ```text
/// MMUKOABI/1|A=64|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0
/// ```
///
/// and for the breaking replacement `double add(double, double)`:
///
/// ```text
/// MMUKOABI/1|A=64|CC=cdecl|R=f64|N=2|P0=f64|P1=f64|V=0
/// ```
///
/// Different bytes, therefore different fingerprint, therefore no bind.
pub fn encode_signature(sig: &FnSig, arch: Arch, out: &mut Canon) -> Result<(), EncodeError> {
    if arch == Arch::None {
        return Err(EncodeError::NoArch);
    }
    if sig.args.len() as u32 > MAX_ARGS {
        return Err(EncodeError::TooManyArgs);
    }
    if sig.cc == CallConv::Invalid {
        return Err(EncodeError::BadCallConv);
    }

    out.put(crate::ABI_TAG);
    out.put("|A=");
    out.put_u64(arch as u32 as u64);
    out.put("|CC=");
    out.put(sig.cc.name());
    out.put("|R=");
    encode_type(sig.ret, out, 0)?;
    out.put("|N=");
    out.put_u64(sig.args.len() as u64);

    for (i, a) in sig.args.iter().enumerate() {
        out.put("|P");
        out.put_u64(i as u64);
        out.put_u8(b'=');
        // `void` is not a parameter type. Conflating `f(void)` with `f()`
        // would let two different calling contracts share a fingerprint on
        // any platform where they differ.
        if a.code == TypeCode::Void {
            return Err(EncodeError::VoidParameter);
        }
        encode_type(a, out, 0)?;
    }

    out.put("|V=");
    out.put_u64(if sig.variadic { 1 } else { 0 });

    if out.overflowed() {
        Err(EncodeError::Overflow)
    } else {
        Ok(())
    }
}

/// Fingerprint of a calling shape alone, independent of any symbol name.
pub fn signature(sig: &FnSig, arch: Arch) -> Result<Fingerprint, EncodeError> {
    let mut c = Canon::new();
    encode_signature(sig, arch, &mut c)?;
    Ok(Fingerprint::of_bytes(c.as_bytes()))
}

/// Fingerprint of a named export.
///
/// The symbol name is bound into the identity, so a correctly shaped function
/// under a different name cannot fill the slot.
pub fn export(symbol: &str, sig: &FnSig, arch: Arch) -> Result<Fingerprint, EncodeError> {
    if symbol.is_empty() {
        return Err(EncodeError::EmptySymbol);
    }
    let mut c = Canon::new();
    c.put("SYM=");
    c.put(symbol);
    c.put_u8(b'|');
    encode_signature(sig, arch, &mut c)?;
    if c.overflowed() {
        return Err(EncodeError::Overflow);
    }
    Ok(Fingerprint::of_bytes(c.as_bytes()))
}

/// Encode a named export into `out` without hashing it. For diagnostics and
/// for the differential suite, which compares the ENCODINGS as well as the
/// hashes -- agreement on the hash alone could hide a shared encoding bug.
pub fn encode_export(symbol: &str, sig: &FnSig, arch: Arch, out: &mut Canon) -> Result<(), EncodeError> {
    if symbol.is_empty() {
        return Err(EncodeError::EmptySymbol);
    }
    out.put("SYM=");
    out.put(symbol);
    out.put_u8(b'|');
    encode_signature(sig, arch, out)
}
