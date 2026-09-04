/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * guarded_caller.c -- the same caller, with the MMUKO trident in the path.
 *
 * Compiled once, exactly like naive_caller.  The difference is that its
 * understanding of the contract is not only a prototype the compiler consumed
 * and discarded -- it is also a REQUIRED TABLE that survives into the binary
 * and can be compared, at run time, against what the library says it provides.
 *
 * Usage: guarded_caller <path-to-shared-object>
 */

#include "mmuko/mmuko_loader.h"
#include <stdio.h>

/* The caller's u1 hook: int32 add(int32, int32), cdecl, since 1.stable. */
MMUKO_SIG(req_add, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);

static const mmuko_export_desc_t required[] = {
    MMUKO_REQUIRE("add", req_add, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE))
};

typedef int32_t (*add_fn_t)(int32_t, int32_t);

int main(int argc, char **argv)
{
    mmuko_load_request_t req;
    mmuko_module_t mod;
    mmuko_load_result_t rc;
    char report[2048];

    if (argc < 2) {
        fprintf(stderr, "usage: %s <shared-object>\n", argv[0]);
        return 2;
    }

    mmuko_load_request_init(&req, argv[1], required, 1u);
    req.quarantine = 1;   /* report per-symbol rather than refusing wholesale */

    rc = mmuko_load(&req, &mod);
    if (rc != MMUKO_LOAD_OK && rc != MMUKO_LOAD_E_UNBOUND) {
        printf("guarded_caller: load refused (%s): %s\n",
               mmuko_load_result_name(rc), mod.err);
        return 1;
    }

    (void)mmuko_module_report(&mod, report, sizeof(report));
    fputs(report, stdout);

    {
        add_fn_t add = (add_fn_t)mmuko_module_sym(&mod, "add");
        if (add) {
            printf("guarded_caller: add(2, 3) = %d\n", add(2, 3));
        } else {
            const mmuko_trident_t *t = mmuko_module_slot(&mod, "add");
            printf("guarded_caller: REFUSED to call add -- %s\n",
                   t ? mmuko_fault_name(t->fault) : "no slot");
            printf("guarded_caller: the process is alive and no wrong answer "
                   "was produced.\n");
        }
    }

    mmuko_unload(&mod);
    return 0;
}
