/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_abi.c -- canonical encoding and 128-bit ABI fingerprinting.
 *
 * Freestanding.  No libc, no allocator, no floating point.
 */

#include "mmuko/mmuko_abi.h"

/* ------------------------------------------------------------------------ */
/* Freestanding helpers                                                      */
/* ------------------------------------------------------------------------ */

uint32_t mmuko_strlen(const char *s)
{
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n] != '\0') n++;
    return n;
}

int mmuko_streq(const char *a, const char *b)
{
    if (a == b)      return 1;
    if (!a || !b)    return 0;
    while (*a && (*a == *b)) { a++; b++; }
    return (*a == '\0') && (*b == '\0');
}

/* ------------------------------------------------------------------------ */
/* Names                                                                     */
/* ------------------------------------------------------------------------ */

const char *mmuko_arch_name(mmuko_arch_t arch)
{
    switch (arch) {
    case MMUKO_ARCH_MMUKO32: return "mmuko32";
    case MMUKO_ARCH_MMUKO64: return "mmuko64";
    default:                 return "arch-none";
    }
}

const char *mmuko_cc_name(mmuko_cc_t cc)
{
    switch (cc) {
    case MMUKO_CC_CDECL:    return "cdecl";
    case MMUKO_CC_STDCALL:  return "stdcall";
    case MMUKO_CC_FASTCALL: return "fastcall";
    case MMUKO_CC_SYSCALL:  return "syscall";
    default:                return "cc-invalid";
    }
}

/* ------------------------------------------------------------------------ */
/* Scalar type singletons                                                    */
/* ------------------------------------------------------------------------ */

#define SCALAR(NAME, CODE) \
    const mmuko_type_desc_t NAME = { (uint32_t)(CODE), 0u, 0u, 0u, NULL }

SCALAR(mmuko_ty_void, MMUKO_T_VOID);
SCALAR(mmuko_ty_bool, MMUKO_T_BOOL);
SCALAR(mmuko_ty_i8,   MMUKO_T_I8);
SCALAR(mmuko_ty_u8,   MMUKO_T_U8);
SCALAR(mmuko_ty_i16,  MMUKO_T_I16);
SCALAR(mmuko_ty_u16,  MMUKO_T_U16);
SCALAR(mmuko_ty_i32,  MMUKO_T_I32);
SCALAR(mmuko_ty_u32,  MMUKO_T_U32);
SCALAR(mmuko_ty_i64,  MMUKO_T_I64);
SCALAR(mmuko_ty_u64,  MMUKO_T_U64);
SCALAR(mmuko_ty_f32,  MMUKO_T_F32);
SCALAR(mmuko_ty_f64,  MMUKO_T_F64);
SCALAR(mmuko_ty_ptr,  MMUKO_T_PTR);
SCALAR(mmuko_ty_cstr, MMUKO_T_CSTR);

#undef SCALAR

const char *mmuko_typecode_token(uint32_t code)
{
    switch (code) {
    case MMUKO_T_VOID: return "v";
    case MMUKO_T_BOOL: return "b8";
    case MMUKO_T_I8:   return "i8";
    case MMUKO_T_U8:   return "u8";
    case MMUKO_T_I16:  return "i16";
    case MMUKO_T_U16:  return "u16";
    case MMUKO_T_I32:  return "i32";
    case MMUKO_T_U32:  return "u32";
    case MMUKO_T_I64:  return "i64";
    case MMUKO_T_U64:  return "u64";
    case MMUKO_T_F32:  return "f32";
    case MMUKO_T_F64:  return "f64";
    case MMUKO_T_PTR:  return "ptr";
    case MMUKO_T_CSTR: return "cstr";
    default:           return NULL;   /* aggregate or unknown */
    }
}

uint32_t mmuko_type_width(const mmuko_type_desc_t *t, mmuko_arch_t arch)
{
    if (!t) return 0;
    switch (t->code) {
    case MMUKO_T_VOID: return 0;
    case MMUKO_T_BOOL:
    case MMUKO_T_I8:
    case MMUKO_T_U8:   return 1;
    case MMUKO_T_I16:
    case MMUKO_T_U16:  return 2;
    case MMUKO_T_I32:
    case MMUKO_T_U32:
    case MMUKO_T_F32:  return 4;
    case MMUKO_T_I64:
    case MMUKO_T_U64:
    case MMUKO_T_F64:  return 8;
    case MMUKO_T_PTR:
    case MMUKO_T_CSTR:
        /* The one width that depends on the profile -- which is precisely why
         * the architecture tag is an input to every fingerprint. */
        return (arch == MMUKO_ARCH_MMUKO64) ? 8u
             : (arch == MMUKO_ARCH_MMUKO32) ? 4u : 0u;
    case MMUKO_T_STRUCT:
    case MMUKO_T_UNION:
    case MMUKO_T_ENUM:
        return t->size;           /* 0 means malformed */
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Bounded buffer                                                            */
/* ------------------------------------------------------------------------ */

void mmuko_buf_init(mmuko_buf_t *b, char *storage, uint32_t cap)
{
    b->data = storage;
    b->cap  = cap;
    b->len  = 0;
    b->overflow = 0;
    if (cap > 0) storage[0] = '\0';
}

void mmuko_buf_putc(mmuko_buf_t *b, char c)
{
    if (b->overflow) return;
    if (b->len + 1u >= b->cap) { b->overflow = 1; return; }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void mmuko_buf_putn(mmuko_buf_t *b, const char *s, uint32_t n)
{
    uint32_t i;
    if (b->overflow || !s) return;
    if (b->len + n + 1u > b->cap) { b->overflow = 1; return; }
    for (i = 0; i < n; i++) b->data[b->len + i] = s[i];
    b->len += n;
    b->data[b->len] = '\0';
}

void mmuko_buf_put(mmuko_buf_t *b, const char *s)
{
    mmuko_buf_putn(b, s, mmuko_strlen(s));
}

void mmuko_buf_putu(mmuko_buf_t *b, uint64_t v)
{
    char tmp[24];
    int  i = 0;
    if (v == 0) { mmuko_buf_putc(b, '0'); return; }
    while (v > 0 && i < (int)sizeof(tmp)) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i-- > 0) mmuko_buf_putc(b, tmp[i]);
}

/* ------------------------------------------------------------------------ */
/* FNV-1a, two independent bases -> 128 bits                                 */
/* ------------------------------------------------------------------------ */

#define FNV1A_OFFSET_A  0xcbf29ce484222325ULL
#define FNV1A_PRIME_A   0x00000100000001B3ULL
/* Second basis: a different odd offset and a different odd prime, so the two
 * lanes are not affine transforms of one another and a collision in one lane
 * gives no information about the other. */
#define FNV1A_OFFSET_B  0x9ae16a3b2f90404fULL
#define FNV1A_PRIME_B   0x00000100000001C9ULL

mmuko_fingerprint_t mmuko_fp_bytes(const void *data, uint32_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    mmuko_fingerprint_t fp;
    uint64_t a = FNV1A_OFFSET_A;
    uint64_t b = FNV1A_OFFSET_B;
    uint32_t i;

    if (!p) len = 0;

    /* Lane A: forward.  Lane B: reverse, so transpositions that leave lane A
     * unchanged still move lane B. */
    for (i = 0; i < len; i++) {
        a ^= (uint64_t)p[i];
        a *= FNV1A_PRIME_A;
    }
    for (i = len; i > 0; i--) {
        b ^= (uint64_t)p[i - 1u];
        b *= FNV1A_PRIME_B;
    }
    /* Mix the length in so that a prefix relationship cannot collide. */
    a ^= (uint64_t)len; a *= FNV1A_PRIME_A;
    b ^= (uint64_t)len; b *= FNV1A_PRIME_B;

    fp.lo = a;
    fp.hi = b;
    return fp;
}

int mmuko_fp_equal(mmuko_fingerprint_t a, mmuko_fingerprint_t b)
{
    return (a.lo == b.lo) && (a.hi == b.hi);
}

int mmuko_fp_is_zero(mmuko_fingerprint_t a)
{
    return (a.lo == 0u) && (a.hi == 0u);
}

static void hex64(uint64_t v, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 15; i >= 0; i--) { out[i] = digits[v & 0xFu]; v >>= 4; }
}

void mmuko_fp_format(mmuko_fingerprint_t fp, char out[MMUKO_FP_STRLEN])
{
    hex64(fp.hi, out);
    out[16] = ':';
    hex64(fp.lo, out + 17);
    out[33] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Canonical type encoding                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Scalars encode as their frozen token: "i32", "f64", "ptr".
 *
 * Aggregates encode as their kind, measured size, measured alignment and their
 * ordered field encodings:
 *
 *     S{sz=16,al=8:i32,f64}
 *
 * Size and alignment are included because two structs with identical field
 * types can still have different padding under different packing pragmas, and
 * a struct passed by value is classified for register assignment by its size
 * and its field classes together.  Encoding the fields alone would let a
 * packed and an unpacked struct share a fingerprint.
 */
static int encode_type(const mmuko_type_desc_t *t,
                       mmuko_arch_t arch,
                       mmuko_buf_t *out,
                       int depth)
{
    const char *token;
    uint32_t i;

    if (!t) return -1;
    if (depth > MMUKO_TYPE_MAX_DEPTH) return -2;

    token = mmuko_typecode_token(t->code);
    if (token) {
        /* A scalar must not carry aggregate fields; a descriptor that does is
         * malformed and must not be silently normalised away. */
        if (t->nfields != 0u || t->fields != NULL) return -3;
        mmuko_buf_put(out, token);
        return out->overflow ? -4 : 0;
    }

    switch (t->code) {
    case MMUKO_T_STRUCT: mmuko_buf_put(out, "S{"); break;
    case MMUKO_T_UNION:  mmuko_buf_put(out, "U{"); break;
    case MMUKO_T_ENUM:   mmuko_buf_put(out, "E{"); break;
    default:             return -5;   /* unknown code */
    }

    if (t->size == 0u || t->align == 0u) return -6;
    mmuko_buf_put(out, "sz=");
    mmuko_buf_putu(out, (uint64_t)t->size);
    mmuko_buf_put(out, ",al=");
    mmuko_buf_putu(out, (uint64_t)t->align);
    mmuko_buf_putc(out, ':');

    if (t->nfields > 0u && !t->fields) return -7;
    for (i = 0; i < t->nfields; i++) {
        int rc;
        if (i) mmuko_buf_putc(out, ',');
        rc = encode_type(t->fields[i], arch, out, depth + 1);
        if (rc != 0) return rc;
    }
    mmuko_buf_putc(out, '}');
    return out->overflow ? -4 : 0;
}

/* ------------------------------------------------------------------------ */
/* Canonical signature encoding                                              */
/* ------------------------------------------------------------------------ */

int mmuko_encode_signature(const mmuko_fn_sig_t *sig,
                           mmuko_arch_t arch,
                           mmuko_buf_t *out)
{
    uint32_t i;
    int rc;

    if (!sig || !out) return -1;
    if (arch != MMUKO_ARCH_MMUKO32 && arch != MMUKO_ARCH_MMUKO64) return -2;
    if (sig->nargs > MMUKO_MAX_ARGS) return -3;
    if (sig->nargs > 0u && !sig->args) return -4;
    /* An unrecognised convention is rejected rather than encoded, so that two
     * different unknown values can never share the "cc-invalid" spelling and
     * therefore a fingerprint. */
    switch ((mmuko_cc_t)sig->cc) {
    case MMUKO_CC_CDECL:
    case MMUKO_CC_STDCALL:
    case MMUKO_CC_FASTCALL:
    case MMUKO_CC_SYSCALL:
        break;
    default:
        return -5;
    }
    if (!sig->ret) return -6;

    /* MMUKOABI/1|A=64|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0 */
    mmuko_buf_put(out, MMUKO_ABI_TAG);

    mmuko_buf_put(out, "|A=");
    mmuko_buf_putu(out, (uint64_t)(uint32_t)arch);

    mmuko_buf_put(out, "|CC=");
    mmuko_buf_put(out, mmuko_cc_name((mmuko_cc_t)sig->cc));

    mmuko_buf_put(out, "|R=");
    rc = encode_type(sig->ret, arch, out, 0);
    if (rc != 0) return rc;

    mmuko_buf_put(out, "|N=");
    mmuko_buf_putu(out, (uint64_t)sig->nargs);

    for (i = 0; i < sig->nargs; i++) {
        mmuko_buf_put(out, "|P");
        mmuko_buf_putu(out, (uint64_t)i);
        mmuko_buf_putc(out, '=');
        /* void is not a legal parameter type; `f(void)` is arity zero, and
         * conflating the two would let f(void) and f() share a fingerprint on
         * a platform where they do not share a calling contract. */
        if (!sig->args[i] || sig->args[i]->code == (uint32_t)MMUKO_T_VOID) return -7;
        rc = encode_type(sig->args[i], arch, out, 0);
        if (rc != 0) return rc;
    }

    mmuko_buf_put(out, "|V=");
    mmuko_buf_putu(out, (sig->flags & MMUKO_FN_VARIADIC) ? 1u : 0u);

    if (out->overflow) return -8;
    return 0;
}

int mmuko_fp_signature(const mmuko_fn_sig_t *sig,
                       mmuko_arch_t arch,
                       mmuko_fingerprint_t *out)
{
    char storage[MMUKO_CANON_MAX];
    mmuko_buf_t buf;
    int rc;

    if (!out) return -1;
    *out = MMUKO_FP_ZERO;
    mmuko_buf_init(&buf, storage, (uint32_t)sizeof(storage));
    rc = mmuko_encode_signature(sig, arch, &buf);
    if (rc != 0) return rc;
    *out = mmuko_fp_bytes(buf.data, buf.len);
    return 0;
}

int mmuko_fp_export(const char *symbol,
                    const mmuko_fn_sig_t *sig,
                    mmuko_arch_t arch,
                    mmuko_fingerprint_t *out)
{
    char storage[MMUKO_CANON_MAX];
    mmuko_buf_t buf;
    int rc;

    if (!out) return -1;
    *out = MMUKO_FP_ZERO;
    if (!symbol || symbol[0] == '\0') return -2;

    mmuko_buf_init(&buf, storage, (uint32_t)sizeof(storage));
    /* Symbol first: the name is part of the export's identity, so a correctly
     * shaped function under a different name cannot fill the slot. */
    mmuko_buf_put(&buf, "SYM=");
    mmuko_buf_put(&buf, symbol);
    mmuko_buf_putc(&buf, '|');
    rc = mmuko_encode_signature(sig, arch, &buf);
    if (rc != 0) return rc;
    if (buf.overflow) return -8;
    *out = mmuko_fp_bytes(buf.data, buf.len);
    return 0;
}
