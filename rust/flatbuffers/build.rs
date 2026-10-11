use rustc_version::{version_meta, Channel};

fn main() {
    let version_meta = version_meta().unwrap();

    // To use nightly features we declare this and then we can use
    // #[cfg(nightly)]
    // for nightly only features
    println!("cargo:rustc-check-cfg=cfg(nightly)");
    if version_meta.channel == Channel::Nightly {
        println!("cargo:rustc-cfg=nightly")
    }

    // `core::error::Error` is stable since Rust 1.81 on every channel, so a
    // no_std build implements it whenever the compiler has it. Compare
    // major.minor only: semver sorts 1.81.0-nightly before 1.81.0.
    println!("cargo:rustc-check-cfg=cfg(core_error)");
    let version = &version_meta.semver;
    if (version.major, version.minor) >= (1, 81) {
        println!("cargo:rustc-cfg=core_error")
    }
}
