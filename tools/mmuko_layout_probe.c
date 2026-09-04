/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_layout_probe.c -- report the C descriptor layout, for cross-language
 * verification.
 *
 * The Rust kernel and the C modules it loads pass descriptor structures across
 * the boundary by pointer. Rust's `#[repr(C)]` mirrors are written by hand, and
 * a hand-written mirror is a claim about layout that nothing checks -- until a
 * padding rule differs between the two compilers, or somebody adds a field to
 * one side, and the kernel starts reading a symbol name out of the middle of a
 * version number.
 *
 * That failure would be indistinguishable, at ring 0, from memory corruption.
 *
 * So the C side reports its own measured layout and the Rust differential
 * suite asserts against it. It is a small amount of machinery to catch a fault
 * that would otherwise present as an unrelated mystery.
 */

#include "mmuko/mmuko_loader.h"
#include <stddef.h>

/* Keys are frozen. Appending is allowed; renumbering is a breaking change to
 * this probe's own contract, which would be an unusually direct irony. */
enum {
    MMUKO_LAYOUT_TYPE_DESC_SIZE      = 1,
    MMUKO_LAYOUT_TYPE_DESC_CODE      = 2,
    MMUKO_LAYOUT_TYPE_DESC_SZ        = 3,
    MMUKO_LAYOUT_TYPE_DESC_ALIGN     = 4,
    MMUKO_LAYOUT_TYPE_DESC_NFIELDS   = 5,
    MMUKO_LAYOUT_TYPE_DESC_FIELDS    = 6,

    MMUKO_LAYOUT_FN_SIG_SIZE         = 10,
    MMUKO_LAYOUT_FN_SIG_CC           = 11,
    MMUKO_LAYOUT_FN_SIG_FLAGS        = 12,
    MMUKO_LAYOUT_FN_SIG_RET          = 13,
    MMUKO_LAYOUT_FN_SIG_NARGS        = 14,
    MMUKO_LAYOUT_FN_SIG_ARGS         = 15,

    MMUKO_LAYOUT_SEMVERX_SIZE        = 20,
    MMUKO_LAYOUT_SEMVERX_MAJOR       = 21,
    MMUKO_LAYOUT_SEMVERX_MINOR       = 22,
    MMUKO_LAYOUT_SEMVERX_PATCH       = 23,
    MMUKO_LAYOUT_SEMVERX_MAJOR_STATE = 24,
    MMUKO_LAYOUT_SEMVERX_MINOR_STATE = 25,
    MMUKO_LAYOUT_SEMVERX_PATCH_STATE = 26,

    MMUKO_LAYOUT_EXPORT_SIZE         = 30,
    MMUKO_LAYOUT_EXPORT_SYMBOL       = 31,
    MMUKO_LAYOUT_EXPORT_SIG          = 32,
    MMUKO_LAYOUT_EXPORT_SINCE        = 33,
    MMUKO_LAYOUT_EXPORT_ADDRESS      = 34,

    MMUKO_LAYOUT_MODULE_SIZE         = 40,
    MMUKO_LAYOUT_MODULE_MAGIC        = 41,
    MMUKO_LAYOUT_MODULE_ABI_REV      = 42,
    MMUKO_LAYOUT_MODULE_ARCH         = 43,
    MMUKO_LAYOUT_MODULE_NEXPORTS     = 44,
    MMUKO_LAYOUT_MODULE_NAME         = 45,
    MMUKO_LAYOUT_MODULE_VERSION      = 46,
    MMUKO_LAYOUT_MODULE_EXPORTS      = 47,

    MMUKO_LAYOUT_FINGERPRINT_SIZE    = 50,
    MMUKO_LAYOUT_FNPTR_SIZE          = 51,
    MMUKO_LAYOUT_VOIDPTR_SIZE        = 52
};

uint32_t mmuko_layout_probe(uint32_t key);

uint32_t mmuko_layout_probe(uint32_t key)
{
    switch (key) {
    case MMUKO_LAYOUT_TYPE_DESC_SIZE:    return (uint32_t)sizeof(mmuko_type_desc_t);
    case MMUKO_LAYOUT_TYPE_DESC_CODE:    return (uint32_t)offsetof(mmuko_type_desc_t, code);
    case MMUKO_LAYOUT_TYPE_DESC_SZ:      return (uint32_t)offsetof(mmuko_type_desc_t, size);
    case MMUKO_LAYOUT_TYPE_DESC_ALIGN:   return (uint32_t)offsetof(mmuko_type_desc_t, align);
    case MMUKO_LAYOUT_TYPE_DESC_NFIELDS: return (uint32_t)offsetof(mmuko_type_desc_t, nfields);
    case MMUKO_LAYOUT_TYPE_DESC_FIELDS:  return (uint32_t)offsetof(mmuko_type_desc_t, fields);

    case MMUKO_LAYOUT_FN_SIG_SIZE:       return (uint32_t)sizeof(mmuko_fn_sig_t);
    case MMUKO_LAYOUT_FN_SIG_CC:         return (uint32_t)offsetof(mmuko_fn_sig_t, cc);
    case MMUKO_LAYOUT_FN_SIG_FLAGS:      return (uint32_t)offsetof(mmuko_fn_sig_t, flags);
    case MMUKO_LAYOUT_FN_SIG_RET:        return (uint32_t)offsetof(mmuko_fn_sig_t, ret);
    case MMUKO_LAYOUT_FN_SIG_NARGS:      return (uint32_t)offsetof(mmuko_fn_sig_t, nargs);
    case MMUKO_LAYOUT_FN_SIG_ARGS:       return (uint32_t)offsetof(mmuko_fn_sig_t, args);

    case MMUKO_LAYOUT_SEMVERX_SIZE:        return (uint32_t)sizeof(mmuko_semverx_t);
    case MMUKO_LAYOUT_SEMVERX_MAJOR:       return (uint32_t)offsetof(mmuko_semverx_t, major);
    case MMUKO_LAYOUT_SEMVERX_MINOR:       return (uint32_t)offsetof(mmuko_semverx_t, minor);
    case MMUKO_LAYOUT_SEMVERX_PATCH:       return (uint32_t)offsetof(mmuko_semverx_t, patch);
    case MMUKO_LAYOUT_SEMVERX_MAJOR_STATE: return (uint32_t)offsetof(mmuko_semverx_t, major_state);
    case MMUKO_LAYOUT_SEMVERX_MINOR_STATE: return (uint32_t)offsetof(mmuko_semverx_t, minor_state);
    case MMUKO_LAYOUT_SEMVERX_PATCH_STATE: return (uint32_t)offsetof(mmuko_semverx_t, patch_state);

    case MMUKO_LAYOUT_EXPORT_SIZE:    return (uint32_t)sizeof(mmuko_export_desc_t);
    case MMUKO_LAYOUT_EXPORT_SYMBOL:  return (uint32_t)offsetof(mmuko_export_desc_t, symbol);
    case MMUKO_LAYOUT_EXPORT_SIG:     return (uint32_t)offsetof(mmuko_export_desc_t, sig);
    case MMUKO_LAYOUT_EXPORT_SINCE:   return (uint32_t)offsetof(mmuko_export_desc_t, since);
    case MMUKO_LAYOUT_EXPORT_ADDRESS: return (uint32_t)offsetof(mmuko_export_desc_t, address);

    case MMUKO_LAYOUT_MODULE_SIZE:     return (uint32_t)sizeof(mmuko_module_desc_t);
    case MMUKO_LAYOUT_MODULE_MAGIC:    return (uint32_t)offsetof(mmuko_module_desc_t, magic);
    case MMUKO_LAYOUT_MODULE_ABI_REV:  return (uint32_t)offsetof(mmuko_module_desc_t, abi_rev);
    case MMUKO_LAYOUT_MODULE_ARCH:     return (uint32_t)offsetof(mmuko_module_desc_t, arch);
    case MMUKO_LAYOUT_MODULE_NEXPORTS: return (uint32_t)offsetof(mmuko_module_desc_t, nexports);
    case MMUKO_LAYOUT_MODULE_NAME:     return (uint32_t)offsetof(mmuko_module_desc_t, module);
    case MMUKO_LAYOUT_MODULE_VERSION:  return (uint32_t)offsetof(mmuko_module_desc_t, version);
    case MMUKO_LAYOUT_MODULE_EXPORTS:  return (uint32_t)offsetof(mmuko_module_desc_t, exports);

    case MMUKO_LAYOUT_FINGERPRINT_SIZE: return (uint32_t)sizeof(mmuko_fingerprint_t);
    case MMUKO_LAYOUT_FNPTR_SIZE:       return (uint32_t)sizeof(mmuko_fnptr_t);
    case MMUKO_LAYOUT_VOIDPTR_SIZE:     return (uint32_t)sizeof(void *);

    default: return 0xFFFFFFFFu;
    }
}
