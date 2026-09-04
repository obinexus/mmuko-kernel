/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * boot32.S -- multiboot entry for the mmuko32 ring-0 harness.
 *
 * QEMU's -kernel loads a multiboot ELF directly, so no bootloader and no disk
 * image are needed.  We arrive in 32-bit protected mode with paging off, a
 * flat GDT already installed by the loader, and interrupts disabled.
 *
 * There is nothing to do but give C a stack and call it.
 */

.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000000
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .bss, "aw", @nobits
.align 16
stack_bottom:
.skip 16384          /* 16 KiB.  The resolver's deepest frame is the recursive
                      * type encoder, bounded at MMUKO_TYPE_MAX_DEPTH = 8. */
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    movl $stack_top, %esp
    xorl %ebp, %ebp          /* terminate the frame chain for any backtrace */

    call kmain

    /* kmain exits through the debug port and does not return.  If the port is
     * absent, halt rather than executing whatever follows in memory. */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start

/* This image has no executable stack; say so explicitly so the linker does
 * not assume otherwise and warn. */
.section .note.GNU-stack, "", @progbits
