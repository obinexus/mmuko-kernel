#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-OBINexus-1.0
#
# bootstrap.sh -- install what this tree needs to run its full verification.
#
#   ./tools/bootstrap.sh          # report what is missing, install it
#   ./tools/bootstrap.sh --check  # report only, install nothing
#
# What each piece is FOR, so you can decide what you actually want:
#
#   gcc / clang     the C core and the hosted suites.  Required.
#   gcc-multilib    the mmuko32 profile.  Without it only mmuko64 is tested,
#                   which is half the point of a dual-profile ABI.
#   rustup/cargo    kernel.rs and the Rust/C differential suite.  Without it
#                   nothing checks that the two implementations of the
#                   fingerprint agree, which is the guarantee that lets a Rust
#                   kernel load C modules at all.
#   qemu-system-x86 the ring-0 boot test.  Without it nothing proves the
#                   resolver runs freestanding -- only that it is correct in
#                   userspace, which is a weaker and different claim.
#   nasm            assembles boot.asm for the mmuko-boot sequence.  Without it
#                   phases 0-8 cannot be booted, only the minimal ring-0 image.
#   mingw-w64       cross-compiles the Windows build.  Without it the
#                   LoadLibrary/GetProcAddress branch of the loader is never
#                   compiled, and untested code ships on Windows.
#   cmake           the alternative build.  Optional; make is primary.
#
# Nothing here is required to USE the library. It is required to verify it.

set -u
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

have() { command -v "$1" >/dev/null 2>&1; }

# --- detect the package manager -------------------------------------------
PM=""
PM_INSTALL=""
if   have apt-get; then PM=apt;    PM_INSTALL="apt-get install -y"
elif have dnf;     then PM=dnf;    PM_INSTALL="dnf install -y"
elif have pacman;  then PM=pacman; PM_INSTALL="pacman -S --noconfirm"
elif have zypper;  then PM=zypper; PM_INSTALL="zypper install -y"
elif have brew;    then PM=brew;   PM_INSTALL="brew install"
fi

SUDO=""
if [ "$(id -u)" != "0" ] && [ "$PM" != "brew" ] && have sudo; then SUDO="sudo"; fi

# --- probe -----------------------------------------------------------------
probe_m32() {
    have gcc || return 1
    printf 'int main(void){return 0;}' > /tmp/.mmuko_probe.c
    gcc -m32 /tmp/.mmuko_probe.c -o /tmp/.mmuko_probe.out >/dev/null 2>&1
    local rc=$?
    rm -f /tmp/.mmuko_probe.c /tmp/.mmuko_probe.out
    return $rc
}

echo "MMUKO ABI -- verification toolchain"
echo

MISSING=""

status() {
    local name="$1" ok="$2" why="$3"
    if [ "$ok" = "yes" ]; then
        printf '  %-18s present\n' "$name"
    else
        printf '  %-18s MISSING  -- %s\n' "$name" "$why"
        MISSING="$MISSING $name"
    fi
}

have gcc || have clang && CC_OK=yes || CC_OK=no
probe_m32 && M32_OK=yes || M32_OK=no
have cargo && CARGO_OK=yes || CARGO_OK=no
{ have qemu-system-i386 && have qemu-system-x86_64; } && QEMU_OK=yes || QEMU_OK=no
have nasm && NASM_OK=yes || NASM_OK=no
{ have x86_64-w64-mingw32-gcc || have i686-w64-mingw32-gcc; } && MINGW_OK=yes || MINGW_OK=no
have cmake && CMAKE_OK=yes || CMAKE_OK=no

status "c compiler"    "$CC_OK"    "the C core and hosted suites cannot be built"
status "32-bit libc"   "$M32_OK"   "the mmuko32 profile will not be tested"
status "cargo"         "$CARGO_OK" "kernel.rs and the Rust/C differential suite will not run"
status "qemu"          "$QEMU_OK"  "the ring-0 boot test will not run"
status "nasm"          "$NASM_OK"  "the mmuko-boot sequence (phases 0-8) cannot be assembled"
status "mingw-w64"     "$MINGW_OK" "the Win32 loader branch will not be compile-checked"
status "cmake"         "$CMAKE_OK" "optional; make is the primary build"

if [ -z "$MISSING" ]; then
    echo
    echo "Everything is present. Run: make check"
    exit 0
fi

echo
if [ "$CHECK_ONLY" = "1" ]; then
    echo "(--check: nothing installed)"
    exit 0
fi

if [ -z "$PM" ]; then
    echo "No package manager recognised. Install by hand:"
    echo "  32-bit libc  gcc-multilib (Debian/Ubuntu) or glibc-devel.i686 (Fedora)"
    echo "  cargo        https://rustup.rs"
    echo "  qemu         qemu-system-x86"
    exit 1
fi

# --- install ---------------------------------------------------------------
PKGS=""
case "$PM" in
  apt)
    [ "$CC_OK"    = no ] && PKGS="$PKGS build-essential"
    [ "$M32_OK"   = no ] && PKGS="$PKGS gcc-multilib"
    [ "$QEMU_OK"  = no ] && PKGS="$PKGS qemu-system-x86"
    [ "$NASM_OK"  = no ] && PKGS="$PKGS nasm"
    [ "$NASM_OK"  = no ] && PKGS="$PKGS nasm"
    [ "$NASM_OK"  = no ] && PKGS="$PKGS nasm"
    [ "$NASM_OK"  = no ] && PKGS="$PKGS nasm"
    [ "$NASM_OK"  = no ] && PKGS="$PKGS nasm"
    [ "$MINGW_OK" = no ] && PKGS="$PKGS mingw-w64"
    [ "$CMAKE_OK" = no ] && PKGS="$PKGS cmake"
    ;;
  dnf)
    [ "$CC_OK"    = no ] && PKGS="$PKGS gcc make binutils"
    [ "$M32_OK"   = no ] && PKGS="$PKGS glibc-devel.i686 libgcc.i686"
    [ "$QEMU_OK"  = no ] && PKGS="$PKGS qemu-system-x86"
    [ "$CMAKE_OK" = no ] && PKGS="$PKGS cmake"
    ;;
  pacman)
    [ "$CC_OK"    = no ] && PKGS="$PKGS base-devel"
    [ "$M32_OK"   = no ] && PKGS="$PKGS lib32-glibc lib32-gcc-libs"
    [ "$QEMU_OK"  = no ] && PKGS="$PKGS qemu-system-x86"
    [ "$CMAKE_OK" = no ] && PKGS="$PKGS cmake"
    ;;
  zypper)
    [ "$CC_OK"    = no ] && PKGS="$PKGS gcc make"
    [ "$M32_OK"   = no ] && PKGS="$PKGS glibc-devel-32bit gcc-32bit"
    [ "$QEMU_OK"  = no ] && PKGS="$PKGS qemu-x86"
    [ "$CMAKE_OK" = no ] && PKGS="$PKGS cmake"
    ;;
  brew)
    # macOS has no 32-bit userspace and no ELF, so mmuko32 and the multiboot
    # harness are simply not available there. Said plainly rather than
    # attempted and failed halfway.
    [ "$QEMU_OK"  = no ] && PKGS="$PKGS qemu"
    [ "$CMAKE_OK" = no ] && PKGS="$PKGS cmake"
    if [ "$M32_OK" = no ]; then
      echo "note: macOS has no 32-bit userspace; the mmuko32 profile cannot be"
      echo "      tested here. Use a Linux host or a container for it."
    fi
    ;;
esac

if [ -n "$PKGS" ]; then
    echo "installing:$PKGS"
    # shellcheck disable=SC2086
    $SUDO $PM_INSTALL $PKGS || { echo "package install failed"; exit 1; }
fi

if [ "$CARGO_OK" = no ]; then
    echo
    echo "Installing Rust via rustup (needs network)."
    if have curl; then
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
          | sh -s -- -y --profile minimal || {
            echo "rustup install failed; see https://rustup.rs"; }
        # shellcheck disable=SC1090
        [ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
    else
        echo "curl not available. Install Rust from https://rustup.rs"
    fi
fi

echo
echo "Re-checking:"
"$0" --check

if [ -x "$HOME/.cargo/bin/cargo" ] && ! have cargo; then
    cat <<'EOF'

Note: rustup installed cargo into $HOME/.cargo/bin, but THIS shell's PATH was
fixed when it started, so `cargo` will not be found here until you either:

    . "$HOME/.cargo/env"      # this shell, now
    exec $SHELL -l            # or start a fresh login shell

The Makefile looks in $HOME/.cargo/bin as a fallback, so `make check` works
either way -- this note is so the rest of your session behaves as you expect.
EOF
fi
