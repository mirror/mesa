#include <xf86drm.h>
#include <virtgpu_drm.h>

/// DRM_IOCTL_VIRTGPU_* are functional macros and unfortunately are not exported
/// by rust bindgen. See https://github.com/rust-lang/rust-bindgen/issues/753
/// With this workaround we are able to provide those values to VCL Rust code.
enum drm_ioctl_virtgpu : uint64_t {
    GET_CAPS = DRM_IOCTL_VIRTGPU_GET_CAPS
};
