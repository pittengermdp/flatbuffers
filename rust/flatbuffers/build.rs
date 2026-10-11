use rustc_version::{version_meta, Channel, Version};

fn main() {
    let version_meta = version_meta().unwrap();

    // To use nightly features we declare this and then we can use
    // #[cfg(nightly)]
    // for nightly only features
    println!("cargo:rustc-check-cfg=cfg(nightly)");
    // `core::error::Error` is stable from 1.81; only older nightlies need
    // #![feature(error_in_core)], and newer ones warn that it is stable.
    println!("cargo:rustc-check-cfg=cfg(error_in_core_unstable)");
    if version_meta.channel == Channel::Nightly {
        println!("cargo:rustc-cfg=nightly");
        if version_meta.semver < Version::new(1, 81, 0) {
            println!("cargo:rustc-cfg=error_in_core_unstable");
        }
    }
}
