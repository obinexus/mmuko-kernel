/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_loader.h -- hosted dynamic C ABI loader.
 *
 * This is the only part of the MMUKO ABI that is NOT freestanding: it needs
 * dlopen/dlsym (POSIX) or LoadLibrary/GetProcAddress (Win32).  The kernel-side
 * resolver in mmuko_trident.c has no such dependency and is what runs at
 * ring 0; this file is the userspace and early-boot module-loading front end
 * that feeds it.
 *
 * Sequence
 * --------
 *   1. open       -- dlopen(RTLD_NOW | RTLD_LOCAL).  RTLD_NOW so unresolved
 *                    symbols surface here rather than at first call.
 *   2. interrogate-- dlsym(MMUKO_QUERY_SYMBOL).  An object with no ABI table
 *                    is rejected: it will not state its contract, so it cannot
 *                    be held to one.
 *   3. validate   -- magic, revision, architecture, structural soundness.
 *   4. converge   -- one trident per required symbol; resolve them all.
 *   5. publish    -- the vtable is released to the caller only if EVERY
 *                    trident bound.  Consensus is per-module, not per-symbol,
 *                    under the default policy: a module that is half-correct
 *                    is a module whose author did not know what changed.
 *
 * Quarantine mode relaxes step 5: bound slots are usable, faulted slots hold
 * the trap, and the module reports as degraded.  Use it when a large module
 * has one unrelated broken export and the alternative is no service at all.
 */

#ifndef MMUKO_LOADER_H
#define MMUKO_LOADER_H

#include "mmuko_abi.h"
#include "mmuko_desc.h"
#include "mmuko_trident.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MMUKO_LOADER_MAX_SLOTS   64
#define MMUKO_LOADER_ERRLEN      256

typedef enum mmuko_load_result {
    MMUKO_LOAD_OK            =  0,
    MMUKO_LOAD_E_OPEN        = -1,  /* dlopen failed */
    MMUKO_LOAD_E_NO_TABLE    = -2,  /* no mmuko_abi_query_v1 */
    MMUKO_LOAD_E_TABLE       = -3,  /* table failed validation */
    MMUKO_LOAD_E_ARCH        = -4,
    MMUKO_LOAD_E_CAPACITY    = -5,  /* more required symbols than slots */
    MMUKO_LOAD_E_UNBOUND     = -6,  /* one or more tridents faulted */
    MMUKO_LOAD_E_ARGS        = -7
} mmuko_load_result_t;

const char *mmuko_load_result_name(mmuko_load_result_t r);

typedef struct mmuko_module {
    void                      *handle;      /* dlopen handle */
    const mmuko_module_desc_t *provided;    /* the object's u2 table */
    const char                *path;

    /* One trident per required symbol, in the caller's declaration order. */
    mmuko_trident_t            slots[MMUKO_LOADER_MAX_SLOTS];
    mmuko_trident_t           *slotp[MMUKO_LOADER_MAX_SLOTS];
    uint32_t                   nslots;

    /* Provider-side descriptors synthesised from the object's table with the
     * real dlsym address filled in.  Parallel to slots[]. */
    mmuko_export_desc_t        resolved[MMUKO_LOADER_MAX_SLOTS];

    uint32_t                   bound;
    uint32_t                   faulted;
    int                        degraded;    /* quarantine mode, partial bind */
    char                       err[MMUKO_LOADER_ERRLEN];
} mmuko_module_t;

typedef struct mmuko_load_request {
    const char                *path;        /* path to the shared object */
    const char                *module_name; /* expected module name, or NULL */
    const mmuko_export_desc_t *required;    /* the caller's u1 table */
    uint32_t                   nrequired;
    mmuko_semverx_t            min_version;
    mmuko_policy_t             policy;
    int                        quarantine;  /* allow partial bind */
} mmuko_load_request_t;

/* Sensible defaults: native arch, stable-only, no quarantine. */
void mmuko_load_request_init(mmuko_load_request_t *req,
                             const char *path,
                             const mmuko_export_desc_t *required,
                             uint32_t nrequired);

mmuko_load_result_t mmuko_load(const mmuko_load_request_t *req,
                               mmuko_module_t *out);

/* Re-interrogate the object at `path` and hot-swap every provided hook, then
 * re-resolve.  The caller's u1 table and its cached slot pointers stay valid
 * across this call; slots that heal have their generation incremented.
 *
 * This is the ABI-level form of the SemVerX prototype's registry hot-swap: the
 * already-running binary picks up a corrected dependency with no restart. */
mmuko_load_result_t mmuko_reload(mmuko_module_t *mod,
                                 const char *path,
                                 const mmuko_policy_t *policy);

void mmuko_unload(mmuko_module_t *mod);

/* Slot lookup by required symbol name.  NULL if the caller never required it. */
const mmuko_trident_t *mmuko_module_slot(const mmuko_module_t *mod,
                                         const char *symbol);

/* Fetch the callable address for a bound slot.  NULL when faulted -- callers
 * that prefer the trap read mmuko_module_slot(...)->w_slot instead. */
mmuko_fnptr_t mmuko_module_sym(const mmuko_module_t *mod, const char *symbol);

/* Human-readable dump of every slot, its two fingerprints and its verdict.
 * Writes at most `cap` bytes including NUL.  Hosted only (uses snprintf). */
uint32_t mmuko_module_report(const mmuko_module_t *mod, char *out, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MMUKO_LOADER_H */
