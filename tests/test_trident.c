/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * test_trident.c -- consensus, fault containment, hot-swap safety.
 *
 * These tests exercise the resolver directly, with hand-built descriptors and
 * no dynamic loading, so that the four properties can be checked in isolation
 * from dlopen's behaviour.  test_loader.c then checks the same properties end
 * to end against real shared objects.
 */

#include "mmuko/mmuko_trident.h"
#include "mmuko_test.h"
#include <stdio.h>

MMUKO_TEST_STATE_DEFS

/* ------------------------------------------------------------------------ */
/* Fixture functions -- the transcript's two shapes                          */
/* ------------------------------------------------------------------------ */

static int32_t add_i32(int32_t a, int32_t b) { return a + b; }
static double  add_f64(double a, double b)   { return a + b; }
static int32_t mul_i32(int32_t a, int32_t b) { return a * b; }

MMUKO_SIG(sig_ii_i, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);
MMUKO_SIG(sig_dd_d, MMUKO_CC_CDECL, MMUKO_F64, MMUKO_F64, MMUKO_F64);
MMUKO_SIG(sig_ii_i_stdcall, MMUKO_CC_STDCALL, MMUKO_I32, MMUKO_I32, MMUKO_I32);

#define V1  MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE)
#define V11 MMUKO_VER(1, STABLE, 1, STABLE, 0, STABLE)
#define V2X MMUKO_VER(2, EXPERIMENTAL, 0, STABLE, 0, STABLE)
#define V2S MMUKO_VER(2, STABLE, 0, STABLE, 0, STABLE)

/* u1: what the caller was compiled against.  int32 add(int32, int32) @ 1.stable */
static const mmuko_export_desc_t req_add   = MMUKO_REQUIRE("add", sig_ii_i, V1);

/* u2 candidates. */
static const mmuko_export_desc_t prov_v1   = MMUKO_EXPORT("add", sig_ii_i, V1,  add_i32);
static const mmuko_export_desc_t prov_v11  = MMUKO_EXPORT("add", sig_ii_i, V1,  add_i32);
static const mmuko_export_desc_t prov_v2x  = MMUKO_EXPORT("add", sig_dd_d, V2X, add_f64);
static const mmuko_export_desc_t prov_v2s  = MMUKO_EXPORT("add", sig_dd_d, V2S, add_f64);
static const mmuko_export_desc_t prov_wrongname =
    MMUKO_EXPORT("mul", sig_ii_i, V1, mul_i32);
static const mmuko_export_desc_t prov_stdcall =
    MMUKO_EXPORT("add", sig_ii_i_stdcall, V1, add_i32);
static const mmuko_export_desc_t prov_noaddr = MMUKO_EXPORT("add", sig_ii_i, V1, NULL);
static const mmuko_export_desc_t prov_old =
    MMUKO_EXPORT("add", sig_ii_i, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), add_i32);

typedef int32_t (*add_fn_t)(int32_t, int32_t);

/* ------------------------------------------------------------------------ */

static void test_consensus_binds(void)
{
    mmuko_trident_t t;
    mmuko_policy_t pol = mmuko_policy_default();

    MMUKO_CASE("consensus: identical contracts on both hooks bind");

    mmuko_trident_init(&t, "add", &req_add);
    MMUKO_CHECK(t.state == MMUKO_BIND_UNRESOLVED,
                "a fresh node is UNRESOLVED, not faulted");
    MMUKO_CHECK(t.w_slot == mmuko_trident_trap_address(),
                "an unresolved slot already points at the trap, never at NULL");

    MMUKO_CHECK(mmuko_trident_swap(&t, &prov_v1, &pol) == MMUKO_BIND_BOUND,
                "u1 == u2 binds");
    MMUKO_CHECK(t.fault == MMUKO_FAULT_NONE, "a bound node carries no fault");
    MMUKO_CHECK(mmuko_fp_equal(t.fp_required, t.fp_provided),
                "both hooks fingerprint identically");
    MMUKO_CHECK(t.generation == 1u, "the generation counter advanced on binding");
    MMUKO_CHECK(mmuko_trident_address(&t) == (mmuko_fnptr_t)add_i32,
                "the slot holds the provider's real address");

    {
        add_fn_t f = (add_fn_t)mmuko_trident_address(&t);
        MMUKO_CHECK(f && f(2, 3) == 5,
                    "calling through the bound slot gives 5 -- the transcript's "
                    "correct answer");
    }
}

static void test_the_transcript_fault(void)
{
    mmuko_trident_t t;
    mmuko_policy_t pol = mmuko_policy_default();
    uint64_t traps_before;

    MMUKO_CASE("THE BREAK: int32 add(int32,int32) meets double add(double,double)");

    mmuko_trident_init(&t, "add", &req_add);
    (void)mmuko_trident_swap(&t, &prov_v1, &pol);
    MMUKO_CHECK(t.state == MMUKO_BIND_BOUND, "the caller starts correctly bound to v1");

    /* The transcript's `ln -sf libmath_v2.so libmath.so`: same slot, same
     * symbol name, different machine contract behind it. */
    MMUKO_CHECK(mmuko_trident_swap(&t, &prov_v2x, &pol) == MMUKO_BIND_FAULT,
                "swapping in the double-shaped provider refuses to bind");
    MMUKO_CHECK(t.fault == MMUKO_FAULT_FINGERPRINT,
                "the fault is named: abi-fingerprint-mismatch");
    MMUKO_CHECK(!mmuko_fp_equal(t.fp_required, t.fp_provided),
                "the two hooks disagree");

    /* This is the property the whole library exists for.  Without it the slot
     * would still hold add_f64 and the caller would read EAX/RAX where the
     * callee wrote XMM0, returning a plausible number that is not a sum. */
    MMUKO_CHECK(mmuko_trident_address(&t) == NULL,
                "the faulted slot exposes no callable address");
    MMUKO_CHECK(t.w_slot != (mmuko_fnptr_t)add_f64,
                "the wrongly-shaped function is NEVER reachable through the slot");
    MMUKO_CHECK(t.w_slot == mmuko_trident_trap_address(),
                "the slot holds the trap instead");

    /* Defence in depth: a caller that ignores the state and calls anyway hits
     * a counted trap rather than a mis-shaped callee. */
    traps_before = mmuko_trident_trap_count();
    {
        add_fn_t f = (add_fn_t)t.w_slot;
        (void)f(2, 3);
    }
    MMUKO_CHECK(mmuko_trident_trap_count() == traps_before + 1u,
                "a caller that ignores the fault trips a counted trap");
}

static void test_promotion_cannot_repair_abi(void)
{
    mmuko_trident_t t;
    mmuko_policy_t pol = mmuko_policy_default();

    MMUKO_CASE("promoting experimental to stable does NOT repair an ABI break");

    mmuko_trident_init(&t, "add", &req_add);
    (void)mmuko_trident_swap(&t, &prov_v2s, &pol);

    /* The single most important ordering claim in the library.  If this ever
     * reported FAULT_VERSION or FAULT_STATE it would mean the version policy
     * had been consulted about a contract the machine already refused -- and a
     * policy that can be consulted can eventually be relaxed. */
    MMUKO_CHECK(t.state == MMUKO_BIND_FAULT, "2.stable with a changed shape still faults");
    MMUKO_CHECK(t.fault == MMUKO_FAULT_FINGERPRINT,
                "the fault is FINGERPRINT, not VERSION: fact precedes policy");
    MMUKO_CHECK(t.generation == 0u, "a node that never bound has generation 0");
}

static void test_version_and_state_gates(void)
{
    mmuko_trident_t t;
    mmuko_policy_t stable_only = mmuko_policy_default();
    mmuko_policy_t testing = mmuko_policy_default();
    static const mmuko_export_desc_t prov_beta =
        MMUKO_EXPORT("add", sig_ii_i, MMUKO_VER(1, STABLE, 2, BETA, 0, STABLE), add_i32);
    static const mmuko_export_desc_t req_newer =
        MMUKO_REQUIRE("add", sig_ii_i, MMUKO_VER(1, STABLE, 5, STABLE, 0, STABLE));

    testing.state_mask = MMUKO_STATEMASK_TESTING;

    MMUKO_CASE("version and state gates apply once the shape already agrees");

    mmuko_trident_init(&t, "add", &req_add);
    (void)mmuko_trident_swap(&t, &prov_beta, &stable_only);
    MMUKO_CHECK(t.state == MMUKO_BIND_FAULT && t.fault == MMUKO_FAULT_STATE,
                "a beta minor state is refused under a stable-only policy");
    MMUKO_CHECK(mmuko_fp_equal(t.fp_required, t.fp_provided),
                "...even though the two shapes agree perfectly");

    (void)mmuko_trident_resolve(&t, &testing);
    MMUKO_CHECK(t.state == MMUKO_BIND_BOUND,
                "the same provider binds under a testing policy");
    MMUKO_CHECK(t.generation == 1u, "loosening policy is a bind, and it counts");

    mmuko_trident_init(&t, "add-newer", &req_newer);
    (void)mmuko_trident_swap(&t, &prov_v1, &stable_only);
    MMUKO_CHECK(t.state == MMUKO_BIND_FAULT && t.fault == MMUKO_FAULT_VERSION,
                "a provider older than required faults on version");
}

static void test_structural_faults(void)
{
    mmuko_trident_t t;
    mmuko_policy_t pol = mmuko_policy_default();

    MMUKO_CASE("every other way the hooks can disagree is named separately");

    mmuko_trident_init(&t, "add", &req_add);
    (void)mmuko_trident_swap(&t, NULL, &pol);
    MMUKO_CHECK(t.state == MMUKO_BIND_FAULT && t.fault == MMUKO_FAULT_MISSING,
                "an absent u2 hook is MISSING, not a silent unresolved");

    (void)mmuko_trident_swap(&t, &prov_stdcall, &pol);
    MMUKO_CHECK(t.fault == MMUKO_FAULT_CC,
                "a calling-convention change is named CC, not FINGERPRINT");

    (void)mmuko_trident_swap(&t, &prov_wrongname, &pol);
    MMUKO_CHECK(t.fault == MMUKO_FAULT_FINGERPRINT,
                "a correctly shaped function under the wrong name still faults");

    (void)mmuko_trident_swap(&t, &prov_noaddr, &pol);
    MMUKO_CHECK(t.fault == MMUKO_FAULT_MALFORMED,
                "a descriptor with no address is malformed, not bound to NULL");

    {
        mmuko_policy_t bad_arch = pol;
        bad_arch.arch = MMUKO_ARCH_NONE;
        (void)mmuko_trident_swap(&t, &prov_v1, &bad_arch);
        MMUKO_CHECK(t.fault == MMUKO_FAULT_ARCH,
                    "an unspecified architecture profile faults on ARCH");
    }
}

static void test_hot_swap_heals(void)
{
    mmuko_trident_t t;
    mmuko_policy_t pol = mmuko_policy_default();
    uint64_t gen_after_break;

    MMUKO_CASE("hot-swap: a fix lands under a running caller and it heals");

    mmuko_trident_init(&t, "add", &req_add);
    (void)mmuko_trident_swap(&t, &prov_v1, &pol);
    MMUKO_CHECK(t.state == MMUKO_BIND_BOUND && t.generation == 1u, "bound to v1");

    (void)mmuko_trident_swap(&t, &prov_v2x, &pol);
    gen_after_break = t.generation;
    MMUKO_CHECK(t.state == MMUKO_BIND_FAULT, "the bad v2 lands and the slot faults");
    MMUKO_CHECK(gen_after_break == 1u,
                "a fault does NOT advance the generation -- it is not a new binding");

    /* "One line change in the registry": swap the hook.  In the loader this is
     * a re-interrogation of a replaced .so; here it is the same operation with
     * the dynamic linker taken out of the picture. */
    MMUKO_CHECK(mmuko_trident_swap(&t, &prov_v11, &pol) == MMUKO_BIND_BOUND,
                "publishing a correct provider re-binds the slot");
    MMUKO_CHECK(t.generation == 2u,
                "the generation advances so a caller holding a cached pointer "
                "knows to re-read");
    {
        add_fn_t f = (add_fn_t)mmuko_trident_address(&t);
        MMUKO_CHECK(f && f(2, 3) == 5, "the healed slot computes 5 again");
    }

    /* Monotone: re-resolving an already-bound node with unchanged hooks must
     * not churn the counter, or the "has this changed?" test becomes useless. */
    (void)mmuko_trident_resolve(&t, &pol);
    (void)mmuko_trident_resolve(&t, &pol);
    MMUKO_CHECK(t.generation == 2u,
                "re-resolving an unchanged bound node does not advance it");
}

static void test_fault_containment(void)
{
    /* A -> B -> C.  A is the leaf provider; C is furthest downstream. */
    mmuko_trident_t a, b, c;
    mmuko_trident_t *nodes[3];
    mmuko_policy_t pol = mmuko_policy_default();
    uint32_t bound = 0, faulted = 0;
    int rc;

    MMUKO_CASE("fault containment: no downstream node receives inconsistent state");

    mmuko_trident_init(&a, "A.add", &req_add);
    mmuko_trident_init(&b, "B.add", &req_add);
    mmuko_trident_init(&c, "C.add", &req_add);
    nodes[0] = &a; nodes[1] = &b; nodes[2] = &c;

    {
        static mmuko_trident_t *deps_b[1];
        static mmuko_trident_t *deps_c[1];
        deps_b[0] = &a; mmuko_trident_set_deps(&b, deps_b, 1);
        deps_c[0] = &b; mmuko_trident_set_deps(&c, deps_c, 1);
    }

    a.u2_provided = &prov_v1;
    b.u2_provided = &prov_v1;
    c.u2_provided = &prov_v1;

    rc = mmuko_trident_graph_resolve(nodes, 3, &pol, &bound, &faulted);
    MMUKO_CHECK(rc == 0 && bound == 3u && faulted == 0u,
                "a consistent chain binds end to end");

    /* Break the ROOT only.  B and C have perfectly good hooks of their own. */
    a.u2_provided = &prov_v2x;
    rc = mmuko_trident_graph_resolve(nodes, 3, &pol, &bound, &faulted);
    MMUKO_CHECK(rc != 0 && bound == 0u && faulted == 3u,
                "breaking the root faults the whole chain");
    MMUKO_CHECK(a.fault == MMUKO_FAULT_FINGERPRINT,
                "the root reports the real cause");
    MMUKO_CHECK(b.fault == MMUKO_FAULT_UPSTREAM && c.fault == MMUKO_FAULT_UPSTREAM,
                "downstream nodes report containment, not a cause of their own");
    MMUKO_CHECK(c.w_slot == mmuko_trident_trap_address(),
                "the furthest downstream slot holds the trap");
    MMUKO_CHECK(mmuko_trident_address(&c) == NULL,
                "no inconsistent contract reaches code two hops away");

    /* Heal the root.  Containment must be exactly as reversible as it was
     * strict, or the property is a one-way ratchet that eventually forces the
     * restart it was meant to avoid. */
    a.u2_provided = &prov_v11;
    rc = mmuko_trident_graph_resolve(nodes, 3, &pol, &bound, &faulted);
    MMUKO_CHECK(rc == 0 && bound == 3u, "healing the root heals the whole chain");
    MMUKO_CHECK(c.generation == 2u, "the downstream node counts its re-binding");
}

static void test_containment_is_ordering_independent(void)
{
    /* Same chain, nodes presented to the resolver in REVERSE dependency order.
     * A fixpoint loop is required precisely so the answer does not depend on
     * the order the loader happened to emit. */
    mmuko_trident_t a, b, c;
    mmuko_trident_t *nodes[3];
    static mmuko_trident_t *deps_b[1];
    static mmuko_trident_t *deps_c[1];
    mmuko_policy_t pol = mmuko_policy_default();
    uint32_t bound = 0, faulted = 0;

    MMUKO_CASE("resolution reaches the same fixpoint whatever the node order");

    mmuko_trident_init(&a, "A", &req_add);
    mmuko_trident_init(&b, "B", &req_add);
    mmuko_trident_init(&c, "C", &req_add);
    deps_b[0] = &a; mmuko_trident_set_deps(&b, deps_b, 1);
    deps_c[0] = &b; mmuko_trident_set_deps(&c, deps_c, 1);
    a.u2_provided = &prov_v1;
    b.u2_provided = &prov_v1;
    c.u2_provided = &prov_v1;

    nodes[0] = &c; nodes[1] = &b; nodes[2] = &a;   /* worst case ordering */
    MMUKO_CHECK(mmuko_trident_graph_resolve(nodes, 3, &pol, &bound, &faulted) == 0,
                "a backwards-ordered chain still binds completely");
    MMUKO_CHECK(bound == 3u && faulted == 0u, "all three nodes bound");
}

static void test_policy_and_naming(void)
{
    mmuko_policy_t p = mmuko_policy_default();

    MMUKO_CASE("defaults and diagnostics");

    MMUKO_CHECK(p.arch == MMUKO_ARCH_NATIVE, "the default policy targets the native profile");
    MMUKO_CHECK(p.state_mask == MMUKO_STATEMASK_STABLE,
                "the default policy is stable-only: opting in to risk is explicit");
    MMUKO_CHECK(p.require_fingerprint == 1u, "fingerprint checking is not optional");
    MMUKO_CHECK(mmuko_streq(mmuko_bind_state_name(MMUKO_BIND_FAULT), "UNBOUND_FAULT"),
                "the faulted state is named UNBOUND_FAULT, as in the topology");
    MMUKO_CHECK(mmuko_streq(mmuko_fault_name(MMUKO_FAULT_FINGERPRINT),
                            "abi-fingerprint-mismatch"),
                "faults have diagnosable names");
    MMUKO_CHECK(prov_old.address == (mmuko_fnptr_t)add_i32, "fixture wiring sanity");
}

int main(void)
{
    MMUKO_SUITE_BEGIN("mmuko trident consensus");
    mmuko_trident_trap_reset();
    test_consensus_binds();
    test_the_transcript_fault();
    test_promotion_cannot_repair_abi();
    test_version_and_state_gates();
    test_structural_faults();
    test_hot_swap_heals();
    test_fault_containment();
    test_containment_is_ordering_independent();
    test_policy_and_naming();
    printf("\n%u passed, %u failed  [test_trident, native=%s, traps=%llu]\n",
           mmuko_test_passed, mmuko_test_failed,
           mmuko_arch_name(MMUKO_ARCH_NATIVE),
           (unsigned long long)mmuko_trident_trap_count());
    return mmuko_test_failed == 0 ? 0 : 1;
}
