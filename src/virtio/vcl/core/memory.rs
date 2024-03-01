/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::context::*;
use crate::dev::renderer::Vcl;
use crate::impl_cl_type_trait;

use vcl_opencl_gen::*;

use std::os::raw::c_void;
use std::sync::Arc;

pub struct Mem {
    pub base: CLObjectBase<CL_INVALID_MEM_OBJECT>,
    pub context: Arc<Context>,
    pub flags: cl_mem_flags,
    pub size: usize,
}

impl_cl_type_trait!(cl_mem, Mem, CL_INVALID_MEM_OBJECT);

impl Mem {
    pub fn new_buffer(
        context: &Arc<Context>,
        flags: cl_mem_flags,
        size: usize,
        host_ptr: *mut c_void,
    ) -> CLResult<cl_mem> {
        let buffer = Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            flags,
            size,
        });

        let handle = cl_mem::from_arc(buffer);
        let mut guest_handle = handle;

        Vcl::get().call_clCreateBufferMESA(
            context.get_handle(),
            flags,
            size,
            host_ptr,
            &mut guest_handle,
        )?;

        Ok(handle)
    }
}
