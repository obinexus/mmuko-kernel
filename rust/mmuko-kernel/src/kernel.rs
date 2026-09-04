// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! The MMUKO kernel's ABI registry.
//!
//! This is the module the kernel proper calls. It owns every trident node, the
//! dependency edges between them, and the `extern "C"` entry points a C kernel
//! or a C module uses to reach the resolver.
//!
//! # Freestanding by construction
//!
//! [`KernelRegistry`] is a fixed-capacity structure with no allocator, no
//! `Vec`, no `Box`, and no libc. It is sized at compile time by a const
//! generic and lives in static storage, so the resolver runs before any memory
//! manager exists. Dependency edges are a bitmap rather than a pointer graph,
//! which keeps the whole structure `Copy`-able, position-independent, and free
//! of the aliasing questions a graph of `&mut` nodes would raise.
//!
//! # What the kernel does with it
//!
//! ```text
//!   boot          KernelRegistry::new()
//!   declare       register(slot, required)          -- one per call site
//!   depend        add_dependency(dependent, on)     -- optional edges
//!   a module      offer(slot, provided)             -- from the loader
//!     arrives
//!   converge      resolve_all()                     -- fixpoint
//!   call          address(slot)  -> Option<addr>
//!   a module      offer(slot, new_provided) + resolve_all()
//!     is replaced                                   -- no restart
//! ```

use crate::abi::{Arch, ExportDesc, ModuleDesc};
use crate::semverx::StateMask;
use crate::trident::{BindState, Fault, Trident};

/// Binding policy for a call site.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Policy {
    /// The call site's architecture profile.
    pub arch: Arch,
    /// Which SemVerX states a provider may carry.
    pub state_mask: StateMask,
}

impl Default for Policy {
    /// Native profile, stable only.
    ///
    /// Stable-only is the default because opting in to unreviewed code should
    /// be a thing someone wrote down, not a thing that happens because nobody
    /// changed a setting.
    fn default() -> Self {
        Policy {
            arch: crate::NATIVE_ARCH,
            state_mask: StateMask::STABLE,
        }
    }
}

impl Policy {
    /// A policy for an explicit profile and mask.
    pub const fn new(arch: Arch, state_mask: StateMask) -> Policy {
        Policy { arch, state_mask }
    }
}

/// Why a registry operation failed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RegistryError {
    /// The registry is full. Raise `N`.
    Full,
    /// No slot by that name.
    UnknownSlot,
    /// A slot with that name is already registered. Duplicate slots would make
    /// lookup order-dependent, and an order-dependent resolver can bind
    /// differently on two machines from the same inputs.
    DuplicateSlot,
    /// A dependency edge would exceed the per-node edge capacity.
    TooManyDependencies,
    /// A dependency edge would create a cycle.
    CyclicDependency,
}

/// Outcome of a whole-registry resolve.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ResolveReport {
    /// Nodes that converged.
    pub bound: u32,
    /// Nodes that refused.
    pub faulted: u32,
    /// Passes taken to reach the fixpoint.
    pub passes: u32,
}

impl ResolveReport {
    /// True when every registered node bound.
    pub fn is_complete(&self) -> bool {
        self.faulted == 0
    }
}

/// Maximum dependency edges per node.
///
/// A trident has exactly two incoming hooks and one outgoing hook; these edges
/// are the separate, coarser "this slot is useless without that one" relation
/// the loader builds between modules. Eight is generous for that and keeps the
/// bitmap a single `u64` per node at `N <= 64`.
pub const MAX_DEPS: usize = 8;

/// The kernel's ABI registry: `N` trident nodes in static storage.
///
/// `N` is a const generic so the kernel sizes it at compile time and the whole
/// structure can live in `.bss` with no allocator behind it.
pub struct KernelRegistry<const N: usize> {
    nodes: [Trident; N],
    /// `deps[i][k]` is the index of the k-th node that node `i` depends on.
    deps: [[u16; MAX_DEPS]; N],
    ndeps: [u8; N],
    len: usize,
    policy: Policy,
}

impl<const N: usize> Default for KernelRegistry<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> KernelRegistry<N> {
    /// An empty registry with the default policy.
    pub const fn new() -> Self {
        KernelRegistry {
            nodes: [Trident::empty(); N],
            deps: [[0u16; MAX_DEPS]; N],
            ndeps: [0u8; N],
            len: 0,
            policy: Policy {
                arch: crate::NATIVE_ARCH,
                state_mask: StateMask::STABLE,
            },
        }
    }

    /// Replace the binding policy. Takes effect at the next resolve.
    pub fn set_policy(&mut self, policy: Policy) {
        self.policy = policy;
    }

    /// The current policy.
    pub fn policy(&self) -> Policy {
        self.policy
    }

    /// How many slots are registered.
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether no slots are registered.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Capacity, `N`.
    pub const fn capacity(&self) -> usize {
        N
    }

    /// Declare a call site: "this kernel calls `slot`, and it was compiled
    /// against `required`."
    ///
    /// Returns the slot's index, which is a stable handle for the registry's
    /// lifetime.
    pub fn register(
        &mut self,
        slot: &'static str,
        required: ExportDesc,
    ) -> Result<usize, RegistryError> {
        if self.find(slot).is_some() {
            return Err(RegistryError::DuplicateSlot);
        }
        if self.len >= N {
            return Err(RegistryError::Full);
        }
        let idx = self.len;
        self.nodes[idx] = Trident::new(slot, required);
        self.ndeps[idx] = 0;
        self.len += 1;
        Ok(idx)
    }

    /// Index of a slot by name.
    pub fn find(&self, slot: &str) -> Option<usize> {
        self.nodes[..self.len].iter().position(|n| n.slot == slot)
    }

    /// Read a node.
    pub fn node(&self, idx: usize) -> Option<&Trident> {
        self.nodes.get(idx).filter(|_| idx < self.len)
    }

    /// Every registered node.
    pub fn nodes(&self) -> &[Trident] {
        &self.nodes[..self.len]
    }

    /// Record that `dependent` is useless unless `on` is bound.
    ///
    /// Cycles are rejected at insertion rather than tolerated at resolve time.
    /// The fixpoint loop would terminate on a cycle regardless -- every node in
    /// it would simply stay unbound -- but it would do so by producing a
    /// permanent, silent `Upstream` fault on every member with no indication
    /// of where the loop was. Refusing the edge names the problem at the
    /// moment someone creates it.
    pub fn add_dependency(&mut self, dependent: usize, on: usize) -> Result<(), RegistryError> {
        if dependent >= self.len || on >= self.len {
            return Err(RegistryError::UnknownSlot);
        }
        if self.reaches(on, dependent) || dependent == on {
            return Err(RegistryError::CyclicDependency);
        }
        let n = self.ndeps[dependent] as usize;
        if n >= MAX_DEPS {
            return Err(RegistryError::TooManyDependencies);
        }
        self.deps[dependent][n] = on as u16;
        self.ndeps[dependent] = (n + 1) as u8;
        Ok(())
    }

    /// Depth-first reachability over the existing edges. Bounded by `self.len`
    /// because the edge set is acyclic by this function's own guarantee.
    fn reaches(&self, from: usize, target: usize) -> bool {
        if from == target {
            return true;
        }
        for k in 0..self.ndeps[from] as usize {
            let next = self.deps[from][k] as usize;
            if self.reaches(next, target) {
                return true;
            }
        }
        false
    }

    /// Offer a provider for a slot. This is hook `u2`.
    ///
    /// Passing `None` withdraws the provider, which is what a module unload
    /// does: the slot returns to the trap rather than keeping an address whose
    /// pages have gone.
    pub fn offer(&mut self, idx: usize, provided: Option<ExportDesc>) -> Result<(), RegistryError> {
        if idx >= self.len {
            return Err(RegistryError::UnknownSlot);
        }
        self.nodes[idx].provided = provided;
        Ok(())
    }

    /// Offer providers for every slot a module satisfies, matching by symbol
    /// name. Slots the module does not export are left untouched.
    ///
    /// Returns how many slots were offered a provider.
    pub fn offer_module(&mut self, module: &ModuleDesc) -> u32 {
        let mut n = 0;
        for i in 0..self.len {
            let symbol = match self.nodes[i].required.as_ref() {
                Some(r) => r.symbol,
                None => continue,
            };
            if let Some(e) = module.find(symbol) {
                self.nodes[i].provided = Some(*e);
                n += 1;
            }
        }
        n
    }

    /// Resolve every node to a fixpoint, honouring dependency edges.
    ///
    /// Termination: the bound count is non-decreasing across passes within a
    /// run and bounded above by `len`, so the loop runs at most `len + 1`
    /// passes and exits as soon as a pass adds nothing.
    pub fn resolve_all(&mut self) -> ResolveReport {
        let policy = self.policy;
        let mut report = ResolveReport::default();
        let mut prev_bound = u32::MAX;

        for pass in 0..=self.len {
            let mut bound = 0u32;
            let mut faulted = 0u32;

            for i in 0..self.len {
                // Read dependency states before touching node i, so the pass
                // is a function of the previous state rather than of the order
                // within the pass.
                let mut deps_bound = true;
                for k in 0..self.ndeps[i] as usize {
                    let d = self.deps[i][k] as usize;
                    if self.nodes[d].state != BindState::Bound {
                        deps_bound = false;
                        break;
                    }
                }
                match self.nodes[i].resolve(&policy, deps_bound) {
                    BindState::Bound => bound += 1,
                    _ => faulted += 1,
                }
            }

            report.bound = bound;
            report.faulted = faulted;
            report.passes = (pass + 1) as u32;
            if bound == prev_bound {
                break;
            }
            prev_bound = bound;
        }
        report
    }

    /// The callable address for a slot, or `None` unless bound.
    pub fn address(&self, slot: &str) -> Option<*mut core::ffi::c_void> {
        self.find(slot).and_then(|i| self.nodes[i].address())
    }

    /// A slot's state and fault.
    pub fn status(&self, slot: &str) -> Option<(BindState, Fault)> {
        self.find(slot).map(|i| (self.nodes[i].state, self.nodes[i].fault))
    }

    /// A slot's generation counter. A caller that cached a function pointer
    /// compares this against what it saw to learn it must re-read the slot.
    pub fn generation(&self, slot: &str) -> Option<u64> {
        self.find(slot).map(|i| self.nodes[i].generation)
    }
}

// ---------------------------------------------------------------------------
// C entry points
// ---------------------------------------------------------------------------

/// Default registry capacity for the `extern "C"` surface.
pub const KERNEL_SLOTS: usize = 64;

/// Result codes for the C entry points, matching `mmuko_bind_state_t` where
/// they overlap.
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CResult {
    /// Every node bound.
    Ok = 0,
    /// One or more nodes faulted.
    Unbound = -1,
    /// A null or structurally invalid argument.
    BadArgs = -2,
    /// The registry is full.
    Full = -3,
}

/// Validate a raw C module descriptor.
///
/// This is the entire unsafe surface of the crate, and it is deliberately one
/// function: everything past it works on the safe types in [`crate::abi`].
///
/// # Safety
///
/// `desc` must either be null or point at a `mmuko_module_desc_t` that remains
/// mapped for the duration of the call. The function reads only the header
/// fields before validating them, and dereferences nothing further unless the
/// magic and revision match.
///
/// Note what this cannot check: whether the pointer is mapped at all. A caller
/// handing over a dangling table gets undefined behaviour, which is why the
/// loader drops its table pointer whenever it closes a handle.
#[no_mangle]
pub unsafe extern "C" fn mmuko_kernel_validate_table(
    desc: *const crate::abi::raw::CModuleDesc,
) -> i32 {
    if desc.is_null() {
        return CResult::BadArgs as i32;
    }
    let d = &*desc;
    if d.magic != crate::ABI_MAGIC {
        return CResult::BadArgs as i32;
    }
    if d.abi_rev != crate::ABI_REV {
        return CResult::BadArgs as i32;
    }
    if Arch::from_raw(d.arch) != crate::NATIVE_ARCH {
        return CResult::BadArgs as i32;
    }
    if d.module.is_null() {
        return CResult::BadArgs as i32;
    }
    if d.nexports > 0 && d.exports.is_null() {
        return CResult::BadArgs as i32;
    }
    if !crate::semverx::SemVerX::from_raw(&d.version).is_valid() {
        return CResult::BadArgs as i32;
    }
    CResult::Ok as i32
}

/// The trap address, for a C kernel that wants to fill its own vtable slots
/// with something that fails loudly before the resolver has run.
#[no_mangle]
pub extern "C" fn mmuko_kernel_trap_address() -> *mut core::ffi::c_void {
    crate::trident::trap_address()
}

/// How many times any trap slot has been entered.
#[no_mangle]
pub extern "C" fn mmuko_kernel_trap_count() -> u64 {
    crate::trident::trap_count()
}

/// The profile this kernel was built for: 32 or 64.
#[no_mangle]
pub extern "C" fn mmuko_kernel_arch() -> u32 {
    crate::NATIVE_ARCH as u32
}

/// Fingerprint an export described by a raw C signature, writing the two lanes
/// through `out_lo` and `out_hi`.
///
/// This is what makes the differential suite possible from the C side, and
/// what a C module uses to ask the Rust kernel "what identity do you compute
/// for this?" before it ships a release.
///
/// Returns 0 on success, negative on a malformed descriptor.
///
/// # Safety
///
/// `symbol` must be a NUL-terminated ASCII string; `sig` must point at a valid
/// `mmuko_fn_sig_t` whose type pointers are all valid; `out_lo` and `out_hi`
/// must be writable.
#[no_mangle]
pub unsafe extern "C" fn mmuko_kernel_fingerprint_export(
    symbol: *const u8,
    sig: *const crate::abi::raw::CFnSig,
    arch: u32,
    out_lo: *mut u64,
    out_hi: *mut u64,
) -> i32 {
    if symbol.is_null() || sig.is_null() || out_lo.is_null() || out_hi.is_null() {
        return CResult::BadArgs as i32;
    }
    let arch = Arch::from_raw(arch);
    if arch == Arch::None {
        return CResult::BadArgs as i32;
    }

    let name = match cstr(symbol, 256) {
        Some(s) => s,
        None => return CResult::BadArgs as i32,
    };

    let mut canon = crate::fingerprint::Canon::new();
    canon.put("SYM=");
    canon.put(name);
    canon.put_u8(b'|');
    if encode_raw_sig(&*sig, arch, &mut canon).is_err() {
        return CResult::BadArgs as i32;
    }
    let fp = crate::fingerprint::Fingerprint::of_bytes(canon.as_bytes());
    *out_lo = fp.lo;
    *out_hi = fp.hi;
    CResult::Ok as i32
}

/// Read a bounded NUL-terminated ASCII string.
///
/// # Safety
/// `p` must be readable for up to `max` bytes or up to a NUL, whichever first.
unsafe fn cstr<'a>(p: *const u8, max: usize) -> Option<&'a str> {
    let mut n = 0usize;
    while n < max && *p.add(n) != 0 {
        // Reject anything outside ASCII: a symbol name is a linker name, and
        // accepting arbitrary bytes here would make `as_str` lossy and the
        // encoding non-canonical.
        if *p.add(n) >= 0x80 {
            return None;
        }
        n += 1;
    }
    if n == max {
        return None; // unterminated within bounds
    }
    core::str::from_utf8(core::slice::from_raw_parts(p, n)).ok()
}

/// Encode a raw C signature using the Rust encoder.
///
/// # Safety
/// Every pointer reachable from `sig` must be valid.
unsafe fn encode_raw_sig(
    sig: &crate::abi::raw::CFnSig,
    arch: Arch,
    out: &mut crate::fingerprint::Canon,
) -> Result<(), ()> {
    use crate::abi::CallConv;

    let cc = CallConv::from_raw(sig.cc);
    if cc == CallConv::Invalid {
        return Err(());
    }
    if sig.nargs > crate::fingerprint::MAX_ARGS {
        return Err(());
    }
    if sig.ret.is_null() {
        return Err(());
    }
    if sig.nargs > 0 && sig.args.is_null() {
        return Err(());
    }

    out.put(crate::ABI_TAG);
    out.put("|A=");
    out.put_u64(arch as u32 as u64);
    out.put("|CC=");
    out.put(cc.name());
    out.put("|R=");
    encode_raw_type(&*sig.ret, arch, out, 0)?;
    out.put("|N=");
    out.put_u64(sig.nargs as u64);

    for i in 0..sig.nargs as usize {
        out.put("|P");
        out.put_u64(i as u64);
        out.put_u8(b'=');
        let ap = *sig.args.add(i);
        if ap.is_null() {
            return Err(());
        }
        let a = &*ap;
        if a.code == crate::abi::TypeCode::Void as u32 {
            return Err(());
        }
        encode_raw_type(a, arch, out, 0)?;
    }

    out.put("|V=");
    out.put_u64(if sig.flags & crate::abi::raw::FN_VARIADIC != 0 {
        1
    } else {
        0
    });

    if out.overflowed() {
        Err(())
    } else {
        Ok(())
    }
}

/// # Safety
/// Every pointer reachable from `t` must be valid.
unsafe fn encode_raw_type(
    t: &crate::abi::raw::CTypeDesc,
    arch: Arch,
    out: &mut crate::fingerprint::Canon,
    depth: u32,
) -> Result<(), ()> {
    use crate::abi::TypeCode;

    if depth > crate::fingerprint::TYPE_MAX_DEPTH {
        return Err(());
    }
    let code = TypeCode::from_raw(t.code).ok_or(())?;

    if let Some(tok) = crate::fingerprint::scalar_token(code) {
        if t.nfields != 0 || !t.fields.is_null() {
            return Err(());
        }
        out.put(tok);
        return Ok(());
    }

    out.put(match code {
        TypeCode::Struct => "S{",
        TypeCode::Union => "U{",
        TypeCode::Enum => "E{",
        _ => return Err(()),
    });
    if t.size == 0 || t.align == 0 {
        return Err(());
    }
    out.put("sz=");
    out.put_u64(t.size as u64);
    out.put(",al=");
    out.put_u64(t.align as u64);
    out.put_u8(b':');

    if t.nfields > 0 && t.fields.is_null() {
        return Err(());
    }
    for i in 0..t.nfields as usize {
        if i > 0 {
            out.put_u8(b',');
        }
        let fp = *t.fields.add(i);
        if fp.is_null() {
            return Err(());
        }
        encode_raw_type(&*fp, arch, out, depth + 1)?;
    }
    out.put_u8(b'}');
    Ok(())
}
