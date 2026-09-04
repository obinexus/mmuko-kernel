/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * mmuko_test.h -- minimal assertion harness.
 *
 * Deliberately dependency-free: the same assertions compile into the hosted
 * test binaries and into the freestanding QEMU kernel, where MMUKO_TEST_PRINT
 * is redirected to the serial port and there is no exit() to call.
 */

#ifndef MMUKO_TEST_H
#define MMUKO_TEST_H

#include <stdint.h>

#ifndef MMUKO_TEST_FREESTANDING
#  include <stdio.h>
#  define MMUKO_TEST_PRINT(s)   fputs((s), stdout)
#else
   void mmuko_test_print(const char *s);
#  define MMUKO_TEST_PRINT(s)   mmuko_test_print(s)
#endif

extern unsigned mmuko_test_passed;
extern unsigned mmuko_test_failed;
extern const char *mmuko_test_current;

#define MMUKO_TEST_STATE_DEFS                       \
    unsigned mmuko_test_passed = 0;                 \
    unsigned mmuko_test_failed = 0;                 \
    const char *mmuko_test_current = "";

#define MMUKO_CASE(NAME)                            \
    do { mmuko_test_current = (NAME);               \
         MMUKO_TEST_PRINT("\n  [case] " NAME "\n"); \
    } while (0)

#define MMUKO_CHECK(COND, MSG)                                        \
    do {                                                              \
        if (COND) {                                                   \
            mmuko_test_passed++;                                      \
            MMUKO_TEST_PRINT("    ok   " MSG "\n");                   \
        } else {                                                      \
            mmuko_test_failed++;                                      \
            MMUKO_TEST_PRINT("    FAIL " MSG "  (" __FILE__ ")\n");   \
        }                                                             \
    } while (0)

/* Same as MMUKO_CHECK but prints an extra caller-formatted line on failure. */
#define MMUKO_CHECK_MSG(COND, MSG, EXTRA)                             \
    do {                                                              \
        if (COND) {                                                   \
            mmuko_test_passed++;                                      \
            MMUKO_TEST_PRINT("    ok   " MSG "\n");                   \
        } else {                                                      \
            mmuko_test_failed++;                                      \
            MMUKO_TEST_PRINT("    FAIL " MSG "\n");                   \
            MMUKO_TEST_PRINT("         ");                            \
            MMUKO_TEST_PRINT(EXTRA);                                  \
            MMUKO_TEST_PRINT("\n");                                   \
        }                                                             \
    } while (0)

#define MMUKO_SUITE_BEGIN(NAME)  MMUKO_TEST_PRINT("\n=== " NAME " ===\n")

/*
 * Note for anyone adding a case here: a callable address in MMUKO is an
 * mmuko_fnptr_t, never a void*.  Casting between function pointer types is
 * fully defined by ISO C so long as the value is called only through its true
 * type, which is exactly what the resolver guarantees before it publishes a
 * slot.  No union or -Wpedantic exemption is needed anywhere in the tests.
 */

#endif /* MMUKO_TEST_H */
