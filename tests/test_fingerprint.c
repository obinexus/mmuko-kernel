/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * test_fingerprint.c -- the canonical encoder and the 128-bit fingerprint.
 *
 * These are the tests that decide whether the ABI can break.  Every other
 * guarantee in the library reduces to "two different machine contracts produce
 * two different fingerprints", so this file is where that is actually checked,
 * exhaustively over the scalar type vocabulary rather than by example.
 */

#include "mmuko/mmuko_abi.h"
#include "mmuko_test.h"
#include <stdio.h>
#include <string.h>

MMUKO_TEST_STATE_DEFS

static mmuko_fingerprint_t fp_of(const mmuko_type_desc_t *ret,
                                 const mmuko_type_desc_t *const *args,
                                 uint32_t n,
                                 mmuko_arch_t arch,
                                 mmuko_cc_t cc)
{
    mmuko_fn_sig_t sig;
    mmuko_fingerprint_t fp = MMUKO_FP_ZERO;
    sig.cc = (uint32_t)cc; sig.flags = 0; sig.ret = ret; sig.nargs = n; sig.args = args;
    (void)mmuko_fp_signature(&sig, arch, &fp);
    return fp;
}

/* Every scalar in the vocabulary, for exhaustive pairwise distinctness. */
static const mmuko_type_desc_t *const kScalars[] = {
    MMUKO_BOOL, MMUKO_I8, MMUKO_U8, MMUKO_I16, MMUKO_U16,
    MMUKO_I32, MMUKO_U32, MMUKO_I64, MMUKO_U64,
    MMUKO_F32, MMUKO_F64, MMUKO_PTR, MMUKO_CSTR
};
#define NSCALARS ((uint32_t)(sizeof(kScalars) / sizeof(kScalars[0])))

/* ------------------------------------------------------------------------ */

static void test_canonical_form(void)
{
    char store[MMUKO_CANON_MAX];
    mmuko_buf_t b;
    const mmuko_type_desc_t *args[2] = { MMUKO_I32, MMUKO_I32 };
    mmuko_fn_sig_t sig = { (uint32_t)MMUKO_CC_CDECL, 0u, MMUKO_I32, 2u, args };

    MMUKO_CASE("canonical encoding is the documented literal string");

    mmuko_buf_init(&b, store, sizeof(store));
    MMUKO_CHECK(mmuko_encode_signature(&sig, MMUKO_ARCH_MMUKO64, &b) == 0,
                "int32 add(int32,int32) encodes on mmuko64");
    MMUKO_CHECK_MSG(strcmp(b.data,
                    "MMUKOABI/1|A=64|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0") == 0,
                    "encoding matches the specification byte for byte", b.data);

    mmuko_buf_init(&b, store, sizeof(store));
    (void)mmuko_encode_signature(&sig, MMUKO_ARCH_MMUKO32, &b);
    MMUKO_CHECK_MSG(strcmp(b.data,
                    "MMUKOABI/1|A=32|CC=cdecl|R=i32|N=2|P0=i32|P1=i32|V=0") == 0,
                    "mmuko32 encoding differs only in the arch tag", b.data);
}

static void test_the_transcript_break(void)
{
    const mmuko_type_desc_t *ii[2] = { MMUKO_I32, MMUKO_I32 };
    const mmuko_type_desc_t *dd[2] = { MMUKO_F64, MMUKO_F64 };
    mmuko_fingerprint_t v1_64, v2_64, v1_32, v2_32;

    MMUKO_CASE("the transcript's break: int add(int,int) vs double add(double,double)");

    v1_64 = fp_of(MMUKO_I32, ii, 2, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL);
    v2_64 = fp_of(MMUKO_F64, dd, 2, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL);
    v1_32 = fp_of(MMUKO_I32, ii, 2, MMUKO_ARCH_MMUKO32, MMUKO_CC_CDECL);
    v2_32 = fp_of(MMUKO_F64, dd, 2, MMUKO_ARCH_MMUKO32, MMUKO_CC_CDECL);

    MMUKO_CHECK(!mmuko_fp_equal(v1_64, v2_64), "mmuko64: v1 and v2 fingerprints differ");
    MMUKO_CHECK(!mmuko_fp_equal(v1_32, v2_32), "mmuko32: v1 and v2 fingerprints differ");
    MMUKO_CHECK(!mmuko_fp_is_zero(v1_64),      "a valid signature never fingerprints to zero");

    /* Same shape, same arch, computed twice: the encoder is a function, not a
     * sampler.  Without this nothing else in the library means anything. */
    MMUKO_CHECK(mmuko_fp_equal(v1_64, fp_of(MMUKO_I32, ii, 2, MMUKO_ARCH_MMUKO64,
                                            MMUKO_CC_CDECL)),
                "identical signatures fingerprint identically (determinism)");
}

static void test_arch_separation(void)
{
    const mmuko_type_desc_t *p1[1] = { MMUKO_PTR };
    const mmuko_type_desc_t *i1[1] = { MMUKO_I32 };

    MMUKO_CASE("mmuko32 and mmuko64 are disjoint profiles");

    MMUKO_CHECK(!mmuko_fp_equal(fp_of(MMUKO_PTR, p1, 1, MMUKO_ARCH_MMUKO32, MMUKO_CC_CDECL),
                                fp_of(MMUKO_PTR, p1, 1, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL)),
                "a pointer signature differs across profiles (4 vs 8 bytes)");

    /* Even a signature with no pointer in it must differ across profiles: the
     * arch tag is unconditional, because register assignment, stack alignment
     * and return conventions all differ even for identical scalar types. */
    MMUKO_CHECK(!mmuko_fp_equal(fp_of(MMUKO_I32, i1, 1, MMUKO_ARCH_MMUKO32, MMUKO_CC_CDECL),
                                fp_of(MMUKO_I32, i1, 1, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL)),
                "an all-int32 signature still differs across profiles");

    MMUKO_CHECK(mmuko_type_width(MMUKO_PTR, MMUKO_ARCH_MMUKO32) == 4u, "ptr is 4 on mmuko32");
    MMUKO_CHECK(mmuko_type_width(MMUKO_PTR, MMUKO_ARCH_MMUKO64) == 8u, "ptr is 8 on mmuko64");
}

static void test_pairwise_scalar_distinctness(void)
{
    mmuko_fingerprint_t ret_fp[NSCALARS];
    mmuko_fingerprint_t arg_fp[NSCALARS];
    uint32_t i, j;
    int ret_collision = 0, arg_collision = 0;
    const mmuko_arch_t arches[2] = { MMUKO_ARCH_MMUKO32, MMUKO_ARCH_MMUKO64 };
    uint32_t a;

    MMUKO_CASE("exhaustive pairwise distinctness over the scalar vocabulary");

    for (a = 0; a < 2; a++) {
        for (i = 0; i < NSCALARS; i++) {
            const mmuko_type_desc_t *one[1];
            one[0] = kScalars[i];
            ret_fp[i] = fp_of(kScalars[i], NULL, 0, arches[a], MMUKO_CC_CDECL);
            arg_fp[i] = fp_of(MMUKO_VOID, one, 1, arches[a], MMUKO_CC_CDECL);
        }
        for (i = 0; i < NSCALARS; i++) {
            for (j = i + 1u; j < NSCALARS; j++) {
                if (mmuko_fp_equal(ret_fp[i], ret_fp[j])) ret_collision++;
                if (mmuko_fp_equal(arg_fp[i], arg_fp[j])) arg_collision++;
            }
        }
    }
    /* 13 scalars, 78 unordered pairs, two profiles: 156 comparisons per role. */
    MMUKO_CHECK(ret_collision == 0, "no two return types share a fingerprint");
    MMUKO_CHECK(arg_collision == 0, "no two parameter types share a fingerprint");

    /* Signedness is a machine fact -- it changes the widening and the compare
     * instructions the caller emits -- so i32 and u32 must not be conflated. */
    MMUKO_CHECK(!mmuko_fp_equal(fp_of(MMUKO_I32, NULL, 0, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL),
                                fp_of(MMUKO_U32, NULL, 0, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL)),
                "i32 and u32 are distinct despite equal width");
    MMUKO_CHECK(!mmuko_fp_equal(fp_of(MMUKO_I64, NULL, 0, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL),
                                fp_of(MMUKO_F64, NULL, 0, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL)),
                "i64 and f64 are distinct despite equal width (different register class)");
    MMUKO_CHECK(!mmuko_fp_equal(fp_of(MMUKO_PTR, NULL, 0, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL),
                                fp_of(MMUKO_CSTR, NULL, 0, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL)),
                "ptr and cstr are distinct despite equal width");
}

static void test_arity_and_order(void)
{
    const mmuko_type_desc_t *a1[1] = { MMUKO_I32 };
    const mmuko_type_desc_t *a2[2] = { MMUKO_I32, MMUKO_I32 };
    const mmuko_type_desc_t *ab[2] = { MMUKO_I32, MMUKO_F64 };
    const mmuko_type_desc_t *ba[2] = { MMUKO_F64, MMUKO_I32 };

    MMUKO_CASE("arity, order and variadic-ness are all part of the contract");

    MMUKO_CHECK(!mmuko_fp_equal(fp_of(MMUKO_I32, a1, 1, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL),
                                fp_of(MMUKO_I32, a2, 2, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL)),
                "add(i32) and add(i32,i32) differ");

    /* Argument ORDER changes which register each value lands in.  A hash that
     * folded the argument multiset would miss this; the lane-B reverse pass
     * exists precisely so a transposition cannot cancel out. */
    MMUKO_CHECK(!mmuko_fp_equal(fp_of(MMUKO_VOID, ab, 2, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL),
                                fp_of(MMUKO_VOID, ba, 2, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL)),
                "f(i32,f64) and f(f64,i32) differ -- order matters");

    {
        mmuko_fn_sig_t fixed = { (uint32_t)MMUKO_CC_CDECL, 0u, MMUKO_I32, 1u, a1 };
        mmuko_fn_sig_t vari  = { (uint32_t)MMUKO_CC_CDECL, MMUKO_FN_VARIADIC,
                                 MMUKO_I32, 1u, a1 };
        mmuko_fingerprint_t f1, f2;
        (void)mmuko_fp_signature(&fixed, MMUKO_ARCH_MMUKO64, &f1);
        (void)mmuko_fp_signature(&vari,  MMUKO_ARCH_MMUKO64, &f2);
        MMUKO_CHECK(!mmuko_fp_equal(f1, f2),
                    "variadic and fixed forms differ (AL/register-save area)");
    }
}

static void test_calling_convention(void)
{
    const mmuko_type_desc_t *a2[2] = { MMUKO_I32, MMUKO_I32 };

    MMUKO_CASE("calling convention is part of the contract");

    MMUKO_CHECK(!mmuko_fp_equal(fp_of(MMUKO_I32, a2, 2, MMUKO_ARCH_MMUKO32, MMUKO_CC_CDECL),
                                fp_of(MMUKO_I32, a2, 2, MMUKO_ARCH_MMUKO32, MMUKO_CC_STDCALL)),
                "cdecl and stdcall differ (who cleans the stack)");
}

static void test_aggregates(void)
{
    /* Two structs with the same fields but different measured layout. */
    static const mmuko_type_desc_t *const f_id[2] = { MMUKO_I32, MMUKO_F64 };
    static const mmuko_type_desc_t packed   = { MMUKO_T_STRUCT, 12u, 1u, 2u, f_id };
    static const mmuko_type_desc_t natural  = { MMUKO_T_STRUCT, 16u, 8u, 2u, f_id };
    static const mmuko_type_desc_t *const f_di[2] = { MMUKO_F64, MMUKO_I32 };
    static const mmuko_type_desc_t reordered = { MMUKO_T_STRUCT, 16u, 8u, 2u, f_di };

    mmuko_fingerprint_t fp_p, fp_n, fp_r;
    const mmuko_type_desc_t *ap[1], *an[1], *ar[1];
    ap[0] = &packed; an[0] = &natural; ar[0] = &reordered;

    MMUKO_CASE("aggregates: layout, not just field types");

    fp_p = fp_of(MMUKO_VOID, ap, 1, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL);
    fp_n = fp_of(MMUKO_VOID, an, 1, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL);
    fp_r = fp_of(MMUKO_VOID, ar, 1, MMUKO_ARCH_MMUKO64, MMUKO_CC_CDECL);

    MMUKO_CHECK(!mmuko_fp_is_zero(fp_p), "a struct-by-value signature encodes");
    MMUKO_CHECK(!mmuko_fp_equal(fp_p, fp_n),
                "packed and naturally-aligned structs differ (padding is contract)");
    MMUKO_CHECK(!mmuko_fp_equal(fp_n, fp_r),
                "field order changes the struct's fingerprint");

    {
        /* A struct with no measured size is malformed, not zero-sized. */
        static const mmuko_type_desc_t bad = { MMUKO_T_STRUCT, 0u, 0u, 2u, f_id };
        const mmuko_type_desc_t *ab[1]; mmuko_fingerprint_t fp;
        mmuko_fn_sig_t s;
        ab[0] = &bad;
        s.cc = MMUKO_CC_CDECL; s.flags = 0; s.ret = MMUKO_VOID; s.nargs = 1; s.args = ab;
        MMUKO_CHECK(mmuko_fp_signature(&s, MMUKO_ARCH_MMUKO64, &fp) != 0,
                    "a struct without size/align is rejected, not encoded");
    }
}

static void test_malformed_input(void)
{
    mmuko_fingerprint_t fp;
    mmuko_fn_sig_t s;
    const mmuko_type_desc_t *av[1];

    MMUKO_CASE("malformed descriptors are refused, never normalised");

    s.cc = 0u; s.flags = 0; s.ret = MMUKO_I32; s.nargs = 0; s.args = NULL;
    MMUKO_CHECK(mmuko_fp_signature(&s, MMUKO_ARCH_MMUKO64, &fp) != 0,
                "an invalid calling convention is rejected");

    s.cc = MMUKO_CC_CDECL; s.ret = NULL;
    MMUKO_CHECK(mmuko_fp_signature(&s, MMUKO_ARCH_MMUKO64, &fp) != 0,
                "a NULL return type is rejected");

    s.ret = MMUKO_I32; s.nargs = 1; av[0] = MMUKO_VOID; s.args = av;
    MMUKO_CHECK(mmuko_fp_signature(&s, MMUKO_ARCH_MMUKO64, &fp) != 0,
                "void as a parameter type is rejected (f(void) is arity zero)");

    s.nargs = MMUKO_MAX_ARGS + 1u;
    MMUKO_CHECK(mmuko_fp_signature(&s, MMUKO_ARCH_MMUKO64, &fp) != 0,
                "arity beyond MMUKO_MAX_ARGS is rejected");

    s.nargs = 0; s.args = NULL;
    MMUKO_CHECK(mmuko_fp_signature(&s, MMUKO_ARCH_NONE, &fp) != 0,
                "an unspecified architecture profile is rejected");
}

static void test_export_identity(void)
{
    mmuko_fn_sig_t s;
    const mmuko_type_desc_t *a2[2] = { MMUKO_I32, MMUKO_I32 };
    mmuko_fingerprint_t add_fp, mul_fp, sig_fp;

    MMUKO_CASE("export identity binds the symbol name into the fingerprint");

    s.cc = MMUKO_CC_CDECL; s.flags = 0; s.ret = MMUKO_I32; s.nargs = 2; s.args = a2;

    MMUKO_CHECK(mmuko_fp_export("add", &s, MMUKO_ARCH_MMUKO64, &add_fp) == 0, "add encodes");
    MMUKO_CHECK(mmuko_fp_export("mul", &s, MMUKO_ARCH_MMUKO64, &mul_fp) == 0, "mul encodes");
    MMUKO_CHECK(!mmuko_fp_equal(add_fp, mul_fp),
                "identically shaped functions under different names differ");

    (void)mmuko_fp_signature(&s, MMUKO_ARCH_MMUKO64, &sig_fp);
    MMUKO_CHECK(!mmuko_fp_equal(add_fp, sig_fp),
                "an export fingerprint is not its bare signature fingerprint");
    MMUKO_CHECK(mmuko_fp_export("", &s, MMUKO_ARCH_MMUKO64, &add_fp) != 0,
                "an empty symbol name is rejected");
}

static void test_hash_properties(void)
{
    mmuko_fingerprint_t a, b, c;
    char fmt[MMUKO_FP_STRLEN];

    MMUKO_CASE("hash lanes and formatting");

    a = mmuko_fp_bytes("abc", 3);
    b = mmuko_fp_bytes("cba", 3);
    c = mmuko_fp_bytes("abc", 3);
    MMUKO_CHECK(mmuko_fp_equal(a, c), "the byte hash is deterministic");
    MMUKO_CHECK(!mmuko_fp_equal(a, b), "reversal changes the fingerprint (lane B)");
    MMUKO_CHECK(a.lo != a.hi, "the two lanes are independent, not duplicated");

    /* Length mixing: without it, a prefix could collide with its extension in
     * a padded encoding. */
    MMUKO_CHECK(!mmuko_fp_equal(mmuko_fp_bytes("ab", 2), mmuko_fp_bytes("ab\0", 3)),
                "length is mixed in: a prefix cannot collide with its extension");

    mmuko_fp_format(a, fmt);
    MMUKO_CHECK(strlen(fmt) == 33u && fmt[16] == ':', "fingerprints format as hi:lo hex");
}

int main(void)
{
    MMUKO_SUITE_BEGIN("mmuko fingerprint / canonical encoding");
    test_canonical_form();
    test_the_transcript_break();
    test_arch_separation();
    test_pairwise_scalar_distinctness();
    test_arity_and_order();
    test_calling_convention();
    test_aggregates();
    test_malformed_input();
    test_export_identity();
    test_hash_properties();

    printf("\n%u passed, %u failed  [%s, native=%s]\n",
           mmuko_test_passed, mmuko_test_failed,
           "test_fingerprint", mmuko_arch_name(MMUKO_ARCH_NATIVE));
    return mmuko_test_failed == 0 ? 0 : 1;
}
