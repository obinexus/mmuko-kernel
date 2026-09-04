// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! Core ABI vocabulary, and the `#[repr(C)]` mirrors of the C descriptors.
//!
//! Two layers live here, and the distinction matters:
//!
//! - The **safe Rust** types ([`TypeDesc`], [`FnSig`], [`ExportDesc`]) use
//!   slices and `&str`. Kernel code constructs these in `const` context and
//!   they carry Rust's lifetimes and bounds.
//! - The **raw** types ([`raw::CTypeDesc`] and friends) are byte-identical to
//!   the structures in `include/mmuko/`. They exist only at the boundary where
//!   a C module hands the kernel a table, and every field is validated before
//!   anything reads through a pointer in one.
//!
//! Keeping them apart means the resolver's own logic never touches a raw
//! pointer, and the unsafe surface is a single conversion function that can be
//! read in one sitting.

/// Architecture profile.
///
/// The two profiles are disjoint. An `Mmuko32` descriptor can never satisfy an
/// `Mmuko64` call site, because the profile tag is an input to every
/// fingerprint -- so the fault is caught by comparison rather than trusted to
/// convention.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Arch {
    /// Unspecified. Never valid for binding.
    None = 0,
    /// ILP32. `void*` is 4 bytes. SysV i386 cdecl.
    Mmuko32 = 32,
    /// LP64. `void*` is 8 bytes. SysV AMD64.
    Mmuko64 = 64,
}

impl Arch {
    /// The profile's name, as it appears in diagnostics.
    pub const fn name(self) -> &'static str {
        match self {
            Arch::Mmuko32 => "mmuko32",
            Arch::Mmuko64 => "mmuko64",
            Arch::None => "arch-none",
        }
    }

    /// Pointer width in bytes, or 0 for [`Arch::None`].
    pub const fn pointer_width(self) -> u32 {
        match self {
            Arch::Mmuko32 => 4,
            Arch::Mmuko64 => 8,
            Arch::None => 0,
        }
    }

    /// Convert from the raw `u32` a C table carries. Unknown values map to
    /// [`Arch::None`] rather than being trusted.
    pub const fn from_raw(v: u32) -> Arch {
        match v {
            32 => Arch::Mmuko32,
            64 => Arch::Mmuko64,
            _ => Arch::None,
        }
    }
}

/// Calling convention.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum CallConv {
    /// Not a recognised convention. Never encodable.
    Invalid = 0,
    /// The standard C convention for the profile: SysV i386 cdecl on
    /// `mmuko32`, SysV AMD64 on `mmuko64`.
    Cdecl = 1,
    /// Callee cleans the argument area.
    Stdcall = 2,
    /// Register-first, platform defined.
    Fastcall = 3,
    /// MMUKO kernel trap gate. Reserved.
    Syscall = 4,
}

impl CallConv {
    /// The token used in the canonical encoding.
    pub const fn name(self) -> &'static str {
        match self {
            CallConv::Cdecl => "cdecl",
            CallConv::Stdcall => "stdcall",
            CallConv::Fastcall => "fastcall",
            CallConv::Syscall => "syscall",
            CallConv::Invalid => "cc-invalid",
        }
    }

    /// Convert from the raw `u32` a C table carries.
    pub const fn from_raw(v: u32) -> CallConv {
        match v {
            1 => CallConv::Cdecl,
            2 => CallConv::Stdcall,
            3 => CallConv::Fastcall,
            4 => CallConv::Syscall,
            _ => CallConv::Invalid,
        }
    }
}

/// Type codes.
///
/// Every scalar carries an explicit bit width. There is deliberately no `Int`
/// and no `Long`: `long` is 32 bits under Windows LLP64 and 64 under SysV
/// LP64, so a vocabulary that could name it would let one fingerprint describe
/// two machine contracts. Removing the width-ambiguous types removes that
/// fault class structurally rather than by review.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
#[allow(missing_docs)]
pub enum TypeCode {
    Void = 0x00,
    Bool = 0x01,
    I8 = 0x10,
    U8 = 0x11,
    I16 = 0x12,
    U16 = 0x13,
    I32 = 0x14,
    U32 = 0x15,
    I64 = 0x16,
    U64 = 0x17,
    F32 = 0x20,
    F64 = 0x21,
    /// Opaque `void*`. Width follows the profile.
    Ptr = 0x30,
    /// `const char*`, NUL terminated.
    CStr = 0x31,
    /// Passed by value. Requires size, alignment and fields.
    Struct = 0x40,
    /// Passed by value. Requires size, alignment and fields.
    Union = 0x41,
    /// Requires size; underlying type in `fields[0]`.
    Enum = 0x42,
}

impl TypeCode {
    /// Convert from the raw `u32` a C table carries. Returns `None` for an
    /// unrecognised code rather than inventing a meaning for it.
    pub const fn from_raw(v: u32) -> Option<TypeCode> {
        Some(match v {
            0x00 => TypeCode::Void,
            0x01 => TypeCode::Bool,
            0x10 => TypeCode::I8,
            0x11 => TypeCode::U8,
            0x12 => TypeCode::I16,
            0x13 => TypeCode::U16,
            0x14 => TypeCode::I32,
            0x15 => TypeCode::U32,
            0x16 => TypeCode::I64,
            0x17 => TypeCode::U64,
            0x20 => TypeCode::F32,
            0x21 => TypeCode::F64,
            0x30 => TypeCode::Ptr,
            0x31 => TypeCode::CStr,
            0x40 => TypeCode::Struct,
            0x41 => TypeCode::Union,
            0x42 => TypeCode::Enum,
            _ => return None,
        })
    }

    /// True for the aggregate kinds, which require measured size and align.
    pub const fn is_aggregate(self) -> bool {
        matches!(self, TypeCode::Struct | TypeCode::Union | TypeCode::Enum)
    }
}

/// A type descriptor.
///
/// Scalars need only `code`. Aggregates must give the size and alignment
/// measured by the providing compiler on the providing profile, plus the
/// ordered field list.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TypeDesc {
    /// The type's code.
    pub code: TypeCode,
    /// Size in bytes. Zero for scalars, where the code implies it.
    pub size: u32,
    /// Alignment in bytes. Zero for scalars.
    pub align: u32,
    /// Ordered fields, for aggregates only.
    pub fields: &'static [TypeDesc],
}

impl TypeDesc {
    /// A scalar descriptor.
    pub const fn scalar(code: TypeCode) -> TypeDesc {
        TypeDesc {
            code,
            size: 0,
            align: 0,
            fields: &[],
        }
    }

    /// An aggregate descriptor with measured layout.
    pub const fn aggregate(
        code: TypeCode,
        size: u32,
        align: u32,
        fields: &'static [TypeDesc],
    ) -> TypeDesc {
        TypeDesc {
            code,
            size,
            align,
            fields,
        }
    }

    /// Storage width under a profile. Zero if malformed.
    pub const fn width(&self, arch: Arch) -> u32 {
        match self.code {
            TypeCode::Void => 0,
            TypeCode::Bool | TypeCode::I8 | TypeCode::U8 => 1,
            TypeCode::I16 | TypeCode::U16 => 2,
            TypeCode::I32 | TypeCode::U32 | TypeCode::F32 => 4,
            TypeCode::I64 | TypeCode::U64 | TypeCode::F64 => 8,
            // The one width that depends on the profile -- which is exactly
            // why the profile tag is an unconditional fingerprint input.
            TypeCode::Ptr | TypeCode::CStr => arch.pointer_width(),
            TypeCode::Struct | TypeCode::Union | TypeCode::Enum => self.size,
        }
    }
}

/// Canonical scalar descriptors.
#[allow(missing_docs)]
pub mod ty {
    use super::{TypeCode, TypeDesc};
    pub const VOID: TypeDesc = TypeDesc::scalar(TypeCode::Void);
    pub const BOOL: TypeDesc = TypeDesc::scalar(TypeCode::Bool);
    pub const I8: TypeDesc = TypeDesc::scalar(TypeCode::I8);
    pub const U8: TypeDesc = TypeDesc::scalar(TypeCode::U8);
    pub const I16: TypeDesc = TypeDesc::scalar(TypeCode::I16);
    pub const U16: TypeDesc = TypeDesc::scalar(TypeCode::U16);
    pub const I32: TypeDesc = TypeDesc::scalar(TypeCode::I32);
    pub const U32: TypeDesc = TypeDesc::scalar(TypeCode::U32);
    pub const I64: TypeDesc = TypeDesc::scalar(TypeCode::I64);
    pub const U64: TypeDesc = TypeDesc::scalar(TypeCode::U64);
    pub const F32: TypeDesc = TypeDesc::scalar(TypeCode::F32);
    pub const F64: TypeDesc = TypeDesc::scalar(TypeCode::F64);
    pub const PTR: TypeDesc = TypeDesc::scalar(TypeCode::Ptr);
    pub const CSTR: TypeDesc = TypeDesc::scalar(TypeCode::CStr);
}

/// A function signature: the complete calling contract, minus the name.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FnSig {
    /// Calling convention.
    pub cc: CallConv,
    /// Return type.
    pub ret: &'static TypeDesc,
    /// Parameter types, in order. Order is contract: it decides which register
    /// each value lands in.
    pub args: &'static [TypeDesc],
    /// Whether the callee is variadic. Changes the register-save protocol, so
    /// it is part of the contract and not a documentation note.
    pub variadic: bool,
}

impl FnSig {
    /// A non-variadic signature.
    pub const fn new(cc: CallConv, ret: &'static TypeDesc, args: &'static [TypeDesc]) -> FnSig {
        FnSig {
            cc,
            ret,
            args,
            variadic: false,
        }
    }

    /// A variadic signature. `args` describes the fixed prefix.
    pub const fn variadic(
        cc: CallConv,
        ret: &'static TypeDesc,
        args: &'static [TypeDesc],
    ) -> FnSig {
        FnSig {
            cc,
            ret,
            args,
            variadic: true,
        }
    }
}

/// One named export or one requirement.
///
/// The same structure serves both hooks of a trident. On the required side
/// (`u1`) `address` is `None`: a requirement is a claim about shape, not about
/// where anything lives. On the provided side (`u2`) it carries the address
/// the loader resolved.
#[derive(Debug, Clone, Copy)]
pub struct ExportDesc {
    /// Linker symbol name.
    pub symbol: &'static str,
    /// The calling contract.
    pub sig: &'static FnSig,
    /// The version at which this exact SHAPE first appeared -- not the version
    /// of the object carrying it. A module may be at 1.stable.7 and still
    /// declare `since = 1.stable.0` for a symbol whose contract has not moved,
    /// which is what lets an old caller bind to a new object.
    pub since: crate::semverx::SemVerX,
    /// The provider's address, or `None` on the required side.
    pub address: Option<core::ptr::NonNull<core::ffi::c_void>>,
}

// A descriptor is shared, immutable data. It is Send + Sync because nothing
// reachable from it is ever mutated after construction; the address, when
// present, is code, not state.
unsafe impl Send for ExportDesc {}
unsafe impl Sync for ExportDesc {}

impl ExportDesc {
    /// A requirement: what the caller was compiled against.
    pub const fn require(
        symbol: &'static str,
        sig: &'static FnSig,
        since: crate::semverx::SemVerX,
    ) -> ExportDesc {
        ExportDesc {
            symbol,
            sig,
            since,
            address: None,
        }
    }

    /// A provision: what an object actually exports, and where it is.
    pub fn provide(
        symbol: &'static str,
        sig: &'static FnSig,
        since: crate::semverx::SemVerX,
        address: *mut core::ffi::c_void,
    ) -> ExportDesc {
        ExportDesc {
            symbol,
            sig,
            since,
            address: core::ptr::NonNull::new(address),
        }
    }

    /// The export's 128-bit identity under a profile.
    pub fn fingerprint(
        &self,
        arch: Arch,
    ) -> Result<crate::fingerprint::Fingerprint, crate::fingerprint::EncodeError> {
        crate::fingerprint::export(self.symbol, self.sig, arch)
    }
}

/// A module's self-description.
#[derive(Debug, Clone, Copy)]
pub struct ModuleDesc {
    /// Module name.
    pub name: &'static str,
    /// The profile the object was built for.
    pub arch: Arch,
    /// The release's version.
    pub version: crate::semverx::SemVerX,
    /// Everything the object exports.
    pub exports: &'static [ExportDesc],
}

impl ModuleDesc {
    /// Find an export by symbol name.
    pub fn find(&self, symbol: &str) -> Option<&ExportDesc> {
        self.exports.iter().find(|e| e.symbol == symbol)
    }

    /// The module's identity: an order-independent XOR fold of its export
    /// fingerprints, mixed with the module name, profile and version.
    ///
    /// Order independence is deliberate. Reordering a declaration list changes
    /// nothing any caller can observe, so it must not read as a breaking
    /// change -- a sequential hash would make a cosmetic reshuffle look like
    /// one and train everybody to ignore the signal.
    pub fn fingerprint(&self) -> Result<crate::fingerprint::Fingerprint, crate::fingerprint::EncodeError> {
        use crate::fingerprint::{Canon, Fingerprint};
        let mut c = Canon::new();
        c_put_header(&mut c, self)?;
        let head = Fingerprint::of_bytes(c.as_bytes());

        let mut fold = Fingerprint::ZERO;
        for e in self.exports {
            let f = e.fingerprint(self.arch)?;
            fold.lo ^= f.lo;
            fold.hi ^= f.hi;
        }
        Ok(Fingerprint {
            lo: head.lo ^ fold.lo,
            hi: head.hi ^ fold.hi,
        })
    }
}

/// Mirrors the header half of `mmuko_desc_fingerprint()` in
/// `src/mmuko_desc.c`, byte for byte.
fn c_put_header(
    c: &mut crate::fingerprint::Canon,
    m: &ModuleDesc,
) -> Result<(), crate::fingerprint::EncodeError> {
    c.put("MOD=");
    c.put(m.name);
    c.put("|A=");
    c.put_u64(m.arch as u32 as u64);
    c.put("|V=");
    m.version.encode(c)?;
    Ok(())
}

/// Raw `#[repr(C)]` mirrors of the structures in `include/mmuko/`.
///
/// These exist only at the FFI boundary. Nothing in the resolver reads them
/// directly: [`super::kernel`] validates a raw table and converts it into the
/// safe types above before any decision is made from it.
pub mod raw {
    use core::ffi::c_void;

    /// Mirror of `mmuko_type_desc_t`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct CTypeDesc {
        /// `mmuko_typecode_t`
        pub code: u32,
        /// Bytes; 0 for scalars.
        pub size: u32,
        /// Bytes; 0 for scalars.
        pub align: u32,
        /// Field count, aggregates only.
        pub nfields: u32,
        /// `nfields` pointers to field descriptors.
        pub fields: *const *const CTypeDesc,
    }

    /// Mirror of `mmuko_fn_sig_t`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct CFnSig {
        /// `mmuko_cc_t`
        pub cc: u32,
        /// `MMUKO_FN_*`
        pub flags: u32,
        /// Return type.
        pub ret: *const CTypeDesc,
        /// Parameter count.
        pub nargs: u32,
        /// `nargs` pointers to parameter descriptors.
        pub args: *const *const CTypeDesc,
    }

    /// Mirror of `mmuko_semverx_t`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct CSemVerX {
        /// Major number.
        pub major: u16,
        /// Minor number.
        pub minor: u16,
        /// Patch number.
        pub patch: u16,
        /// `mmuko_state_t` for the major field.
        pub major_state: u8,
        /// `mmuko_state_t` for the minor field.
        pub minor_state: u8,
        /// `mmuko_state_t` for the patch field.
        pub patch_state: u8,
        /// Explicit padding, so the layout is stated rather than inferred.
        pub _pad: u8,
    }

    /// Mirror of `mmuko_export_desc_t`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct CExportDesc {
        /// NUL-terminated symbol name.
        pub symbol: *const u8,
        /// The calling contract.
        pub sig: *const CFnSig,
        /// Version at which this shape appeared.
        pub since: CSemVerX,
        /// Provider address, or null on the required side.
        ///
        /// The C side declares this as `mmuko_fnptr_t` (`void (*)(void)`),
        /// not `void *`, so that its descriptor layer stays inside strict ISO
        /// C. The two are layout-identical on both MMUKO profiles -- a code
        /// address and a data address are the same width on every target this
        /// ABI supports -- and `*mut c_void` is the form the rest of this
        /// crate's FFI surface uses, so the mirror keeps it. The differential
        /// suite checks the struct's size and field offsets against the C
        /// side, which is what actually holds the two in step.
        pub address: *mut c_void,
    }

    /// Mirror of `mmuko_module_desc_t`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct CModuleDesc {
        /// `MMUKO_ABI_MAGIC`
        pub magic: u32,
        /// `MMUKO_ABI_REV`
        pub abi_rev: u32,
        /// `mmuko_arch_t`
        pub arch: u32,
        /// Export count.
        pub nexports: u32,
        /// NUL-terminated module name.
        pub module: *const u8,
        /// Release version.
        pub version: CSemVerX,
        /// `nexports` export descriptors.
        pub exports: *const CExportDesc,
    }

    /// `MMUKO_FN_VARIADIC`
    pub const FN_VARIADIC: u32 = 0x0001;
    /// `MMUKO_FN_NORETURN`
    pub const FN_NORETURN: u32 = 0x0002;
    /// `MMUKO_FN_BOOTSTRAP`
    pub const FN_BOOTSTRAP: u32 = 0x0004;
}
