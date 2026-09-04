/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mathlib v1.1 -- 1.stable.1.stable.0.stable -- THE HEAL.
 *
 * This is what the transcript's author should have shipped, and what MMUKO
 * makes the only shippable option.
 *
 * The requirement to expose double-precision addition is real.  The mistake in
 * v2 was to satisfy it by changing the machine contract of an EXISTING symbol.
 * Here the same requirement is met by adding a NEW symbol, `addf`, and leaving
 * `add` exactly as every already-compiled caller understands it.
 *
 * Under SemVerX this is a minor bump inside the same major line, 1.stable:
 * nothing that existed has changed shape, so nothing that exists can break.
 *
 * When this object replaces the broken v2 under a RUNNING caller, the caller's
 * faulted `add` slot re-converges on the next resolve and its generation
 * counter advances.  No restart, no relink, no reload of the caller's text --
 * which is the ABI-level form of the SemVerX prototype's registry hot-swap.
 */

#include "mmuko/mmuko_desc.h"

/* Declared as well as defined: an exported symbol with no visible prototype
 * is exactly the undeclared contract this library exists to eliminate. */
MMUKO_PUBLIC int32_t add(int32_t a, int32_t b);
MMUKO_PUBLIC int32_t mul(int32_t a, int32_t b);
MMUKO_PUBLIC double addf(double a, double b);

MMUKO_PUBLIC int32_t add(int32_t a, int32_t b)
{
    return a + b;
}

MMUKO_PUBLIC int32_t mul(int32_t a, int32_t b)
{
    return a * b;
}

/* The new capability, under a new name, with its own contract. */
MMUKO_PUBLIC double addf(double a, double b)
{
    return a + b;
}

/* --- ABI table ---------------------------------------------------------- */

MMUKO_SIG(sig_add,  MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);
MMUKO_SIG(sig_mul,  MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);
MMUKO_SIG(sig_addf, MMUKO_CC_CDECL, MMUKO_F64, MMUKO_F64, MMUKO_F64);

static const mmuko_export_desc_t mathlib_v1_1_exports[] = {
    /* `since` is 1.stable.0.stable.0.stable, not 1.stable.1.stable.0.stable:
     * this symbol's SHAPE has not changed since the module's first release,
     * and `since` records the version at which the shape appeared, not the
     * version of the object carrying it.  A caller that requires the 1.0.0
     * contract is satisfied by this object, which is the point. */
    MMUKO_EXPORT("add",  sig_add,  MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), add),
    MMUKO_EXPORT("mul",  sig_mul,  MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), mul),
    MMUKO_EXPORT("addf", sig_addf, MMUKO_VER(1, STABLE, 1, STABLE, 0, STABLE), addf)
};

MMUKO_MODULE(mathlib_v1_1_desc, "mathlib",
             MMUKO_VER(1, STABLE, 1, STABLE, 0, STABLE),
             mathlib_v1_1_exports);

MMUKO_QUERY_IMPL(mathlib_v1_1_desc)
