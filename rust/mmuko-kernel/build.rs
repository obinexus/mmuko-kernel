// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
// build.rs -- compile the C ABI core beside the Rust one, for the differential
// suite.
//
// Deliberately does NOT use the `cc` crate.  This tree carries no external
// dependencies, so it builds on a machine with no network and no registry
// cache -- the normal condition for kernel work.  Invoking the C compiler
// directly costs a few lines and removes that whole class of failure.
//
// If no C compiler is available the build still succeeds: it sets
// `cfg(mmuko_no_c_core)` and the differential test compiles to a skip.  A
// missing toolchain must degrade the evidence, never break the build.

use std::env;
use std::path::PathBuf;
use std::process::Command;

const CORE: &[&str] = &[
    "mmuko_abi.c",
    "mmuko_semverx.c",
    "mmuko_desc.c",
    "mmuko_trident.c",
];

/// Compiled alongside the core, from tools/, so the differential suite can
/// assert the Rust `#[repr(C)]` mirrors against the C compiler's own measured
/// layout rather than against a hand-written claim about it.
const PROBE: &str = "mmuko_layout_probe.c";

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    // rust/mmuko-kernel -> ../..
    let root = manifest.parent().unwrap().parent().unwrap().to_path_buf();
    let src = root.join("src");
    let include = root.join("include");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());

    // Declare the cfg we may emit, so `cargo check` does not warn about it.
    println!("cargo:rustc-check-cfg=cfg(mmuko_no_c_core)");
    println!("cargo:rerun-if-changed=build.rs");
    for f in CORE {
        println!("cargo:rerun-if-changed={}", src.join(f).display());
    }
    println!("cargo:rerun-if-changed={}", include.display());

    let cc = env::var("CC").unwrap_or_else(|_| "cc".to_string());

    // Match the Rust target's pointer width so the C core is built for the
    // same MMUKO profile.  A 64-bit Rust crate linked against a 32-bit C core
    // is exactly the fault this library exists to detect, and it would be
    // embarrassing to introduce it here.
    let width = env::var("CARGO_CFG_TARGET_POINTER_WIDTH").unwrap_or_default();
    let arch_flag: &[&str] = match width.as_str() {
        "32" => &["-m32"],
        _ => &[],
    };

    println!("cargo:rerun-if-changed={}", root.join("tools").join(PROBE).display());

    let mut units: Vec<PathBuf> = CORE.iter().map(|f| src.join(f)).collect();
    units.push(root.join("tools").join(PROBE));

    let mut objects = Vec::new();
    for path in &units {
        let f = path.file_name().unwrap().to_string_lossy().into_owned();
        let obj = out.join(format!("{}.o", f));
        let status = Command::new(&cc)
            .args(arch_flag)
            .args(["-std=c99", "-O2", "-fPIC", "-Wall", "-Wextra", "-c"])
            .arg("-I")
            .arg(&include)
            .arg(path)
            .arg("-o")
            .arg(&obj)
            .status();
        match status {
            Ok(s) if s.success() => objects.push(obj),
            _ => {
                println!(
                    "cargo:warning=mmuko: no usable C compiler for the {}-bit profile; \
                     the Rust/C differential suite will be skipped",
                    if width == "32" { "32" } else { "64" }
                );
                println!("cargo:rustc-cfg=mmuko_no_c_core");
                return;
            }
        }
    }

    let lib = out.join("libmmuko_abi_core.a");
    let _ = std::fs::remove_file(&lib);
    let ar = env::var("AR").unwrap_or_else(|_| "ar".to_string());
    let status = Command::new(&ar).arg("rcs").arg(&lib).args(&objects).status();
    match status {
        Ok(s) if s.success() => {
            println!("cargo:rustc-link-search=native={}", out.display());
            println!("cargo:rustc-link-lib=static=mmuko_abi_core");
        }
        _ => {
            println!("cargo:warning=mmuko: `ar` failed; differential suite skipped");
            println!("cargo:rustc-cfg=mmuko_no_c_core");
        }
    }
}
