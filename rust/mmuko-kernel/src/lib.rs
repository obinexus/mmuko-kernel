// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! # mmuko-kernel
//!
//! The MMUKO kernel's dynamic C ABI: SemVerX identity, 128-bit ABI
//! fingerprints, and trident consensus binding, for the `mmuko32` and
//! `mmuko64` profiles.
//!
//! ## What this crate is for
//!
//! A shared object exports `int add(int, int)`. A later release changes it to
//! `double add(double, double)` and keeps the same soname and symbol name. The
//! dynamic linker resolves by NAME, so the caller's already-compiled code
//! passes two integers and reads the integer return register while the callee
//! reads and writes the floating-point ones. Nothing traps. The program keeps
//! running and returns a number that is not a sum.
//!
//! This crate makes that outcome unreachable. A symbol's identity is the
//! 128-bit fingerprint of its complete calling contract, and a slot is
//! populated only when the contract the caller was compiled against and the
//! contract the object provides are byte-for-byte identical.
//!
//! ## Freestanding
//!
//! `no_std` by default. No allocator, no libc, no floating point in the
//! resolver path, no external crates. [`kernel::KernelRegistry`] is a
//! fixed-capacity structure that lives in static storage, so the whole
//! resolver runs at ring 0 before any memory manager exists.
//!
//! Enable the `hosted` feature (or build the tests) to pull in `std` for the
//! test harness and the C differential suite. It changes nothing about the
//! resolver's behaviour, only what it is permitted to call.
//!
//! ## Layout
//!
//! - [`fingerprint`] — the canonical encoder and the 128-bit hash. This is a
//!   second, independent implementation of the algorithm in `src/mmuko_abi.c`,
//!   and `tests/differential.rs` asserts the two agree on every signature it
//!   can construct. Two implementations that must agree is the only way to
//!   keep a Rust kernel and a C module honest about the same boundary.
//! - [`semverx`] — `major.state.minor.state.patch.state`.
//! - [`abi`] — `#[repr(C)]` mirrors of the C descriptor structures, layout
//!   checked against the C side at test time.
//! - [`trident`] — the consensus rule and fault containment.
//! - [`kernel`] — the registry the MMUKO kernel actually calls, plus its
//!   `extern "C"` entry points.
//! - [`select`] — choosing among several candidates that could fill one slot,
//!   by consensus vote, artifact confidence and version weight. The ABI gate
//!   runs FIRST and filters the candidate set; scoring only ever ranks
//!   survivors, and can never admit a contract the gate refused.

#![cfg_attr(not(any(test, feature = "hosted")), no_std)]
#![deny(missing_docs)]
#![warn(clippy::all)]

pub mod abi;
pub mod fingerprint;
pub mod kernel;
pub mod select;
pub mod semverx;
pub mod trident;

pub use abi::{Arch, CallConv, ExportDesc, FnSig, ModuleDesc, TypeCode, TypeDesc};
pub use fingerprint::Fingerprint;
pub use kernel::{KernelRegistry, Policy};
pub use select::{Candidate, SelectGraph, Vote, Votes, Weights};
pub use semverx::{SemVerX, State, StateMask};
pub use trident::{BindState, Fault, Trident};

/// Descriptor format revision understood by this crate.
pub const ABI_REV: u32 = 1;

/// Magic word at the head of every module descriptor: `'M' 'M' 'K' 'O'`.
pub const ABI_MAGIC: u32 = 0x4F4B_4D4D_u32 & 0xFFFF_FFFF;

/// Domain-separation tag prefixed to every canonical encoding.
///
/// Changing this string changes every fingerprint in existence, which is
/// exactly what a breaking change to the encoding itself must do.
pub const ABI_TAG: &str = "MMUKOABI/1";

/// The `mmuko32`/`mmuko64` profile this crate was compiled for.
pub const NATIVE_ARCH: Arch = if cfg!(target_pointer_width = "64") {
    Arch::Mmuko64
} else {
    Arch::Mmuko32
};

// The C side computes MMUKO_ABI_MAGIC as the little-endian word 'M','M','K','O'
// = 0x4F4B4D4D. Written out longhand here so the two constants can be compared
// by eye as well as by the differential test.
const _: () = assert!(ABI_MAGIC == 0x4F4B_4D4D);

// ---------------------------------------------------------------------------
// Freestanding panic handling
// ---------------------------------------------------------------------------

/// Halt handler for the freestanding build.
///
/// A `staticlib` built as `no_std` must supply a `#[panic_handler]` at link
/// time, so one lives here. It halts rather than returning, because there is
/// nothing sensible to return to: a panic in the resolver means a descriptor
/// invariant this crate is supposed to enforce has already been violated, and
/// continuing would mean binding on the strength of state we know is wrong.
///
/// The resolver itself is written not to reach this. Every fallible path
/// returns a typed error, arithmetic is saturating, and the one indexing-heavy
/// structure ([`kernel::KernelRegistry`]) is bounds-checked against its own
/// `len` before every access. The handler is the backstop, not the strategy.
///
/// A larger MMUKO kernel that supplies its own handler should depend on this
/// crate as an `rlib` with `features = ["extern-panic"]`, which suppresses
/// this definition and leaves the symbol to the host.
#[cfg(all(not(test), not(feature = "hosted"), not(feature = "extern-panic")))]
#[panic_handler]
fn mmuko_panic(_info: &core::panic::PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}
