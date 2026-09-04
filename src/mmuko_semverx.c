/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_semverx.c -- SemVerX parsing, formatting and satisfaction.
 * Freestanding.  No libc.
 */

#include "mmuko/mmuko_semverx.h"

/* ------------------------------------------------------------------------ */
/* States                                                                    */
/* ------------------------------------------------------------------------ */

const char *mmuko_state_name(mmuko_state_t s)
{
    switch (s) {
    case MMUKO_STATE_EXPERIMENTAL: return "experimental";
    case MMUKO_STATE_BETA:         return "beta";
    case MMUKO_STATE_STABLE:       return "stable";
    case MMUKO_STATE_LEGACY:       return "legacy";
    case MMUKO_STATE_LTS:          return "lts";
    default:                       return "invalid";
    }
}

static int token_is(const char *tok, uint32_t len, const char *lit)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (lit[i] == '\0' || tok[i] != lit[i]) return 0;
    }
    return lit[len] == '\0';
}

mmuko_state_t mmuko_state_parse(const char *token, uint32_t len)
{
    if (!token || len == 0u) return MMUKO_STATE_INVALID;
    if (token_is(token, len, "experimental")) return MMUKO_STATE_EXPERIMENTAL;
    if (token_is(token, len, "beta"))         return MMUKO_STATE_BETA;
    if (token_is(token, len, "stable"))       return MMUKO_STATE_STABLE;
    if (token_is(token, len, "legacy"))       return MMUKO_STATE_LEGACY;
    if (token_is(token, len, "lts"))          return MMUKO_STATE_LTS;
    return MMUKO_STATE_INVALID;
}

/* ------------------------------------------------------------------------ */
/* Parse / format                                                            */
/* ------------------------------------------------------------------------ */

/* Consume one '.'-delimited field.  Returns the field length, or -1 at end. */
static int field(const char *s, uint32_t *pos)
{
    uint32_t start = *pos;
    while (s[*pos] != '\0' && s[*pos] != '.') (*pos)++;
    if (*pos == start) return -1;
    return (int)(*pos - start);
}

static int parse_u16(const char *s, uint32_t len, uint16_t *out)
{
    uint32_t v = 0, i;
    if (len == 0u || len > 5u) return -1;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > 65535u) return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

int mmuko_semverx_parse(const char *s, mmuko_semverx_t *out)
{
    uint32_t pos = 0;
    int      len;
    uint16_t nums[3];
    mmuko_state_t states[3];
    int i;

    if (!s || !out) return -1;

    /* major.state.minor.state.patch.state -- exactly six fields.  A shorter
     * form is rejected rather than defaulted: an omitted state is the very
     * ambiguity SemVerX exists to remove, so guessing one would reintroduce
     * it at the parser. */
    for (i = 0; i < 3; i++) {
        len = field(s, &pos);
        if (len < 0) return -2;
        if (parse_u16(s + pos - (uint32_t)len, (uint32_t)len, &nums[i]) != 0) return -3;
        if (s[pos] != '.') return -4;
        pos++;

        len = field(s, &pos);
        if (len < 0) return -5;
        states[i] = mmuko_state_parse(s + pos - (uint32_t)len, (uint32_t)len);
        if (states[i] == MMUKO_STATE_INVALID) return -6;

        if (i < 2) {
            if (s[pos] != '.') return -7;
            pos++;
        }
    }
    if (s[pos] != '\0') return -8;   /* trailing junk */

    out->major = nums[0];
    out->minor = nums[1];
    out->patch = nums[2];
    out->major_state = (uint8_t)states[0];
    out->minor_state = (uint8_t)states[1];
    out->patch_state = (uint8_t)states[2];
    out->_pad = 0;
    return 0;
}

void mmuko_semverx_format(const mmuko_semverx_t *v, char out[MMUKO_SEMVERX_STRLEN])
{
    mmuko_buf_t b;
    mmuko_buf_init(&b, out, MMUKO_SEMVERX_STRLEN);
    if (!v) { mmuko_buf_put(&b, "<null>"); return; }
    mmuko_buf_putu(&b, (uint64_t)v->major);
    mmuko_buf_putc(&b, '.');
    mmuko_buf_put(&b, mmuko_state_name((mmuko_state_t)v->major_state));
    mmuko_buf_putc(&b, '.');
    mmuko_buf_putu(&b, (uint64_t)v->minor);
    mmuko_buf_putc(&b, '.');
    mmuko_buf_put(&b, mmuko_state_name((mmuko_state_t)v->minor_state));
    mmuko_buf_putc(&b, '.');
    mmuko_buf_putu(&b, (uint64_t)v->patch);
    mmuko_buf_putc(&b, '.');
    mmuko_buf_put(&b, mmuko_state_name((mmuko_state_t)v->patch_state));
}

/* ------------------------------------------------------------------------ */
/* Comparison                                                                */
/* ------------------------------------------------------------------------ */

static int state_ok(uint8_t s)
{
    return s >= (uint8_t)MMUKO_STATE_EXPERIMENTAL && s <= (uint8_t)MMUKO_STATE_LTS;
}

int mmuko_semverx_valid(const mmuko_semverx_t *v)
{
    if (!v) return 0;
    return state_ok(v->major_state) && state_ok(v->minor_state) && state_ok(v->patch_state);
}

int mmuko_semverx_identical(const mmuko_semverx_t *a, const mmuko_semverx_t *b)
{
    if (!a || !b) return 0;
    return a->major == b->major && a->minor == b->minor && a->patch == b->patch
        && a->major_state == b->major_state
        && a->minor_state == b->minor_state
        && a->patch_state == b->patch_state;
}

int mmuko_semverx_cmp_numeric(const mmuko_semverx_t *a, const mmuko_semverx_t *b)
{
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Satisfaction                                                              */
/* ------------------------------------------------------------------------ */

mmuko_semverx_result_t mmuko_semverx_satisfies(const mmuko_semverx_t *required,
                                               const mmuko_semverx_t *provided,
                                               uint32_t state_mask)
{
    if (!required || !provided) return MMUKO_SEMVERX_E_MALFORMED;
    if (!mmuko_semverx_valid(required) || !mmuko_semverx_valid(provided))
        return MMUKO_SEMVERX_E_MALFORMED;

    /* Every state field of the provider must be inside the policy mask.  A
     * package whose patch line is experimental is an experimental package
     * however stable its major line claims to be. */
    if (!(state_mask & MMUKO_STATE_BIT((mmuko_state_t)provided->major_state)))
        return MMUKO_SEMVERX_E_STATE;
    if (!(state_mask & MMUKO_STATE_BIT((mmuko_state_t)provided->minor_state)))
        return MMUKO_SEMVERX_E_STATE;
    if (!(state_mask & MMUKO_STATE_BIT((mmuko_state_t)provided->patch_state)))
        return MMUKO_SEMVERX_E_STATE;

    /* Identity of the version line: major number AND major state.  This is the
     * rule that makes 4.17.15-stable and 4.17.15-legacy different packages
     * rather than the same package with a label. */
    if (required->major != provided->major)
        return MMUKO_SEMVERX_E_MAJOR;
    if (required->major_state != provided->major_state)
        return MMUKO_SEMVERX_E_STATE;

    /* Within the line, the provider must be at least as new. */
    if (provided->minor < required->minor) return MMUKO_SEMVERX_E_TOO_OLD;
    if (provided->minor == required->minor && provided->patch < required->patch)
        return MMUKO_SEMVERX_E_TOO_OLD;

    return MMUKO_SEMVERX_OK;
}

int mmuko_semverx_encode(const mmuko_semverx_t *v, mmuko_buf_t *out)
{
    if (!v || !out) return -1;
    if (!mmuko_semverx_valid(v)) return -2;
    mmuko_buf_putu(out, (uint64_t)v->major);
    mmuko_buf_putc(out, '.');
    mmuko_buf_put(out, mmuko_state_name((mmuko_state_t)v->major_state));
    mmuko_buf_putc(out, '.');
    mmuko_buf_putu(out, (uint64_t)v->minor);
    mmuko_buf_putc(out, '.');
    mmuko_buf_put(out, mmuko_state_name((mmuko_state_t)v->minor_state));
    mmuko_buf_putc(out, '.');
    mmuko_buf_putu(out, (uint64_t)v->patch);
    mmuko_buf_putc(out, '.');
    mmuko_buf_put(out, mmuko_state_name((mmuko_state_t)v->patch_state));
    return out->overflow ? -3 : 0;
}
