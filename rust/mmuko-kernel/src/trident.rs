// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! Trident consensus binding.
//!
//! ```text
//!       u1      u2          u1 = REQUIRED contract, frozen at the caller's
//!         \    /                 compile time
//!          \  /             u2 = PROVIDED contract, read from the loaded
//!           v                    object's self-describing table
//!           |                v  = convergence: the binding decision
//!           w                w  = propagation: the callable slot
//! ```
//!
//! Two incoming hooks, one outgoing hook. Constant degree, so a graph of these
//! costs exactly three edges per node and resolves in time linear in the node
//! count.
//!
//! **The rule.** A node binds if and only if its two incoming contracts are
//! identical. Otherwise it stays unbound and blocks propagation.
//!
//! Applied to a C ABI, "identical" means identical 128-bit export
//! fingerprint: same profile, same convention, same return class and width,
//! same arity, same argument classes and widths, same symbol name.
//!
//! `no_std`. No allocation. Nodes live in caller-provided storage.

use crate::abi::{Arch, ExportDesc};
use crate::fingerprint::Fingerprint;
use crate::kernel::Policy;
use crate::semverx::Unsatisfied;

/// A node's binding state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BindState {
    /// Never resolved. Distinct from a fault: a node that has not been asked
    /// is not a node that was asked and refused.
    Unresolved,
    /// Both hooks present and identical. `w` holds the provider's address.
    Bound,
    /// Resolved and refused. `w` holds the trap.
    Fault,
}

impl BindState {
    /// Diagnostic name, matching the C side.
    pub const fn name(self) -> &'static str {
        match self {
            BindState::Unresolved => "UNRESOLVED",
            BindState::Bound => "BOUND",
            BindState::Fault => "UNBOUND_FAULT",
        }
    }
}

/// Why a node refused to bind.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Fault {
    /// No fault.
    None,
    /// A hook is absent.
    Missing,
    /// A descriptor for the other profile met this call site.
    Arch,
    /// The calling conventions differ.
    CallConv,
    /// The shapes differ. This is the transcript's break.
    Fingerprint,
    /// The shape agrees but the version line does not.
    Version,
    /// A SemVerX state fell outside the policy mask.
    State,
    /// A dependency is faulted. Containment.
    Upstream,
    /// A descriptor failed structural validation.
    Malformed,
}

impl Fault {
    /// Diagnostic name, matching the C side.
    pub const fn name(self) -> &'static str {
        match self {
            Fault::None => "none",
            Fault::Missing => "missing-hook",
            Fault::Arch => "arch-mismatch",
            Fault::CallConv => "calling-convention-mismatch",
            Fault::Fingerprint => "abi-fingerprint-mismatch",
            Fault::Version => "semverx-version-mismatch",
            Fault::State => "semverx-state-refused",
            Fault::Upstream => "upstream-fault-contained",
            Fault::Malformed => "malformed-descriptor",
        }
    }
}

/// The address every unbound slot points at.
///
/// A conforming caller checks [`Trident::state`] and never reaches this. It
/// exists for the caller that does not: it turns "jump through a pointer that
/// was never validated" into a single counted event at a known address, rather
/// than a jump to null at some arbitrary later moment or -- much worse -- a
/// jump to a stale address that still holds executable code of the wrong
/// shape.
///
/// It takes no arguments and returns `i32`. Under both supported profiles the
/// CALLER cleans the argument area, so entering it through a pointer of any
/// non-variadic signature does not unbalance the stack.
pub extern "C" fn trap() -> i32 {
    TRAP_COUNT.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
    TRAP_SENTINEL
}

/// Value the trap leaves in the integer return register: `'M' 'K' 'O' '!'`.
pub const TRAP_SENTINEL: i32 = 0x4D4B_4F21;

static TRAP_COUNT: core::sync::atomic::AtomicU64 = core::sync::atomic::AtomicU64::new(0);

/// How many times any trap slot has been entered.
///
/// This counter, not the trap's return value, is the observable a supervisor
/// reads: for a slot whose declared return class is floating point the caller
/// reads XMM0, which the trap does not write, so the returned value there is
/// unspecified by construction.
pub fn trap_count() -> u64 {
    TRAP_COUNT.load(core::sync::atomic::Ordering::Relaxed)
}

/// Reset the trap counter. For tests and for a supervisor that has handled the
/// faults it observed.
pub fn trap_reset() {
    TRAP_COUNT.store(0, core::sync::atomic::Ordering::Relaxed);
}

/// The address of [`trap`], as an opaque pointer.
pub fn trap_address() -> *mut core::ffi::c_void {
    trap as extern "C" fn() -> i32 as *mut core::ffi::c_void
}

/// One trident node: one symbol's binding.
#[derive(Debug, Clone, Copy)]
pub struct Trident {
    /// Human name, for diagnostics.
    pub slot: &'static str,
    /// `u1` -- what the caller was compiled against.
    pub required: Option<ExportDesc>,
    /// `u2` -- what the loaded object provides. Replaced on hot-swap.
    pub provided: Option<ExportDesc>,

    /// `v` -- the verdict.
    pub state: BindState,
    /// Why, when the verdict is [`BindState::Fault`].
    pub fault: Fault,
    /// `u1`'s computed identity.
    pub fp_required: Fingerprint,
    /// `u2`'s computed identity.
    pub fp_provided: Fingerprint,

    /// `w` -- the callable slot. Never null after the first resolve.
    pub slot_addr: *mut core::ffi::c_void,
    /// Monotonic. Increments on every transition INTO bound.
    pub generation: u64,
    /// Number of dependency edges currently satisfied, recorded at the last
    /// resolve. Used only for diagnostics.
    pub deps_ok: bool,
}

// The node holds a code address, never mutable state shared across threads.
unsafe impl Send for Trident {}
unsafe impl Sync for Trident {}

impl Trident {
    /// A fresh node with no provider. Starts unresolved with `w` at the trap.
    pub fn new(slot: &'static str, required: ExportDesc) -> Trident {
        Trident {
            slot,
            required: Some(required),
            provided: None,
            state: BindState::Unresolved,
            fault: Fault::None,
            fp_required: Fingerprint::ZERO,
            fp_provided: Fingerprint::ZERO,
            slot_addr: trap_address(),
            generation: 0,
            deps_ok: true,
        }
    }

    /// An empty node, for static array initialisation.
    pub const fn empty() -> Trident {
        Trident {
            slot: "",
            required: None,
            provided: None,
            state: BindState::Unresolved,
            fault: Fault::None,
            fp_required: Fingerprint::ZERO,
            fp_provided: Fingerprint::ZERO,
            slot_addr: core::ptr::null_mut(),
            generation: 0,
            deps_ok: true,
        }
    }

    /// The callable address, or `None` unless bound.
    ///
    /// Callers that prefer the trap's fail-loud semantics read
    /// [`Trident::slot_addr`] directly.
    pub fn address(&self) -> Option<*mut core::ffi::c_void> {
        if self.state == BindState::Bound {
            Some(self.slot_addr)
        } else {
            None
        }
    }

    /// Resolve this node under `policy`, given whether its dependencies are
    /// all bound.
    ///
    /// **Decide first, publish last.** The verdict is computed entirely into
    /// locals and `slot_addr` is written exactly once, at the end, to either
    /// the validated provider address or the trap. A concurrent reader
    /// therefore sees either the previous value or the new one, and both
    /// passed consensus at the moment they were written. There is no instant
    /// at which the slot holds an unvalidated address. That is the whole of
    /// the hot-swap safety argument, and it is why a fix can land under a
    /// running caller with no restart.
    pub fn resolve(&mut self, policy: &Policy, deps_bound: bool) -> BindState {
        let mut fault = Fault::None;
        let mut fp_req = Fingerprint::ZERO;
        let mut fp_prv = Fingerprint::ZERO;
        let mut provider_addr: *mut core::ffi::c_void = core::ptr::null_mut();

        self.deps_ok = deps_bound;

        'decide: {
            // Containment first: an upstream fault disqualifies this node
            // before its own hooks are examined at all. Checking it here
            // rather than after is what makes containment transitive rather
            // than merely local.
            if !deps_bound {
                fault = Fault::Upstream;
                break 'decide;
            }

            let (req, prv) = match (self.required.as_ref(), self.provided.as_ref()) {
                (None, _) => {
                    fault = Fault::Malformed;
                    break 'decide;
                }
                (Some(_), None) => {
                    fault = Fault::Missing;
                    break 'decide;
                }
                (Some(r), Some(p)) => (r, p),
            };

            let addr = match prv.address {
                Some(a) => a.as_ptr(),
                None => {
                    fault = Fault::Malformed;
                    break 'decide;
                }
            };

            if policy.arch == Arch::None {
                fault = Fault::Arch;
                break 'decide;
            }
            // Checked explicitly as well as through the fingerprint, so the
            // diagnostic names the real cause instead of reporting every
            // structural disagreement as a shape mismatch.
            if req.sig.cc != prv.sig.cc {
                fault = Fault::CallConv;
                break 'decide;
            }

            // THE RULE.
            fp_req = match req.fingerprint(policy.arch) {
                Ok(f) => f,
                Err(_) => {
                    fault = Fault::Malformed;
                    break 'decide;
                }
            };
            fp_prv = match prv.fingerprint(policy.arch) {
                Ok(f) => f,
                Err(_) => {
                    fault = Fault::Malformed;
                    break 'decide;
                }
            };
            if fp_req != fp_prv {
                fault = Fault::Fingerprint;
                break 'decide;
            }

            // SemVerX comes AFTER the fingerprint, deliberately. ABI identity
            // is a machine fact; version compatibility is a policy claim; and
            // a policy claim must never be able to admit a contract the
            // machine has already refused.
            match req.since.satisfies(&prv.since, policy.state_mask) {
                Ok(()) => {}
                Err(Unsatisfied::StateRefused) => {
                    fault = Fault::State;
                    break 'decide;
                }
                Err(_) => {
                    fault = Fault::Version;
                    break 'decide;
                }
            }

            provider_addr = addr;
        }

        let (next_state, next_slot) = if fault == Fault::None {
            (BindState::Bound, provider_addr)
        } else {
            (BindState::Fault, trap_address())
        };

        self.fp_required = fp_req;
        self.fp_provided = fp_prv;
        self.fault = fault;

        // Monotone: only ever advances, and only on a transition INTO bound.
        // A caller that cached a function pointer compares the generation it
        // saw against the current one to learn it must re-read the slot. It is
        // never told, and it never restarts.
        if next_state == BindState::Bound && self.state != BindState::Bound {
            self.generation += 1;
        }

        self.state = next_state;
        self.slot_addr = next_slot; // single publication point
        next_state
    }

    /// Replace the provided hook and re-resolve.
    ///
    /// This is the SemVerX prototype's registry hot-swap at ABI granularity:
    /// swap `u2`, re-run consensus, and the already-running caller picks up
    /// the corrected dependency at its next call through the slot.
    pub fn swap(
        &mut self,
        provided: Option<ExportDesc>,
        policy: &Policy,
        deps_bound: bool,
    ) -> BindState {
        self.provided = provided;
        self.resolve(policy, deps_bound)
    }
}
