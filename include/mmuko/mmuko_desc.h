/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_desc.h -- self-describing ABI tables.
 *
 * Every MMUKO-conformant shared object exports exactly one well-known symbol:
 *
 *     const mmuko_module_desc_t *mmuko_abi_query_v1(void);
 *
 * That table is the object's u2 hook -- the contract it ACTUALLY provides,
 * emitted by the same compiler invocation that emitted the machine code, so it
 * cannot drift from the code the way a hand-maintained header can.
 *
 * A library without this symbol is not loadable.  That is not a limitation, it
 * is the point: an object that will not state its contract cannot be held to
 * one, and the transcript's libmath.so -- which states nothing and silently
 * changes shape between v1 and v2 -- is precisely that object.
 */

#ifndef MMUKO_DESC_H
#define MMUKO_DESC_H

#include "mmuko_abi.h"
#include "mmuko_semverx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MMUKO_QUERY_SYMBOL   "mmuko_abi_query_v1"

/* ------------------------------------------------------------------------ */
/* Export descriptor                                                         */
/* ------------------------------------------------------------------------ */

typedef struct mmuko_export_desc {
    const char             *symbol;   /* linker name */
    const mmuko_fn_sig_t   *sig;
    mmuko_semverx_t         since;    /* version at which this exact shape
                                       * first appeared */
    mmuko_fnptr_t           address;  /* provider side: the real function.
                                       * requirement side: NULL. */
} mmuko_export_desc_t;

/* ------------------------------------------------------------------------ */
/* Module descriptor                                                         */
/* ------------------------------------------------------------------------ */

typedef struct mmuko_module_desc {
    uint32_t                    magic;     /* MMUKO_ABI_MAGIC */
    uint32_t                    abi_rev;   /* MMUKO_ABI_REV */
    uint32_t                    arch;      /* mmuko_arch_t, as built */
    uint32_t                    nexports;
    const char                 *module;    /* "mathlib" */
    mmuko_semverx_t             version;
    const mmuko_export_desc_t  *exports;   /* nexports entries */
} mmuko_module_desc_t;

typedef const mmuko_module_desc_t *(*mmuko_query_fn)(void);

/* Structural validation of a table handed to us by a foreign object.  Checks
 * magic, revision, arch, non-NULL symbol names and signatures, arity bounds,
 * version validity, and duplicate symbol names.  Returns 0 or negative. */
typedef enum mmuko_desc_result {
    MMUKO_DESC_OK            =  0,
    MMUKO_DESC_E_NULL        = -1,
    MMUKO_DESC_E_MAGIC       = -2,
    MMUKO_DESC_E_REV         = -3,
    MMUKO_DESC_E_ARCH        = -4,
    MMUKO_DESC_E_EXPORT      = -5,
    MMUKO_DESC_E_VERSION     = -6,
    MMUKO_DESC_E_DUPLICATE   = -7
} mmuko_desc_result_t;

mmuko_desc_result_t mmuko_desc_validate(const mmuko_module_desc_t *m,
                                        mmuko_arch_t expect_arch);

const char *mmuko_desc_result_name(mmuko_desc_result_t r);

/* Linear lookup by symbol name.  NULL if absent. */
const mmuko_export_desc_t *mmuko_desc_find(const mmuko_module_desc_t *m,
                                           const char *symbol);

/* Fingerprint of a whole module: order-independent XOR-fold of every export
 * fingerprint, mixed with the module name and version.  Order independence is
 * deliberate -- reordering a declaration list must not be a breaking change. */
int mmuko_desc_fingerprint(const mmuko_module_desc_t *m,
                           mmuko_fingerprint_t *out);

/* ------------------------------------------------------------------------ */
/* Declaration macros                                                        */
/* ------------------------------------------------------------------------ */

/*
 * Provider side, in the shared object:
 *
 *     MMUKO_SIG(sig_add, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32)
 *
 *     static const mmuko_export_desc_t exports[] = {
 *         MMUKO_EXPORT("add", sig_add, MMUKO_VER(1,STABLE,0,STABLE,0,STABLE),
 *                      (void *)add),
 *     };
 *     MMUKO_MODULE(mathlib_desc, "mathlib",
 *                  MMUKO_VER(1,STABLE,0,STABLE,0,STABLE), exports)
 *     MMUKO_QUERY_IMPL(mathlib_desc)
 *
 * Consumer side, in the caller:
 *
 *     MMUKO_SIG(req_add, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32)
 *     static const mmuko_export_desc_t required[] = {
 *         MMUKO_REQUIRE("add", req_add,
 *                       MMUKO_VER(1,STABLE,0,STABLE,0,STABLE)),
 *     };
 *
 * Both sides go through the same encoder.  If the shapes differ by so much as
 * one argument's width, the fingerprints differ and the slot never binds.
 */

#define MMUKO_SIG(NAME, CC, RET, ...)                                        \
    static const mmuko_type_desc_t *const NAME##_args[] = { __VA_ARGS__ };   \
    static const mmuko_fn_sig_t NAME = {                                     \
        (uint32_t)(CC), 0u, (RET),                                           \
        (uint32_t)(sizeof(NAME##_args) / sizeof(NAME##_args[0])),            \
        NAME##_args                                                          \
    }

#define MMUKO_SIG0(NAME, CC, RET)                                            \
    static const mmuko_fn_sig_t NAME = {                                     \
        (uint32_t)(CC), 0u, (RET), 0u, NULL                                  \
    }

#define MMUKO_SIG_FLAGS(NAME, CC, FLAGS, RET, ...)                           \
    static const mmuko_type_desc_t *const NAME##_args[] = { __VA_ARGS__ };   \
    static const mmuko_fn_sig_t NAME = {                                     \
        (uint32_t)(CC), (uint32_t)(FLAGS), (RET),                            \
        (uint32_t)(sizeof(NAME##_args) / sizeof(NAME##_args[0])),            \
        NAME##_args                                                          \
    }

#define MMUKO_EXPORT(SYM, SIG, VER, ADDR)   { (SYM), &(SIG), VER, (mmuko_fnptr_t)(ADDR) }
#define MMUKO_REQUIRE(SYM, SIG, VER)        { (SYM), &(SIG), VER, NULL }

#define MMUKO_MODULE(NAME, MODNAME, VER, EXPORTS)                            \
    static const mmuko_module_desc_t NAME = {                                \
        MMUKO_ABI_MAGIC, MMUKO_ABI_REV, (uint32_t)MMUKO_ARCH_NATIVE,         \
        (uint32_t)(sizeof(EXPORTS) / sizeof((EXPORTS)[0])),                  \
        (MODNAME), VER, (EXPORTS)                                            \
    }

#if defined(_WIN32)
#  define MMUKO_PUBLIC __declspec(dllexport)
#elif defined(__GNUC__)
#  define MMUKO_PUBLIC __attribute__((visibility("default")))
#else
#  define MMUKO_PUBLIC
#endif

/* Declares as well as defines, so a fixture built with -Wmissing-prototypes
 * does not have to repeat the prototype by hand. */
#define MMUKO_QUERY_IMPL(DESC)                                               \
    MMUKO_PUBLIC const mmuko_module_desc_t *mmuko_abi_query_v1(void);        \
    MMUKO_PUBLIC const mmuko_module_desc_t *mmuko_abi_query_v1(void)         \
    { return &(DESC); }

/* Aggregate type helper. */
#define MMUKO_STRUCT_TY(NAME, SIZE, ALIGN, ...)                              \
    static const mmuko_type_desc_t *const NAME##_fields[] = { __VA_ARGS__ }; \
    static const mmuko_type_desc_t NAME = {                                  \
        (uint32_t)MMUKO_T_STRUCT, (uint32_t)(SIZE), (uint32_t)(ALIGN),       \
        (uint32_t)(sizeof(NAME##_fields) / sizeof(NAME##_fields[0])),        \
        NAME##_fields                                                        \
    }

#ifdef __cplusplus
}
#endif

#endif /* MMUKO_DESC_H */
