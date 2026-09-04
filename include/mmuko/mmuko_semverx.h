/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_semverx.h -- SemVerX version identity for the MMUKO ABI.
 *
 * Classic SemVer:  major.minor.patch
 * SemVerX:         major.state.minor.state.patch.state
 *
 *   lodash@4.stable.17.beta.2.stable
 *
 * The .state field is part of the IDENTITY, not metadata attached to it.
 * 4.17.15-stable and 4.17.15-legacy are different packages to the resolver.
 * That is the whole point: a CI cache that quietly served a legacy build where
 * the developer tested against a stable one can no longer masquerade as the
 * same version, because the resolver never considered them the same version.
 *
 * Freestanding: no libc, no allocator, no locale.
 */

#ifndef MMUKO_SEMVERX_H
#define MMUKO_SEMVERX_H

#include <stdint.h>
#include "mmuko_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* State lattice                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Ordered by maturity, but the ordering is NOT a compatibility ordering.  It
 * exists so a policy can say "accept stable and above" or "accept beta and
 * above" with a single comparison.  Two versions with different states are
 * never the same version regardless of how the numbers compare.
 */
typedef enum mmuko_state {
    MMUKO_STATE_INVALID      = 0,
    MMUKO_STATE_EXPERIMENTAL = 1,
    MMUKO_STATE_BETA         = 2,
    MMUKO_STATE_STABLE       = 3,
    MMUKO_STATE_LEGACY       = 4,
    /* Long-term support: a stable line held open past the point where the
     * mainline moved on.  Distinct from STABLE because a caller may want one
     * and not the other, and identical to it in every other respect. */
    MMUKO_STATE_LTS          = 5
} mmuko_state_t;

/* Acceptance masks for a binding policy. */
#define MMUKO_STATE_BIT(s)        (1u << (unsigned)(s))
#define MMUKO_STATEMASK_STABLE    (MMUKO_STATE_BIT(MMUKO_STATE_STABLE) | \
                                   MMUKO_STATE_BIT(MMUKO_STATE_LTS))
#define MMUKO_STATEMASK_TESTING   (MMUKO_STATEMASK_STABLE | \
                                   MMUKO_STATE_BIT(MMUKO_STATE_BETA))
#define MMUKO_STATEMASK_DEV       (MMUKO_STATEMASK_TESTING | \
                                   MMUKO_STATE_BIT(MMUKO_STATE_EXPERIMENTAL))
#define MMUKO_STATEMASK_ANY       (MMUKO_STATEMASK_DEV | \
                                   MMUKO_STATE_BIT(MMUKO_STATE_LEGACY))

const char   *mmuko_state_name(mmuko_state_t s);
mmuko_state_t mmuko_state_parse(const char *token, uint32_t len);

/* ------------------------------------------------------------------------ */
/* Version                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct mmuko_semverx {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint8_t  major_state;   /* mmuko_state_t */
    uint8_t  minor_state;
    uint8_t  patch_state;
    uint8_t  _pad;
} mmuko_semverx_t;

/* Compound literal helper: MMUKO_VER(1, STABLE, 4, BETA, 0, STABLE) */
#define MMUKO_VER(MAJ, MS, MIN, NS, PAT, PS)                     \
    { (uint16_t)(MAJ), (uint16_t)(MIN), (uint16_t)(PAT),         \
      (uint8_t)(MMUKO_STATE_##MS), (uint8_t)(MMUKO_STATE_##NS),  \
      (uint8_t)(MMUKO_STATE_##PS), 0 }

/* Parse "4.stable.17.beta.2.stable".  Returns 0 on success, negative on a
 * malformed string.  Rejects partial forms: all six fields are required,
 * because an omitted state is exactly the ambiguity SemVerX exists to remove. */
int mmuko_semverx_parse(const char *s, mmuko_semverx_t *out);

/* Render into `out`.  Needs MMUKO_SEMVERX_STRLEN bytes. */
#define MMUKO_SEMVERX_STRLEN  64
void mmuko_semverx_format(const mmuko_semverx_t *v, char out[MMUKO_SEMVERX_STRLEN]);

/* Structural validity: every state in range, no zero states. */
int mmuko_semverx_valid(const mmuko_semverx_t *v);

/* Exact identity, all six fields. */
int mmuko_semverx_identical(const mmuko_semverx_t *a, const mmuko_semverx_t *b);

/* Ordering on (major, minor, patch) only; states are deliberately ignored
 * because they do not form a compatibility order.  -1 / 0 / +1. */
int mmuko_semverx_cmp_numeric(const mmuko_semverx_t *a, const mmuko_semverx_t *b);

/* ------------------------------------------------------------------------ */
/* Satisfaction                                                              */
/* ------------------------------------------------------------------------ */

typedef enum mmuko_semverx_result {
    MMUKO_SEMVERX_OK             = 0,
    MMUKO_SEMVERX_E_MALFORMED    = -1,
    MMUKO_SEMVERX_E_MAJOR        = -2,  /* different major line */
    MMUKO_SEMVERX_E_STATE        = -3,  /* state not in the accepted mask, or
                                         * major-state identity differs */
    MMUKO_SEMVERX_E_TOO_OLD      = -4   /* provided older than required */
} mmuko_semverx_result_t;

/*
 * Does `provided` satisfy a call site that requires `required`, under a policy
 * accepting the states in `state_mask`?
 *
 * Rules, in order:
 *   1. Both versions must be structurally valid.
 *   2. Every state field of `provided` must be in `state_mask`.
 *   3. major and major_state must be IDENTICAL.  The major state is part of
 *      the identity of the version line -- 2.legacy is not 2.stable.
 *   4. (minor, patch) of `provided` must be >= that of `required`.
 *
 * Note what this function is NOT.  It is not sufficient for binding.  Version
 * satisfaction is a policy question; ABI identity is a machine question, and
 * the machine question is decided first and independently by the fingerprint.
 * A module may bump only its patch number and still have broken the ABI; the
 * fingerprint catches that and no version rule ever overrides it.
 */
mmuko_semverx_result_t mmuko_semverx_satisfies(const mmuko_semverx_t *required,
                                               const mmuko_semverx_t *provided,
                                               uint32_t state_mask);

/* Canonical encoding contribution, for fingerprinting module identity. */
int mmuko_semverx_encode(const mmuko_semverx_t *v, mmuko_buf_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MMUKO_SEMVERX_H */
