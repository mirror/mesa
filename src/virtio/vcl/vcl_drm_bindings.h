#include <errno.h>
#include <xf86drm.h>
#include <drm-uapi/virtgpu_drm.h>

/// DRM_IOCTL_VIRTGPU_* are functional macros and unfortunately are not exported
/// by rust bindgen. See https://github.com/rust-lang/rust-bindgen/issues/753
/// With this workaround we are able to provide those values to VCL Rust code.
enum drm_ioctl_virtgpu : uint64_t {
    GETPARAM = DRM_IOCTL_VIRTGPU_GETPARAM,
    GET_CAPS = DRM_IOCTL_VIRTGPU_GET_CAPS,
    EXECBUFFER = DRM_IOCTL_VIRTGPU_EXECBUFFER,
    CONTEXT_INIT = DRM_IOCTL_VIRTGPU_CONTEXT_INIT,
    RESOURCE_CREATE = DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,
    WAIT = DRM_IOCTL_VIRTGPU_WAIT,
    MAP = DRM_IOCTL_VIRTGPU_MAP,
    TRANSFER_FROM_HOST = DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST,
    TRANSFER_TO_HOST = DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST,
};

enum virtgpu_context_param {
    CAPSET_ID = VIRTGPU_CONTEXT_PARAM_CAPSET_ID,
};
