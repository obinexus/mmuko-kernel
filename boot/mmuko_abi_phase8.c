/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_abi_phase8.c -- PHASE 8: ABI NAMESPACE BRING-UP.
 *
 * Freestanding.  No libc, no allocator, no FPU.  Everything below runs at ring 0
 * with the machine model already coherent (phases 0-6) but nothing yet launched.
 *
 * See mmuko_abi_phase8.h for why this phase exists and why it runs before
 * phase 7 rather than after it.
 */

#include "mmuko_abi_phase8.h"

/* ------------------------------------------------------------------------ */
/* Bound kernel services                                                     */
/* ------------------------------------------------------------------------ */

/*
 * Phase 8's u1 hooks: what it was compiled against.
 *
 * These are frozen into this translation unit at ITS compile time, exactly like
 * the machine code that pushes a pointer and expects nothing back.  Nothing
 * below ever rewrites them; that is the whole point of a required table.
 */
typedef void (*ksvc_puts_fn)(const char *);
typedef void (*ksvc_hex_fn)(uint32_t);

MMUKO_SIG(sig_kputs,   MMUKO_CC_CDECL, MMUKO_VOID, MMUKO_CSTR);
MMUKO_SIG(sig_kputhex, MMUKO_CC_CDECL, MMUKO_VOID, MMUKO_U32);

#define KSVC_V MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE)

static const mmuko_export_desc_t g_ksvc_required[MMUKO_KSVC_NEXPORTS] = {
    MMUKO_REQUIRE("kputs",   sig_kputs,   KSVC_V),
    MMUKO_REQUIRE("kputhex", sig_kputhex, KSVC_V)
};

static mmuko_trident_t g_ksvc_slot[MMUKO_KSVC_NEXPORTS];
static ksvc_puts_fn    P;    /* bound kputs   */
static ksvc_hex_fn     H;    /* bound kputhex */

/* ------------------------------------------------------------------------ */
/* Small freestanding output helpers, built on the BOUND services             */
/* ------------------------------------------------------------------------ */

static void put_u32(uint32_t v)
{
    char tmp[12];
    int i = 0;
    if (!P) return;
    if (v == 0) { P("0"); return; }
    while (v && i < (int)sizeof(tmp) - 1) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    {
        char out[12];
        int j = 0;
        while (i-- > 0) out[j++] = tmp[i];
        out[j] = '\0';
        P(out);
    }
}

static void put_fp(mmuko_fingerprint_t fp)
{
    char buf[MMUKO_FP_STRLEN];
    mmuko_fp_format(fp, buf);
    if (P) P(buf);
}

/* ------------------------------------------------------------------------ */
/* The demonstration namespace                                               */
/* ------------------------------------------------------------------------ */

/*
 * The transcript's two shapes, as ring-0 functions.
 *
 * `k_add_i32` is real and gets called.  `k_add_f64_stub` stands in for
 * `double add(double, double)`: this image is built with no SSE and no x87
 * because no FPU is initialised at this point in boot, so a genuine f64
 * function could not be compiled into it.  Nor does it need to be -- the
 * resolver decides entirely from the DESCRIPTOR, and the property under test is
 * that this address is never reached.  A stand-in that would misbehave if
 * entered is more honest than one that would quietly work.
 */
static int32_t k_add_i32(int32_t a, int32_t b) { return a + b; }
static int32_t k_mul_i32(int32_t a, int32_t b) { return a * b; }

static volatile int g_stub_entered = 0;
static void k_add_f64_stub(void) { g_stub_entered = 1; }

MMUKO_SIG(sig_ii_i, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);
MMUKO_SIG(sig_dd_d, MMUKO_CC_CDECL, MMUKO_F64, MMUKO_F64, MMUKO_F64);

#define V1  MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE)
#define V2X MMUKO_VER(2, EXPERIMENTAL, 0, STABLE, 0, STABLE)

/* What the kernel's arithmetic call site was compiled against. */
static const mmuko_export_desc_t g_req_add = MMUKO_REQUIRE("add", sig_ii_i, V1);
static const mmuko_export_desc_t g_req_mul = MMUKO_REQUIRE("mul", sig_ii_i, V1);

/* Candidate providers. */
static const mmuko_export_desc_t g_prov_add_v1  =
    MMUKO_EXPORT("add", sig_ii_i, V1,  k_add_i32);
static const mmuko_export_desc_t g_prov_add_v11 =
    MMUKO_EXPORT("add", sig_ii_i, V1,  k_add_i32);
static const mmuko_export_desc_t g_prov_add_bad =
    MMUKO_EXPORT("add", sig_dd_d, V2X, k_add_f64_stub);
static const mmuko_export_desc_t g_prov_mul_v1  =
    MMUKO_EXPORT("mul", sig_ii_i, V1,  k_mul_i32);

static mmuko_trident_t g_add_slot;
static mmuko_trident_t g_mul_slot;

/* Containment chain: mul depends on add, so a break in add must reach mul. */
static mmuko_trident_t *g_mul_deps[1];

static mmuko_fingerprint_t g_namespace_fp;

/* ------------------------------------------------------------------------ */
/* Reporting                                                                 */
/* ------------------------------------------------------------------------ */

const char *mmuko_phase8_result_name(mmuko_phase8_result_t r)
{
    switch (r) {
    case MMUKO_PHASE8_OK:             return "ok";
    case MMUKO_PHASE8_E_KSVC_TABLE:   return "kernel-services-table-invalid";
    case MMUKO_PHASE8_E_KSVC_UNBOUND: return "kernel-services-unbound";
    case MMUKO_PHASE8_E_RESOLVER:     return "resolver-misbehaved";
    default:                          return "?";
    }
}

static void report_slot(const mmuko_trident_t *t)
{
    if (!P) return;
    P("           ");
    P(t->slot ? t->slot : "?");
    P("  ");
    P(mmuko_bind_state_name(t->state));
    P("  ");
    P(mmuko_fault_name(t->fault));
    P("  gen=");
    put_u32((uint32_t)t->generation);
    P("\n");
}

static void report_hooks(const mmuko_trident_t *t)
{
    if (!P) return;
    P("             u1 required ");
    put_fp(t->fp_required);
    P("\n             u2 provided ");
    put_fp(t->fp_provided);
    P("\n");
}

/* ------------------------------------------------------------------------ */
/* Step 1 -- bind the kernel's own services                                  */
/* ------------------------------------------------------------------------ */

/*
 * Note the ordering constraint this step operates under: it cannot print
 * anything until it succeeds, because printing is the very thing it is binding.
 *
 * That is not an inconvenience to work around.  It is the honest situation of
 * every module bring-up: you do not get diagnostics from a facility until you
 * have established you may use it.  So a failure here returns a code and stays
 * silent, and the kernel -- which still has its own unbound print functions --
 * reports it.
 */
static mmuko_phase8_result_t bind_kernel_services(const mmuko_module_desc_t *ksvc)
{
    mmuko_policy_t pol = mmuko_policy_default();
    uint32_t i;

    P = NULL;
    H = NULL;

    if (mmuko_desc_validate(ksvc, MMUKO_ARCH_NATIVE) != MMUKO_DESC_OK)
        return MMUKO_PHASE8_E_KSVC_TABLE;

    for (i = 0; i < MMUKO_KSVC_NEXPORTS; i++) {
        const mmuko_export_desc_t *provided =
            mmuko_desc_find(ksvc, g_ksvc_required[i].symbol);
        mmuko_trident_init(&g_ksvc_slot[i], g_ksvc_required[i].symbol,
                           &g_ksvc_required[i]);
        if (mmuko_trident_swap(&g_ksvc_slot[i], provided, &pol) != MMUKO_BIND_BOUND)
            return MMUKO_PHASE8_E_KSVC_UNBOUND;
    }

    /* Published only after every slot converged.  Between entry and here there
     * is no instant at which P or H holds an address that has not passed
     * consensus. */
    P = (ksvc_puts_fn)mmuko_trident_address(&g_ksvc_slot[0]);
    H = (ksvc_hex_fn) mmuko_trident_address(&g_ksvc_slot[1]);
    if (!P || !H) return MMUKO_PHASE8_E_KSVC_UNBOUND;

    return MMUKO_PHASE8_OK;
}

/* ------------------------------------------------------------------------ */
/* Step 2 -- bring the arithmetic namespace up                               */
/* ------------------------------------------------------------------------ */

static int bring_up_namespace(void)
{
    mmuko_policy_t pol = mmuko_policy_default();
    mmuko_trident_t *nodes[2];
    uint32_t bound = 0, faulted = 0;

    mmuko_trident_init(&g_add_slot, "add", &g_req_add);
    mmuko_trident_init(&g_mul_slot, "mul", &g_req_mul);
    g_mul_deps[0] = &g_add_slot;
    mmuko_trident_set_deps(&g_mul_slot, g_mul_deps, 1);

    g_add_slot.u2_provided = &g_prov_add_v1;
    g_mul_slot.u2_provided = &g_prov_mul_v1;

    nodes[0] = &g_add_slot;
    nodes[1] = &g_mul_slot;

    if (mmuko_trident_graph_resolve(nodes, 2, &pol, &bound, &faulted) != 0)
        return -1;
    return (bound == 2u && faulted == 0u) ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* Step 3 -- prove the guarantee, here, at ring 0, before anything runs      */
/* ------------------------------------------------------------------------ */

/*
 * A boot-time self-test rather than a unit test.
 *
 * The hosted suites already prove the resolver is correct on a developer's
 * machine.  This proves it is behaving on THIS machine, in THIS image, with
 * whatever compiler and flags actually produced the running binary -- which is
 * a different claim, and the only one that matters to a system about to launch
 * a program.
 *
 * It costs microseconds and it runs every boot.
 */
static int prove_containment(void)
{
    mmuko_policy_t pol = mmuko_policy_default();
    mmuko_trident_t *nodes[2];
    uint32_t bound = 0, faulted = 0;
    uint64_t gen_before, traps_before;
    int ok = 1;

    nodes[0] = &g_add_slot;
    nodes[1] = &g_mul_slot;

    P("  [PHASE 8] self-test: the shape break, at ring 0\n");

    gen_before = g_add_slot.generation;

    /* The transcript's `ln -sf`: same symbol, different machine contract. */
    g_add_slot.u2_provided = &g_prov_add_bad;
    (void)mmuko_trident_graph_resolve(nodes, 2, &pol, &bound, &faulted);

    report_slot(&g_add_slot);
    report_hooks(&g_add_slot);
    report_slot(&g_mul_slot);

    if (g_add_slot.fault != MMUKO_FAULT_FINGERPRINT) {
        P("  [PHASE 8] FAIL: expected abi-fingerprint-mismatch\n");
        ok = 0;
    }
    if (mmuko_trident_address(&g_add_slot) != NULL) {
        P("  [PHASE 8] FAIL: a faulted slot exposed a callable address\n");
        ok = 0;
    }
    if (g_add_slot.w_slot == (mmuko_fnptr_t)k_add_f64_stub) {
        P("  [PHASE 8] FAIL: the mis-shaped function was reachable\n");
        ok = 0;
    }
    if (g_stub_entered != 0) {
        P("  [PHASE 8] FAIL: the mis-shaped function was entered\n");
        ok = 0;
    }
    if (g_add_slot.generation != gen_before) {
        P("  [PHASE 8] FAIL: a fault advanced the generation counter\n");
        ok = 0;
    }
    /* Containment: mul's own hooks are perfectly good and it must still fault. */
    if (g_mul_slot.fault != MMUKO_FAULT_UPSTREAM) {
        P("  [PHASE 8] FAIL: the fault did not propagate downstream\n");
        ok = 0;
    }

    /* Defence in depth: a caller that ignores the state and calls anyway must
     * hit a counted trap, not a mis-shaped callee. */
    traps_before = mmuko_trident_trap_count();
    {
        mmuko_add_fn_t f = (mmuko_add_fn_t)g_add_slot.w_slot;
        (void)f(2, 3);
    }
    if (mmuko_trident_trap_count() != traps_before + 1u) {
        P("  [PHASE 8] FAIL: the trap was not entered or not counted\n");
        ok = 0;
    }

    /* The heal.  Containment must be as reversible as it is strict, or the
     * first fault in a long-running system permanently disables everything
     * downstream and forces the restart this design exists to avoid. */
    P("  [PHASE 8] self-test: the heal, with nothing restarted\n");
    g_add_slot.u2_provided = &g_prov_add_v11;
    (void)mmuko_trident_graph_resolve(nodes, 2, &pol, &bound, &faulted);

    report_slot(&g_add_slot);
    report_slot(&g_mul_slot);

    if (bound != 2u || faulted != 0u) {
        P("  [PHASE 8] FAIL: the namespace did not heal\n");
        ok = 0;
    }
    if (g_add_slot.generation != gen_before + 1u) {
        P("  [PHASE 8] FAIL: the heal did not advance the generation counter\n");
        ok = 0;
    }
    {
        mmuko_add_fn_t f = mmuko_abi_bound_add();
        if (!f || f(20, 22) != 42) {
            P("  [PHASE 8] FAIL: the healed slot does not compute\n");
            ok = 0;
        }
    }

    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* Entry                                                                     */
/* ------------------------------------------------------------------------ */

mmuko_phase8_result_t mmuko_abi_phase8(const mmuko_module_desc_t *ksvc)
{
    mmuko_phase8_result_t rc;

    rc = bind_kernel_services(ksvc);
    if (rc != MMUKO_PHASE8_OK) return rc;   /* silent by necessity; see above */

    P("[PHASE 8] ABI namespace bring-up\n");
    P("  [PHASE 8] profile ");
    P(mmuko_arch_name(MMUKO_ARCH_NATIVE));
    P(", descriptor revision ");
    put_u32(MMUKO_ABI_REV);
    P("\n");

    P("  [PHASE 8] kernel services bound through tridents:\n");
    report_slot(&g_ksvc_slot[0]);
    report_slot(&g_ksvc_slot[1]);

    if (bring_up_namespace() != 0) {
        P("  [PHASE 8] FAIL: the arithmetic namespace did not converge\n");
        return MMUKO_PHASE8_E_RESOLVER;
    }
    P("  [PHASE 8] arithmetic namespace bound:\n");
    report_slot(&g_add_slot);
    report_hooks(&g_add_slot);
    report_slot(&g_mul_slot);

    if (prove_containment() != 0)
        return MMUKO_PHASE8_E_RESOLVER;

    /* Namespace identity, for the boot summary. */
    {
        mmuko_fingerprint_t a, m;
        (void)mmuko_fp_export(g_req_add.symbol, g_req_add.sig,
                              MMUKO_ARCH_NATIVE, &a);
        (void)mmuko_fp_export(g_req_mul.symbol, g_req_mul.sig,
                              MMUKO_ARCH_NATIVE, &m);
        /* Order-independent fold, matching mmuko_desc_fingerprint(). */
        g_namespace_fp.lo = a.lo ^ m.lo;
        g_namespace_fp.hi = a.hi ^ m.hi;
    }

    P("  [PHASE 8] namespace fingerprint ");
    put_fp(g_namespace_fp);
    P("\n[PHASE 8] ABI namespace ready: every call site has a verified contract\n");
    return MMUKO_PHASE8_OK;
}

mmuko_add_fn_t mmuko_abi_bound_add(void)
{
    return (mmuko_add_fn_t)mmuko_trident_address(&g_add_slot);
}

void mmuko_abi_namespace_fingerprint(uint32_t *hi_out, uint32_t *lo_out)
{
    /* Folded to 32 bits each so it sits beside the memory checksum in the boot
     * summary without dominating it.  The full 128 bits are printed above; this
     * is the at-a-glance form. */
    if (hi_out) *hi_out = (uint32_t)(g_namespace_fp.hi ^ (g_namespace_fp.hi >> 32));
    if (lo_out) *lo_out = (uint32_t)(g_namespace_fp.lo ^ (g_namespace_fp.lo >> 32));
}
