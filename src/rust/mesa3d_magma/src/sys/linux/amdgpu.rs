// Copyright 2024 The Chromium OS Authors. All rights reservsize.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::os::raw::c_int;
use std::sync::Arc;

use log::error;
use mesa3d_util::log_status;
use mesa3d_util::AsRawDescriptor;
use mesa3d_util::MappedRegion;
use mesa3d_util::MesaError;
use mesa3d_util::MesaHandle;
use mesa3d_util::MesaResult;
use nix::ioctl_readwrite;
use nix::ioctl_write_ptr;

use crate::magma_defines::MagmaCreateBufferInfo;
use crate::magma_defines::MagmaHeap;
use crate::magma_defines::MagmaHeapBudget;
use crate::magma_defines::MagmaMemoryProperties;
use crate::magma_defines::MAGMA_BUFFER_FLAG_AMD_GDS;
use crate::magma_defines::MAGMA_BUFFER_FLAG_AMD_OA;
use crate::magma_defines::MAGMA_HEAP_DEVICE_LOCAL_BIT;
use crate::magma_defines::MAGMA_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
use crate::magma_defines::MAGMA_MEMORY_PROPERTY_HOST_CACHED_BIT;
use crate::magma_defines::MAGMA_MEMORY_PROPERTY_HOST_COHERENT_BIT;
use crate::magma_defines::MAGMA_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
use crate::magma_defines::MAGMA_MEMORY_PROPERTY_PROTECTED_BIT;
use crate::sys::linux::bindings::amdgpu_bindings::*;
use crate::sys::linux::bindings::drm_bindings::DRM_COMMAND_BASE;
use crate::sys::linux::bindings::drm_bindings::DRM_IOCTL_BASE;
use crate::sys::linux::PlatformDevice;
use crate::traits::Buffer;
use crate::traits::Context;
use crate::traits::Device;
use crate::traits::GenericBuffer;
use crate::traits::GenericDevice;
use crate::traits::PhysicalDevice;

const AMDGPU_HEAP_GTT: u64 = 0x00000010;
const AMDGPU_HEAP_VRAM: u64 = 0x00000100;
const AMDGPU_HEAP_VRAM_CPU: u64 = 0x00001000;

ioctl_readwrite!(
    drm_ioctl_amdgpu_ctx,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_AMDGPU_CTX,
    drm_amdgpu_ctx
);

ioctl_write_ptr!(
    drm_ioctl_amdgpu_info,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_AMDGPU_INFO,
    drm_amdgpu_info
);

macro_rules! amdgpu_info_ioctl {
    ($(#[$attr:meta])* $name:ident, $nr:expr, $ty:ty) => (
        $(#[$attr])*
        pub unsafe fn $name(descriptor: c_int,
                            data: *mut $ty)
                            -> MesaResult<()> {
            let mut info: drm_amdgpu_info = Default::default();
            info.query = $nr;
            info.return_size = ::std::mem::size_of::<$ty>() as u32;
            info.return_pointer = data as __u64;
            drm_ioctl_amdgpu_info(descriptor, &info)?;
            Ok(())
        }
    )
}

amdgpu_info_ioctl!(
    drm_ioctl_amdgpu_info_memory,
    AMDGPU_INFO_MEMORY,
    drm_amdgpu_memory_info
);

amdgpu_info_ioctl!(
    drm_ioctl_amdgpu_info_vram_gtt,
    AMDGPU_INFO_VRAM_GTT,
    drm_amdgpu_info_vram_gtt
);

amdgpu_info_ioctl!(drm_ioctl_amdgpu_info_gtt_usage, AMDGPU_INFO_GTT_USAGE, u64);

amdgpu_info_ioctl!(
    drm_ioctl_amdgpu_info_vram_usage,
    AMDGPU_INFO_VRAM_USAGE,
    u64
);

amdgpu_info_ioctl!(
    drm_ioctl_amdgpu_info_vis_vram_usage,
    AMDGPU_INFO_VIS_VRAM_USAGE,
    u64
);

ioctl_readwrite!(
    drm_ioctl_amdgpu_gem_create,
    DRM_IOCTL_BASE,
    DRM_COMMAND_BASE + DRM_AMDGPU_GEM_CREATE,
    drm_amdgpu_gem_create
);

pub struct AmdGpu {
    physical_device: Arc<dyn PhysicalDevice>,
    mem_props: MagmaMemoryProperties,
}

struct AmdGpuContext {
    physical_device: Arc<dyn PhysicalDevice>,
    context_id: u32,
}

struct AmdGpuBuffer {
    physical_device: Arc<dyn PhysicalDevice>,
    gem_handle: u32,
}

impl AmdGpu {
    pub fn new(physical_device: Arc<dyn PhysicalDevice>) -> MesaResult<AmdGpu> {
        let mut mem_props: MagmaMemoryProperties = Default::default();
        let mut memory_info: drm_amdgpu_memory_info = Default::default();

        // SAFETY:
        // Valid arguments are supplied for the following arguments:
        //   - Underlying descriptor
        //   - drm_amdgpu_memory_info struct
        let result = unsafe {
            drm_ioctl_amdgpu_info_memory(physical_device.as_raw_descriptor(), &mut memory_info)?;
        };

        if memory_info.gtt.total_heap_size > 0 {
            mem_props.add_heap(memory_info.gtt.total_heap_size, AMDGPU_HEAP_GTT);
            mem_props.add_memory_type(
                MAGMA_MEMORY_PROPERTY_HOST_COHERENT_BIT | MAGMA_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            );
            mem_props.add_memory_type(
                MAGMA_MEMORY_PROPERTY_HOST_COHERENT_BIT
                    | MAGMA_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                    | MAGMA_MEMORY_PROPERTY_HOST_CACHED_BIT,
            );
            mem_props.increment_heap_count();
        }

        if memory_info.vram.total_heap_size > 0 {
            mem_props.add_heap(
                memory_info.vram.total_heap_size,
                MAGMA_HEAP_DEVICE_LOCAL_BIT | AMDGPU_HEAP_VRAM,
            );
            mem_props.add_memory_type(MAGMA_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            mem_props.increment_heap_count();
        }

        if memory_info.cpu_accessible_vram.total_heap_size > 0 {
            mem_props.add_heap(
                memory_info.cpu_accessible_vram.total_heap_size,
                MAGMA_HEAP_DEVICE_LOCAL_BIT | AMDGPU_HEAP_VRAM_CPU,
            );
            mem_props.add_memory_type(
                MAGMA_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                    | MAGMA_MEMORY_PROPERTY_HOST_COHERENT_BIT
                    | MAGMA_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            );
            mem_props.increment_heap_count();
        }

        Ok(AmdGpu {
            physical_device,
            mem_props,
        })
    }
}

impl GenericDevice for AmdGpu {
    fn get_memory_properties(&self) -> MesaResult<MagmaMemoryProperties> {
        Ok(self.mem_props.clone())
    }

    fn get_memory_budget(&self, heap_idx: u32) -> MesaResult<MagmaHeapBudget> {
        if heap_idx >= self.mem_props.memory_heap_count {
            return Err(MesaError::SpecViolation("Heap Index out of bounds"));
        }

        let mut vram_gtt: drm_amdgpu_info_vram_gtt = Default::default();

        // SAFETY:
        // Valid arguments are supplied for the following arguments:
        //   - Underlying descriptor
        //   - drm_amdgpu_memory_info_vram_gtt struct
        unsafe {
            drm_ioctl_amdgpu_info_vram_gtt(
                self.physical_device.as_raw_descriptor(),
                &mut vram_gtt,
            )?;
        };

        let mut budget: u64 = 0;
        let mut usage: u64 = 0;
        let heap_flags = self.mem_props.memory_heaps[heap_idx as usize].heap_flags;

        if heap_flags & AMDGPU_HEAP_GTT != 0 {
            budget = vram_gtt.gtt_size;

            // SAFETY:
            // Valid arguments are supplied for the following arguments:
            //   - Underlying descriptor
            //   - usage
            unsafe {
                drm_ioctl_amdgpu_info_gtt_usage(
                    self.physical_device.as_raw_descriptor(),
                    &mut usage,
                )?;
            };
        } else if heap_flags & AMDGPU_HEAP_VRAM != 0 {
            budget = vram_gtt.vram_size;

            // SAFETY:
            // Valid arguments are supplied for the following arguments:
            //   - Underlying descriptor
            //   - usage
            unsafe {
                drm_ioctl_amdgpu_info_vram_usage(
                    self.physical_device.as_raw_descriptor(),
                    &mut usage,
                )?;
            };
        } else if heap_flags & AMDGPU_HEAP_VRAM_CPU != 0 {
            budget = vram_gtt.vram_cpu_accessible_size;

            // SAFETY:
            // Valid arguments are supplied for the following arguments:
            //   - Underlying descriptor
            //   - usage
            unsafe {
                drm_ioctl_amdgpu_info_vis_vram_usage(
                    self.physical_device.as_raw_descriptor(),
                    &mut usage,
                )?;
            };
        }

        Ok(MagmaHeapBudget { budget, usage })
    }

    fn create_context(&self, device: &Arc<dyn Device>) -> MesaResult<Arc<dyn Context>> {
        let ctx = AmdGpuContext::new(self.physical_device.clone(), 0)?;
        Ok(Arc::new(ctx))
    }

    fn create_buffer(
        &self,
        physical_device: &Arc<dyn Device>,
        create_info: &MagmaCreateBufferInfo,
    ) -> MesaResult<Arc<dyn Buffer>> {
        let buf = AmdGpuBuffer::new(self.physical_device.clone(), create_info, &self.mem_props)?;
        Ok(Arc::new(buf))
    }
}

impl Device for AmdGpu {}
impl PlatformDevice for AmdGpu {}

impl AmdGpuContext {
    fn new(physical_device: Arc<dyn PhysicalDevice>, priority: i32) -> MesaResult<AmdGpuContext> {
        let mut ctx_arg = drm_amdgpu_ctx::default();
        ctx_arg.in_.op = AMDGPU_CTX_OP_ALLOC_CTX;

        // SAFETY:
        // Valid arguments are supplied for the following arguments:
        //   - Underlying descriptor
        //   - drm_amdgpu_ctx struct
        let context_id: u32 = unsafe {
            drm_ioctl_amdgpu_ctx(physical_device.as_raw_descriptor(), &mut ctx_arg)?;
            ctx_arg.out.alloc.ctx_id
        };

        Ok(AmdGpuContext {
            physical_device,
            context_id,
        })
    }
}

impl Drop for AmdGpuContext {
    fn drop(&mut self) {
        let mut ctx_arg = drm_amdgpu_ctx::default();
        ctx_arg.in_.op = AMDGPU_CTX_OP_FREE_CTX;
        ctx_arg.in_.ctx_id = self.context_id;

        // SAFETY:
        // Valid arguments are supplied for the following arguments:
        //   - Underlying descriptor
        //   - drm_amdgpu_ctx struct
        let result =
            unsafe { drm_ioctl_amdgpu_ctx(self.physical_device.as_raw_descriptor(), &mut ctx_arg) };
        log_status!(result);
    }
}

impl Context for AmdGpuContext {}

impl AmdGpuBuffer {
    fn new(
        physical_device: Arc<dyn PhysicalDevice>,
        create_info: &MagmaCreateBufferInfo,
        mem_props: &MagmaMemoryProperties,
    ) -> MesaResult<AmdGpuBuffer> {
        let mut gem_create_in: drm_amdgpu_gem_create_in = Default::default();
        let mut gem_create: drm_amdgpu_gem_create = Default::default();

        let memory_type = mem_props.get_memory_type(create_info.memory_type_idx);

        gem_create_in.bo_size = create_info.size;
        // FIXME: gpu_info.pte_fragment_size, alignment
        // Need GPU topology crate
        gem_create_in.alignment = create_info.alignment as u64;

        // Goal: An explicit sync world + discardable world only.
        gem_create_in.domain_flags |= AMDGPU_GEM_CREATE_EXPLICIT_SYNC as u64;
        gem_create_in.domain_flags |= AMDGPU_GEM_CREATE_DISCARDABLE as u64;

        if memory_type.is_coherent() {
            gem_create_in.domain_flags |= AMDGPU_GEM_CREATE_CPU_GTT_USWC as u64;
        } else {
            gem_create_in.domain_flags |= AMDGPU_GEM_CREATE_NO_CPU_ACCESS as u64;
        }

        if memory_type.is_protected() {
            gem_create_in.domain_flags |= AMDGPU_GEM_CREATE_ENCRYPTED as u64;
        }

        // Should these be "heaps" of zero size?
        if create_info.vendor_flags & MAGMA_BUFFER_FLAG_AMD_OA != 0 {
            gem_create_in.domains |= AMDGPU_GEM_DOMAIN_OA as u64
        } else if create_info.vendor_flags & MAGMA_BUFFER_FLAG_AMD_GDS != 0 {
            gem_create_in.domains |= AMDGPU_GEM_DOMAIN_GDS as u64;
        } else if memory_type.is_device_local() {
            gem_create_in.domains |= AMDGPU_GEM_DOMAIN_VRAM as u64;
        } else {
            gem_create_in.domains |= AMDGPU_GEM_DOMAIN_GTT as u64;
        }

        // SAFETY:
        // Valid arguments are supplied for the following arguments:
        //   - Underlying descriptor
        //   - drm_amgpu_gem_create_args
        let gem_handle = unsafe {
            gem_create.in_ = gem_create_in;
            drm_ioctl_amdgpu_gem_create(physical_device.as_raw_descriptor(), &mut gem_create)?;
            gem_create.out.handle
        };

        Ok(AmdGpuBuffer {
            physical_device,
            gem_handle,
        })
    }
}

impl GenericBuffer for AmdGpuBuffer {
    fn map(&self) -> MesaResult<Arc<dyn MappedRegion>> {
        Err(MesaError::Unsupported)
    }

    fn export(&self) -> MesaResult<MesaHandle> {
        Err(MesaError::Unsupported)
    }
}

impl Drop for AmdGpuBuffer {
    fn drop(&mut self) {
        // GEM close
    }
}

impl Buffer for AmdGpuBuffer {}
