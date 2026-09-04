/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mathlib v2 promoted -- 2.stable.0.stable.0.stable
 *
 * Identical machine code to mathlib_v2.c.  The ONLY difference is the SemVerX
 * state: experimental has been promoted to stable.
 *
 * This fixture exists to prove a single ordering claim, and it is the most
 * important claim in the library:
 *
 *     PROMOTING A VERSION CANNOT REPAIR AN ABI BREAK.
 *
 * A caller compiled against 1.stable's int32 `add` still faults against this
 * object with MMUKO_FAULT_FINGERPRINT -- not FAULT_VERSION, not FAULT_STATE.
 * The version machinery never even runs, because the fingerprint comparison
 * happens first and refuses first.
 *
 * That ordering is what stops SemVerX from degenerating into the thing it was
 * built to replace.  Every existing package manager ultimately trusts a human
 * assertion about compatibility encoded in a number.  MMUKO treats the version
 * as policy and the fingerprint as fact, and never lets the policy admit what
 * the fact refuses.
 */

#include "mmuko/mmuko_desc.h"

/* Declared as well as defined: an exported symbol with no visible prototype
 * is exactly the undeclared contract this library exists to eliminate. */
MMUKO_PUBLIC double add(double a, double b);
MMUKO_PUBLIC int32_t mul(int32_t a, int32_t b);

MMUKO_PUBLIC double add(double a, double b)
{
    return a + b;
}

MMUKO_PUBLIC int32_t mul(int32_t a, int32_t b)
{
    return a * b;
}

MMUKO_SIG(sig_add, MMUKO_CC_CDECL, MMUKO_F64, MMUKO_F64, MMUKO_F64);
MMUKO_SIG(sig_mul, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);

static const mmuko_export_desc_t mathlib_v2s_exports[] = {
    MMUKO_EXPORT("add", sig_add, MMUKO_VER(2, STABLE, 0, STABLE, 0, STABLE), add),
    MMUKO_EXPORT("mul", sig_mul, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), mul)
};

MMUKO_MODULE(mathlib_v2s_desc, "mathlib",
             MMUKO_VER(2, STABLE, 0, STABLE, 0, STABLE),
             mathlib_v2s_exports);

MMUKO_QUERY_IMPL(mathlib_v2s_desc)
