/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * test_loader.c -- end to end, against real shared objects.
 *
 * This is the transcript, executed.  The same sequence of events -- build v1,
 * link a caller against it, get 5; replace the library with v2 behind the same
 * name; run the SAME already-compiled caller -- but with the MMUKO loader
 * between the caller and the dynamic linker.
 *
 * Usage: test_loader <dir-containing-the-fixture-shared-objects>
 */

#include "mmuko/mmuko_loader.h"
#include "mmuko_test.h"

#include <stdio.h>
#include <string.h>

MMUKO_TEST_STATE_DEFS

static char g_dir[512] = "build";

/* Rotating buffers: several fixture paths are live at once in the hot-swap and
 * fingerprint cases, so a single static buffer would alias them. */
static const char *so(const char *stem)
{
    static char paths[4][1024];
    static unsigned turn = 0;
    char *path = paths[turn++ & 3u];
#if defined(_WIN32)
    snprintf(path, 1024, "%s\\%s.dll", g_dir, stem);
#elif defined(__APPLE__)
    snprintf(path, 1024, "%s/lib%s.dylib", g_dir, stem);
#else
    snprintf(path, 1024, "%s/lib%s.so", g_dir, stem);
#endif
    return path;
}

/* ------------------------------------------------------------------------ */
/* The caller's u1 table: what THIS binary was compiled against.             */
/*                                                                          */
/* Compiled once, frozen here, exactly like the machine code that pushes two */
/* 32-bit integers and reads the integer return register.  Nothing below     */
/* ever rewrites it -- that is the point of a required table.                */
/* ------------------------------------------------------------------------ */

MMUKO_SIG(req_sig_add, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);
MMUKO_SIG(req_sig_mul, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);

static const mmuko_export_desc_t g_required[] = {
    MMUKO_REQUIRE("add", req_sig_add, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE)),
    MMUKO_REQUIRE("mul", req_sig_mul, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE))
};
#define NREQ ((uint32_t)(sizeof(g_required) / sizeof(g_required[0])))

typedef int32_t (*add_fn_t)(int32_t, int32_t);

/* Distinct never-called addresses, for descriptors built by hand. Two real
 * functions rather than two invented integers: the address field holds a
 * function pointer, so an arbitrary integer would not fit it even nominally. */
static void stub_a(void) { }
static void stub_b(void) { }

static void request(mmuko_load_request_t *r, const char *path, int quarantine)
{
    mmuko_load_request_init(r, path, g_required, NREQ);
    r->module_name = "mathlib";
    r->quarantine  = quarantine;
    /* No module-level version constraint.  The point of these tests is that the
     * SHAPE check alone is sufficient: even when the breaking v2 is allowed
     * through the front door as a legitimate new release, the caller still
     * cannot reach a wrongly-shaped `add`. */
}

/* ------------------------------------------------------------------------ */

static void test_v1_loads_and_computes(void)
{
    mmuko_load_request_t req;
    mmuko_module_t mod;
    mmuko_load_result_t rc;

    MMUKO_CASE("v1 loads, every slot binds, add(2,3) == 5");

    request(&req, so("mathlib_v1"), 0);
    rc = mmuko_load(&req, &mod);
    MMUKO_CHECK_MSG(rc == MMUKO_LOAD_OK, "libmathlib_v1 loads cleanly", mod.err);
    if (rc != MMUKO_LOAD_OK) return;

    MMUKO_CHECK(mod.bound == NREQ && mod.faulted == 0u, "all required slots bound");
    MMUKO_CHECK(mod.provided && mmuko_streq(mod.provided->module, "mathlib"),
                "the object identifies itself as mathlib");

    {
        add_fn_t add = (add_fn_t)mmuko_module_sym(&mod, "add");
        MMUKO_CHECK(add != NULL, "add resolves to a callable address");
        MMUKO_CHECK(add && add(2, 3) == 5, "add(2,3) == 5, as in the transcript");
    }
    mmuko_unload(&mod);
    MMUKO_CHECK(mmuko_module_sym(&mod, "add") == NULL,
                "after unload every slot is unreachable, not dangling");
}

static void test_v2_is_refused(void)
{
    mmuko_load_request_t req;
    mmuko_module_t mod;
    mmuko_load_result_t rc;
    const mmuko_trident_t *slot;

    MMUKO_CASE("THE BREAK: v2 changes add's shape and the loader refuses it");

    request(&req, so("mathlib_v2"), 0);
    rc = mmuko_load(&req, &mod);

    /* In the transcript this is the moment the program silently starts
     * returning nonsense.  Here it is a load that does not complete. */
    MMUKO_CHECK(rc == MMUKO_LOAD_E_UNBOUND,
                "loading v2 against a v1-compiled caller fails consensus");
    MMUKO_CHECK(mod.faulted >= 1u, "at least one slot faulted");

    slot = mmuko_module_slot(&mod, "add");
    MMUKO_CHECK(slot != NULL, "the add slot exists for inspection");
    MMUKO_CHECK(slot && slot->fault == MMUKO_FAULT_FINGERPRINT,
                "and it names the cause: abi-fingerprint-mismatch");
    MMUKO_CHECK(mmuko_module_sym(&mod, "add") == NULL,
                "no callable address is ever exposed for the mis-shaped add");

    /* mul was untouched by the v2 release.  Its slot converged even though the
     * module as a whole was refused -- the diagnosis is per symbol, so the
     * author is told exactly what changed rather than that "v2 is broken". */
    slot = mmuko_module_slot(&mod, "mul");
    MMUKO_CHECK(slot && slot->state == MMUKO_BIND_BOUND,
                "mul, which did not change, still converged");

    mmuko_unload(&mod);
}

static void test_quarantine_partial_service(void)
{
    mmuko_load_request_t req;
    mmuko_module_t mod;

    MMUKO_CASE("quarantine: the sound half of a broken module stays usable");

    request(&req, so("mathlib_v2"), 1);
    MMUKO_CHECK(mmuko_load(&req, &mod) == MMUKO_LOAD_OK,
                "quarantine mode admits the module in a degraded state");
    MMUKO_CHECK(mod.degraded == 1, "the module reports itself degraded");

    {
        add_fn_t mul = (add_fn_t)mmuko_module_sym(&mod, "mul");
        MMUKO_CHECK(mul && mul(6, 7) == 42, "the sound symbol is fully usable");
    }
    MMUKO_CHECK(mmuko_module_sym(&mod, "add") == NULL,
                "the broken symbol remains unreachable inside a degraded module");
    mmuko_unload(&mod);
}

static void test_promotion_does_not_repair(void)
{
    mmuko_load_request_t req;
    mmuko_module_t mod;
    const mmuko_trident_t *slot;

    MMUKO_CASE("2.stable carries the same broken shape and is refused identically");

    request(&req, so("mathlib_v2_stable"), 1);
    (void)mmuko_load(&req, &mod);
    slot = mmuko_module_slot(&mod, "add");

    /* If a maintainer under pressure promotes the experimental release to
     * stable to make a deployment go through, nothing changes.  The version
     * was never what was being checked. */
    MMUKO_CHECK(slot && slot->state == MMUKO_BIND_FAULT,
                "promoting the release does not make the slot bind");
    MMUKO_CHECK(slot && slot->fault == MMUKO_FAULT_FINGERPRINT,
                "and the fault is still FINGERPRINT, never VERSION");
    mmuko_unload(&mod);
}

static void test_bare_object_refused(void)
{
    mmuko_load_request_t req;
    mmuko_module_t mod;

    MMUKO_CASE("an object with no ABI table cannot be loaded at all");

    request(&req, so("mathlib_bare"), 0);
    MMUKO_CHECK(mmuko_load(&req, &mod) == MMUKO_LOAD_E_NO_TABLE,
                "the transcript's undeclared libmath.so is refused, not guessed at");
    MMUKO_CHECK(mod.handle == NULL, "the refused object is closed, not left mapped");
    MMUKO_CHECK(mod.provided == NULL,
                "and its ABI table pointer is dropped with it -- inspecting a "
                "refused load must not read unmapped memory");
}

static void test_missing_file(void)
{
    mmuko_load_request_t req;
    mmuko_module_t mod;

    MMUKO_CASE("a missing object fails at open, with a message");

    request(&req, so("mathlib_does_not_exist"), 0);
    MMUKO_CHECK(mmuko_load(&req, &mod) == MMUKO_LOAD_E_OPEN, "dlopen failure is reported");
    MMUKO_CHECK(mod.err[0] != '\0', "and carries the platform's reason");
}

static void test_hot_swap_under_a_running_caller(void)
{
    mmuko_load_request_t req;
    mmuko_module_t mod;
    const mmuko_trident_t *slot;
    uint64_t gen_v1, gen_broken, gen_healed;

    MMUKO_CASE("hot-swap: break it and heal it under a caller that never restarts");

    /* --- the caller starts up and binds correctly. */
    request(&req, so("mathlib_v1"), 1);
    MMUKO_CHECK(mmuko_load(&req, &mod) == MMUKO_LOAD_OK, "start bound to v1");
    slot = mmuko_module_slot(&mod, "add");
    gen_v1 = slot ? slot->generation : 0u;
    {
        add_fn_t add = (add_fn_t)mmuko_module_sym(&mod, "add");
        MMUKO_CHECK(add && add(20, 22) == 42, "add(20,22) == 42");
    }

    /* --- someone publishes the breaking v2.  The transcript's `ln -sf`. */
    (void)mmuko_reload(&mod, so("mathlib_v2"), &req.policy);
    slot = mmuko_module_slot(&mod, "add");
    gen_broken = slot ? slot->generation : 0u;
    MMUKO_CHECK(slot && slot->state == MMUKO_BIND_FAULT,
                "the running caller's add slot faults the moment v2 arrives");
    MMUKO_CHECK(gen_broken == gen_v1,
                "the generation is unchanged: a fault is not a new binding");
    MMUKO_CHECK(mmuko_module_sym(&mod, "add") == NULL,
                "the caller can no longer obtain a wrong-shaped add");

    /* --- the fix lands: v1.1 keeps add's contract and adds addf beside it. */
    MMUKO_CHECK(mmuko_reload(&mod, so("mathlib_v1_1"), &req.policy) == MMUKO_LOAD_OK,
                "publishing v1.1 rebinds every slot");
    slot = mmuko_module_slot(&mod, "add");
    gen_healed = slot ? slot->generation : 0u;
    MMUKO_CHECK(gen_healed == gen_broken + 1u,
                "the generation advances so a caller holding a cached pointer "
                "knows to re-read the slot");
    {
        add_fn_t add = (add_fn_t)mmuko_module_sym(&mod, "add");
        MMUKO_CHECK(add && add(20, 22) == 42,
                    "the same process, never restarted, computes 42 again");
    }
    MMUKO_CHECK(mod.provided && mod.provided->version.minor == 1u,
                "and it is genuinely running the newer object");

    /* A failed reload must leave a working module working: refusing an update
     * is only useful if refusing it is also safe. */
    (void)mmuko_reload(&mod, so("mathlib_does_not_exist"), &req.policy);
    {
        add_fn_t add = (add_fn_t)mmuko_module_sym(&mod, "add");
        MMUKO_CHECK(add && add(20, 22) == 42,
                    "a reload that fails to open leaves the module untouched");
    }
    mmuko_unload(&mod);
}

static void test_module_version_gate(void)
{
    mmuko_load_request_t req;
    mmuko_module_t mod;

    MMUKO_CASE("module-level version and name gates run before any symbol work");

    request(&req, so("mathlib_v1"), 0);
    req.min_version = (mmuko_semverx_t)MMUKO_VER(1, STABLE, 9, STABLE, 0, STABLE);
    /* v1 is 1.stable.0.stable.0.stable, older than the 1.stable.9 required. */
    MMUKO_CHECK(mmuko_load(&req, &mod) == MMUKO_LOAD_E_TABLE,
                "an object older than min_version is refused at the module level");
    MMUKO_CHECK(mod.handle == NULL && mod.provided == NULL,
                "a version refusal unmaps the object and drops its table pointer");

    request(&req, so("mathlib_v1"), 0);
    req.module_name = "not-mathlib";
    MMUKO_CHECK(mmuko_load(&req, &mod) == MMUKO_LOAD_E_TABLE,
                "an object under the wrong module name is refused");
}

static void test_descriptor_validation(void)
{
    /* Hand-built tables that a hostile or merely broken object could present. */
    static const mmuko_type_desc_t *const a2[2] = { MMUKO_I32, MMUKO_I32 };
    static const mmuko_fn_sig_t sig =
        { (uint32_t)MMUKO_CC_CDECL, 0u, MMUKO_I32, 2u, a2 };
    static const mmuko_export_desc_t dup[2] = {
        { "add", &sig, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE),
          (mmuko_fnptr_t)stub_a },
        { "add", &sig, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE),
          (mmuko_fnptr_t)stub_b }
    };
    mmuko_module_desc_t m;

    MMUKO_CASE("foreign ABI tables are validated before a single pointer is trusted");

    m.magic = MMUKO_ABI_MAGIC; m.abi_rev = MMUKO_ABI_REV;
    m.arch = (uint32_t)MMUKO_ARCH_NATIVE; m.nexports = 1u; m.module = "m";
    m.version = (mmuko_semverx_t)MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE);
    m.exports = dup;
    MMUKO_CHECK(mmuko_desc_validate(&m, MMUKO_ARCH_NATIVE) == MMUKO_DESC_OK,
                "a well-formed table validates");

    m.nexports = 2u;
    MMUKO_CHECK(mmuko_desc_validate(&m, MMUKO_ARCH_NATIVE) == MMUKO_DESC_E_DUPLICATE,
                "duplicate symbols are rejected: lookup must not be order-dependent");

    m.nexports = 1u;
    m.magic = 0xDEADBEEFu;
    MMUKO_CHECK(mmuko_desc_validate(&m, MMUKO_ARCH_NATIVE) == MMUKO_DESC_E_MAGIC,
                "a table with the wrong magic is rejected");

    m.magic = MMUKO_ABI_MAGIC; m.abi_rev = 99u;
    MMUKO_CHECK(mmuko_desc_validate(&m, MMUKO_ARCH_NATIVE) == MMUKO_DESC_E_REV,
                "a future descriptor revision is rejected, not partially read");

    m.abi_rev = MMUKO_ABI_REV;
    m.arch = (MMUKO_ARCH_NATIVE == MMUKO_ARCH_MMUKO64)
           ? (uint32_t)MMUKO_ARCH_MMUKO32 : (uint32_t)MMUKO_ARCH_MMUKO64;
    MMUKO_CHECK(mmuko_desc_validate(&m, MMUKO_ARCH_NATIVE) == MMUKO_DESC_E_ARCH,
                "a table built for the other profile is rejected");

    m.arch = (uint32_t)MMUKO_ARCH_NATIVE;
    m.version.major_state = 0;
    MMUKO_CHECK(mmuko_desc_validate(&m, MMUKO_ARCH_NATIVE) == MMUKO_DESC_E_VERSION,
                "an invalid SemVerX state is rejected");

    MMUKO_CHECK(mmuko_desc_validate(NULL, MMUKO_ARCH_NATIVE) == MMUKO_DESC_E_NULL,
                "a NULL table is rejected");
}

static void test_module_fingerprint(void)
{
    mmuko_load_request_t req;
    mmuko_module_t v1, v2;
    mmuko_fingerprint_t f1, f2;

    MMUKO_CASE("module fingerprints distinguish releases");

    request(&req, so("mathlib_v1"), 1);
    (void)mmuko_load(&req, &v1);
    request(&req, so("mathlib_v2"), 1);
    (void)mmuko_load(&req, &v2);

    if (v1.provided && v2.provided) {
        MMUKO_CHECK(mmuko_desc_fingerprint(v1.provided, &f1) == 0 &&
                    mmuko_desc_fingerprint(v2.provided, &f2) == 0,
                    "both modules fingerprint");
        MMUKO_CHECK(!mmuko_fp_equal(f1, f2), "v1 and v2 have different module fingerprints");
    }
    mmuko_unload(&v1);
    mmuko_unload(&v2);
}

int main(int argc, char **argv)
{
    char report[4096];

    if (argc > 1) {
        strncpy(g_dir, argv[1], sizeof(g_dir) - 1u);
        g_dir[sizeof(g_dir) - 1u] = '\0';
    }

    MMUKO_SUITE_BEGIN("mmuko dynamic C ABI loader (end to end)");
    printf("  fixtures: %s   profile: %s\n", g_dir, mmuko_arch_name(MMUKO_ARCH_NATIVE));

    test_v1_loads_and_computes();
    test_v2_is_refused();
    test_quarantine_partial_service();
    test_promotion_does_not_repair();
    test_bare_object_refused();
    test_missing_file();
    test_hot_swap_under_a_running_caller();
    test_module_version_gate();
    test_descriptor_validation();
    test_module_fingerprint();

    /* Show one real report, so the diagnostic surface is exercised and a human
     * reading CI output can see what a refusal actually looks like. */
    {
        mmuko_load_request_t req;
        mmuko_module_t mod;
        request(&req, so("mathlib_v2"), 1);
        (void)mmuko_load(&req, &mod);
        (void)mmuko_module_report(&mod, report, sizeof(report));
        printf("\n--- consensus report for the breaking v2 ---\n%s", report);
        mmuko_unload(&mod);
    }

    printf("\n%u passed, %u failed  [test_loader, %s]\n",
           mmuko_test_passed, mmuko_test_failed, mmuko_arch_name(MMUKO_ARCH_NATIVE));
    return mmuko_test_failed == 0 ? 0 : 1;
}
