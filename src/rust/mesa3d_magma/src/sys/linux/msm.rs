// Copyright 2024 The Chromium OS Authors. All rights reservsize.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use std::sync::Arc;

use nix::ioctl_readwrite;
use nix::ioctl_write_ptr;

use mesa3d_util::AsRawDescriptor;
use mesa3d_util::MappedRegion;
use mesa3d_util::MesaError;
use mesa3d_util::MesaHandle;
use mesa3d_util::MesaResult;
use mesa3d_util::OwnedDescriptor;

use crate::traits::Buffer;
use crate::traits::Context;
use crate::traits::Device;
use crate::traits::GenericBuffer;
use crate::traits::GenericDevice;
use crate::traits::PhysicalDevice;

use crate::magma_defines::MagmaCreateBufferInfo;
use crate::magma_defines::MagmaHeap;
use crate::magma_defines::MagmaHeapBudget;
use crate::magma_defines::MagmaMemoryProperties;

use crate::sys::linux::bindings::drm_bindings::DRM_COMMAND_BASE;
use crate::sys::linux::bindings::drm_bindings::DRM_IOCTL_BASE;
use crate::sys::linux::bindings::msm_bindings::*;
use crate::sys::linux::PlatformDevice;

ioctl_readwrite!(
    drm_ioctl_msm_get_param,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_GET_PARAM,
    drm_msm_param
);

ioctl_readwrite!(
    drm_ioctl_msm_gem_new,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_GEM_NEW,
    drm_msm_gem_new
);

ioctl_readwrite!(
    drm_ioctl_mem_gem_info,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_GEM_INFO,
    drm_msm_gem_info
);

ioctl_write_ptr!(
    msm_gem_cpu_prep,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_PREP,
    drm_msm_gem_cpu_prep
);

ioctl_write_ptr!(
    msm_gem_cpu_fini,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_FINI,
    drm_msm_gem_cpu_fini
);

ioctl_readwrite!(
    msm_gem_submit,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT,
    drm_msm_gem_submit
);

ioctl_write_ptr!(
    msm_wait_fence,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE,
    drm_msm_wait_fence
);

ioctl_readwrite!(
    msm_gem_madvise,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_GEM_MADVISE,
    drm_msm_gem_madvise
);

ioctl_readwrite!(
    msm_submitqueue_new,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_NEW,
    drm_msm_submitqueue
);

ioctl_write_ptr!(
    msm_submitqueue_close,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_CLOSE,
    __u32
);

ioctl_write_ptr!(
    msm_submitqueue_query,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_QUERY,
    drm_msm_submitqueue_query
);

pub struct Msm {
    physical_device: Arc<dyn PhysicalDevice>,
    mem_props: MagmaMemoryProperties,
}

struct MsmBuffer {
    physical_device: Arc<dyn PhysicalDevice>,
    gem_handle: u32,
}

impl Msm {
    pub fn new(physical_device: Arc<dyn PhysicalDevice>) -> Msm {
        Msm {
            physical_device,
            mem_props: Default::default(),
        }
    }
}

impl GenericDevice for Msm {
    fn get_memory_properties(&self) -> MesaResult<MagmaMemoryProperties> {
        Err(MesaError::Unsupported)
    }

    fn get_memory_budget(&self, heap_idx: u32) -> MesaResult<MagmaHeapBudget> {
        Err(MesaError::Unsupported)
    }

    fn create_context(&self, device: &Arc<dyn Device>) -> MesaResult<Arc<dyn Context>> {
        Err(MesaError::Unsupported)
    }

    fn create_buffer(
        &self,
        device: &Arc<dyn Device>,
        create_info: &MagmaCreateBufferInfo,
    ) -> MesaResult<Arc<dyn Buffer>> {
        let buf = MsmBuffer::new(self.physical_device.clone(), create_info, &self.mem_props)?;
        Ok(Arc::new(buf))
    }
}

impl PlatformDevice for Msm {}
impl Device for Msm {}

impl MsmBuffer {
    fn new(
        physical_device: Arc<dyn PhysicalDevice>,
        create_info: &MagmaCreateBufferInfo,
        mem_props: &MagmaMemoryProperties,
    ) -> MesaResult<MsmBuffer> {
        let mut gem_new = drm_msm_gem_new::default();
        gem_new.size = create_info.size;
        gem_new.flags = 0;

        // SAFETY: This is a well-formed ioctl conforming the driver specificiation.
        unsafe {
            drm_ioctl_msm_gem_new(physical_device.as_raw_descriptor(), &mut gem_new)?;
        }

        Ok(MsmBuffer {
            physical_device,
            gem_handle: gem_new.handle,
        })
    }
}

impl GenericBuffer for MsmBuffer {
    fn map(&self) -> MesaResult<Arc<dyn MappedRegion>> {
        Err(MesaError::Unsupported)
    }

    fn export(&self) -> MesaResult<MesaHandle> {
        Err(MesaError::Unsupported)
    }
}

impl Drop for MsmBuffer {
    fn drop(&mut self) {
        // GEM close
    }
}

impl Buffer for MsmBuffer {}
