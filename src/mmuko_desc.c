/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_desc.c -- validation and fingerprinting of self-describing ABI tables.
 * Freestanding.
 */

#include "mmuko/mmuko_desc.h"

const char *mmuko_desc_result_name(mmuko_desc_result_t r)
{
    switch (r) {
    case MMUKO_DESC_OK:          return "ok";
    case MMUKO_DESC_E_NULL:      return "null-table";
    case MMUKO_DESC_E_MAGIC:     return "bad-magic";
    case MMUKO_DESC_E_REV:       return "unsupported-abi-revision";
    case MMUKO_DESC_E_ARCH:      return "arch-mismatch";
    case MMUKO_DESC_E_EXPORT:    return "malformed-export";
    case MMUKO_DESC_E_VERSION:   return "invalid-semverx";
    case MMUKO_DESC_E_DUPLICATE: return "duplicate-symbol";
    default:                     return "?";
    }
}

mmuko_desc_result_t mmuko_desc_validate(const mmuko_module_desc_t *m,
                                        mmuko_arch_t expect_arch)
{
    uint32_t i, j;

    if (!m) return MMUKO_DESC_E_NULL;

    /* Magic first.  Everything after this point dereferences pointers that
     * came out of a foreign object; the magic is the cheapest evidence that we
     * are looking at an MMUKO table at all and not at whatever else happened
     * to be exported under that name. */
    if (m->magic != MMUKO_ABI_MAGIC) return MMUKO_DESC_E_MAGIC;
    if (m->abi_rev != MMUKO_ABI_REV) return MMUKO_DESC_E_REV;

    if (m->arch != (uint32_t)MMUKO_ARCH_MMUKO32 &&
        m->arch != (uint32_t)MMUKO_ARCH_MMUKO64)
        return MMUKO_DESC_E_ARCH;
    if (expect_arch != MMUKO_ARCH_NONE && m->arch != (uint32_t)expect_arch)
        return MMUKO_DESC_E_ARCH;

    if (!m->module || m->module[0] == '\0') return MMUKO_DESC_E_NULL;
    if (!mmuko_semverx_valid(&m->version)) return MMUKO_DESC_E_VERSION;

    if (m->nexports > 0u && !m->exports) return MMUKO_DESC_E_EXPORT;

    for (i = 0; i < m->nexports; i++) {
        const mmuko_export_desc_t *e = &m->exports[i];
        mmuko_fingerprint_t probe;

        if (!e->symbol || e->symbol[0] == '\0') return MMUKO_DESC_E_EXPORT;
        if (!e->sig) return MMUKO_DESC_E_EXPORT;
        if (e->sig->nargs > MMUKO_MAX_ARGS) return MMUKO_DESC_E_EXPORT;
        if (!mmuko_semverx_valid(&e->since)) return MMUKO_DESC_E_VERSION;

        /* Encodability is part of structural validity: a signature the encoder
         * refuses can never be compared to anything, so a table containing one
         * is rejected here rather than producing a fingerprint fault later at
         * every call site that touches it. */
        if (mmuko_fp_export(e->symbol, e->sig, (mmuko_arch_t)m->arch, &probe) != 0)
            return MMUKO_DESC_E_EXPORT;

        /* Duplicate symbols would make lookup order-dependent, and an
         * order-dependent resolver is one that can bind differently on two
         * machines from the same inputs. */
        for (j = 0; j < i; j++) {
            if (mmuko_streq(m->exports[j].symbol, e->symbol))
                return MMUKO_DESC_E_DUPLICATE;
        }
    }
    return MMUKO_DESC_OK;
}

const mmuko_export_desc_t *mmuko_desc_find(const mmuko_module_desc_t *m,
                                           const char *symbol)
{
    uint32_t i;
    if (!m || !symbol || !m->exports) return NULL;
    for (i = 0; i < m->nexports; i++) {
        if (mmuko_streq(m->exports[i].symbol, symbol)) return &m->exports[i];
    }
    return NULL;
}

int mmuko_desc_fingerprint(const mmuko_module_desc_t *m,
                           mmuko_fingerprint_t *out)
{
    char storage[MMUKO_CANON_MAX];
    mmuko_buf_t buf;
    mmuko_fingerprint_t fold = MMUKO_FP_ZERO;
    mmuko_fingerprint_t head;
    uint32_t i;

    if (!m || !out) return -1;
    *out = MMUKO_FP_ZERO;

    /* Header: module identity and version. */
    mmuko_buf_init(&buf, storage, (uint32_t)sizeof(storage));
    mmuko_buf_put(&buf, "MOD=");
    mmuko_buf_put(&buf, m->module);
    mmuko_buf_put(&buf, "|A=");
    mmuko_buf_putu(&buf, (uint64_t)m->arch);
    mmuko_buf_put(&buf, "|V=");
    if (mmuko_semverx_encode(&m->version, &buf) != 0) return -2;
    if (buf.overflow) return -3;
    head = mmuko_fp_bytes(buf.data, buf.len);

    /* Body: XOR-fold of the export fingerprints.
     *
     * XOR is chosen precisely because it is order-independent.  Reordering a
     * declaration list changes nothing about the machine contract any caller
     * sees, so it must not change the module fingerprint; a sequential hash
     * would make a cosmetic reshuffle look like a breaking change and train
     * everyone to ignore the signal.  Duplicate symbols -- which XOR would
     * cancel to zero -- are rejected by mmuko_desc_validate() before this
     * function is reachable on any table we act upon. */
    for (i = 0; i < m->nexports; i++) {
        mmuko_fingerprint_t efp;
        if (mmuko_fp_export(m->exports[i].symbol, m->exports[i].sig,
                            (mmuko_arch_t)m->arch, &efp) != 0)
            return -4;
        fold.lo ^= efp.lo;
        fold.hi ^= efp.hi;
    }

    out->lo = head.lo ^ fold.lo;
    out->hi = head.hi ^ fold.hi;
    return 0;
}
