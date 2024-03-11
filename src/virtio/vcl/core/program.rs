/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::context::Context;
use crate::dev::renderer::*;
use crate::impl_cl_type_trait;
use vcl_opencl_gen::*;

use std::ffi::c_char;
use std::sync::Arc;

impl_cl_type_trait!(cl_program, Program, CL_INVALID_PROGRAM);

pub struct Program {
    base: CLObjectBase<CL_INVALID_PROGRAM>,
    pub context: Arc<Context>,
}

impl Program {
    pub fn new_with_source(
        context: Arc<Context>,
        count: cl_uint,
        strings: *mut *const c_char,
        lengths: *const usize,
    ) -> CLResult<Arc<Program>> {
        let program = Arc::new(Program {
            base: Default::default(),
            context: context.clone(),
        });

        Vcl::get().call_clCreateProgramWithSourceMESA(
            context.get_handle(),
            count,
            strings,
            lengths,
            &mut program.get_handle(),
        )?;

        Ok(program)
    }
}
