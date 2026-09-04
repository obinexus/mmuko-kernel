// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! Trident consensus and the kernel registry, in Rust.
//!
//! The same four properties `tests/test_trident.c` checks on the C side --
//! consensus, ordering (shape before version), containment, hot-swap safety --
//! asserted against the Rust implementation. Both implementations must hold
//! them, because either one may be the resolver in a given MMUKO build.

use mmuko_kernel::abi::{ty, Arch, CallConv, ExportDesc, FnSig, ModuleDesc};
use mmuko_kernel::kernel::{KernelRegistry, Policy, RegistryError};
use mmuko_kernel::semverx::{SemVerX, State, StateMask};
use mmuko_kernel::trident::{BindState, Fault, Trident};

// --- fixture functions: the transcript's two shapes ------------------------

extern "C" fn add_i32(a: i32, b: i32) -> i32 {
    a + b
}
extern "C" fn add_f64(a: f64, b: f64) -> f64 {
    a + b
}
extern "C" fn mul_i32(a: i32, b: i32) -> i32 {
    a * b
}

type AddFn = extern "C" fn(i32, i32) -> i32;

static SIG_II_I: FnSig = FnSig::new(CallConv::Cdecl, &ty::I32, &[ty::I32, ty::I32]);
static SIG_DD_D: FnSig = FnSig::new(CallConv::Cdecl, &ty::F64, &[ty::F64, ty::F64]);
static SIG_II_I_STD: FnSig = FnSig::new(CallConv::Stdcall, &ty::I32, &[ty::I32, ty::I32]);

const V1: SemVerX = SemVerX::uniform(1, 0, 0, State::Stable);
const V2X: SemVerX = SemVerX::new(2, State::Experimental, 0, State::Stable, 0, State::Stable);
const V2S: SemVerX = SemVerX::uniform(2, 0, 0, State::Stable);

fn req_add() -> ExportDesc {
    ExportDesc::require("add", &SIG_II_I, V1)
}

fn prov(sig: &'static FnSig, ver: SemVerX, f: usize) -> ExportDesc {
    ExportDesc::provide("add", sig, ver, f as *mut core::ffi::c_void)
}

fn pol() -> Policy {
    Policy::default()
}

// --------------------------------------------------------------------------

#[test]
fn consensus_binds_when_the_contracts_are_identical() {
    let mut t = Trident::new("add", req_add());
    assert_eq!(t.state, BindState::Unresolved);
    assert_eq!(
        t.slot_addr,
        mmuko_kernel::trident::trap_address(),
        "an unresolved slot points at the trap, never at null"
    );

    let p = prov(&SIG_II_I, V1, add_i32 as *const () as usize);
    assert_eq!(t.swap(Some(p), &pol(), true), BindState::Bound);
    assert_eq!(t.fault, Fault::None);
    assert_eq!(t.fp_required, t.fp_provided);
    assert_eq!(t.generation, 1);

    let f: AddFn = unsafe { core::mem::transmute(t.address().unwrap()) };
    assert_eq!(f(2, 3), 5, "the transcript's correct answer");
}

#[test]
fn the_transcript_break_faults_and_exposes_nothing() {
    let mut t = Trident::new("add", req_add());
    t.swap(Some(prov(&SIG_II_I, V1, add_i32 as *const () as usize)), &pol(), true);
    assert_eq!(t.state, BindState::Bound);

    // The `ln -sf` moment.
    let bad = prov(&SIG_DD_D, V2X, add_f64 as *const () as usize);
    assert_eq!(t.swap(Some(bad), &pol(), true), BindState::Fault);
    assert_eq!(t.fault, Fault::Fingerprint);
    assert_ne!(t.fp_required, t.fp_provided);

    assert!(t.address().is_none(), "no callable address for a faulted slot");
    assert_ne!(
        t.slot_addr, add_f64 as *const () as *mut core::ffi::c_void,
        "the wrongly-shaped function is never reachable through the slot"
    );
    assert_eq!(t.slot_addr, mmuko_kernel::trident::trap_address());
}

#[test]
fn the_trap_counts_a_caller_that_ignores_the_fault() {
    mmuko_kernel::trident::trap_reset();
    let mut t = Trident::new("add", req_add());
    t.swap(Some(prov(&SIG_DD_D, V2X, add_f64 as *const () as usize)), &pol(), true);

    let before = mmuko_kernel::trident::trap_count();
    let f: extern "C" fn() -> i32 = unsafe { core::mem::transmute(t.slot_addr) };
    let r = f();
    assert_eq!(mmuko_kernel::trident::trap_count(), before + 1);
    assert_eq!(r, mmuko_kernel::trident::TRAP_SENTINEL);
}

#[test]
fn promotion_cannot_repair_an_abi_break() {
    let mut t = Trident::new("add", req_add());
    t.swap(Some(prov(&SIG_DD_D, V2S, add_f64 as *const () as usize)), &pol(), true);

    // The most important ordering claim in the library. If this ever reported
    // Version or State, it would mean the version policy had been consulted
    // about a contract the machine already refused -- and a policy that can be
    // consulted can eventually be relaxed.
    assert_eq!(t.state, BindState::Fault);
    assert_eq!(
        t.fault,
        Fault::Fingerprint,
        "fact precedes policy: FINGERPRINT, never VERSION"
    );
    assert_eq!(t.generation, 0);
}

#[test]
fn every_structural_disagreement_is_named_separately() {
    let mut t = Trident::new("add", req_add());

    t.swap(None, &pol(), true);
    assert_eq!(t.fault, Fault::Missing);

    t.swap(Some(prov(&SIG_II_I_STD, V1, add_i32 as *const () as usize)), &pol(), true);
    assert_eq!(t.fault, Fault::CallConv, "convention, not shape");

    t.swap(
        Some(ExportDesc::provide("mul", &SIG_II_I, V1, mul_i32 as *const () as *mut _)),
        &pol(),
        true,
    );
    assert_eq!(
        t.fault,
        Fault::Fingerprint,
        "a correctly shaped function under the wrong name still faults"
    );

    t.swap(
        Some(ExportDesc::provide(
            "add",
            &SIG_II_I,
            V1,
            core::ptr::null_mut(),
        )),
        &pol(),
        true,
    );
    assert_eq!(t.fault, Fault::Malformed, "no address is malformed, not null-bound");

    let bad_arch = Policy::new(Arch::None, StateMask::STABLE);
    t.swap(Some(prov(&SIG_II_I, V1, add_i32 as *const () as usize)), &bad_arch, true);
    assert_eq!(t.fault, Fault::Arch);

    t.swap(Some(prov(&SIG_II_I, V1, add_i32 as *const () as usize)), &pol(), false);
    assert_eq!(t.fault, Fault::Upstream, "containment precedes the hooks");
}

#[test]
fn state_and_version_gates_apply_once_the_shape_agrees() {
    let beta = SemVerX::new(1, State::Stable, 2, State::Beta, 0, State::Stable);
    let mut t = Trident::new("add", req_add());

    t.swap(Some(prov(&SIG_II_I, beta, add_i32 as *const () as usize)), &pol(), true);
    assert_eq!(t.fault, Fault::State);
    assert_eq!(
        t.fp_required, t.fp_provided,
        "the shapes agree perfectly; only policy refused"
    );

    let testing = Policy::new(mmuko_kernel::NATIVE_ARCH, StateMask::TESTING);
    assert_eq!(t.resolve(&testing, true), BindState::Bound);
    assert_eq!(t.generation, 1);

    // A provider older than required, shape identical.
    let need_newer = ExportDesc::require("add", &SIG_II_I, SemVerX::uniform(1, 5, 0, State::Stable));
    let mut t2 = Trident::new("add", need_newer);
    t2.swap(Some(prov(&SIG_II_I, V1, add_i32 as *const () as usize)), &pol(), true);
    assert_eq!(t2.fault, Fault::Version);
}

#[test]
fn hot_swap_heals_without_a_restart() {
    let mut t = Trident::new("add", req_add());
    t.swap(Some(prov(&SIG_II_I, V1, add_i32 as *const () as usize)), &pol(), true);
    assert_eq!(t.generation, 1);

    t.swap(Some(prov(&SIG_DD_D, V2X, add_f64 as *const () as usize)), &pol(), true);
    assert_eq!(t.state, BindState::Fault);
    assert_eq!(t.generation, 1, "a fault is not a new binding");

    // The fix lands.
    let fixed = SemVerX::uniform(1, 1, 0, State::Stable);
    assert_eq!(
        t.swap(Some(prov(&SIG_II_I, fixed, add_i32 as *const () as usize)), &pol(), true),
        BindState::Bound
    );
    assert_eq!(
        t.generation, 2,
        "the generation advances so a cached pointer knows to re-read"
    );
    let f: AddFn = unsafe { core::mem::transmute(t.address().unwrap()) };
    assert_eq!(f(20, 22), 42);

    // Monotone: re-resolving an unchanged bound node must not churn.
    t.resolve(&pol(), true);
    t.resolve(&pol(), true);
    assert_eq!(t.generation, 2);
}

// --------------------------------------------------------------------------
// Registry
// --------------------------------------------------------------------------

#[test]
fn registry_registers_resolves_and_reports() {
    let mut reg: KernelRegistry<8> = KernelRegistry::new();
    let a = reg.register("add", req_add()).unwrap();
    let m = reg
        .register("mul", ExportDesc::require("mul", &SIG_II_I, V1))
        .unwrap();
    assert_eq!(reg.len(), 2);

    assert_eq!(
        reg.register("add", req_add()),
        Err(RegistryError::DuplicateSlot),
        "duplicate slots would make lookup order-dependent"
    );

    reg.offer(a, Some(prov(&SIG_II_I, V1, add_i32 as *const () as usize))).unwrap();
    reg.offer(
        m,
        Some(ExportDesc::provide("mul", &SIG_II_I, V1, mul_i32 as *const () as *mut _)),
    )
    .unwrap();

    let r = reg.resolve_all();
    assert!(r.is_complete());
    assert_eq!(r.bound, 2);
    assert!(reg.address("add").is_some());
    assert_eq!(reg.status("mul").unwrap().0, BindState::Bound);
    assert_eq!(reg.generation("add"), Some(1));
}

#[test]
fn registry_is_full_when_it_is_full() {
    let mut reg: KernelRegistry<2> = KernelRegistry::new();
    reg.register("a", req_add()).unwrap();
    reg.register("b", req_add()).unwrap();
    assert_eq!(reg.register("c", req_add()), Err(RegistryError::Full));
}

#[test]
fn fault_containment_is_transitive_and_reversible() {
    let mut reg: KernelRegistry<8> = KernelRegistry::new();
    let a = reg.register("A", req_add()).unwrap();
    let b = reg.register("B", req_add()).unwrap();
    let c = reg.register("C", req_add()).unwrap();
    reg.add_dependency(b, a).unwrap();
    reg.add_dependency(c, b).unwrap();

    let good = prov(&SIG_II_I, V1, add_i32 as *const () as usize);
    for i in [a, b, c] {
        reg.offer(i, Some(good)).unwrap();
    }
    assert!(reg.resolve_all().is_complete(), "a consistent chain binds end to end");

    // Break only the root.
    reg.offer(a, Some(prov(&SIG_DD_D, V2X, add_f64 as *const () as usize))).unwrap();
    let r = reg.resolve_all();
    assert_eq!(r.bound, 0);
    assert_eq!(r.faulted, 3, "breaking the root faults the whole chain");
    assert_eq!(reg.status("A").unwrap().1, Fault::Fingerprint, "root reports the cause");
    assert_eq!(reg.status("B").unwrap().1, Fault::Upstream);
    assert_eq!(
        reg.status("C").unwrap().1,
        Fault::Upstream,
        "no inconsistent contract reaches code two hops away"
    );
    assert!(reg.address("C").is_none());

    // Heal it. Containment must be as reversible as it is strict, or it is a
    // one-way ratchet that eventually forces the restart it was meant to avoid.
    reg.offer(a, Some(good)).unwrap();
    assert!(reg.resolve_all().is_complete());
    assert_eq!(reg.generation("C"), Some(2));
}

#[test]
fn dependency_cycles_are_refused_at_insertion() {
    let mut reg: KernelRegistry<4> = KernelRegistry::new();
    let a = reg.register("A", req_add()).unwrap();
    let b = reg.register("B", req_add()).unwrap();
    reg.add_dependency(b, a).unwrap();
    assert_eq!(
        reg.add_dependency(a, b),
        Err(RegistryError::CyclicDependency),
        "a cycle is named where it is created, not as a permanent silent fault"
    );
    assert_eq!(reg.add_dependency(a, a), Err(RegistryError::CyclicDependency));
}

#[test]
fn resolution_reaches_the_same_fixpoint_whatever_the_registration_order() {
    // C registered before B before A: the worst ordering for a single pass.
    let mut reg: KernelRegistry<8> = KernelRegistry::new();
    let c = reg.register("C", req_add()).unwrap();
    let b = reg.register("B", req_add()).unwrap();
    let a = reg.register("A", req_add()).unwrap();
    reg.add_dependency(b, a).unwrap();
    reg.add_dependency(c, b).unwrap();

    let good = prov(&SIG_II_I, V1, add_i32 as *const () as usize);
    for i in [a, b, c] {
        reg.offer(i, Some(good)).unwrap();
    }
    let r = reg.resolve_all();
    assert!(r.is_complete(), "a backwards-ordered chain still binds completely");
    assert!(r.passes >= 2, "and it took more than one pass to get there");
}

#[test]
fn offer_module_matches_by_symbol_name() {
    static EXPORTS: &[ExportDesc] = &[];
    let _ = EXPORTS;

    let exports: &'static [ExportDesc] = Box::leak(Box::new([
        ExportDesc::provide("add", &SIG_II_I, V1, add_i32 as *const () as *mut _),
        ExportDesc::provide("mul", &SIG_II_I, V1, mul_i32 as *const () as *mut _),
    ]));
    let module = ModuleDesc {
        name: "mathlib",
        arch: mmuko_kernel::NATIVE_ARCH,
        version: V1,
        exports,
    };

    let mut reg: KernelRegistry<8> = KernelRegistry::new();
    reg.register("add", req_add()).unwrap();
    reg.register("mul", ExportDesc::require("mul", &SIG_II_I, V1)).unwrap();
    reg.register("div", ExportDesc::require("div", &SIG_II_I, V1)).unwrap();

    assert_eq!(reg.offer_module(&module), 2, "two of three slots are satisfiable");
    let r = reg.resolve_all();
    assert_eq!(r.bound, 2);
    assert_eq!(r.faulted, 1);
    assert_eq!(
        reg.status("div").unwrap().1,
        Fault::Missing,
        "a slot the module does not export is MISSING, not malformed"
    );

    assert!(module.fingerprint().is_ok());
}

#[test]
fn module_fingerprint_is_order_independent_but_content_sensitive() {
    let ab: &'static [ExportDesc] = Box::leak(Box::new([
        ExportDesc::provide("add", &SIG_II_I, V1, add_i32 as *const () as *mut _),
        ExportDesc::provide("mul", &SIG_II_I, V1, mul_i32 as *const () as *mut _),
    ]));
    let ba: &'static [ExportDesc] = Box::leak(Box::new([
        ExportDesc::provide("mul", &SIG_II_I, V1, mul_i32 as *const () as *mut _),
        ExportDesc::provide("add", &SIG_II_I, V1, add_i32 as *const () as *mut _),
    ]));
    let changed: &'static [ExportDesc] = Box::leak(Box::new([
        ExportDesc::provide("add", &SIG_DD_D, V1, add_f64 as *const () as *mut _),
        ExportDesc::provide("mul", &SIG_II_I, V1, mul_i32 as *const () as *mut _),
    ]));

    let mk = |e: &'static [ExportDesc]| ModuleDesc {
        name: "mathlib",
        arch: mmuko_kernel::NATIVE_ARCH,
        version: V1,
        exports: e,
    };

    assert_eq!(
        mk(ab).fingerprint().unwrap(),
        mk(ba).fingerprint().unwrap(),
        "reordering a declaration list is not a breaking change"
    );
    assert_ne!(
        mk(ab).fingerprint().unwrap(),
        mk(changed).fingerprint().unwrap(),
        "changing a symbol's shape is"
    );
}
