/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_abi.h -- MMUKO kernel dynamic C ABI, core types.
 *
 * MMUKO ABI revision 1.  Freestanding: this header and the translation units
 * that implement it depend on <stdint.h> and <stddef.h> only.  No libc, no
 * allocator, no floating point in the resolver path.  It links into the MMUKO
 * kernel at ring 0 unchanged.
 *
 * The problem this solves
 * ----------------------
 * A shared object exports `int add(int, int)`.  A later release changes it to
 * `double add(double, double)` and keeps the same soname and the same symbol
 * name.  The dynamic linker resolves the symbol by NAME ALONE.  The caller's
 * already-compiled machine code still passes two 32-bit integers in the
 * integer registers and reads EAX/RAX; the callee reads XMM0/XMM1 and writes
 * XMM0.  Nothing traps.  The program keeps running and returns garbage.
 *
 * MMUKO's answer: a symbol is not identified by its name.  It is identified by
 * the 128-bit fingerprint of its complete calling contract -- architecture
 * width, calling convention, return type, arity, and every argument type at
 * explicit bit width -- together with its SemVerX identity.  Binding is gated
 * by a trident consensus: the contract the caller was COMPILED AGAINST (hook
 * u1) and the contract the loaded object ACTUALLY PROVIDES (hook u2) must be
 * identical before the callable slot (w) is ever populated.
 *
 * See SPEC/MMUKO-ABI-SPEC.md for the normative text.
 */

#ifndef MMUKO_ABI_H
#define MMUKO_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Descriptor format revision                                                */
/* ------------------------------------------------------------------------ */

/* 'M''M''K''O' -- guards against a caller handing us an unrelated pointer. */
#define MMUKO_ABI_MAGIC     0x4F4B4D4Du
#define MMUKO_ABI_REV       1u

/* Domain-separation tag baked into every canonical encoding.  Changing this
 * string changes every fingerprint in the world, which is exactly what a
 * breaking change to the encoding itself must do. */
#define MMUKO_ABI_TAG       "MMUKOABI/1"

/* ------------------------------------------------------------------------ */
/* Architecture profile                                                      */
/* ------------------------------------------------------------------------ */

/*
 * mmuko32 -- ILP32.  void* is 4 bytes.  SysV i386 cdecl: all arguments on the
 *            stack, caller cleans up, integers return in EAX:EDX, floating
 *            point returns on the x87 stack (st0).
 *
 * mmuko64 -- LP64.  void* is 8 bytes.  SysV AMD64: integer/pointer arguments
 *            in RDI RSI RDX RCX R8 R9, floating point in XMM0-XMM7, integer
 *            return in RAX:RDX, floating point return in XMM0:XMM1.
 *
 * The two profiles are DISJOINT.  An mmuko32 descriptor can never satisfy an
 * mmuko64 call site: the architecture tag is part of the fingerprint input, so
 * the fault is detected by comparison rather than by convention.
 */
typedef enum mmuko_arch {
    MMUKO_ARCH_NONE   = 0,
    MMUKO_ARCH_MMUKO32 = 32,
    MMUKO_ARCH_MMUKO64 = 64
} mmuko_arch_t;

#if defined(__SIZEOF_POINTER__)
#  if __SIZEOF_POINTER__ == 4
#    define MMUKO_ARCH_NATIVE MMUKO_ARCH_MMUKO32
#  elif __SIZEOF_POINTER__ == 8
#    define MMUKO_ARCH_NATIVE MMUKO_ARCH_MMUKO64
#  else
#    error "MMUKO ABI supports 32-bit and 64-bit pointer widths only"
#  endif
#elif defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
#  define MMUKO_ARCH_NATIVE MMUKO_ARCH_MMUKO64
#else
#  define MMUKO_ARCH_NATIVE MMUKO_ARCH_MMUKO32
#endif

const char *mmuko_arch_name(mmuko_arch_t arch);

/* ------------------------------------------------------------------------ */
/* Generic function pointer                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Callable addresses are held as a function pointer, not as void*.
 *
 * ISO C leaves the conversion between an object pointer and a function pointer
 * undefined -- there have been real machines where code and data pointers had
 * different widths -- but it fully defines the conversion between any two
 * FUNCTION pointer types, provided a value is called only through its true
 * type.  Storing callable addresses as mmuko_fnptr_t therefore keeps the whole
 * descriptor layer inside strict ISO C, and confines the one unavoidable
 * object-to-function conversion to a single documented place: the point in the
 * hosted loader where dlsym hands back a void*.
 *
 * It also makes a class of mistake unrepresentable.  A void* address field
 * would accept a pointer to DATA, and a slot bound to a data address would be
 * a jump into a table.  This field cannot hold one.
 */
typedef void (*mmuko_fnptr_t)(void);

/* ------------------------------------------------------------------------ */
/* Calling convention                                                        */
/* ------------------------------------------------------------------------ */

typedef enum mmuko_cc {
    MMUKO_CC_INVALID  = 0,
    /* Standard C calling convention for the profile: SysV i386 cdecl on
     * mmuko32, SysV AMD64 on mmuko64.  This is the default and the only
     * convention the MMUKO kernel itself uses across module boundaries. */
    MMUKO_CC_CDECL    = 1,
    MMUKO_CC_STDCALL  = 2,
    MMUKO_CC_FASTCALL = 3,
    /* Kernel trap gate: arguments in the syscall register set, no stack
     * spill, callee-clobbers restricted.  Reserved. */
    MMUKO_CC_SYSCALL  = 4
} mmuko_cc_t;

const char *mmuko_cc_name(mmuko_cc_t cc);

/* ------------------------------------------------------------------------ */
/* Type codes                                                                */
/* ------------------------------------------------------------------------ */

/*
 * Every scalar code carries an EXPLICIT bit width.  There is deliberately no
 * MMUKO_T_INT and no MMUKO_T_LONG.  `long` is 32 bits under Windows LLP64 and
 * 64 bits under SysV LP64; a descriptor that could say "long" would let the
 * same fingerprint describe two different machine contracts.  Removing the
 * width-ambiguous types from the vocabulary removes that entire fault class
 * structurally rather than by review.
 *
 * The codes are frozen.  Appending is allowed; renumbering is a breaking
 * change to MMUKO_ABI_TAG.
 */
typedef enum mmuko_typecode {
    MMUKO_T_VOID   = 0x00,
    MMUKO_T_BOOL   = 0x01,   /* 1 byte, 0 or 1 */

    MMUKO_T_I8     = 0x10,
    MMUKO_T_U8     = 0x11,
    MMUKO_T_I16    = 0x12,
    MMUKO_T_U16    = 0x13,
    MMUKO_T_I32    = 0x14,
    MMUKO_T_U32    = 0x15,
    MMUKO_T_I64    = 0x16,
    MMUKO_T_U64    = 0x17,

    MMUKO_T_F32    = 0x20,
    MMUKO_T_F64    = 0x21,

    MMUKO_T_PTR    = 0x30,   /* opaque void*; width follows the arch profile */
    MMUKO_T_CSTR   = 0x31,   /* const char*, NUL terminated */

    MMUKO_T_STRUCT = 0x40,   /* passed by value; requires size/align/fields */
    MMUKO_T_UNION  = 0x41,   /* passed by value; requires size/align/fields */
    MMUKO_T_ENUM   = 0x42    /* requires size; underlying type in fields[0] */
} mmuko_typecode_t;

/* Maximum nesting depth for aggregate types.  Bounded so the encoder needs no
 * heap and cannot be driven into unbounded recursion by a hostile module. */
#define MMUKO_TYPE_MAX_DEPTH   8
/* Maximum arity of a described function. */
#define MMUKO_MAX_ARGS         32

/*
 * A type descriptor.  Scalars need only `code`.  Aggregates must give `size`
 * and `align` as measured by the PROVIDING compiler on the PROVIDING arch,
 * plus the ordered field list -- because two structs with identical field
 * types but different padding are different machine contracts.
 */
typedef struct mmuko_type_desc {
    uint32_t code;        /* mmuko_typecode_t */
    uint32_t size;        /* bytes; 0 for scalars (implied by code) */
    uint32_t align;       /* bytes; 0 for scalars (implied by code) */
    uint32_t nfields;     /* aggregates only */
    const struct mmuko_type_desc *const *fields;  /* nfields entries */
} mmuko_type_desc_t;

/* Canonical singletons for every scalar code. */
extern const mmuko_type_desc_t mmuko_ty_void;
extern const mmuko_type_desc_t mmuko_ty_bool;
extern const mmuko_type_desc_t mmuko_ty_i8;
extern const mmuko_type_desc_t mmuko_ty_u8;
extern const mmuko_type_desc_t mmuko_ty_i16;
extern const mmuko_type_desc_t mmuko_ty_u16;
extern const mmuko_type_desc_t mmuko_ty_i32;
extern const mmuko_type_desc_t mmuko_ty_u32;
extern const mmuko_type_desc_t mmuko_ty_i64;
extern const mmuko_type_desc_t mmuko_ty_u64;
extern const mmuko_type_desc_t mmuko_ty_f32;
extern const mmuko_type_desc_t mmuko_ty_f64;
extern const mmuko_type_desc_t mmuko_ty_ptr;
extern const mmuko_type_desc_t mmuko_ty_cstr;

#define MMUKO_VOID  (&mmuko_ty_void)
#define MMUKO_BOOL  (&mmuko_ty_bool)
#define MMUKO_I8    (&mmuko_ty_i8)
#define MMUKO_U8    (&mmuko_ty_u8)
#define MMUKO_I16   (&mmuko_ty_i16)
#define MMUKO_U16   (&mmuko_ty_u16)
#define MMUKO_I32   (&mmuko_ty_i32)
#define MMUKO_U32   (&mmuko_ty_u32)
#define MMUKO_I64   (&mmuko_ty_i64)
#define MMUKO_U64   (&mmuko_ty_u64)
#define MMUKO_F32   (&mmuko_ty_f32)
#define MMUKO_F64   (&mmuko_ty_f64)
#define MMUKO_PTR   (&mmuko_ty_ptr)
#define MMUKO_CSTR  (&mmuko_ty_cstr)

/* Storage width of a type under a given arch profile.  Returns 0 if the type
 * is malformed (aggregate with size 0, or unknown code). */
uint32_t mmuko_type_width(const mmuko_type_desc_t *t, mmuko_arch_t arch);

/* Short canonical token for a scalar code, e.g. "i32".  NULL for aggregates,
 * which have no fixed-width token. */
const char *mmuko_typecode_token(uint32_t code);

/* ------------------------------------------------------------------------ */
/* Function signature                                                        */
/* ------------------------------------------------------------------------ */

#define MMUKO_FN_VARIADIC   0x0001u
#define MMUKO_FN_NORETURN   0x0002u
/* The callee may be entered before mmuko_kernel_init(); used for the very
 * first bootstrap exports. */
#define MMUKO_FN_BOOTSTRAP  0x0004u

typedef struct mmuko_fn_sig {
    uint32_t cc;          /* mmuko_cc_t */
    uint32_t flags;       /* MMUKO_FN_* */
    const mmuko_type_desc_t *ret;
    uint32_t nargs;
    const mmuko_type_desc_t *const *args;
} mmuko_fn_sig_t;

/* ------------------------------------------------------------------------ */
/* Fingerprint                                                               */
/* ------------------------------------------------------------------------ */

/*
 * 128 bits, produced by two independent FNV-1a passes over the same canonical
 * byte encoding (forward with the standard basis, reverse with a second
 * basis).  No external dependency, no allocation, deterministic across
 * compilers and optimisation levels because it consumes only the canonical
 * ASCII encoding and never raw struct memory.
 */
typedef struct mmuko_fingerprint {
    uint64_t lo;
    uint64_t hi;
} mmuko_fingerprint_t;

#define MMUKO_FP_ZERO   ((mmuko_fingerprint_t){ 0u, 0u })

int mmuko_fp_equal(mmuko_fingerprint_t a, mmuko_fingerprint_t b);
int mmuko_fp_is_zero(mmuko_fingerprint_t a);

/* Write "hhhhhhhhhhhhhhhh:llllllllllllllll" plus NUL.  Needs 34 bytes. */
#define MMUKO_FP_STRLEN  34
void mmuko_fp_format(mmuko_fingerprint_t fp, char out[MMUKO_FP_STRLEN]);

/* ------------------------------------------------------------------------ */
/* Bounded output buffer (no allocator)                                      */
/* ------------------------------------------------------------------------ */

typedef struct mmuko_buf {
    char    *data;
    uint32_t cap;
    uint32_t len;
    int      overflow;   /* set once, sticky; a truncated encoding is invalid */
} mmuko_buf_t;

void mmuko_buf_init(mmuko_buf_t *b, char *storage, uint32_t cap);
void mmuko_buf_put(mmuko_buf_t *b, const char *s);
void mmuko_buf_putn(mmuko_buf_t *b, const char *s, uint32_t n);
void mmuko_buf_putc(mmuko_buf_t *b, char c);
void mmuko_buf_putu(mmuko_buf_t *b, uint64_t v);

/* Canonical encodings are bounded; this is the largest a well-formed
 * signature encoding can be given MMUKO_MAX_ARGS and MMUKO_TYPE_MAX_DEPTH. */
#define MMUKO_CANON_MAX   4096

/* ------------------------------------------------------------------------ */
/* Canonical encoding + fingerprinting                                       */
/* ------------------------------------------------------------------------ */

/*
 * Encode a signature into its canonical form, e.g. for
 *   int32_t add(int32_t, int32_t)   on mmuko64, cdecl:
 *
 *   MMUKOABI/1|A=64|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0
 *
 * and for the breaking replacement  double add(double, double):
 *
 *   MMUKOABI/1|A=64|CC=cdecl|R=f64|N=2|P0=f64|P1=f64|V=0
 *
 * Different bytes, therefore different fingerprint, therefore no bind.
 *
 * Returns 0 on success, negative on malformed input or overflow.
 */
int mmuko_encode_signature(const mmuko_fn_sig_t *sig,
                           mmuko_arch_t arch,
                           mmuko_buf_t *out);

/* Fingerprint of the calling shape alone -- name independent. */
int mmuko_fp_signature(const mmuko_fn_sig_t *sig,
                       mmuko_arch_t arch,
                       mmuko_fingerprint_t *out);

/* Fingerprint of a named export: symbol name bound into the shape, so a
 * correctly shaped function under the wrong name cannot satisfy the slot. */
int mmuko_fp_export(const char *symbol,
                    const mmuko_fn_sig_t *sig,
                    mmuko_arch_t arch,
                    mmuko_fingerprint_t *out);

/* Raw hash over arbitrary bytes, exposed so the Rust implementation and the
 * QEMU harness can be checked against it. */
mmuko_fingerprint_t mmuko_fp_bytes(const void *data, uint32_t len);

/* ------------------------------------------------------------------------ */
/* Freestanding helpers (no libc)                                            */
/* ------------------------------------------------------------------------ */

uint32_t mmuko_strlen(const char *s);
int      mmuko_streq(const char *a, const char *b);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MMUKO_ABI_H */
