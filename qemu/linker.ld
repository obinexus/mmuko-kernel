/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * linker.ld -- layout for the ring-0 harness, both profiles.
 *
 * Loaded at 1 MiB, the conventional place for a multiboot kernel: below that
 * lies the real-mode IVT, the BIOS data area, VGA memory and option ROMs.
 *
 * The multiboot header must appear within the first 8 KiB of the image and be
 * 4-byte aligned, or the loader will not recognise the file as a kernel at
 * all -- hence .multiboot ahead of .text.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(4) : { KEEP(*(.multiboot)) }

    .text ALIGN(4K) : { *(.text .text.*) }

    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }

    .data ALIGN(4K) : { *(.data .data.*) }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    /* Nothing in this image needs relocation processing, an exception table,
     * or a note section.  Discarding them keeps the image honest: if a
     * translation unit ever starts emitting .eh_frame or .init_array, that is
     * a hosted dependency creeping in and it should be noticed, not silently
     * carried. */
    /DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) *(.init_array*) *(.fini_array*) }
}
