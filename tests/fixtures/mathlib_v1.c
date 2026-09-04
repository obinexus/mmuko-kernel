/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mathlib v1 -- 1.stable.0.stable.0.stable
 *
 * This is the library from the transcript, at its first release:
 *
 *     int add(int a, int b) { return a + b; }
 *
 * declared here in MMUKO terms with explicit widths, so that "int" cannot mean
 * two different things on two different profiles.
 *
 * Build:  cc -shared -fPIC -o libmathlib.so.1 mathlib_v1.c mmuko_abi.c ...
 */

#include "mmuko/mmuko_desc.h"

/* Declared as well as defined: an exported symbol with no visible prototype
 * is exactly the undeclared contract this library exists to eliminate. */
MMUKO_PUBLIC int32_t add(int32_t a, int32_t b);
MMUKO_PUBLIC int32_t mul(int32_t a, int32_t b);

MMUKO_PUBLIC int32_t add(int32_t a, int32_t b)
{
    return a + b;
}

MMUKO_PUBLIC int32_t mul(int32_t a, int32_t b)
{
    return a * b;
}

/* --- ABI table: what this object actually provides ---------------------- */

MMUKO_SIG(sig_add, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);
MMUKO_SIG(sig_mul, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);

static const mmuko_export_desc_t mathlib_v1_exports[] = {
    MMUKO_EXPORT("add", sig_add, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), add),
    MMUKO_EXPORT("mul", sig_mul, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), mul)
};

MMUKO_MODULE(mathlib_v1_desc, "mathlib",
             MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE),
             mathlib_v1_exports);

MMUKO_QUERY_IMPL(mathlib_v1_desc)
