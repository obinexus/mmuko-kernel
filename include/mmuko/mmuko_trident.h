/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_trident.h -- trident consensus binding for the MMUKO ABI.
 *
 * The topology
 * ------------
 *
 *            u1      u2          u1 = REQUIRED contract (the caller's,
 *              \    /                 frozen at the caller's compile time)
 *               \  /             u2 = PROVIDED contract (the loaded object's,
 *                v                    read from its self-describing table)
 *                |                v  = convergence: the binding decision
 *                w                w  = propagation: the callable slot
 *
 * Two incoming hooks, one outgoing hook.  Constant degree, so a graph of these
 * costs exactly three edges per node and resolves in O(n).
 *
 * The consensus rule
 * ------------------
 *
 *     A trident node binds IF AND ONLY IF its two incoming contracts are
 *     identical.  Otherwise it stays UNBOUND and blocks propagation.
 *
 * Applied to a C ABI, "identical" means identical 128-bit export fingerprint:
 * same architecture profile, same calling convention, same return width and
 * class, same arity, same argument widths and classes, same symbol name.  The
 * transcript's break -- int add(int,int) replaced by double add(double,double)
 * behind an unchanged soname -- produces two different fingerprints on hooks
 * u1 and u2, so v never converges and w is never populated with the callee's
 * address.  The caller gets a defined fault instead of a plausible-looking
 * wrong number.
 *
 * What w points to when unbound
 * -----------------------------
 * Never NULL, and never a stale address.  It points at a trap thunk that
 * records the fault and returns a defined error.  A null slot would move the
 * failure to the caller's dereference, at an arbitrary later moment, with no
 * record of which contract disagreed.  A trap slot fails at the call, once,
 * with the fingerprints in hand.
 */

#ifndef MMUKO_TRIDENT_H
#define MMUKO_TRIDENT_H

#include "mmuko_abi.h"
#include "mmuko_semverx.h"
#include "mmuko_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Binding state                                                             */
/* ------------------------------------------------------------------------ */

typedef enum mmuko_bind_state {
    /* Not yet resolved.  Distinct from a fault: a node that has never been
     * asked is not a node that was asked and refused. */
    MMUKO_BIND_UNRESOLVED = 0,
    /* Both hooks present and identical.  w holds the provider's address. */
    MMUKO_BIND_BOUND      = 1,
    /* Resolved and refused.  w holds the trap.  See mmuko_fault_t. */
    MMUKO_BIND_FAULT      = 2
} mmuko_bind_state_t;

typedef enum mmuko_fault {
    MMUKO_FAULT_NONE        = 0,
    MMUKO_FAULT_MISSING     = 1,  /* a hook is absent; u2 unresolved symbol */
    MMUKO_FAULT_ARCH        = 2,  /* mmuko32 descriptor at an mmuko64 site */
    MMUKO_FAULT_CC          = 3,  /* calling convention differs */
    MMUKO_FAULT_FINGERPRINT = 4,  /* the shapes differ -- the transcript bug */
    MMUKO_FAULT_VERSION     = 5,  /* shape agrees, version line does not */
    MMUKO_FAULT_STATE       = 6,  /* SemVerX state outside the policy mask */
    MMUKO_FAULT_UPSTREAM    = 7,  /* a dependency is faulted: containment */
    MMUKO_FAULT_MALFORMED   = 8   /* a descriptor failed structural checks */
} mmuko_fault_t;

const char *mmuko_bind_state_name(mmuko_bind_state_t s);
const char *mmuko_fault_name(mmuko_fault_t f);

/* ------------------------------------------------------------------------ */
/* Binding policy                                                            */
/* ------------------------------------------------------------------------ */

typedef struct mmuko_policy {
    mmuko_arch_t arch;         /* the call site's architecture profile */
    uint32_t     state_mask;   /* MMUKO_STATEMASK_* */
    /* If non-zero, a provider whose version is NEWER than required in the same
     * major line is accepted only when the fingerprint is identical.  This is
     * always enforced; the flag exists to document that there is no "minor
     * bump therefore compatible" shortcut in MMUKO.  Reserved, must be 1. */
    uint32_t     require_fingerprint;
} mmuko_policy_t;

/* Default: native arch, stable only, fingerprint mandatory. */
mmuko_policy_t mmuko_policy_default(void);

/* ------------------------------------------------------------------------ */
/* Trident node                                                              */
/* ------------------------------------------------------------------------ */

typedef struct mmuko_trident {
    const char *slot;                     /* human name, for diagnostics */

    /* u1 -- required.  Owned by the caller, constant for the caller's life. */
    const mmuko_export_desc_t *u1_required;
    /* u2 -- provided.  Replaced on hot-swap. */
    const mmuko_export_desc_t *u2_provided;

    /* v -- convergence */
    mmuko_bind_state_t state;
    mmuko_fault_t      fault;
    mmuko_fingerprint_t fp_required;
    mmuko_fingerprint_t fp_provided;

    /* w -- propagation.  Never NULL after the first resolve. */
    mmuko_fnptr_t w_slot;
    /* Monotonic.  Increments on every transition INTO bound.  A caller that
     * caches a function pointer compares generations to learn it must re-read
     * the slot; it never has to be told, and it never has to restart. */
    uint64_t   generation;

    /* Containment: nodes this one depends upon.  If any is faulted, this node
     * cannot bind regardless of its own hooks. */
    struct mmuko_trident *const *deps;
    uint32_t                     ndeps;
} mmuko_trident_t;

/* ------------------------------------------------------------------------ */
/* Resolution                                                                */
/* ------------------------------------------------------------------------ */

/*
 * Initialise a node.  u2 may be NULL (nothing loaded yet); the node starts
 * UNRESOLVED with w pointing at the trap.
 */
void mmuko_trident_init(mmuko_trident_t *t,
                        const char *slot,
                        const mmuko_export_desc_t *u1_required);

/*
 * Resolve one node under `policy`.
 *
 * Publication order matters and is part of the safety argument: the decision
 * is computed entirely into locals, and `w_slot` is written exactly once, at
 * the end, to either the validated provider address or the trap.  There is no
 * instant at which w_slot holds an address that has not been validated, so a
 * concurrent reader either sees the previous slot or the new one, never a
 * half-swapped intermediate.  This is the Hot-Swap Safety property.
 *
 * Returns the resulting state.
 */
mmuko_bind_state_t mmuko_trident_resolve(mmuko_trident_t *t,
                                         const mmuko_policy_t *policy);

/*
 * Hot-swap the provided hook and re-resolve.  This is the "one line change in
 * the registry" from the SemVerX prototype, at ABI granularity: replace u2,
 * re-run consensus, and the already-running caller picks up the corrected
 * dependency at its next call through the slot.  No restart, no relink, no
 * reload of the caller's text.
 */
mmuko_bind_state_t mmuko_trident_swap(mmuko_trident_t *t,
                                      const mmuko_export_desc_t *u2_provided,
                                      const mmuko_policy_t *policy);

/* Attach dependency edges before resolving.  `deps` must outlive `t`. */
void mmuko_trident_set_deps(mmuko_trident_t *t,
                            struct mmuko_trident *const *deps,
                            uint32_t ndeps);

/* Read the callable slot.  Returns NULL if not bound -- callers that want the
 * trap semantics read t->w_slot directly instead. */
mmuko_fnptr_t mmuko_trident_address(const mmuko_trident_t *t);

/* ------------------------------------------------------------------------ */
/* Graph resolution and containment                                          */
/* ------------------------------------------------------------------------ */

/*
 * Resolve a set of nodes to a fixpoint, honouring dependency edges.
 *
 * Fault containment: a node whose dependency is faulted is itself faulted with
 * MMUKO_FAULT_UPSTREAM, transitively, so no faulted contract can reach code
 * downstream of it however many hops away that code is.  Monotone within a
 * pass and bounded by `count` iterations, so it terminates.
 *
 * Writes the number of bound nodes to *bound_out and faulted to *fault_out if
 * those pointers are non-NULL.  Returns 0 if every node bound, negative if any
 * node is faulted.
 */
int mmuko_trident_graph_resolve(mmuko_trident_t *const *nodes,
                                uint32_t count,
                                const mmuko_policy_t *policy,
                                uint32_t *bound_out,
                                uint32_t *fault_out);

/* ------------------------------------------------------------------------ */
/* Trap                                                                      */
/* ------------------------------------------------------------------------ */

/* The address every unbound slot points at.  Calling through it with any
 * signature is defined behaviour to the extent that it does not corrupt the
 * caller: it records the fault and returns a sentinel.  It never returns a
 * value that could be mistaken for a successful result. */
extern mmuko_fnptr_t mmuko_trident_trap_address(void);

/* Number of times any trap slot has been entered, process-wide. */
uint64_t mmuko_trident_trap_count(void);
void     mmuko_trident_trap_reset(void);

/* Sentinel returned by the trap in the integer register. */
#define MMUKO_TRAP_SENTINEL  ((int32_t)0x4D4B4F21)   /* 'MKO!' */

#ifdef __cplusplus
}
#endif

#endif /* MMUKO_TRIDENT_H */
