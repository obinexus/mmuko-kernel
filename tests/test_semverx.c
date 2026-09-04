/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * test_semverx.c -- major.state.minor.state.patch.state
 */

#include "mmuko/mmuko_semverx.h"
#include "mmuko_test.h"
#include <stdio.h>
#include <string.h>

MMUKO_TEST_STATE_DEFS

static mmuko_semverx_t V(uint16_t ma, mmuko_state_t ms,
                         uint16_t mi, mmuko_state_t ns,
                         uint16_t pa, mmuko_state_t ps)
{
    mmuko_semverx_t v;
    v.major = ma; v.minor = mi; v.patch = pa;
    v.major_state = (uint8_t)ms; v.minor_state = (uint8_t)ns; v.patch_state = (uint8_t)ps;
    v._pad = 0;
    return v;
}

static void test_parse_roundtrip(void)
{
    mmuko_semverx_t v;
    char out[MMUKO_SEMVERX_STRLEN];

    MMUKO_CASE("parse and format the six-field form");

    MMUKO_CHECK(mmuko_semverx_parse("4.stable.17.beta.2.stable", &v) == 0,
                "parses the article's lodash@4.stable.17.beta.2.stable");
    MMUKO_CHECK(v.major == 4 && v.minor == 17 && v.patch == 2,
                "numeric fields land in the right slots");
    MMUKO_CHECK(v.major_state == MMUKO_STATE_STABLE &&
                v.minor_state == MMUKO_STATE_BETA &&
                v.patch_state == MMUKO_STATE_STABLE,
                "each field carries its own independent state");

    mmuko_semverx_format(&v, out);
    MMUKO_CHECK_MSG(strcmp(out, "4.stable.17.beta.2.stable") == 0,
                    "format is the exact inverse of parse", out);

    MMUKO_CHECK(mmuko_semverx_parse("0.experimental.0.experimental.1.legacy", &v) == 0,
                "all four states are accepted");
}

static void test_parse_rejects_ambiguity(void)
{
    mmuko_semverx_t v;

    MMUKO_CASE("partial and malformed forms are refused, never defaulted");

    /* An omitted state is exactly the ambiguity SemVerX exists to remove.  A
     * parser that helpfully supplied "stable" would reintroduce it silently at
     * the one point in the system where it is cheapest to catch. */
    MMUKO_CHECK(mmuko_semverx_parse("4.17.2", &v) != 0,
                "classic three-field SemVer is rejected");
    MMUKO_CHECK(mmuko_semverx_parse("4.stable.17.beta", &v) != 0,
                "a truncated form is rejected");
    MMUKO_CHECK(mmuko_semverx_parse("4.stable.17.beta.2.stable.9", &v) != 0,
                "trailing junk is rejected");
    MMUKO_CHECK(mmuko_semverx_parse("4.golden.17.beta.2.stable", &v) != 0,
                "an unknown state name is rejected");
    MMUKO_CHECK(mmuko_semverx_parse("4.stable.17.beta.x.stable", &v) != 0,
                "a non-numeric version field is rejected");
    MMUKO_CHECK(mmuko_semverx_parse("70000.stable.0.stable.0.stable", &v) != 0,
                "a field beyond uint16 range is rejected");
    MMUKO_CHECK(mmuko_semverx_parse("", &v) != 0, "the empty string is rejected");
    MMUKO_CHECK(mmuko_semverx_parse(NULL, &v) != 0, "NULL is rejected");
}

static void test_state_is_identity(void)
{
    mmuko_semverx_t stable = V(4, MMUKO_STATE_STABLE, 17, MMUKO_STATE_STABLE,
                               15, MMUKO_STATE_STABLE);
    mmuko_semverx_t legacy = V(4, MMUKO_STATE_LEGACY, 17, MMUKO_STATE_STABLE,
                               15, MMUKO_STATE_STABLE);

    MMUKO_CASE("state is part of identity, not metadata attached to it");

    /* The article's central claim, as an executable assertion: 4.17.15-stable
     * and 4.17.15-legacy are different packages to the resolver, which is what
     * stops a cached legacy build masquerading as the stable one CI tested. */
    MMUKO_CHECK(!mmuko_semverx_identical(&stable, &legacy),
                "4.stable.17... and 4.legacy.17... are different versions");
    MMUKO_CHECK(mmuko_semverx_cmp_numeric(&stable, &legacy) == 0,
                "...even though their numbers compare equal");
    MMUKO_CHECK(mmuko_semverx_satisfies(&stable, &legacy, MMUKO_STATEMASK_ANY)
                    == MMUKO_SEMVERX_E_STATE,
                "a legacy build never satisfies a stable requirement");
}

static void test_satisfaction(void)
{
    mmuko_semverx_t need = V(1, MMUKO_STATE_STABLE, 2, MMUKO_STATE_STABLE,
                             0, MMUKO_STATE_STABLE);
    mmuko_semverx_t same   = V(1, MMUKO_STATE_STABLE, 2, MMUKO_STATE_STABLE,
                               0, MMUKO_STATE_STABLE);
    mmuko_semverx_t newer  = V(1, MMUKO_STATE_STABLE, 3, MMUKO_STATE_STABLE,
                               0, MMUKO_STATE_STABLE);
    mmuko_semverx_t older  = V(1, MMUKO_STATE_STABLE, 1, MMUKO_STATE_STABLE,
                               9, MMUKO_STATE_STABLE);
    mmuko_semverx_t nextmaj = V(2, MMUKO_STATE_STABLE, 0, MMUKO_STATE_STABLE,
                                0, MMUKO_STATE_STABLE);

    MMUKO_CASE("satisfaction within a version line");

    MMUKO_CHECK(mmuko_semverx_satisfies(&need, &same, MMUKO_STATEMASK_STABLE)
                    == MMUKO_SEMVERX_OK,
                "an exact match satisfies");
    MMUKO_CHECK(mmuko_semverx_satisfies(&need, &newer, MMUKO_STATEMASK_STABLE)
                    == MMUKO_SEMVERX_OK,
                "a newer minor in the same line satisfies");
    MMUKO_CHECK(mmuko_semverx_satisfies(&need, &older, MMUKO_STATEMASK_STABLE)
                    == MMUKO_SEMVERX_E_TOO_OLD,
                "an older minor does not satisfy, however high its patch");
    MMUKO_CHECK(mmuko_semverx_satisfies(&need, &nextmaj, MMUKO_STATEMASK_STABLE)
                    == MMUKO_SEMVERX_E_MAJOR,
                "a different major line does not satisfy");
}

static void test_policy_masks(void)
{
    mmuko_semverx_t need = V(1, MMUKO_STATE_STABLE, 0, MMUKO_STATE_STABLE,
                             0, MMUKO_STATE_STABLE);
    mmuko_semverx_t beta_patch = V(1, MMUKO_STATE_STABLE, 0, MMUKO_STATE_STABLE,
                                   3, MMUKO_STATE_BETA);
    mmuko_semverx_t exp_patch  = V(1, MMUKO_STATE_STABLE, 0, MMUKO_STATE_STABLE,
                                   3, MMUKO_STATE_EXPERIMENTAL);

    MMUKO_CASE("policy masks gate every state field independently");

    /* A package whose patch line is beta is a beta package, whatever its major
     * line claims.  Gating only the major state would let an unreviewed patch
     * ride into a stable-only deployment behind a stable major number. */
    MMUKO_CHECK(mmuko_semverx_satisfies(&need, &beta_patch, MMUKO_STATEMASK_STABLE)
                    == MMUKO_SEMVERX_E_STATE,
                "a beta patch state is refused under a stable-only policy");
    MMUKO_CHECK(mmuko_semverx_satisfies(&need, &beta_patch, MMUKO_STATEMASK_TESTING)
                    == MMUKO_SEMVERX_OK,
                "the same build is accepted under a testing policy");
    MMUKO_CHECK(mmuko_semverx_satisfies(&need, &exp_patch, MMUKO_STATEMASK_TESTING)
                    == MMUKO_SEMVERX_E_STATE,
                "experimental is still refused under a testing policy");
}

static void test_validity(void)
{
    mmuko_semverx_t bad = V(1, MMUKO_STATE_STABLE, 0, MMUKO_STATE_STABLE,
                            0, MMUKO_STATE_STABLE);

    MMUKO_CASE("structural validity");

    MMUKO_CHECK(mmuko_semverx_valid(&bad), "a well-formed version is valid");
    bad.minor_state = 0;
    MMUKO_CHECK(!mmuko_semverx_valid(&bad), "a zero state field is invalid");
    bad.minor_state = 99;
    MMUKO_CHECK(!mmuko_semverx_valid(&bad), "an out-of-range state field is invalid");
    MMUKO_CHECK(!mmuko_semverx_valid(NULL), "NULL is invalid");
}

int main(void)
{
    MMUKO_SUITE_BEGIN("mmuko SemVerX");
    test_parse_roundtrip();
    test_parse_rejects_ambiguity();
    test_state_is_identity();
    test_satisfaction();
    test_policy_masks();
    test_validity();
    printf("\n%u passed, %u failed  [test_semverx]\n",
           mmuko_test_passed, mmuko_test_failed);
    return mmuko_test_failed == 0 ? 0 : 1;
}
