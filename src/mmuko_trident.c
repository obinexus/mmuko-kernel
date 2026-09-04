/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_trident.c -- trident consensus resolver.
 * Freestanding.  No libc, no allocator.  This is the file that runs at ring 0.
 */

#include "mmuko/mmuko_trident.h"

/* ------------------------------------------------------------------------ */
/* Names                                                                     */
/* ------------------------------------------------------------------------ */

const char *mmuko_bind_state_name(mmuko_bind_state_t s)
{
    switch (s) {
    case MMUKO_BIND_UNRESOLVED: return "UNRESOLVED";
    case MMUKO_BIND_BOUND:      return "BOUND";
    case MMUKO_BIND_FAULT:      return "UNBOUND_FAULT";
    default:                    return "?";
    }
}

const char *mmuko_fault_name(mmuko_fault_t f)
{
    switch (f) {
    case MMUKO_FAULT_NONE:        return "none";
    case MMUKO_FAULT_MISSING:     return "missing-hook";
    case MMUKO_FAULT_ARCH:        return "arch-mismatch";
    case MMUKO_FAULT_CC:          return "calling-convention-mismatch";
    case MMUKO_FAULT_FINGERPRINT: return "abi-fingerprint-mismatch";
    case MMUKO_FAULT_VERSION:     return "semverx-version-mismatch";
    case MMUKO_FAULT_STATE:       return "semverx-state-refused";
    case MMUKO_FAULT_UPSTREAM:    return "upstream-fault-contained";
    case MMUKO_FAULT_MALFORMED:   return "malformed-descriptor";
    default:                      return "?";
    }
}

/* ------------------------------------------------------------------------ */
/* Trap                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Every unbound slot points here.
 *
 * A conforming caller checks t->state before calling and never reaches this.
 * The trap exists for the caller that does not: it converts "jump through a
 * pointer that was never validated" into a single, counted, recoverable event
 * at a known address, rather than a jump to NULL at an arbitrary later moment
 * or -- far worse -- a jump to a stale address that still contains executable
 * code of the wrong shape.
 *
 * It takes no arguments and returns int32.  Under both supported profiles the
 * CALLER cleans the argument area (SysV i386 cdecl and SysV AMD64), so
 * entering it through a pointer of any non-variadic signature does not
 * unbalance the stack.  The returned value is meaningful only for slots whose
 * declared return class is integer; for a float-returning slot the caller
 * reads XMM0/st0, which the trap does not write, so the value there is
 * unspecified.  That is why the trap COUNTER, not the trap's return value, is
 * the observable that tests and supervisors read.
 */
static volatile uint64_t g_trap_count = 0;

static int32_t mmuko_trident_trap(void)
{
    g_trap_count++;
    return MMUKO_TRAP_SENTINEL;
}

mmuko_fnptr_t mmuko_trident_trap_address(void)
{
    /* Function pointer to function pointer: fully defined by ISO C, provided
     * the value is called only through its true type.  Nothing here converts
     * a code address to void*. */
    return (mmuko_fnptr_t)mmuko_trident_trap;
}

uint64_t mmuko_trident_trap_count(void) { return g_trap_count; }
void     mmuko_trident_trap_reset(void) { g_trap_count = 0; }

/* ------------------------------------------------------------------------ */
/* Policy                                                                    */
/* ------------------------------------------------------------------------ */

mmuko_policy_t mmuko_policy_default(void)
{
    mmuko_policy_t p;
    p.arch                = MMUKO_ARCH_NATIVE;
    p.state_mask          = MMUKO_STATEMASK_STABLE;
    p.require_fingerprint = 1u;
    return p;
}

/* ------------------------------------------------------------------------ */
/* Node lifecycle                                                            */
/* ------------------------------------------------------------------------ */

void mmuko_trident_init(mmuko_trident_t *t,
                        const char *slot,
                        const mmuko_export_desc_t *u1_required)
{
    if (!t) return;
    t->slot        = slot;
    t->u1_required = u1_required;
    t->u2_provided = NULL;
    t->state       = MMUKO_BIND_UNRESOLVED;
    t->fault       = MMUKO_FAULT_NONE;
    t->fp_required = MMUKO_FP_ZERO;
    t->fp_provided = MMUKO_FP_ZERO;
    t->w_slot      = mmuko_trident_trap_address();
    t->generation  = 0;
    t->deps        = NULL;
    t->ndeps       = 0;
}

void mmuko_trident_set_deps(mmuko_trident_t *t,
                            struct mmuko_trident *const *deps,
                            uint32_t ndeps)
{
    if (!t) return;
    t->deps  = deps;
    t->ndeps = ndeps;
}

mmuko_fnptr_t mmuko_trident_address(const mmuko_trident_t *t)
{
    if (!t || t->state != MMUKO_BIND_BOUND) return NULL;
    return t->w_slot;
}

/* ------------------------------------------------------------------------ */
/* Resolution                                                                */
/* ------------------------------------------------------------------------ */

/*
 * The consensus rule, stated once:
 *
 *     bind  <=>  fingerprint(u1) == fingerprint(u2)  AND  policy accepts u2
 *
 * Everything below is that rule plus the ordering discipline that makes the
 * write to w_slot safe.
 *
 * DECIDE FIRST, PUBLISH LAST.  `next_slot` and `next_state` are computed
 * entirely in locals.  w_slot is assigned exactly once, at the very end.  A
 * concurrent reader of w_slot therefore observes either the previous value or
 * the new one, and both are addresses that passed consensus at the moment they
 * were written.  There is no window in which w_slot holds an unvalidated
 * address.  That is the whole of the hot-swap safety argument, and it is why
 * a fix can land under a running caller without a restart.
 */
mmuko_bind_state_t mmuko_trident_resolve(mmuko_trident_t *t,
                                         const mmuko_policy_t *policy)
{
    mmuko_policy_t      pol;
    mmuko_fault_t       fault = MMUKO_FAULT_NONE;
    mmuko_bind_state_t  next_state;
    mmuko_fnptr_t       next_slot;
    mmuko_fingerprint_t fp_req = MMUKO_FP_ZERO;
    mmuko_fingerprint_t fp_prv = MMUKO_FP_ZERO;
    uint32_t            i;
    int                 rc;

    if (!t) return MMUKO_BIND_FAULT;
    pol = policy ? *policy : mmuko_policy_default();

    /* --- containment: an upstream fault disqualifies this node outright,
     * before its own hooks are even examined.  This is what makes the fault
     * containment property transitive rather than merely local. */
    for (i = 0; i < t->ndeps; i++) {
        const mmuko_trident_t *d = t->deps[i];
        if (!d || d->state != MMUKO_BIND_BOUND) {
            fault = MMUKO_FAULT_UPSTREAM;
            goto publish;
        }
    }

    /* --- hooks present? */
    if (!t->u1_required || !t->u1_required->sig || !t->u1_required->symbol) {
        fault = MMUKO_FAULT_MALFORMED;
        goto publish;
    }
    if (!t->u2_provided) {
        fault = MMUKO_FAULT_MISSING;
        goto publish;
    }
    if (!t->u2_provided->sig || !t->u2_provided->symbol || !t->u2_provided->address) {
        fault = MMUKO_FAULT_MALFORMED;
        goto publish;
    }

    /* --- architecture profile.  Checked explicitly as well as through the
     * fingerprint so the diagnostic names the real cause; an mmuko32 table at
     * an mmuko64 site would otherwise report only "fingerprint mismatch". */
    if (pol.arch != MMUKO_ARCH_MMUKO32 && pol.arch != MMUKO_ARCH_MMUKO64) {
        fault = MMUKO_FAULT_ARCH;
        goto publish;
    }

    /* --- calling convention, likewise for the diagnostic. */
    if (t->u1_required->sig->cc != t->u2_provided->sig->cc) {
        fault = MMUKO_FAULT_CC;
        goto publish;
    }

    /* --- THE RULE.  Both hooks go through the identical encoder under the
     * identical architecture profile.  int32 add(int32,int32) and
     * f64 add(f64,f64) produce different canonical bytes and therefore
     * different fingerprints, and this is where the transcript's silent
     * corruption becomes a refusal. */
    rc = mmuko_fp_export(t->u1_required->symbol, t->u1_required->sig, pol.arch, &fp_req);
    if (rc != 0) { fault = MMUKO_FAULT_MALFORMED; goto publish; }
    rc = mmuko_fp_export(t->u2_provided->symbol, t->u2_provided->sig, pol.arch, &fp_prv);
    if (rc != 0) { fault = MMUKO_FAULT_MALFORMED; goto publish; }

    if (!mmuko_fp_equal(fp_req, fp_prv)) {
        fault = MMUKO_FAULT_FINGERPRINT;
        goto publish;
    }

    /* --- SemVerX.  Deliberately AFTER the fingerprint: ABI identity is a
     * machine fact and version compatibility is a policy claim, and a policy
     * claim must never be able to admit a contract the machine rejects.  A
     * module that bumped only its patch number and still changed a parameter
     * width has already been refused above. */
    {
        mmuko_semverx_result_t sv = mmuko_semverx_satisfies(&t->u1_required->since,
                                                            &t->u2_provided->since,
                                                            pol.state_mask);
        if (sv == MMUKO_SEMVERX_E_STATE) { fault = MMUKO_FAULT_STATE;   goto publish; }
        if (sv != MMUKO_SEMVERX_OK)      { fault = MMUKO_FAULT_VERSION; goto publish; }
    }

publish:
    if (fault == MMUKO_FAULT_NONE) {
        next_state = MMUKO_BIND_BOUND;
        next_slot  = t->u2_provided->address;
    } else {
        next_state = MMUKO_BIND_FAULT;
        next_slot  = mmuko_trident_trap_address();
    }

    t->fp_required = fp_req;
    t->fp_provided = fp_prv;
    t->fault       = fault;

    /* Monotone: the generation counter only ever advances, and only on a
     * transition INTO bound.  A caller that cached a pointer compares the
     * generation it saw against the current one to learn it must re-read the
     * slot.  It is never told; it never restarts. */
    if (next_state == MMUKO_BIND_BOUND && t->state != MMUKO_BIND_BOUND)
        t->generation++;

    t->state  = next_state;
    t->w_slot = next_slot;    /* single publication point */
    return next_state;
}

mmuko_bind_state_t mmuko_trident_swap(mmuko_trident_t *t,
                                      const mmuko_export_desc_t *u2_provided,
                                      const mmuko_policy_t *policy)
{
    if (!t) return MMUKO_BIND_FAULT;
    t->u2_provided = u2_provided;
    return mmuko_trident_resolve(t, policy);
}

/* ------------------------------------------------------------------------ */
/* Graph resolution                                                          */
/* ------------------------------------------------------------------------ */

/*
 * Fault containment, operationally.
 *
 * Each pass resolves every node.  A node whose dependency is not BOUND becomes
 * FAULT/UPSTREAM regardless of its own hooks, so a fault at any depth
 * propagates to everything downstream of it and to nothing else.
 *
 * Termination: the number of BOUND nodes is non-decreasing across passes
 * within a run (a node only becomes bound when its dependencies are bound, and
 * a bound dependency stays bound while the hooks are unchanged), and it is
 * bounded above by `count`.  The loop therefore runs at most `count` passes,
 * and exits as soon as a pass adds nothing.  With three edges per node this is
 * O(count) work per pass and O(count^2) worst case for a pathological chain
 * ordered backwards -- linear in practice because the loader emits nodes in
 * dependency order.
 */
int mmuko_trident_graph_resolve(mmuko_trident_t *const *nodes,
                                uint32_t count,
                                const mmuko_policy_t *policy,
                                uint32_t *bound_out,
                                uint32_t *fault_out)
{
    uint32_t pass, i;
    uint32_t bound = 0, faulted = 0;
    uint32_t prev_bound = 0xFFFFFFFFu;

    if (!nodes && count) return -1;

    for (pass = 0; pass <= count; pass++) {
        bound = 0;
        faulted = 0;
        for (i = 0; i < count; i++) {
            mmuko_bind_state_t s = mmuko_trident_resolve(nodes[i], policy);
            if (s == MMUKO_BIND_BOUND) bound++;
            else                       faulted++;
        }
        if (bound == prev_bound) break;   /* fixpoint */
        prev_bound = bound;
    }

    if (bound_out) *bound_out = bound;
    if (fault_out) *fault_out = faulted;
    return faulted == 0u ? 0 : -1;
}
