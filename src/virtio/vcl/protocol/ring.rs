/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use std::rc::Rc;

use crate::dev::drm::DrmDevice;
use crate::dev::resource::VirtGpuResource;
use crate::dev::virtgpu::*;
use crate::protocol::cs::*;

use vcl_drm_gen::*;

pub struct VirglReplyBuffer {
    /// Resource for receiving the reply
    pub res: VirtGpuResource,
    cur: usize,
}

impl VirglReplyBuffer {
    const VCL_MAX_REPLY_DWORDS: usize = (64 * 1024);

    pub fn new(drm_device: Rc<DrmDevice>) -> Result<Self, VirtGpuError> {
        // TODO: How to determine the size of the reply?
        Ok(Self {
            res: VirtGpuResource::new(drm_device, Self::VCL_MAX_REPLY_DWORDS)?,
            cur: 0,
        })
    }

    pub fn map(&mut self, size: usize) -> Result<&[u8], VirtGpuError> {
        let ret = self.res.map(self.cur, size);
        self.cur += size;
        ret
    }
}

pub struct VirtGpuRing {
    _replybuf: VirglReplyBuffer,
    drm_device: Rc<DrmDevice>,
}

impl VirtGpuRing {
    pub fn new(drm_device: Rc<DrmDevice>) -> Result<Self, VirtGpuError> {
        let replybuf = VirglReplyBuffer::new(drm_device.clone())?;
        Ok(Self {
            _replybuf: replybuf,
            drm_device,
        })
    }
}

pub struct VirtGpuRingReply {
    dec: VclCsDecoder,
}

impl std::ops::Deref for VirtGpuRingReply {
    type Target = VclCsDecoder;

    fn deref(&self) -> &Self::Target {
        &self.dec
    }
}

impl std::ops::DerefMut for VirtGpuRingReply {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.dec
    }
}

impl VirtGpuRingReply {
    pub fn new(buffer: &[u8]) -> Self {
        Self {
            dec: VclCsDecoder::new(buffer),
        }
    }
}

impl VirtGpuRing {
    fn exec_buffer(&self, buffer: &[u8]) -> Result<(), VirtGpuError> {
        let mut eb = drm_virtgpu_execbuffer {
            command: buffer.as_ptr() as _,
            size: buffer.len() as u32,
            num_bo_handles: 0,
            bo_handles: 0,
            fence_fd: -1,
            flags: 0,
            ring_idx: 0,
            pad: 0,
        };
        self.drm_device.ioctl(drm_ioctl_virtgpu_EXECBUFFER, &mut eb)
    }

    pub fn submit(
        &mut self,
        encoder: VclCsEncoder,
        _reply_size: usize,
    ) -> Result<Option<VclCsDecoder>, VirtGpuError> {
        if encoder.is_empty() {
            return Ok(None);
        }

        self.exec_buffer(encoder.get_slice())?;
        Ok(None)
    }
}
