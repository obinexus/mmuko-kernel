# SPDX-License-Identifier: LicenseRef-OBINexus-1.0
#
# build.ps1 -- native Windows build and test, no WSL required.
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Arch 32
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -BuildOnly
#
# What this covers, and what it does not
# --------------------------------------
# COVERS: the four C suites, built with MinGW gcc and run natively. That
# exercises the LoadLibrary/GetProcAddress loader path in src/mmuko_loader.c,
# which the POSIX build never touches -- so this is not a convenience wrapper,
# it is the only thing that tests half the loader.
#
# DOES NOT COVER: the Rust suites, the QEMU ring-0 harness, and the mmuko-boot
# sequence. Those are driven by shell scripts and need WSL, Git Bash or MSYS2.
# `make check` from a Unix shell remains the complete run.
#
# Requirements: MinGW-w64 gcc on PATH. If you have none, install one of:
#   winget install BrechtSanders.WinLibs.POSIX.UCRT
#   choco install mingw
#   MSYS2:  pacman -S mingw-w64-x86_64-gcc

[CmdletBinding()]
param(
    [ValidateSet('64', '32')]
    [string]$Arch = '64',
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'

$Root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Out   = Join-Path $Root "build\win$Arch"

Write-Host "MMUKO ABI -- native Windows build (mmuko$Arch)" -ForegroundColor Cyan
Write-Host ""

# --- compiler -------------------------------------------------------------
$cc = $null
foreach ($candidate in @('gcc', 'x86_64-w64-mingw32-gcc', 'i686-w64-mingw32-gcc', 'clang')) {
    if (Get-Command $candidate -ErrorAction SilentlyContinue) { $cc = $candidate; break }
}
if (-not $cc) {
    Write-Host "ERROR: no C compiler on PATH." -ForegroundColor Red
    Write-Host "  Install MinGW-w64, for example:"
    Write-Host "    winget install BrechtSanders.WinLibs.POSIX.UCRT"
    Write-Host "  or use WSL, where the full suite runs: wsl make check"
    exit 1
}
Write-Host "compiler: $cc"

# A 32-bit build needs a compiler that can target it. Checked with a real
# compile rather than inferred from the name, because a toolchain can be
# present and still not have the 32-bit runtime beside it.
$archFlag = if ($Arch -eq '32') { '-m32' } else { '-m64' }
$probe = Join-Path $env:TEMP 'mmuko_probe.c'
Set-Content -Path $probe -Value 'int main(void){return 0;}' -Encoding ASCII
& $cc $archFlag $probe -o (Join-Path $env:TEMP 'mmuko_probe.exe') 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: $cc cannot target $Arch-bit." -ForegroundColor Red
    if ($Arch -eq '32') {
        Write-Host "  A 64-bit-only MinGW cannot build mmuko32. Install a multilib"
        Write-Host "  toolchain, or run the mmuko32 suites under WSL."
    }
    exit 1
}
Remove-Item $probe -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path $Out | Out-Null

# -Wconversion is on for the same reason it is on in the Makefile: this
# library's whole subject is the silent reinterpretation of values across a
# boundary, and a codebase about width mismatches that tolerated implicit
# narrowing would be making a joke of itself.
$cflags = @(
    '-std=c99', '-O2', '-g', $archFlag,
    '-Wall', '-Wextra', '-Wpedantic', '-Wshadow', '-Wconversion',
    '-Wstrict-prototypes', '-Wmissing-prototypes',
    "-I$Root\include", "-I$Root\tests"
)

$core = @(
    "$Root\src\mmuko_abi.c",
    "$Root\src\mmuko_semverx.c",
    "$Root\src\mmuko_desc.c",
    "$Root\src\mmuko_trident.c"
)
$hosted = $core + @("$Root\src\mmuko_loader.c", "$Root\tools\mmuko_layout_probe.c")

function Invoke-CC {
    param([string[]]$CcArgs, [string]$What)
    & $cc @CcArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "BUILD FAILED: $What" -ForegroundColor Red
        exit 1
    }
}

# --- fixtures -------------------------------------------------------------
# Each fixture carries its own copy of the encoder, exactly as a real
# third-party module would. That is what makes the comparison meaningful: the
# fingerprints are computed by the module's own code, not by the loader's.
Write-Host "[mmuko$Arch] fixtures"
foreach ($fx in @('mathlib_v1', 'mathlib_v2', 'mathlib_v1_1', 'mathlib_v2_stable')) {
    Invoke-CC ($cflags + @(
        '-shared', '-o', "$Out\$fx.dll",
        "$Root\tests\fixtures\$fx.c",
        "$Root\src\mmuko_abi.c", "$Root\src\mmuko_semverx.c"
    )) "$fx.dll"
}
# The undeclared object from the transcript: no ABI table, and therefore
# unloadable. That refusal is the behaviour under test.
Invoke-CC @('-std=c99', '-O2', $archFlag, "-I$Root\include",
            '-shared', '-o', "$Out\mathlib_bare.dll",
            "$Root\tests\fixtures\mathlib_bare.c") 'mathlib_bare.dll'

# --- suites ---------------------------------------------------------------
Write-Host "[mmuko$Arch] suites"
$suites = @('test_fingerprint', 'test_semverx', 'test_trident', 'test_loader')
foreach ($s in $suites) {
    Invoke-CC ($cflags + @('-o', "$Out\$s.exe", "$Root\tests\$s.c") + $hosted) $s
}
Invoke-CC ($cflags + @('-o', "$Out\mmuko-abi-dump.exe",
                       "$Root\tools\mmuko_abi_dump.c") + $hosted) 'mmuko-abi-dump'

Write-Host "built: $Out"

if ($BuildOnly) { exit 0 }

# --- run ------------------------------------------------------------------
Write-Host ""
Write-Host "################  mmuko$Arch (native Windows)  ################" -ForegroundColor Cyan

$failed = 0
foreach ($s in $suites) {
    & "$Out\$s.exe" $Out
    if ($LASTEXITCODE -ne 0) { $failed = 1 }
}

Write-Host ""
if ($failed -eq 0) {
    Write-Host "All C suites passed on native Windows." -ForegroundColor Green
    Write-Host "This run exercised the LoadLibrary/GetProcAddress loader path."
    Write-Host ""
    Write-Host "Not covered here: the Rust suites, the QEMU ring-0 harness and the"
    Write-Host "mmuko-boot sequence. For those:  wsl make check"
    exit 0
} else {
    Write-Host "One or more suites FAILED." -ForegroundColor Red
    exit 1
}
