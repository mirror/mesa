// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::env;
use tar::Archive;
use std::path::PathBuf;
use flate2::read::GzDecoder;

const LIBDXG_URL: &'static str =
    "https://github.com/microsoft/libdxg/archive/refs/heads/main.tar.gz";

fn generate_windows_bindings(out_dir: PathBuf) {
    let response = reqwest::blocking::get(LIBDXG_URL).unwrap().bytes().unwrap();
    let gz = GzDecoder::new(response.as_ref());
    let mut archive = Archive::new(gz);
    archive.unpack(&out_dir).unwrap();

    let clang_arg = format!(
        "-I{}/libdxg-main/include",
        out_dir.display(),
    );

    bindgen::Builder::default()
        .header("headers/windows/d3dkmt_wrapper.h")
        .clang_arg(clang_arg)
        .derive_default(true)
        .derive_debug(true)
        .size_t_is_usize(true)
        .prepend_enum_name(false)
        .generate_comments(false)
        .layout_tests(false)
        .generate()
        .expect("Unable to generate d3dkmt.h bindings")
        .write_to_file(out_dir.join("d3dkmt_bindings.rs"))
        .expect("Unable to generate d3dkmt bindings");
}

fn generate_linux_bindings(out_dir: PathBuf) {
    bindgen::Builder::default()
        .header("../../include/drm-uapi/drm.h")
        .derive_default(true)
        .derive_debug(true)
        .allowlist_var("DRM_.+")
        .allowlist_type("drm_.+")
        .prepend_enum_name(false)
        .generate_comments(false)
        .layout_tests(false)
        .generate()
        .expect("Unable to generate drm bindings")
        .write_to_file(out_dir.join("drm_bindings.rs"))
        .expect("Unable to generate bindings");

    bindgen::Builder::default()
        .header("../../include/drm-uapi/i915_drm.h")
        .derive_default(true)
        .derive_debug(true)
        .allowlist_var("DRM_I915_.+")
        .allowlist_var("I915_.+")
        .allowlist_type("drm_i915_.+")
        .prepend_enum_name(false)
        .generate_comments(false)
        .layout_tests(false)
        .generate()
        .expect("Unable to generate i915 bindings")
        .write_to_file(out_dir.join("i915_bindings.rs"))
        .expect("Unable to generate bindings");

    bindgen::Builder::default()
        .header("../../include/drm-uapi/amdgpu_drm.h")
        .derive_default(true)
        .derive_debug(true)
        .allowlist_var("DRM_AMDGPU_.+")
        .allowlist_var("AMDGPU_.+")
        .allowlist_type("drm_amdgpu_.+")
        .prepend_enum_name(false)
        .generate_comments(false)
        .layout_tests(false)
        .generate()
        .expect("Unable to generate amdgpu bindings")
        .write_to_file(out_dir.join("amdgpu_bindings.rs"))
        .expect("Unable to generate bindings");

    bindgen::Builder::default()
        .header("../../include/drm-uapi/msm_drm.h")
        .derive_default(true)
        .derive_debug(true)
        .allowlist_var("DRM_MSM_.+")
        .allowlist_var("MSM_.+")
        .allowlist_type("drm_msm_.+")
        .prepend_enum_name(false)
        .generate_comments(false)
        .layout_tests(false)
        .generate()
        .expect("Unable to generate msm bindings")
        .write_to_file(out_dir.join("msm_bindings.rs"))
        .expect("Unable to generate bindings");

    bindgen::Builder::default()
        .header("../../include/drm-uapi/virtgpu_drm.h")
        .derive_default(true)
        .derive_debug(true)
        .allowlist_var("DRM_VIRTGPU_.+")
        .allowlist_var("VIRTGPU_.+")
        .allowlist_type("drm_virtgpu_.+")
        .prepend_enum_name(false)
        .generate_comments(false)
        .layout_tests(false)
        .generate()
        .expect("Unable to generate virtgpu bindings")
        .write_to_file(out_dir.join("virtgpu_bindings.rs"))
        .expect("Unable to generate virtgpu bindings");
}

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR should always be set"));

    match target_os.as_str() {
        "linux" => generate_linux_bindings(out_dir),
        "windows" => generate_windows_bindings(out_dir),
        _ => (),
    }
}
