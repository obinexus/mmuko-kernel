#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-OBINexus-1.0
#
# run_boot.sh -- build the mmuko-boot kernel with PHASE 8 and boot it in QEMU.
#
#   ./boot/run_boot.sh            build and boot
#   ./boot/run_boot.sh build      build only
#   ./boot/run_boot.sh negative   build with a deliberately mismatched kernel
#                                 service contract; PASSES only if boot refuses
#
# Toolchain
# ---------
# Prefers an i686-elf cross compiler if one is installed, and falls back to the
# host gcc with -m32. The fallback is legitimate here: nothing in this image
# links against the host libc, the linker script places every section itself,
# and -ffreestanding tells the compiler not to assume hosted semantics. What a
# cross compiler buys is that mistakes in those flags fail loudly instead of
# quietly picking up a host header. So: use the cross compiler when you have
# one; the fallback exists so that not having one is not a reason to skip the
# boot test entirely.
#
# NASM is required for boot.asm. If it is absent this script says so and stops,
# rather than silently substituting a different entry point.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOOT="$ROOT/boot"
OUT="$ROOT/build/boot"
QEMU="${QEMU:-qemu-system-i386}"
MODE="${1:-run}"

# The negative mode compiles the kernel with a deliberately mismatched
# kputhex declaration and asserts that the boot FAILS. A gate that has never
# been observed to refuse anything is not a gate.
NEGATIVE=0
[ "$MODE" = "negative" ] && { NEGATIVE=1; MODE=run; }

mkdir -p "$OUT"

# --- toolchain -------------------------------------------------------------
if command -v i686-elf-gcc >/dev/null 2>&1; then
    CC="i686-elf-gcc"
    ARCH_FLAGS=""
    echo "toolchain: i686-elf-gcc (cross)"
else
    CC="${CC:-gcc}"
    ARCH_FLAGS="-m32"
    echo "toolchain: $CC -m32 (host, freestanding)"
    if ! echo 'int main(void){return 0;}' | $CC -m32 -x c - -o /dev/null 2>/dev/null; then
        echo "ERROR: $CC cannot target 32-bit. Install gcc-multilib, or an"
        echo "       i686-elf cross compiler. See tools/bootstrap.sh."
        exit 1
    fi
fi

if ! command -v nasm >/dev/null 2>&1; then
    echo "ERROR: nasm is required to assemble boot.asm."
    echo "       Debian/Ubuntu: sudo apt install nasm"
    exit 1
fi

# -mno-sse -mno-80387: no FPU is initialised at this point in boot, and neither
# the boot model nor the ABI resolver contains any floating point.
# -fno-builtin: do not let the compiler lower a loop into a memcpy we cannot link.
CFLAGS="-std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
 -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
 -mno-sse -mno-mmx -mno-80387 $ARCH_FLAGS \
 -I$ROOT/include -I$BOOT"
[ "$NEGATIVE" = "1" ] && CFLAGS="$CFLAGS -DMMUKO_ABI_NEGATIVE_TEST"
LDFLAGS="-T $BOOT/linker.ld -ffreestanding -O2 -nostdlib -no-pie \
 -Wl,--build-id=none -Wl,-z,noexecstack $ARCH_FLAGS"

# The four freestanding core translation units. mmuko_loader.c is deliberately
# absent: it needs dlopen, and there is no dynamic loader at ring 0. The
# resolver it feeds has no such dependency, which is the whole point of keeping
# them in separate files.
CORE="$ROOT/src/mmuko_abi.c $ROOT/src/mmuko_semverx.c \
 $ROOT/src/mmuko_desc.c $ROOT/src/mmuko_trident.c"

echo "--- assembling boot.asm ---"
nasm -f elf32 "$BOOT/boot.asm" -o "$OUT/boot.o" || exit 1

echo "--- compiling kernel + phase 8 + ABI core ---"
# shellcheck disable=SC2086
$CC $CFLAGS $LDFLAGS -o "$OUT/mmuko-kernel.elf" \
    "$OUT/boot.o" "$BOOT/kernel.c" "$BOOT/mmuko_abi_phase8.c" $CORE || exit 1

echo "--- image ---"
size "$OUT/mmuko-kernel.elf" 2>/dev/null | sed 's/^/    /'

# Confirm it is a valid multiboot kernel before handing it to QEMU, so a header
# mistake reports as a header mistake rather than as a boot that does nothing.
if command -v grub-file >/dev/null 2>&1; then
    if grub-file --is-x86-multiboot "$OUT/mmuko-kernel.elf"; then
        echo "    multiboot header: ok"
    else
        echo "    multiboot header: MISSING -- QEMU will refuse this image"
        exit 1
    fi
fi

[ "$MODE" = "build" ] && { echo "built: $OUT/mmuko-kernel.elf"; exit 0; }

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "SKIP: $QEMU not installed; built but not booted."
    exit 0
fi

LOG="$OUT/serial.log"
: > "$LOG"

echo
echo "--- booting under $QEMU ---"
# -serial file: rather than -serial stdio. The stdio backend wires the guest
# UART to this terminal in both directions, so QEMU takes the tty and a boot
# that produces nothing is indistinguishable from a hang -- and typing into the
# terminal to check goes to the guest instead.
# The kernel halts rather than exiting (this is a real boot, not a test
# harness), so the timeout is the normal way this run ends.
timeout 25 "$QEMU" \
    -kernel "$OUT/mmuko-kernel.elf" \
    -serial "file:$LOG" \
    -machine accel=tcg \
    -display none \
    -nodefaults \
    -no-reboot \
    < /dev/null > "$OUT/qemu.err" 2>&1
rc=$?

sed 's/^/    /' "$LOG"

if [ ! -s "$LOG" ]; then
    echo "    (no serial output; qemu said:)"
    sed 's/^/    /' "$OUT/qemu.err"
    exit 1
fi

echo
if [ "$NEGATIVE" = "1" ]; then
    # Inverted expectations: this run is correct only if the boot refused.
    if grep -q "BOOT_ABI_UNBOUND" "$LOG"; then
        echo "RESULT: PASS -- the mismatched kputhex contract was refused and the"
        echo "        boot stopped at BOOT_ABI_UNBOUND, before launching anything."
        exit 0
    fi
    echo "RESULT: FAIL -- the negative test BOOTED. A kernel whose declared"
    echo "        service contract does not match its own implementation was"
    echo "        allowed through. The gate is not working."
    exit 1
fi

if grep -q "MMUKO BOOT COMPLETE" "$LOG" && grep -q "MMUKO PROGRAM END" "$LOG"; then
    echo "RESULT: boot reached PHASE 7 and ran a program through the bound slot."
    exit 0
elif grep -q "BOOT FAILED" "$LOG"; then
    echo "RESULT: boot failed -- see the status line above."
    exit 1
else
    echo "RESULT: boot did not complete (qemu exit $rc, ${rc}=124 means the"
    echo "        25s timeout elapsed before the sequence finished)."
    exit 1
fi
