/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko-abi-dump -- print a shared object's declared ABI table.
 *
 *   mmuko-abi-dump libmathlib_v1.so
 *   mmuko-abi-dump libmathlib_v1.so libmathlib_v2.so     # compare two
 *
 * With two objects it diffs them symbol by symbol and names, for each symbol,
 * whether a caller compiled against the first can bind to the second. This is
 * the check a maintainer runs BEFORE publishing: it answers "what did I just
 * break, and for whom" while there is still time to choose a different answer.
 */

#include "mmuko/mmuko_desc.h"
#include "mmuko/mmuko_trident.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
   typedef FARPROC rawsym_t;              /* already a function pointer */
#  define DLOPEN(p)    ((void *)LoadLibraryA(p))
#  define DLSYM(h, s)  GetProcAddress((HMODULE)(h), (s))
#  define DLCLOSE(h)   ((void)FreeLibrary((HMODULE)(h)))
#  define DLERR()      "LoadLibrary failed"
#else
#  include <dlfcn.h>
   typedef void *rawsym_t;                /* dlsym returns void* */
#  define DLOPEN(p)    dlopen((p), RTLD_NOW | RTLD_LOCAL)
#  define DLSYM(h, s)  dlsym((h), (s))
#  define DLCLOSE(h)   ((void)dlclose(h))
#  define DLERR()      dlerror()
#endif

/* See the note in src/mmuko_loader.c: this is the only conversion of its kind,
 * and on Windows there is none. */
static mmuko_query_fn to_query_fn(rawsym_t sym)
{
    union { rawsym_t raw; mmuko_query_fn fn; } u;
    u.raw = sym;
    return u.fn;
}

/* A never-called stand-in, so the comparison path has a valid callable address
 * without any object-to-function pointer conversion. */
static void dummy_target(void) { }

static const mmuko_module_desc_t *open_table(const char *path, void **handle_out)
{
    mmuko_query_fn query;
    void *h = DLOPEN(path);
    const mmuko_module_desc_t *d;
    mmuko_desc_result_t rc;

    if (!h) {
        fprintf(stderr, "%s: cannot open: %s\n", path, DLERR());
        return NULL;
    }
    query = to_query_fn(DLSYM(h, MMUKO_QUERY_SYMBOL));
    if (!query) {
        fprintf(stderr, "%s: no %s -- not an MMUKO ABI module\n",
                path, MMUKO_QUERY_SYMBOL);
        DLCLOSE(h);
        return NULL;
    }
    d = query();
    rc = mmuko_desc_validate(d, MMUKO_ARCH_NONE);
    if (rc != MMUKO_DESC_OK) {
        fprintf(stderr, "%s: ABI table rejected: %s\n",
                path, mmuko_desc_result_name(rc));
        DLCLOSE(h);
        return NULL;
    }
    *handle_out = h;
    return d;
}

static void print_signature(const mmuko_fn_sig_t *sig, mmuko_arch_t arch)
{
    char store[MMUKO_CANON_MAX];
    mmuko_buf_t b;
    mmuko_buf_init(&b, store, sizeof(store));
    if (mmuko_encode_signature(sig, arch, &b) == 0) {
        fputs(b.data, stdout);
    } else {
        fputs("<unencodable>", stdout);
    }
}

static void dump(const char *path, const mmuko_module_desc_t *m)
{
    char ver[MMUKO_SEMVERX_STRLEN], fps[MMUKO_FP_STRLEN];
    mmuko_fingerprint_t mfp;
    uint32_t i;

    mmuko_semverx_format(&m->version, ver);
    printf("%s\n", path);
    printf("  module    %s@%s\n", m->module, ver);
    printf("  profile   %s   abi-rev %u\n",
           mmuko_arch_name((mmuko_arch_t)m->arch), m->abi_rev);
    if (mmuko_desc_fingerprint(m, &mfp) == 0) {
        mmuko_fp_format(mfp, fps);
        printf("  module fp %s\n", fps);
    }
    printf("  exports   %u\n\n", m->nexports);

    for (i = 0; i < m->nexports; i++) {
        const mmuko_export_desc_t *e = &m->exports[i];
        mmuko_fingerprint_t fp;
        mmuko_semverx_format(&e->since, ver);
        printf("  %-16s since %s\n", e->symbol, ver);
        if (mmuko_fp_export(e->symbol, e->sig, (mmuko_arch_t)m->arch, &fp) == 0) {
            mmuko_fp_format(fp, fps);
            printf("    fp      %s\n", fps);
        }
        printf("    canon   ");
        print_signature(e->sig, (mmuko_arch_t)m->arch);
        printf("\n");
    }
}

/*
 * The question a maintainer actually has: if someone compiled against A, can
 * they load B?
 *
 * Answered per symbol with the real resolver, not with a heuristic, so the
 * tool cannot disagree with the loader that will run in production.
 */
static int compare(const mmuko_module_desc_t *a, const mmuko_module_desc_t *b)
{
    mmuko_policy_t pol = mmuko_policy_default();
    uint32_t i;
    int breaking = 0;

    pol.arch = (mmuko_arch_t)a->arch;
    pol.state_mask = MMUKO_STATEMASK_ANY;   /* judge shape, not deployment policy */

    printf("\n=== can a caller compiled against the first bind to the second? ===\n\n");

    for (i = 0; i < a->nexports; i++) {
        const mmuko_export_desc_t *old = &a->exports[i];
        const mmuko_export_desc_t *new_ = mmuko_desc_find(b, old->symbol);
        mmuko_trident_t t;
        mmuko_export_desc_t probe;

        if (!new_) {
            printf("  %-16s REMOVED          every caller of it breaks\n", old->symbol);
            breaking = 1;
            continue;
        }

        /* The resolver wants a provider with a non-NULL address; supply this
         * tool's own entry point, since we are judging the contract and will
         * never call through it. */
        probe = *new_;
        probe.address = (mmuko_fnptr_t)dummy_target;

        mmuko_trident_init(&t, old->symbol, old);
        if (mmuko_trident_swap(&t, &probe, &pol) == MMUKO_BIND_BOUND) {
            printf("  %-16s compatible\n", old->symbol);
        } else {
            printf("  %-16s BREAKING         %s\n",
                   old->symbol, mmuko_fault_name(t.fault));
            breaking = 1;
        }
    }

    for (i = 0; i < b->nexports; i++) {
        if (!mmuko_desc_find(a, b->exports[i].symbol)) {
            printf("  %-16s added            no existing caller is affected\n",
                   b->exports[i].symbol);
        }
    }

    printf("\n");
    if (breaking) {
        printf("VERDICT: BREAKING.  This is a new major line, or the changed\n");
        printf("         symbols need new names beside the old ones.\n");
    } else {
        printf("VERDICT: compatible.  Existing callers bind to the new object.\n");
    }
    return breaking;
}

int main(int argc, char **argv)
{
    void *ha = NULL, *hb = NULL;
    const mmuko_module_desc_t *a, *b;
    int rc = 0;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <object> [<object-to-compare>]\n", argv[0]);
        return 2;
    }

    a = open_table(argv[1], &ha);
    if (!a) return 1;
    dump(argv[1], a);

    if (argc == 3) {
        b = open_table(argv[2], &hb);
        if (!b) { DLCLOSE(ha); return 1; }
        printf("\n");
        dump(argv[2], b);
        rc = compare(a, b);
        DLCLOSE(hb);
    }

    DLCLOSE(ha);
    return rc;
}
