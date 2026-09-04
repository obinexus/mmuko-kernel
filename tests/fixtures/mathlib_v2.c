/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mathlib v2 -- 2.experimental.0.stable.0.stable
 *
 * THE BREAK.  This is the transcript's second version, verbatim in intent:
 *
 *     double add(double a, double b) { return a + b; }
 *
 * Same soname.  Same symbol name.  Same arity.  Completely different machine
 * contract:
 *
 *   mmuko64 (SysV AMD64)
 *     v1  add:  a in EDI, b in ESI, result in EAX
 *     v2  add:  a in XMM0, b in XMM1, result in XMM0
 *
 *   mmuko32 (SysV i386 cdecl)
 *     v1  add:  two 4-byte integers on the stack, result in EAX
 *     v2  add:  two 8-byte doubles on the stack, result on the x87 stack
 *
 * A caller compiled against v1 pushes integers and reads EAX.  Nothing in the
 * dynamic linker objects, because the dynamic linker matches on the NAME.  The
 * process keeps running and returns a number that is not the sum of anything.
 *
 * Under MMUKO the fingerprints of hooks u1 and u2 differ, the trident never
 * converges, and the slot is never populated with this function's address.
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

/* --- ABI table ---------------------------------------------------------- */

/* Note that this object is HONEST: it declares f64 add(f64, f64) truthfully.
 * The fault is not that anyone lied.  It is that a caller was compiled against
 * a different contract and, without MMUKO, nothing in the system was holding
 * the two accounts side by side.  The trident is that comparison. */
MMUKO_SIG(sig_add, MMUKO_CC_CDECL, MMUKO_F64, MMUKO_F64, MMUKO_F64);
MMUKO_SIG(sig_mul, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);

static const mmuko_export_desc_t mathlib_v2_exports[] = {
    MMUKO_EXPORT("add", sig_add,
                 MMUKO_VER(2, EXPERIMENTAL, 0, STABLE, 0, STABLE), add),
    MMUKO_EXPORT("mul", sig_mul,
                 MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), mul)
};

MMUKO_MODULE(mathlib_v2_desc, "mathlib",
             MMUKO_VER(2, EXPERIMENTAL, 0, STABLE, 0, STABLE),
             mathlib_v2_exports);

MMUKO_QUERY_IMPL(mathlib_v2_desc)
