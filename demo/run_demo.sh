#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-OBINexus-1.0
#
# run_demo.sh -- the transcript, twice: once unprotected, once with MMUKO.
#
#   ./demo/run_demo.sh          # native profile
#   ./demo/run_demo.sh -m32     # mmuko32 profile (needs gcc multilib)
#
# Part 1 reproduces the crash-that-isn't-a-crash exactly as described:
# build v1, link a caller, get 5; repoint the soname at v2 WITHOUT recompiling
# the caller; watch the same binary print a different number.
#
# Part 2 runs a caller that carries its required ABI table, against the same
# two objects, and shows the refusal instead of the wrong answer.

set -u
ARCH_FLAG="${1:-}"
CC="${CC:-gcc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/demo"
CFLAGS="-std=c99 -O0 -Wall -I$ROOT/include $ARCH_FLAG"

rm -rf "$OUT"; mkdir -p "$OUT"
cd "$OUT"

hr() { printf '\n%s\n' "----------------------------------------------------------------"; }

hr
echo "PART 1 -- no ABI table.  This is the transcript."
hr

$CC $CFLAGS -fPIC -shared -o libmath_v1.so "$ROOT/demo/bare_v1.c" || exit 1
$CC $CFLAGS -fPIC -shared -o libmath_v2.so "$ROOT/demo/bare_v2.c" || exit 1

# The transcript's `ln -s`: one soname, swappable target.
ln -sf libmath_v1.so libmath.so

# Compile the caller ONCE, against v1.  It is never rebuilt after this line.
$CC $CFLAGS -o naive_caller "$ROOT/demo/naive_caller.c" -L. -lmath \
    -Wl,-rpath,'$ORIGIN' || exit 1

echo "libmath.so -> $(readlink libmath.so)    [int add(int,int)]"
./naive_caller
echo "   correct."

echo
echo "Now repoint the soname at v2.  The caller is NOT recompiled."
ln -sf libmath_v2.so libmath.so
echo "libmath.so -> $(readlink libmath.so)    [double add(double,double)]"
./naive_caller
echo "   ^ same binary, same source, same symbol name."
echo "     No crash.  No linker error.  No warning.  A wrong number, silently."

hr
echo "PART 2 -- the same two objects, with the MMUKO trident in the path."
hr

# The MMUKO-declared equivalents of v1 and v2.
for f in mathlib_v1 mathlib_v2 mathlib_v1_1; do
    $CC $CFLAGS -fPIC -shared -o "lib$f.so" \
        "$ROOT/tests/fixtures/$f.c" "$ROOT/src/mmuko_abi.c" \
        "$ROOT/src/mmuko_semverx.c" || exit 1
done

$CC $CFLAGS -o guarded_caller "$ROOT/demo/guarded_caller.c" \
    "$ROOT/src/mmuko_abi.c" "$ROOT/src/mmuko_semverx.c" \
    "$ROOT/src/mmuko_desc.c" "$ROOT/src/mmuko_trident.c" \
    "$ROOT/src/mmuko_loader.c" -ldl || exit 1

echo "against v1  [int32 add(int32,int32) @ 1.stable]:"
./guarded_caller ./libmathlib_v1.so

echo
echo "against v2  [f64 add(f64,f64) @ 2.experimental]:"
./guarded_caller ./libmathlib_v2.so

echo
echo "against v1.1 -- the heal: add keeps its contract, addf is added beside it:"
./guarded_caller ./libmathlib_v1_1.so

hr
echo "The difference is not that MMUKO detected a bug in v2.  v2 is correct code."
echo "The difference is that the caller's contract survived compilation, so"
echo "there was something to compare against at the moment of binding."
hr
