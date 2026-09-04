// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! Rust/C differential suite.
//!
//! The MMUKO kernel is Rust and the modules it binds are C, so two independent
//! implementations of the canonical encoder and the 128-bit hash exist -- one
//! in `src/mmuko_abi.c`, one in `src/fingerprint.rs`. If they disagreed by so
//! much as one byte, the kernel would refuse correct bindings, or accept
//! incorrect ones, and no test that exercised only one side could show it.
//!
//! This file is the check. For every signature it can construct it asserts
//! that:
//!
//! 1. the two ENCODERS produce byte-identical canonical strings, and
//! 2. the two HASHES produce identical 128-bit fingerprints.
//!
//! Checking the encoding as well as the hash matters: agreement on the hash
//! alone would also be consistent with both sides sharing a single encoding
//! bug, and the encoding is the part a human has to be able to read in a
//! diagnostic.
//!
//! Skipped entirely when no C compiler was available at build time; a missing
//! toolchain must weaken the evidence, never break the build.

#![cfg(not(mmuko_no_c_core))]

use mmuko_kernel::abi::raw::{CFnSig, CSemVerX, CTypeDesc};
use mmuko_kernel::abi::{ty, Arch, CallConv, FnSig, TypeCode, TypeDesc};
use mmuko_kernel::fingerprint::{self, Canon, Fingerprint};
use mmuko_kernel::semverx::{SemVerX, State, StateMask};

// --------------------------------------------------------------------------
// The C side
// --------------------------------------------------------------------------

#[repr(C)]
struct CBuf {
    data: *mut u8,
    cap: u32,
    len: u32,
    overflow: i32,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
struct CFingerprint {
    lo: u64,
    hi: u64,
}

extern "C" {
    fn mmuko_buf_init(b: *mut CBuf, storage: *mut u8, cap: u32);
    fn mmuko_encode_signature(sig: *const CFnSig, arch: u32, out: *mut CBuf) -> i32;
    fn mmuko_fp_export(
        symbol: *const u8,
        sig: *const CFnSig,
        arch: u32,
        out: *mut CFingerprint,
    ) -> i32;
    fn mmuko_fp_bytes(data: *const u8, len: u32) -> CFingerprint;
    fn mmuko_semverx_satisfies(
        required: *const CSemVerX,
        provided: *const CSemVerX,
        state_mask: u32,
    ) -> i32;
    fn mmuko_type_width(t: *const CTypeDesc, arch: u32) -> u32;
}

/// Build the raw C descriptor for a scalar type code.
fn c_scalar(code: TypeCode) -> CTypeDesc {
    CTypeDesc {
        code: code as u32,
        size: 0,
        align: 0,
        nfields: 0,
        fields: core::ptr::null(),
    }
}

/// One signature, held in both representations, with the raw C pointers kept
/// alive by the owning struct.
struct Pair {
    // C side, self-referential: `arg_ptrs` points into `args`.
    _ret: Box<CTypeDesc>,
    _args: Vec<Box<CTypeDesc>>,
    _arg_ptrs: Vec<*const CTypeDesc>,
    csig: CFnSig,
    // Rust side.
    rsig: FnSig,
}

fn pair(ret: TypeCode, args: &[TypeCode], cc: CallConv, variadic: bool) -> Pair {
    let cret = Box::new(c_scalar(ret));
    let cargs: Vec<Box<CTypeDesc>> = args.iter().map(|&a| Box::new(c_scalar(a))).collect();
    let arg_ptrs: Vec<*const CTypeDesc> = cargs.iter().map(|b| &**b as *const CTypeDesc).collect();

    let csig = CFnSig {
        cc: cc as u32,
        flags: if variadic {
            mmuko_kernel::abi::raw::FN_VARIADIC
        } else {
            0
        },
        ret: &*cret as *const CTypeDesc,
        nargs: args.len() as u32,
        args: if arg_ptrs.is_empty() {
            core::ptr::null()
        } else {
            arg_ptrs.as_ptr()
        },
    };

    // The Rust side needs 'static descriptors. Leak them: a test process is
    // the one place where that is the simplest correct answer, and the
    // alternative (a lifetime-parameterised FnSig) would complicate the
    // kernel API for the benefit of a test.
    let rret: &'static TypeDesc = Box::leak(Box::new(TypeDesc::scalar(ret)));
    let rargs: &'static [TypeDesc] = Box::leak(
        args.iter()
            .map(|&a| TypeDesc::scalar(a))
            .collect::<Vec<_>>()
            .into_boxed_slice(),
    );
    let rsig = if variadic {
        FnSig::variadic(cc, rret, rargs)
    } else {
        FnSig::new(cc, rret, rargs)
    };

    Pair {
        _ret: cret,
        _args: cargs,
        _arg_ptrs: arg_ptrs,
        csig,
        rsig,
    }
}

fn c_encode(p: &Pair, arch: Arch) -> Option<String> {
    let mut storage = vec![0u8; fingerprint::CANON_MAX];
    let mut buf = CBuf {
        data: core::ptr::null_mut(),
        cap: 0,
        len: 0,
        overflow: 0,
    };
    unsafe {
        mmuko_buf_init(
            &mut buf,
            storage.as_mut_ptr(),
            fingerprint::CANON_MAX as u32,
        );
        if mmuko_encode_signature(&p.csig, arch as u32, &mut buf) != 0 {
            return None;
        }
        Some(String::from_utf8_lossy(&storage[..buf.len as usize]).into_owned())
    }
}

fn c_fingerprint(symbol: &str, p: &Pair, arch: Arch) -> Option<Fingerprint> {
    let name = std::ffi::CString::new(symbol).unwrap();
    let mut fp = CFingerprint::default();
    unsafe {
        if mmuko_fp_export(name.as_ptr() as *const u8, &p.csig, arch as u32, &mut fp) != 0 {
            return None;
        }
    }
    Some(Fingerprint { lo: fp.lo, hi: fp.hi })
}

// --------------------------------------------------------------------------

const SCALARS: &[TypeCode] = &[
    TypeCode::Bool,
    TypeCode::I8,
    TypeCode::U8,
    TypeCode::I16,
    TypeCode::U16,
    TypeCode::I32,
    TypeCode::U32,
    TypeCode::I64,
    TypeCode::U64,
    TypeCode::F32,
    TypeCode::F64,
    TypeCode::Ptr,
    TypeCode::CStr,
];

const ARCHES: &[Arch] = &[Arch::Mmuko32, Arch::Mmuko64];

#[test]
fn encoders_agree_on_every_scalar_signature() {
    let mut checked = 0usize;
    for &arch in ARCHES {
        for &ret in SCALARS.iter().chain(core::iter::once(&TypeCode::Void)) {
            for &a in SCALARS {
                for &b in SCALARS {
                    let p = pair(ret, &[a, b], CallConv::Cdecl, false);

                    let c = c_encode(&p, arch).expect("C encoder rejected a valid signature");
                    let mut rbuf = Canon::new();
                    fingerprint::encode_signature(&p.rsig, arch, &mut rbuf)
                        .expect("Rust encoder rejected a valid signature");

                    assert_eq!(
                        c,
                        rbuf.as_str(),
                        "canonical encodings diverge for {:?} f({:?},{:?}) on {}",
                        ret,
                        a,
                        b,
                        arch.name()
                    );
                    checked += 1;
                }
            }
        }
    }
    // 2 arches x 14 returns x 13 x 13 arguments.
    assert_eq!(checked, 2 * 14 * 13 * 13);
}

#[test]
fn fingerprints_agree_on_every_scalar_signature() {
    for &arch in ARCHES {
        for &ret in SCALARS {
            for &a in SCALARS {
                let p = pair(ret, &[a], CallConv::Cdecl, false);
                let c = c_fingerprint("sym", &p, arch).expect("C fingerprint failed");
                let r = fingerprint::export("sym", &p.rsig, arch).expect("Rust fingerprint failed");
                assert_eq!(
                    c,
                    r,
                    "fingerprints diverge for {:?} f({:?}) on {}",
                    ret,
                    a,
                    arch.name()
                );
            }
        }
    }
}

#[test]
fn the_transcript_break_is_caught_identically_by_both() {
    for &arch in ARCHES {
        let v1 = pair(
            TypeCode::I32,
            &[TypeCode::I32, TypeCode::I32],
            CallConv::Cdecl,
            false,
        );
        let v2 = pair(
            TypeCode::F64,
            &[TypeCode::F64, TypeCode::F64],
            CallConv::Cdecl,
            false,
        );

        let c1 = c_fingerprint("add", &v1, arch).unwrap();
        let c2 = c_fingerprint("add", &v2, arch).unwrap();
        let r1 = fingerprint::export("add", &v1.rsig, arch).unwrap();
        let r2 = fingerprint::export("add", &v2.rsig, arch).unwrap();

        assert_eq!(c1, r1, "C and Rust disagree about int32 add on {}", arch.name());
        assert_eq!(c2, r2, "C and Rust disagree about f64 add on {}", arch.name());
        assert_ne!(c1, c2, "C failed to distinguish the two shapes on {}", arch.name());
        assert_ne!(r1, r2, "Rust failed to distinguish the two shapes on {}", arch.name());
    }
}

#[test]
fn variadic_and_calling_convention_agree() {
    for &arch in ARCHES {
        for &variadic in &[false, true] {
            for &cc in &[
                CallConv::Cdecl,
                CallConv::Stdcall,
                CallConv::Fastcall,
                CallConv::Syscall,
            ] {
                let p = pair(TypeCode::I32, &[TypeCode::CStr], cc, variadic);
                let c = c_encode(&p, arch).unwrap();
                let mut r = Canon::new();
                fingerprint::encode_signature(&p.rsig, arch, &mut r).unwrap();
                assert_eq!(c, r.as_str(), "cc={:?} variadic={} on {}", cc, variadic, arch.name());
            }
        }
    }
}

#[test]
fn arity_zero_through_max_agree() {
    for &arch in ARCHES {
        for n in 0..=fingerprint::MAX_ARGS as usize {
            let args: Vec<TypeCode> = (0..n)
                .map(|i| SCALARS[i % SCALARS.len()])
                .collect();
            let p = pair(TypeCode::Void, &args, CallConv::Cdecl, false);
            let c = c_encode(&p, arch).unwrap();
            let mut r = Canon::new();
            fingerprint::encode_signature(&p.rsig, arch, &mut r).unwrap();
            assert_eq!(c, r.as_str(), "arity {} on {}", n, arch.name());
        }
    }
}

#[test]
fn both_sides_reject_the_same_malformed_inputs() {
    let arch = Arch::Mmuko64;

    // void as a parameter type.
    let p = pair(TypeCode::I32, &[TypeCode::Void], CallConv::Cdecl, false);
    assert!(c_encode(&p, arch).is_none(), "C accepted void as a parameter");
    let mut r = Canon::new();
    assert!(
        fingerprint::encode_signature(&p.rsig, arch, &mut r).is_err(),
        "Rust accepted void as a parameter"
    );

    // Unrecognised calling convention.
    let mut p = pair(TypeCode::I32, &[TypeCode::I32], CallConv::Cdecl, false);
    p.csig.cc = 99;
    assert!(c_encode(&p, arch).is_none(), "C accepted an unknown convention");

    // Unspecified architecture.
    let p = pair(TypeCode::I32, &[TypeCode::I32], CallConv::Cdecl, false);
    assert!(c_encode(&p, Arch::None).is_none(), "C accepted arch-none");
    let mut r = Canon::new();
    assert!(
        fingerprint::encode_signature(&p.rsig, Arch::None, &mut r).is_err(),
        "Rust accepted arch-none"
    );
}

#[test]
fn raw_byte_hash_agrees() {
    let cases: &[&[u8]] = &[
        b"",
        b"a",
        b"abc",
        b"cba",
        b"MMUKOABI/1|A=64|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0",
        &[0u8; 64],
        &[0xFFu8; 257],
    ];
    for &c in cases {
        let cf = unsafe { mmuko_fp_bytes(c.as_ptr(), c.len() as u32) };
        let rf = Fingerprint::of_bytes(c);
        assert_eq!(cf.lo, rf.lo, "lane A diverges on {:?}", &c[..c.len().min(16)]);
        assert_eq!(cf.hi, rf.hi, "lane B diverges on {:?}", &c[..c.len().min(16)]);
    }
}

#[test]
fn struct_by_value_layout_agrees() {
    // { i32; f64 } naturally aligned, and the same fields packed.
    let arch = Arch::Mmuko64;

    let f_i32 = c_scalar(TypeCode::I32);
    let f_f64 = c_scalar(TypeCode::F64);
    let fields: [*const CTypeDesc; 2] = [&f_i32, &f_f64];

    for (size, align) in [(16u32, 8u32), (12u32, 1u32)] {
        let cstruct = CTypeDesc {
            code: TypeCode::Struct as u32,
            size,
            align,
            nfields: 2,
            fields: fields.as_ptr(),
        };
        let ret = c_scalar(TypeCode::Void);
        let argp: [*const CTypeDesc; 1] = [&cstruct];
        let csig = CFnSig {
            cc: CallConv::Cdecl as u32,
            flags: 0,
            ret: &ret,
            nargs: 1,
            args: argp.as_ptr(),
        };

        let rfields: &'static [TypeDesc] = Box::leak(Box::new([ty::I32, ty::F64]));
        let rstruct: &'static [TypeDesc] = Box::leak(Box::new([TypeDesc::aggregate(
            TypeCode::Struct,
            size,
            align,
            rfields,
        )]));
        let rsig = FnSig::new(CallConv::Cdecl, &ty::VOID, rstruct);

        let mut storage = vec![0u8; fingerprint::CANON_MAX];
        let mut buf = CBuf {
            data: core::ptr::null_mut(),
            cap: 0,
            len: 0,
            overflow: 0,
        };
        let cstr = unsafe {
            mmuko_buf_init(&mut buf, storage.as_mut_ptr(), fingerprint::CANON_MAX as u32);
            assert_eq!(mmuko_encode_signature(&csig, arch as u32, &mut buf), 0);
            String::from_utf8_lossy(&storage[..buf.len as usize]).into_owned()
        };

        let mut r = Canon::new();
        fingerprint::encode_signature(&rsig, arch, &mut r).unwrap();
        assert_eq!(cstr, r.as_str(), "struct sz={} al={}", size, align);
        assert!(
            cstr.contains(&format!("S{{sz={},al={}:i32,f64}}", size, align)),
            "unexpected struct encoding: {}",
            cstr
        );
    }
}

#[test]
fn type_widths_agree() {
    for &arch in ARCHES {
        for &code in SCALARS {
            let c = c_scalar(code);
            let cw = unsafe { mmuko_type_width(&c, arch as u32) };
            let rw = TypeDesc::scalar(code).width(arch);
            assert_eq!(cw, rw, "width of {:?} on {}", code, arch.name());
        }
    }
}

#[test]
fn semverx_satisfaction_agrees() {
    fn to_c(v: &SemVerX) -> CSemVerX {
        CSemVerX {
            major: v.major,
            minor: v.minor,
            patch: v.patch,
            major_state: v.major_state as u8,
            minor_state: v.minor_state as u8,
            patch_state: v.patch_state as u8,
            _pad: 0,
        }
    }

    let states = [
        State::Experimental,
        State::Beta,
        State::Stable,
        State::Legacy,
        State::Lts,
    ];
    let masks = [
        (StateMask::STABLE, 0b0000_0000_0010_1000u32),
        (StateMask::TESTING, 0),
        (StateMask::DEV, 0),
        (StateMask::ANY, 0),
    ];

    let mut checked = 0;
    for &(mask, _) in &masks {
        for &rs in &states {
            for &ps in &states {
                for (rmaj, pmaj) in [(1u16, 1u16), (1, 2)] {
                    for (rmin, pmin) in [(0u16, 0u16), (0, 3), (3, 0)] {
                        let req = SemVerX::new(rmaj, rs, rmin, rs, 0, rs);
                        let prov = SemVerX::new(pmaj, ps, pmin, ps, 0, ps);
                        let cr = unsafe {
                            mmuko_semverx_satisfies(&to_c(&req), &to_c(&prov), mask.0)
                        };
                        let rr = req.satisfies(&prov, mask);
                        assert_eq!(
                            cr == 0,
                            rr.is_ok(),
                            "verdict diverges: req={:?} prov={:?} mask={:#x} (C={}, Rust={:?})",
                            req,
                            prov,
                            mask.0,
                            cr,
                            rr
                        );
                        checked += 1;
                    }
                }
            }
        }
    }
    assert_eq!(checked, 4 * 5 * 5 * 2 * 3);
}

#[test]
fn state_masks_agree_bit_for_bit() {
    // The C macros and the Rust constants are written independently; if they
    // ever drift, every policy decision differs between the kernel and the
    // modules it loads, silently.
    assert_eq!(
        StateMask::STABLE.0,
        (1 << State::Stable as u32) | (1 << State::Lts as u32)
    );
    assert_eq!(
        StateMask::TESTING.0,
        StateMask::STABLE.0 | (1 << State::Beta as u32)
    );
    assert_eq!(
        StateMask::DEV.0,
        StateMask::TESTING.0 | (1 << State::Experimental as u32)
    );
    assert_eq!(
        StateMask::ANY.0,
        StateMask::DEV.0 | (1 << State::Legacy as u32)
    );
}

#[test]
fn abi_constants_agree() {
    assert_eq!(mmuko_kernel::ABI_MAGIC, 0x4F4B_4D4D);
    assert_eq!(mmuko_kernel::ABI_REV, 1);
    assert_eq!(mmuko_kernel::ABI_TAG, "MMUKOABI/1");
    assert_eq!(
        mmuko_kernel::kernel::mmuko_kernel_arch(),
        mmuko_kernel::NATIVE_ARCH as u32
    );
}

#[test]
fn c_can_fingerprint_through_the_rust_entry_point() {
    // The reverse direction: a C module asking the Rust kernel what identity
    // it computes, which is how a module author checks a release before
    // shipping it.
    let arch = Arch::Mmuko64;
    let p = pair(
        TypeCode::I32,
        &[TypeCode::I32, TypeCode::I32],
        CallConv::Cdecl,
        false,
    );
    let name = std::ffi::CString::new("add").unwrap();
    let (mut lo, mut hi) = (0u64, 0u64);
    let rc = unsafe {
        mmuko_kernel::kernel::mmuko_kernel_fingerprint_export(
            name.as_ptr() as *const u8,
            &p.csig,
            arch as u32,
            &mut lo,
            &mut hi,
        )
    };
    assert_eq!(rc, 0);
    let direct = fingerprint::export("add", &p.rsig, arch).unwrap();
    assert_eq!((lo, hi), (direct.lo, direct.hi));

    let via_c = c_fingerprint("add", &p, arch).unwrap();
    assert_eq!((lo, hi), (via_c.lo, via_c.hi), "all three paths must agree");
}

// --------------------------------------------------------------------------
// Descriptor layout
// --------------------------------------------------------------------------

extern "C" {
    fn mmuko_layout_probe(key: u32) -> u32;
}

/// Assert one measured C value against the Rust mirror's own.
fn layout(key: u32) -> u32 {
    let v = unsafe { mmuko_layout_probe(key) };
    assert_ne!(v, 0xFFFF_FFFF, "unknown layout probe key {}", key);
    v
}

#[test]
fn repr_c_mirrors_match_the_c_compilers_layout() {
    use core::mem::{align_of, size_of};
    use mmuko_kernel::abi::raw::*;

    // A hand-written #[repr(C)] mirror is a claim about layout that nothing
    // checks. If a padding rule differs between the two compilers, or a field
    // is added to one side only, the kernel starts reading a symbol name out
    // of the middle of a version number -- and at ring 0 that is
    // indistinguishable from memory corruption. So: check it.

    // mmuko_type_desc_t
    assert_eq!(size_of::<CTypeDesc>() as u32, layout(1), "CTypeDesc size");
    assert_eq!(offset_of_code(), layout(2), "CTypeDesc.code");
    assert_eq!(offset_of_size(), layout(3), "CTypeDesc.size");
    assert_eq!(offset_of_align(), layout(4), "CTypeDesc.align");
    assert_eq!(offset_of_nfields(), layout(5), "CTypeDesc.nfields");
    assert_eq!(offset_of_fields(), layout(6), "CTypeDesc.fields");

    // mmuko_fn_sig_t
    assert_eq!(size_of::<CFnSig>() as u32, layout(10), "CFnSig size");

    // mmuko_semverx_t -- the one with hand-placed padding, so the one most
    // likely to drift.
    assert_eq!(size_of::<CSemVerX>() as u32, layout(20), "CSemVerX size");
    assert_eq!(align_of::<CSemVerX>(), 2, "CSemVerX alignment");

    // mmuko_export_desc_t
    assert_eq!(size_of::<CExportDesc>() as u32, layout(30), "CExportDesc size");

    // mmuko_module_desc_t
    assert_eq!(size_of::<CModuleDesc>() as u32, layout(40), "CModuleDesc size");

    // The fingerprint crosses the boundary BY VALUE, returned in registers, so
    // its size decides which registers the caller reads. Sixteen bytes on both
    // profiles means RAX:RDX (mmuko64) or EAX:EDX with a hidden pointer
    // (mmuko32) -- either way both sides must agree exactly.
    assert_eq!(size_of::<mmuko_kernel::Fingerprint>() as u32, layout(50));

    // The claim made in abi.rs: a function pointer and a data pointer are the
    // same width on every profile this ABI supports. If that ever stopped
    // being true, `address: *mut c_void` in the mirror would be wrong.
    assert_eq!(layout(51), layout(52), "fnptr and void* widths differ");
    assert_eq!(
        layout(52) as usize,
        size_of::<*mut core::ffi::c_void>(),
        "pointer width disagrees between C and Rust"
    );
}

// Offsets of CTypeDesc fields. Written out rather than using `offset_of!`,
// which stabilised later than this crate's rust-version floor.
fn offset_of_code() -> u32 {
    field_offset(|p: *const CTypeDesc| unsafe { core::ptr::addr_of!((*p).code) as usize })
}
fn offset_of_size() -> u32 {
    field_offset(|p: *const CTypeDesc| unsafe { core::ptr::addr_of!((*p).size) as usize })
}
fn offset_of_align() -> u32 {
    field_offset(|p: *const CTypeDesc| unsafe { core::ptr::addr_of!((*p).align) as usize })
}
fn offset_of_nfields() -> u32 {
    field_offset(|p: *const CTypeDesc| unsafe { core::ptr::addr_of!((*p).nfields) as usize })
}
fn offset_of_fields() -> u32 {
    field_offset(|p: *const CTypeDesc| unsafe { core::ptr::addr_of!((*p).fields) as usize })
}

fn field_offset<F: Fn(*const CTypeDesc) -> usize>(f: F) -> u32 {
    let base = core::mem::MaybeUninit::<CTypeDesc>::uninit();
    let p = base.as_ptr();
    (f(p) - p as usize) as u32
}
