# SPDX-License-Identifier: LicenseRef-OBINexus-1.0
#
# MMUKO kernel dynamic C ABI -- build and verification.
#
#   make            build everything available on this machine
#   make test       C suites for both profiles, Rust suites, QEMU ring 0
#   make test-c     C suites only
#   make test-rust  Rust suites only
#   make qemu       ring-0 boot test only
#   make boot       full mmuko-boot sequence (phases 0-8) under QEMU
#   make boot-neg   negative test: a mismatched contract must FAIL the boot
#   make win-check  cross-compile the Windows build (MinGW), if available
#   make demo       reproduce the transcript, unguarded then guarded
#   make check      test + strict warnings + freestanding link check
#   make clean
#
# Everything degrades gracefully. A machine with no 32-bit toolchain, no Rust
# and no QEMU still builds and runs the 64-bit C suites, and says clearly what
# it skipped. A missing tool must weaken the evidence, never break the build --
# otherwise the first thing anyone does is stop running the tests.

# ---------------------------------------------------------------------------
# This Makefile's recipes are POSIX shell: `for f in ...; do ... done`,
# `command -v`, `test -x`.  Run from a Windows command prompt or PowerShell,
# GNU make hands them to cmd.exe, which produces a cascade of
# "The system cannot find the path specified" and "'test' is not recognized"
# before failing on something unrelated to the real problem.
#
# Detect that here and say so once, plainly, instead.
# ---------------------------------------------------------------------------
ifeq ($(wildcard /bin/sh),)
$(info )
$(info ================================================================)
$(info  This Makefile needs a POSIX shell, and cmd.exe is not one.)
$(info )
$(info  On Windows, use one of:)
$(info    WSL          wsl  then  cd /mnt/c/Users/<you>/... && make check)
$(info    Git Bash     or MSYS2, either of which provides /bin/sh)
$(info    PowerShell   powershell -ExecutionPolicy Bypass -File tools\build.ps1)
$(info )
$(info  The PowerShell path builds and runs the C suites natively with MinGW)
$(info  gcc, which also exercises the LoadLibrary/GetProcAddress loader that)
$(info  the POSIX build never touches.  It does not run the Rust, QEMU or)
$(info  mmuko-boot suites -- those need the Unix shell.)
$(info ================================================================)
$(info )
$(error no POSIX shell available)
endif

CC        ?= gcc
BUILD     ?= build
INCLUDE   := -Iinclude
TESTINC   := -Itests

# -Wconversion is on because this library's entire subject is the silent
# reinterpretation of values across a boundary. A codebase about width
# mismatches that tolerated implicit narrowing would be making a joke of
# itself.
WARN      := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
             -Wmissing-prototypes -Wpointer-arith -Wcast-qual -Wwrite-strings
CFLAGS    ?= -std=c99 -O2 -g
ALLCFLAGS  = $(CFLAGS) $(WARN) $(INCLUDE)

CORE_SRC  := src/mmuko_abi.c src/mmuko_semverx.c src/mmuko_desc.c src/mmuko_trident.c
PROBE_SRC := tools/mmuko_layout_probe.c
HOST_SRC  := $(CORE_SRC) src/mmuko_loader.c $(PROBE_SRC)

SUITES    := test_fingerprint test_semverx test_trident test_loader
FIXTURES  := mathlib_v1 mathlib_v2 mathlib_v1_1 mathlib_v2_stable

# --- 32-bit availability probe --------------------------------------------
# Ask the compiler rather than guessing from the host triple: a machine can
# have gcc without multilib, and the failure then appears halfway through a
# build instead of here.
HAVE_M32 := $(shell printf 'int main(void){return 0;}' > /tmp/.mmuko32$$$$.c 2>/dev/null && \
              $(CC) -m32 /tmp/.mmuko32$$$$.c -o /tmp/.mmuko32$$$$.out >/dev/null 2>&1 && \
              echo yes || echo no; rm -f /tmp/.mmuko32$$$$.c /tmp/.mmuko32$$$$.out)
# rustup installs into $HOME/.cargo/bin and puts it on PATH via a shell profile
# hook, so a shell that was already open when rustup ran -- or a `make` run from
# one -- does not see it.  Look in the install location as well, rather than
# telling someone their freshly-installed toolchain is missing.
CARGO_ON_PATH := $(shell command -v cargo 2>/dev/null)
CARGO_FALLBACK := $(shell test -x "$$HOME/.cargo/bin/cargo" && echo "$$HOME/.cargo/bin/cargo")
CARGO      := $(if $(CARGO_ON_PATH),$(CARGO_ON_PATH),$(CARGO_FALLBACK))
HAVE_CARGO := $(if $(CARGO),yes,no)
HAVE_QEMU  := $(shell command -v qemu-system-i386 >/dev/null 2>&1 || \
                      command -v qemu-system-x86_64 >/dev/null 2>&1 && echo yes || echo no)
HAVE_NASM  := $(shell command -v nasm >/dev/null 2>&1 && echo yes || echo no)
MINGW64    := $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null)
MINGW32    := $(shell command -v i686-w64-mingw32-gcc 2>/dev/null)

ifeq ($(HAVE_M32),yes)
PROFILES := 64 32
else
PROFILES := 64
endif

.PHONY: all test test-c test-rust qemu boot boot-neg win-check demo check clean tools help probe

all: $(addprefix build-,$(PROFILES))
	@echo
	@$(MAKE) --no-print-directory probe

help:
	@sed -n '3,20p' Makefile | sed 's/^# \{0,1\}//'

probe:
	@echo "toolchain: 32-bit=$(HAVE_M32)  cargo=$(HAVE_CARGO)  qemu=$(HAVE_QEMU)  nasm=$(HAVE_NASM)  mingw=$(if $(MINGW64)$(MINGW32),yes,no)"
ifeq ($(CARGO_ON_PATH),)
ifneq ($(CARGO),)
	@echo "  note: cargo is not on this shell's PATH; using $(CARGO)."
	@echo "        run  . \"$$HOME/.cargo/env\"  so the rest of your session sees it."
endif
endif
ifeq ($(HAVE_M32),no)
	@echo "  note: no 32-bit toolchain; the mmuko32 profile is not being tested."
	@echo "        run tools/bootstrap.sh to install it."
endif
ifeq ($(HAVE_CARGO),no)
	@echo "  note: cargo not found; kernel.rs and the Rust/C differential suite"
	@echo "        are not being tested.  run tools/bootstrap.sh."
endif
ifeq ($(HAVE_NASM),no)
	@echo "  note: nasm not found; the mmuko-boot sequence cannot be assembled."
	@echo "        run tools/bootstrap.sh."
endif

# --------------------------------------------------------------------------
# Per-profile C build
# --------------------------------------------------------------------------

define PROFILE_RULES

BUILD_$(1) := $$(BUILD)/$(1)
ARCH_$(1)  := $$(if $$(filter 32,$(1)),-m32,)

.PHONY: build-$(1) test-$(1)

build-$(1):
	@mkdir -p $$(BUILD_$(1))
	@echo "[mmuko$(1)] library"
	@for f in $$(HOST_SRC); do \
	   $$(CC) $$(ARCH_$(1)) $$(ALLCFLAGS) -fPIC -c $$$$f \
	     -o $$(BUILD_$(1))/$$$$(basename $$$$f .c).o || exit 1; \
	 done
	@ar rcs $$(BUILD_$(1))/libmmuko_abi.a $$(BUILD_$(1))/*.o
	@echo "[mmuko$(1)] fixtures"
	@for f in $$(FIXTURES); do \
	   $$(CC) $$(ARCH_$(1)) $$(ALLCFLAGS) -fPIC -shared \
	     -o $$(BUILD_$(1))/lib$$$$f.so tests/fixtures/$$$$f.c \
	     src/mmuko_abi.c src/mmuko_semverx.c || exit 1; \
	 done
	@$$(CC) $$(ARCH_$(1)) $$(CFLAGS) $$(INCLUDE) -fPIC -shared \
	   -o $$(BUILD_$(1))/libmathlib_bare.so tests/fixtures/mathlib_bare.c
	@echo "[mmuko$(1)] suites"
	@for s in $$(SUITES); do \
	   $$(CC) $$(ARCH_$(1)) $$(ALLCFLAGS) $$(TESTINC) -o $$(BUILD_$(1))/$$$$s \
	     tests/$$$$s.c $$(BUILD_$(1))/libmmuko_abi.a -ldl || exit 1; \
	 done
	@$$(CC) $$(ARCH_$(1)) $$(ALLCFLAGS) -o $$(BUILD_$(1))/mmuko-abi-dump \
	   tools/mmuko_abi_dump.c $$(BUILD_$(1))/libmmuko_abi.a -ldl

test-$(1): build-$(1)
	@echo
	@echo "################  mmuko$(1)  ################"
	@fail=0; for s in $$(SUITES); do \
	   $$(BUILD_$(1))/$$$$s $$(BUILD_$(1)) || fail=1; \
	 done; exit $$$$fail

endef

$(foreach p,$(PROFILES),$(eval $(call PROFILE_RULES,$(p))))

# --------------------------------------------------------------------------
# Aggregate targets
# --------------------------------------------------------------------------

test-c: $(addprefix test-,$(PROFILES))

test-rust:
ifeq ($(HAVE_CARGO),yes)
	@echo
	@echo "################  kernel.rs  ################"
	@cd rust/mmuko-kernel && $(CARGO) test --features hosted
	@echo "-- freestanding build (no_std, no allocator, no libc):"
	@cd rust/mmuko-kernel && $(CARGO) build --release
else
	@echo "SKIP: cargo not installed; kernel.rs not tested"
endif

qemu:
	@echo
	@echo "################  ring 0 under QEMU  ################"
	@./qemu/run_qemu.sh

# The mmuko-boot sequence with PHASE 8 spliced in. Distinct from `make qemu`:
# that boots a minimal image whose only job is to prove the resolver runs
# freestanding. This boots the real MMUKO boot sequence and shows phase 8 in
# its actual position, gating phase 7.
boot:
	@echo
	@echo "################  mmuko-boot: phases 0-8  ################"
	@./boot/run_boot.sh

boot-neg:
	@echo
	@echo "################  mmuko-boot: negative gate test  ################"
	@./boot/run_boot.sh negative

# Cross-compile the Windows build from a POSIX host.
#
# src/mmuko_loader.c has a LoadLibrary/GetProcAddress branch that the POSIX
# build never compiles, so without this it is untested code shipping on the
# platform its author uses daily. This does not RUN the binaries -- that needs
# Windows or wine, and tools/build.ps1 is the native path -- but it does prove
# the branch compiles and links clean under the same warning set as everything
# else, which is where its mistakes would be.
win-check:
	@echo
	@echo "################  Windows cross-build (compile + link)  ################"
ifeq ($(MINGW64)$(MINGW32),)
	@echo "SKIP: no MinGW-w64 toolchain; the Win32 loader branch is not being"
	@echo "      compile-checked.  Debian/Ubuntu: sudo apt install mingw-w64"
else
	@for cc in $(MINGW64) $(MINGW32); do 	   [ -z "$$cc" ] && continue; 	   b=$(BUILD)/win-$$(basename $$cc | cut -d- -f1); mkdir -p $$b; 	   echo "  [$$(basename $$cc)]"; 	   for f in $(FIXTURES); do 	     $$cc $(ALLCFLAGS) -shared -o $$b/$$f.dll tests/fixtures/$$f.c 	       src/mmuko_abi.c src/mmuko_semverx.c || exit 1; 	   done; 	   $$cc $(CFLAGS) $(INCLUDE) -shared -o $$b/mathlib_bare.dll 	     tests/fixtures/mathlib_bare.c || exit 1; 	   for s in $(SUITES); do 	     $$cc $(ALLCFLAGS) $(TESTINC) -o $$b/$$s.exe tests/$$s.c $(HOST_SRC) || exit 1; 	   done; 	   $$cc $(ALLCFLAGS) -o $$b/mmuko-abi-dump.exe tools/mmuko_abi_dump.c 	     $(HOST_SRC) || exit 1; 	 done
	@echo "  ok: the Win32 loader branch compiles and links clean"
	@printf '  note: not RUN here.  On Windows: powershell -File tools/build.ps1\n'
endif

demo:
	@./demo/run_demo.sh

test: test-c test-rust qemu boot boot-neg win-check
	@echo
	@echo "================================================================"
	@echo "all available suites passed"
	@$(MAKE) --no-print-directory probe

# Link the freestanding core with no libc at all. If any of the four core
# translation units ever acquires a hosted dependency -- a memcpy the compiler
# lowered a loop into, a stack-protector guard, an __udivdi3 from libgcc -- it
# fails HERE, at the cheapest possible moment, rather than in someone's kernel.
freestanding-check:
	@echo "[check] freestanding link, no libc"
	@mkdir -p $(BUILD)/freestanding
	@for f in $(CORE_SRC); do \
	   $(CC) -std=c99 -O2 $(INCLUDE) -ffreestanding -fno-builtin \
	     -fno-stack-protector -c $$f -o $(BUILD)/freestanding/$$(basename $$f .c).o || exit 1; \
	 done
	@undef=$$(nm --undefined-only $(BUILD)/freestanding/*.o 2>/dev/null \
	          | awk '{print $$2}' | grep -v '^mmuko_' | sort -u); \
	 if [ -n "$$undef" ]; then \
	   echo "  FAIL: the freestanding core references non-MMUKO symbols:"; \
	   echo "$$undef" | sed 's/^/    /'; exit 1; \
	 else \
	   echo "  ok: the core references no symbol outside its own namespace"; \
	 fi

check: freestanding-check test

tools: all

clean:
	@rm -rf $(BUILD)
	@if [ "$(HAVE_CARGO)" = "yes" ]; then cd rust/mmuko-kernel && $(CARGO) clean; fi
	@echo "cleaned"
