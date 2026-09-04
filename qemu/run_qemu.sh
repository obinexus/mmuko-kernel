#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-OBINexus-1.0
#
# run_qemu.sh -- build and boot the ring-0 harness for both MMUKO profiles.
#
#   ./qemu/run_qemu.sh          # both profiles
#   ./qemu/run_qemu.sh 32       # mmuko32 only
#   ./qemu/run_qemu.sh 64       # mmuko64 only
#
# Exits non-zero if either profile fails or cannot be built.  Skips (exit 0,
# with a clear message) when the toolchain is not present: a missing qemu must
# weaken the evidence, never break someone's build.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/qemu"
CC="${CC:-gcc}"
WHICH="${1:-both}"

CORE="$ROOT/src/mmuko_abi.c $ROOT/src/mmuko_semverx.c $ROOT/src/mmuko_desc.c $ROOT/src/mmuko_trident.c"

# Freestanding, and deliberately hostile to accidental hosted dependencies:
#   -ffreestanding  no assumptions about libc semantics
#   -fno-builtin    no lowering of loops into memcpy/memset calls we cannot link
#   -nostdlib       no crt, no libc, no libgcc
#   -fno-stack-protector  the guard symbol lives in libc
#   -fno-pic        we are loaded at a fixed address with no dynamic loader
#   -mno-sse -mno-mmx -mno-80387  the FPU is not initialised at this point, and
#                   the resolver has no floating point in it to need one
COMMON_CFLAGS="-std=c99 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
 -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables \
 -mno-sse -mno-mmx -mno-80387 -I$ROOT/include -I$ROOT/tests"
LDFLAGS="-nostdlib -no-pie -Wl,--build-id=none -Wl,-z,noexecstack -T $ROOT/qemu/linker.ld"

mkdir -p "$OUT"

skip() { echo "SKIP: $*"; }

build_profile() {
    local bits="$1" arch_flag boot elf
    if [ "$bits" = "32" ]; then
        arch_flag="-m32 -march=i686"
        boot="$ROOT/qemu/boot32.S"
    else
        arch_flag="-m64 -mno-red-zone"   # small code model: we load at 1 MiB
        boot="$ROOT/qemu/boot64.S"
    fi
    elf="$OUT/mmuko$bits.elf"

    # shellcheck disable=SC2086
    $CC $arch_flag $COMMON_CFLAGS $LDFLAGS -o "$elf" "$boot" "$ROOT/qemu/kmain.c" $CORE 2>&1 || return 1

    if [ "$bits" = "64" ]; then
        # QEMU's built-in multiboot loader accepts multiboot1, which is an
        # ELF32-only specification: it refuses an ELF64 file outright with
        # "Cannot load x86-64 image, give a 32bit one."
        #
        # The image is nonetheless a legitimate 32-bit multiboot kernel -- it
        # enters at _start in protected mode and only reaches long mode after
        # setting up its own page tables. Only the ELF CONTAINER is 64-bit, and
        # every address in it is below 4 GiB. So rewrite the container and
        # leave the contents alone.
        #
        # This is a property of the loader, not of the code: a GRUB2 multiboot2
        # boot of the original ELF64 works without this step.
        objcopy -O elf32-i386 "$elf" "$OUT/mmuko64.mb32.elf" 2>&1 || return 1
    fi
}

run_profile() {
    local bits="$1" qemu elf log rc
    log="$OUT/mmuko$bits.log"
    if [ "$bits" = "32" ]; then
        qemu=qemu-system-i386
        elf="$OUT/mmuko32.elf"
    else
        qemu=qemu-system-x86_64
        elf="$OUT/mmuko64.mb32.elf"
    fi

    if ! command -v "$qemu" >/dev/null 2>&1; then
        skip "$qemu not installed; mmuko$bits ring-0 test not run"
        return 0
    fi

    echo "--- booting mmuko$bits under $qemu ---"
    : > "$log"
    set +e
    # Notes on the flags, because two of them are load-bearing and non-obvious:
    #
    #   -serial file:      NOT `-serial stdio`.  The stdio backend wires the
    #                      guest UART to this process's terminal in both
    #                      directions, so QEMU takes the tty and a boot that
    #                      produces nothing looks exactly like a hang -- and
    #                      typing into the terminal to check goes to the guest.
    #                      Writing straight to a file keeps the terminal ours.
    #   < /dev/null        belt and braces: the guest gets no stdin to wait on.
    #   -machine accel=tcg pure emulation.  KVM is unavailable under WSL2 and in
    #                      most containers, and probing for it can stall.
    #   -nodefaults        no default NIC, VGA or floppy to enumerate.
    timeout 60 "$qemu" \
        -kernel "$elf" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -serial "file:$log" \
        -machine accel=tcg \
        -display none \
        -nodefaults \
        -no-reboot \
        < /dev/null > "$OUT/mmuko$bits.qemu.err" 2>&1
    rc=$?
    set -e

    sed 's/^/    /' "$log"
    # QEMU's own complaints go to a separate file; surface them only when the
    # boot did not produce a verdict, so a working run stays quiet.
    if [ ! -s "$log" ] && [ -s "$OUT/mmuko$bits.qemu.err" ]; then
        echo "    (no serial output; qemu said:)"
        sed 's/^/    /' "$OUT/mmuko$bits.qemu.err"
    fi

    # isa-debug-exit turns the value written into (value << 1) | 1.
    #   0x10 -> 33 = every vector passed
    #   0x11 -> 35 = at least one failed
    case "$rc" in
        33) echo "    mmuko$bits: PASS (qemu exit 33)"; return 0 ;;
        35) echo "    mmuko$bits: FAIL (qemu exit 35)"; return 1 ;;
        124) echo "    mmuko$bits: TIMEOUT after 60s -- the kernel did not reach"
             echo "               its exit port.  Serial output above, if any."
             return 1 ;;
        *)  echo "    mmuko$bits: unexpected qemu exit $rc"; return 1 ;;
    esac
}

status=0
for bits in 32 64; do
    case "$WHICH" in
        both) ;;
        "$bits") ;;
        *) continue ;;
    esac

    echo "=== mmuko$bits ==="
    if ! out=$(build_profile "$bits"); then
        echo "$out"
        # A 32-bit build needs gcc multilib; treat its absence as a skip rather
        # than a failure, because a 64-bit-only host is a normal machine.
        if [ "$bits" = "32" ] && echo "$out" | grep -qi "cannot find\|not found\|no such file"; then
            skip "no 32-bit freestanding toolchain (install gcc-multilib)"
            continue
        fi
        echo "    mmuko$bits: BUILD FAILED"
        status=1
        continue
    fi
    [ -n "$out" ] && echo "$out"

    run_profile "$bits" || status=1
done

exit $status
