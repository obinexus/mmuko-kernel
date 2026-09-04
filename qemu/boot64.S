/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * boot64.S -- multiboot entry into long mode, for the mmuko64 ring-0 harness.
 *
 * QEMU's -kernel hands us 32-bit protected mode with paging off, so reaching
 * mmuko64 means doing the transition ourselves:
 *
 *   1. identity map the first 1 GiB with 2 MiB pages
 *   2. enable PAE            (CR4.PAE)
 *   3. set the long mode bit (EFER.LME)
 *   4. enable paging         (CR0.PG)  -- this activates IA-32e mode
 *   5. load a 64-bit GDT and far-jump into a 64-bit code segment
 *
 * Only the first gigabyte is mapped, which is ample: the image is a few
 * kilobytes and nothing here allocates.
 *
 * The point of taking this trouble rather than running the 32-bit image under
 * qemu-system-x86_64 is that the mmuko64 profile is defined by the SysV AMD64
 * calling convention, and the only way to test that the resolver works under
 * it is to actually be in long mode when the calls happen.
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
.align 4096
pml4:  .skip 4096
pdpt:  .skip 4096
pd:    .skip 4096
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .rodata
.align 8
gdt64:
    .quad 0                                         /* null */
gdt64_code = . - gdt64
    /* present | descriptor type | executable | 64-bit */
    .quad (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
gdt64_data = . - gdt64
    /* present | descriptor type | writable */
    .quad (1 << 41) | (1 << 44) | (1 << 47)
gdt64_pointer:
    .word gdt64_pointer - gdt64 - 1
    .quad gdt64

.section .text
.code32
.global _start
.type _start, @function
_start:
    cli
    movl $stack_top, %esp
    xorl %ebp, %ebp

    /* --- page tables -------------------------------------------------- */
    /* PML4[0] -> PDPT */
    movl $pdpt, %eax
    orl  $0x03, %eax                 /* present | writable */
    movl %eax, pml4
    movl $0, pml4 + 4

    /* PDPT[0] -> PD */
    movl $pd, %eax
    orl  $0x03, %eax
    movl %eax, pdpt
    movl $0, pdpt + 4

    /* PD[i] -> 2 MiB page at i * 2 MiB, for i in [0, 512) = 1 GiB */
    xorl %ecx, %ecx
1:
    movl $0x200000, %eax
    mull %ecx                        /* edx:eax = 2MiB * ecx */
    orl  $0x83, %eax                 /* present | writable | page size (2 MiB) */
    movl %eax, pd(, %ecx, 8)
    movl %edx, pd + 4(, %ecx, 8)
    incl %ecx
    cmpl $512, %ecx
    jne  1b

    /* --- enter long mode ---------------------------------------------- */
    movl $pml4, %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl  $(1 << 5), %eax             /* CR4.PAE */
    movl %eax, %cr4

    movl $0xC0000080, %ecx           /* IA32_EFER */
    rdmsr
    orl  $(1 << 8), %eax             /* EFER.LME */
    wrmsr

    movl %cr0, %eax
    orl  $(1 << 31), %eax            /* CR0.PG -- IA-32e activates here */
    movl %eax, %cr0

    lgdt gdt64_pointer
    ljmp $gdt64_code, $long_mode_entry

.code64
.type long_mode_entry, @function
long_mode_entry:
    movw $gdt64_data, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq $stack_top, %rsp
    xorq %rbp, %rbp

    call kmain

    cli
1:  hlt
    jmp 1b

.size _start, . - _start

/* This image has no executable stack; say so explicitly so the linker does
 * not assume otherwise and warn. */
.section .note.GNU-stack, "", @progbits
