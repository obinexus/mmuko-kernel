/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * naive_caller.c -- the caller from the transcript, unchanged and unprotected.
 *
 * It is compiled ONCE, against the v1 prototype, and never recompiled.  Its
 * machine code passes two 32-bit integers in the integer registers (mmuko64)
 * or on the stack (mmuko32) and reads the integer return register.
 *
 * Run it against libmath.so -> v1 and it prints 5.
 * Point libmath.so at v2 and run the SAME BINARY: it prints something else,
 * with no crash, no error, and no diagnostic of any kind.
 *
 * Built without optimisation on purpose: at -O2 the compiler is entitled to
 * constant-fold a call it can see through, and the point here is to observe
 * what the LINKER does at run time, not what the optimiser does at build time.
 */

#include <stdio.h>

/* This prototype is the caller's entire understanding of the contract.  It is
 * a promise made at compile time and never checked again. */
int add(int a, int b);

int main(void)
{
    int result = add(2, 3);
    printf("naive_caller: add(2, 3) = %d\n", result);
    return 0;
}
