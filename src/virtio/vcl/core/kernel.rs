/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::program::Program;
use crate::dev::renderer::Vcl;
use crate::impl_cl_type_trait;

use vcl_opencl_gen::*;

use std::os::raw::c_char;
use std::sync::Arc;

impl_cl_type_trait!(cl_kernel, Kernel, CL_INVALID_KERNEL);

pub struct Kernel {
    pub base: CLObjectBase<CL_INVALID_KERNEL>,
    pub program: Arc<Program>,
}

impl Kernel {
    pub fn new(program: &Arc<Program>) -> Arc<Kernel> {
        Arc::new(Self {
            base: CLObjectBase::new(),
            program: program.clone(),
        })
    }

    pub fn create(program: &Arc<Program>, kernel_name: *const c_char) -> CLResult<Arc<Kernel>> {
        let kernel = Self::new(program);
        Vcl::get().call_clCreateKernelMESA(
            program.get_handle(),
            kernel_name,
            &mut kernel.get_handle(),
        )?;
        Ok(kernel)
    }

    pub fn clone(source_kernel: &Arc<Kernel>) -> CLResult<Arc<Kernel>> {
        let kernel = Self::new(&source_kernel.program);
        Vcl::get().call_clCloneKernelMESA(source_kernel.get_handle(), &mut kernel.get_handle())?;
        Ok(kernel)
    }
}
