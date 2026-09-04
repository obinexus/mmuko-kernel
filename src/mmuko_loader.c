/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_loader.c -- hosted dynamic C ABI loader.
 *
 * The only non-freestanding translation unit in the library.  It needs the
 * platform's dynamic loader; the resolver it feeds does not.
 */

#include "mmuko/mmuko_loader.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
   /* GetProcAddress returns FARPROC, which is ALREADY a function pointer, so
    * the Win32 path never converts a code address to void* at all.  Keeping the
    * raw symbol type per platform rather than flattening both to void* is what
    * makes that difference expressible. */
   typedef FARPROC mmuko_rawsym_t;
#  define MMUKO_DLOPEN(p)     ((void *)LoadLibraryA(p))
#  define MMUKO_DLSYM(h, s)   GetProcAddress((HMODULE)(h), (s))
#  define MMUKO_DLCLOSE(h)    ((void)FreeLibrary((HMODULE)(h)))
#else
#  include <dlfcn.h>
   /* dlsym returns void*, so the POSIX path has one unavoidable conversion.
    * It happens in mmuko_sym_to_fnptr() below and nowhere else. */
   typedef void *mmuko_rawsym_t;
#  define MMUKO_DLOPEN(p)     dlopen((p), RTLD_NOW | RTLD_LOCAL)
#  define MMUKO_DLSYM(h, s)   dlsym((h), (s))
#  define MMUKO_DLCLOSE(h)    ((void)dlclose(h))
#endif

/*
 * The platform's raw symbol type, as an MMUKO callable address.
 *
 * This is the entire object-to-function pointer surface of the library, and on
 * Windows it is empty: FARPROC is a function pointer already, so the conversion
 * is function-to-function, which ISO C fully defines.
 *
 * On POSIX, dlsym hands back void* and somebody has to make a conversion ISO C
 * does not define.  Every ABI MMUKO targets does define it -- a dynamic loader
 * that could not return a callable address as void* could not exist -- so this
 * is the ground the library stands on rather than an oversight.  Doing it here,
 * once, means the descriptor layer and the resolver stay inside strict ISO C
 * and this function is the only place a reviewer has to accept the assumption.
 */
static mmuko_fnptr_t mmuko_sym_to_fnptr(mmuko_rawsym_t sym)
{
    union { mmuko_rawsym_t raw; mmuko_fnptr_t fn; } u;
    u.raw = sym;
    return u.fn;
}

const char *mmuko_load_result_name(mmuko_load_result_t r)
{
    switch (r) {
    case MMUKO_LOAD_OK:         return "ok";
    case MMUKO_LOAD_E_OPEN:     return "open-failed";
    case MMUKO_LOAD_E_NO_TABLE: return "no-abi-table";
    case MMUKO_LOAD_E_TABLE:    return "invalid-abi-table";
    case MMUKO_LOAD_E_ARCH:     return "arch-mismatch";
    case MMUKO_LOAD_E_CAPACITY: return "too-many-required-symbols";
    case MMUKO_LOAD_E_UNBOUND:  return "unbound-fault";
    case MMUKO_LOAD_E_ARGS:     return "bad-arguments";
    default:                    return "?";
    }
}

static void set_err(mmuko_module_t *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->err, sizeof(m->err), fmt, ap);
    va_end(ap);
}

void mmuko_load_request_init(mmuko_load_request_t *req,
                             const char *path,
                             const mmuko_export_desc_t *required,
                             uint32_t nrequired)
{
    if (!req) return;
    memset(req, 0, sizeof(*req));
    req->path       = path;
    req->required   = required;
    req->nrequired  = nrequired;
    req->policy     = mmuko_policy_default();
    req->quarantine = 0;
    /* min_version is left structurally invalid (all-zero states), which the
     * loader reads as "no module-level version constraint".
     *
     * That is deliberate, and it is not a weak default.  The module-level gate
     * is a POLICY control -- "I only deploy from the 1.stable line" -- and a
     * caller that has not expressed such a policy should not have one invented
     * for it.  Safety does not depend on it: every symbol the caller actually
     * uses is still gated by its own fingerprint, and no module version, however
     * permissive, can admit a symbol whose shape disagrees.  Callers that want
     * the line restriction set min_version explicitly. */
}

/* ------------------------------------------------------------------------ */
/* Interrogation: read the object's own account of itself                    */
/* ------------------------------------------------------------------------ */

static mmuko_load_result_t interrogate(void *handle,
                                       mmuko_arch_t arch,
                                       const mmuko_module_desc_t **out,
                                       mmuko_module_t *mod)
{
    union { mmuko_fnptr_t generic; mmuko_query_fn fn; } u;
    const mmuko_module_desc_t *desc;
    mmuko_desc_result_t dr;

    u.generic = mmuko_sym_to_fnptr(MMUKO_DLSYM(handle, MMUKO_QUERY_SYMBOL));
    if (!u.generic) {
        /* An object that exports no ABI table will not state its contract, so
         * there is nothing to hold it to.  This is the transcript's libmath.so
         * exactly: it changed shape between v1 and v2 and said nothing, and
         * there was no place in the system where it could have said anything.
         * MMUKO's answer is not to guess better -- it is to refuse to load an
         * object that has not declared what it provides. */
        set_err(mod, "object exports no %s: not an MMUKO ABI module",
                MMUKO_QUERY_SYMBOL);
        return MMUKO_LOAD_E_NO_TABLE;
    }

    desc = u.fn();
    dr = mmuko_desc_validate(desc, arch);
    if (dr != MMUKO_DESC_OK) {
        set_err(mod, "ABI table rejected: %s", mmuko_desc_result_name(dr));
        return (dr == MMUKO_DESC_E_ARCH) ? MMUKO_LOAD_E_ARCH : MMUKO_LOAD_E_TABLE;
    }

    *out = desc;
    return MMUKO_LOAD_OK;
}

/* ------------------------------------------------------------------------ */
/* Binding                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Build one trident per required symbol and resolve them all.
 *
 * The provider-side descriptor we hand to hook u2 is synthesised here: the
 * SIGNATURE and VERSION come from the object's own table, but the ADDRESS
 * comes from dlsym.  Both must be present.  Taking the address from the table
 * instead would trust a pointer the object computed about itself; taking the
 * signature from dlsym is impossible, which is the whole reason the table has
 * to exist.
 */
static void bind_all(mmuko_module_t *mod, const mmuko_policy_t *policy)
{
    uint32_t i;

    for (i = 0; i < mod->nslots; i++) {
        const mmuko_export_desc_t *req = mod->slots[i].u1_required;
        const mmuko_export_desc_t *prov = mmuko_desc_find(mod->provided, req->symbol);
        mmuko_fnptr_t addr = NULL;

        if (prov) addr = mmuko_sym_to_fnptr(MMUKO_DLSYM(mod->handle, req->symbol));

        if (prov && addr) {
            mod->resolved[i].symbol  = prov->symbol;
            mod->resolved[i].sig     = prov->sig;
            mod->resolved[i].since   = prov->since;
            mod->resolved[i].address = addr;
            mmuko_trident_swap(&mod->slots[i], &mod->resolved[i], policy);
        } else {
            /* Declared but not linkable, or not declared at all.  Either way
             * hook u2 is absent and the node cannot converge. */
            mmuko_trident_swap(&mod->slots[i], NULL, policy);
        }
    }

    mod->bound = 0;
    mod->faulted = 0;
    for (i = 0; i < mod->nslots; i++) {
        if (mod->slots[i].state == MMUKO_BIND_BOUND) mod->bound++;
        else                                          mod->faulted++;
    }
}

mmuko_load_result_t mmuko_load(const mmuko_load_request_t *req,
                               mmuko_module_t *out)
{
    mmuko_load_result_t rc;
    uint32_t i;

    if (!req || !out || !req->path) return MMUKO_LOAD_E_ARGS;
    if (req->nrequired > 0u && !req->required) return MMUKO_LOAD_E_ARGS;

    memset(out, 0, sizeof(*out));
    out->path = req->path;

    if (req->nrequired > MMUKO_LOADER_MAX_SLOTS) {
        set_err(out, "%u required symbols exceeds MMUKO_LOADER_MAX_SLOTS (%d)",
                req->nrequired, MMUKO_LOADER_MAX_SLOTS);
        return MMUKO_LOAD_E_CAPACITY;
    }

    /* 1. open.  RTLD_NOW so that unresolved symbols surface here, at a moment
     * we control, rather than at an arbitrary first call deep in the run. */
    out->handle = MMUKO_DLOPEN(req->path);
    if (!out->handle) {
#if defined(_WIN32)
        set_err(out, "LoadLibraryA(%s) failed (%lu)", req->path,
                (unsigned long)GetLastError());
#else
        set_err(out, "dlopen(%s) failed: %s", req->path, dlerror());
#endif
        return MMUKO_LOAD_E_OPEN;
    }

    /* 2 + 3. interrogate and validate. */
    rc = interrogate(out->handle, req->policy.arch, &out->provided, out);
    if (rc != MMUKO_LOAD_OK) {
        MMUKO_DLCLOSE(out->handle);
        out->handle = NULL;
        out->provided = NULL;
        return rc;
    }

    /* Every rejection from here on must drop `provided` along with the handle.
     * The table lives in the object's own read-only data, so closing the handle
     * unmaps it; a module struct left holding that pointer is a dangling read
     * for anyone who later inspects a refused load -- which is exactly what a
     * caller does when it wants to report WHY the load was refused. */
    if (req->module_name && !mmuko_streq(out->provided->module, req->module_name)) {
        set_err(out, "module name mismatch: wanted '%s', object says '%s'",
                req->module_name, out->provided->module);
        MMUKO_DLCLOSE(out->handle);
        out->handle = NULL;
        out->provided = NULL;
        return MMUKO_LOAD_E_TABLE;
    }

    /* Module-level version gate, before any per-symbol work. */
    if (mmuko_semverx_valid(&req->min_version)) {
        mmuko_semverx_result_t sv =
            mmuko_semverx_satisfies(&req->min_version, &out->provided->version,
                                    req->policy.state_mask);
        if (sv != MMUKO_SEMVERX_OK) {
            char want[MMUKO_SEMVERX_STRLEN], got[MMUKO_SEMVERX_STRLEN];
            mmuko_semverx_format(&req->min_version, want);
            mmuko_semverx_format(&out->provided->version, got);
            set_err(out, "module version refused: wanted >= %s, object is %s",
                    want, got);
            MMUKO_DLCLOSE(out->handle);
            out->handle = NULL;
            out->provided = NULL;
            return MMUKO_LOAD_E_TABLE;
        }
    }

    /* 4. converge. */
    out->nslots = req->nrequired;
    for (i = 0; i < out->nslots; i++) {
        mmuko_trident_init(&out->slots[i], req->required[i].symbol, &req->required[i]);
        out->slotp[i] = &out->slots[i];
    }
    bind_all(out, &req->policy);

    /* 5. publish. */
    if (out->faulted > 0u) {
        if (req->quarantine) {
            out->degraded = 1;
            set_err(out, "degraded: %u/%u bound, %u faulted (quarantine)",
                    out->bound, out->nslots, out->faulted);
            return MMUKO_LOAD_OK;
        }
        set_err(out, "%u of %u slots failed consensus", out->faulted, out->nslots);
        return MMUKO_LOAD_E_UNBOUND;
    }
    return MMUKO_LOAD_OK;
}

/* ------------------------------------------------------------------------ */
/* Hot swap                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Re-interrogate and rebind under a running caller.
 *
 * The caller's u1 table is untouched, so every trident keeps its identity and
 * its generation counter.  Slots that were faulted and are now satisfiable
 * flip to BOUND and their generation advances; slots already bound to an
 * unchanged contract keep theirs.  Nothing about the caller's text, stack or
 * heap changes.  This is the "publish the correct .state and the running
 * process heals" behaviour, at ABI granularity.
 *
 * The old handle is closed only AFTER the new one has produced a valid table,
 * so a failed reload leaves the module exactly as it was rather than
 * degrading a working process on the strength of a bad update.
 */
mmuko_load_result_t mmuko_reload(mmuko_module_t *mod,
                                 const char *path,
                                 const mmuko_policy_t *policy)
{
    void *new_handle;
    const mmuko_module_desc_t *new_desc = NULL;
    mmuko_load_result_t rc;
    void *old_handle;
    mmuko_policy_t pol;

    if (!mod) return MMUKO_LOAD_E_ARGS;
    pol = policy ? *policy : mmuko_policy_default();
    if (!path) path = mod->path;
    if (!path) return MMUKO_LOAD_E_ARGS;

    new_handle = MMUKO_DLOPEN(path);
    if (!new_handle) {
#if defined(_WIN32)
        set_err(mod, "reload: LoadLibraryA(%s) failed (%lu)", path,
                (unsigned long)GetLastError());
#else
        set_err(mod, "reload: dlopen(%s) failed: %s", path, dlerror());
#endif
        return MMUKO_LOAD_E_OPEN;   /* module untouched */
    }

    rc = interrogate(new_handle, pol.arch, &new_desc, mod);
    if (rc != MMUKO_LOAD_OK) {
        MMUKO_DLCLOSE(new_handle);
        return rc;                  /* module untouched */
    }

    old_handle    = mod->handle;
    mod->handle   = new_handle;
    mod->provided = new_desc;
    mod->path     = path;
    mod->err[0]   = '\0';
    mod->degraded = 0;

    bind_all(mod, &pol);

    if (old_handle && old_handle != new_handle) MMUKO_DLCLOSE(old_handle);

    if (mod->faulted > 0u) {
        set_err(mod, "reload: %u of %u slots failed consensus",
                mod->faulted, mod->nslots);
        return MMUKO_LOAD_E_UNBOUND;
    }
    return MMUKO_LOAD_OK;
}

void mmuko_unload(mmuko_module_t *mod)
{
    uint32_t i;
    if (!mod) return;
    /* Point every slot at the trap BEFORE the text is unmapped.  A caller that
     * still holds a slot pointer then traps instead of jumping into an address
     * whose pages have gone. */
    for (i = 0; i < mod->nslots; i++) {
        mod->slots[i].state       = MMUKO_BIND_FAULT;
        mod->slots[i].fault       = MMUKO_FAULT_MISSING;
        mod->slots[i].u2_provided = NULL;
        mod->slots[i].w_slot      = mmuko_trident_trap_address();
    }
    if (mod->handle) MMUKO_DLCLOSE(mod->handle);
    mod->handle   = NULL;
    mod->provided = NULL;
    mod->bound    = 0;
    mod->faulted  = mod->nslots;
}

/* ------------------------------------------------------------------------ */
/* Queries and reporting                                                     */
/* ------------------------------------------------------------------------ */

const mmuko_trident_t *mmuko_module_slot(const mmuko_module_t *mod,
                                         const char *symbol)
{
    uint32_t i;
    if (!mod || !symbol) return NULL;
    for (i = 0; i < mod->nslots; i++) {
        const mmuko_export_desc_t *r = mod->slots[i].u1_required;
        if (r && mmuko_streq(r->symbol, symbol)) return &mod->slots[i];
    }
    return NULL;
}

mmuko_fnptr_t mmuko_module_sym(const mmuko_module_t *mod, const char *symbol)
{
    const mmuko_trident_t *t = mmuko_module_slot(mod, symbol);
    return mmuko_trident_address(t);
}

uint32_t mmuko_module_report(const mmuko_module_t *mod, char *out, uint32_t cap)
{
    uint32_t n = 0;
    uint32_t i;
    char ver[MMUKO_SEMVERX_STRLEN];
    int w;

    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!mod) return 0;

    if (mod->provided) {
        mmuko_semverx_format(&mod->provided->version, ver);
        w = snprintf(out + n, cap - n,
                     "module %s@%s  arch=%s  %u/%u bound%s\n",
                     mod->provided->module, ver,
                     mmuko_arch_name((mmuko_arch_t)mod->provided->arch),
                     mod->bound, mod->nslots,
                     mod->degraded ? "  [DEGRADED]" : "");
    } else {
        w = snprintf(out + n, cap - n, "module <unloaded>  %s\n", mod->err);
    }
    if (w > 0) n += (uint32_t)w;
    if (n >= cap) return cap - 1;

    for (i = 0; i < mod->nslots && n < cap; i++) {
        const mmuko_trident_t *t = &mod->slots[i];
        char fr[MMUKO_FP_STRLEN], fp[MMUKO_FP_STRLEN];
        mmuko_fp_format(t->fp_required, fr);
        mmuko_fp_format(t->fp_provided, fp);
        w = snprintf(out + n, cap - n,
                     "  %-20s %-14s %-28s gen=%llu\n"
                     "      u1 required %s\n"
                     "      u2 provided %s\n",
                     t->slot ? t->slot : "?",
                     mmuko_bind_state_name(t->state),
                     mmuko_fault_name(t->fault),
                     (unsigned long long)t->generation,
                     fr, fp);
        if (w > 0) n += (uint32_t)w;
    }
    if (n >= cap) n = cap - 1;
    out[n] = '\0';
    return n;
}
