/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_abi_phase8.h -- PHASE 8: ABI NAMESPACE BRING-UP.
 *
 * Where this sits in the boot sequence
 * ------------------------------------
 *   PHASE 0  vacuum medium init
 *   PHASE 1  cubit ring initialisation
 *   PHASE 2  compass alignment
 *   PHASE 3  superposition entanglement
 *   PHASE 4  frame centering
 *   PHASE 5  nonlinear resolution
 *   PHASE 6  rotation verification
 *   PHASE 8  ABI NAMESPACE BRING-UP        <-- this file
 *   PHASE 7  BOOT COMPLETE
 *
 * Phases 0-6 bring the MACHINE MODEL up: the medium, the cubit rings, the frame
 * of reference, the guarantee that every cubit can still rotate.  At the end of
 * phase 6 the machine is coherent.
 *
 * Phase 8 brings the MODULE NAMESPACE up.  It establishes that this kernel can
 * bind a module's exports to a call site and be certain the two agree about the
 * machine contract.
 *
 * It runs BEFORE phase 7 declares the boot complete, and that ordering is the
 * argument.  A program is a thing that calls into modules.  A kernel that has
 * not established it can verify module contracts has no business launching one,
 * because the first thing that program does is make a call it cannot check.  So
 * a failure here is a BOOT failure -- `BOOT_ABI_UNBOUND` -- not a warning the
 * system carries forward.
 *
 * Self-hosting
 * ------------
 * Phase 8 is itself a module, and it does not get a free pass.  It declares
 * what it requires from the kernel -- `kputs` and `kputhex` -- and the kernel
 * hands it a table describing what it actually provides.  Those two contracts
 * are bound by tridents before phase 8 prints its first character.
 *
 * If the kernel's print functions do not match phase 8's declared expectations,
 * phase 8 refuses to run and the boot fails.  That is not ceremony: it is the
 * mechanism proving itself at the earliest moment there are two parties in the
 * system to disagree, using the same code path every later module will use.
 *
 * A structural note, offered as an observation and not a claim of identity:
 * phase 3 resolves cubit entanglement by requiring paired cubits (0<->7, 1<->6,
 * 2<->5) to agree, leaving indices 3 and 4 unpaired and therefore unresolved.
 * The trident's rule has the same shape one level up -- two incoming hooks must
 * agree or the node stays unbound.  The two mechanisms are independent; the
 * echo is worth noticing because it is the same discipline applied to a
 * different substrate.
 */

#ifndef MMUKO_ABI_PHASE8_H
#define MMUKO_ABI_PHASE8_H

#include <stdint.h>
#include "mmuko/mmuko_desc.h"
#include "mmuko/mmuko_trident.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* The kernel services contract                                              */
/* ------------------------------------------------------------------------ */

/*
 * What phase 8 requires of the kernel.  Written out as ordinary MMUKO
 * signatures so the kernel-to-module boundary is described in exactly the same
 * vocabulary as every other boundary in the system, and checked by exactly the
 * same resolver.
 *
 *   kputs   : void (cstr)   -- write a NUL-terminated string
 *   kputhex : void (u32)    -- write a 32-bit value in hex
 */
#define MMUKO_KSVC_MODULE   "mmuko-kernel-services"
#define MMUKO_KSVC_NEXPORTS 2

/* The kernel builds this table over its own print functions and passes it in.
 * Declared here rather than in the kernel so that both sides are reading the
 * same header, which is the only way the declaration can be a shared contract
 * rather than two independent guesses that happen to match today. */
const mmuko_module_desc_t *mmuko_ksvc_table(void);

/* ------------------------------------------------------------------------ */
/* Phase 8 result                                                            */
/* ------------------------------------------------------------------------ */

typedef enum mmuko_phase8_result {
    MMUKO_PHASE8_OK            = 0,
    /* The kernel's own services table failed validation.  Nothing further can
     * be trusted, including the ability to report the failure, so phase 8
     * returns without printing. */
    MMUKO_PHASE8_E_KSVC_TABLE  = -1,
    /* A kernel service slot did not converge: the kernel's print functions do
     * not match what phase 8 was compiled against. */
    MMUKO_PHASE8_E_KSVC_UNBOUND = -2,
    /* The demonstration namespace did not reach the state it must reach.  This
     * means the resolver itself is not behaving, which is the one failure that
     * must stop a boot outright. */
    MMUKO_PHASE8_E_RESOLVER    = -3
} mmuko_phase8_result_t;

const char *mmuko_phase8_result_name(mmuko_phase8_result_t r);

/*
 * Run phase 8.
 *
 * `ksvc` is the kernel's own services table.  Everything phase 8 prints goes
 * through slots bound from it, so a caller that hands over a table it does not
 * actually satisfy gets silence and a fault code rather than a jump into the
 * wrong function.
 */
mmuko_phase8_result_t mmuko_abi_phase8(const mmuko_module_desc_t *ksvc);

/* ------------------------------------------------------------------------ */
/* The arithmetic namespace the program uses                                 */
/* ------------------------------------------------------------------------ */

/*
 * After a successful phase 8, the kernel's `add` slot is bound and callable.
 * `mmuko_program_main` calls arithmetic THROUGH this slot rather than calling a
 * function directly, so that the boot's guarantee is load-bearing rather than
 * decorative: if the resolver ever stopped working, the program would stop
 * computing, and that is the correct coupling.
 *
 * Returns NULL if the slot is not bound.
 */
typedef int32_t (*mmuko_add_fn_t)(int32_t, int32_t);
mmuko_add_fn_t mmuko_abi_bound_add(void);

/* The ABI namespace's 128-bit identity, for the boot summary.  Sits alongside
 * the memory checksum: one says what the machine holds, the other says what the
 * module namespace agreed to. */
void mmuko_abi_namespace_fingerprint(uint32_t *hi_out, uint32_t *lo_out);

#ifdef __cplusplus
}
#endif

#endif /* MMUKO_ABI_PHASE8_H */
