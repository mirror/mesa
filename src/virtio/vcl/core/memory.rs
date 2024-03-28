/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::context::*;
use crate::dev::renderer::Vcl;
use crate::impl_cl_type_trait;

use mesa_rust_util::properties::Properties;
use vcl_opencl_gen::*;

use std::os::raw::c_void;
use std::ptr;
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
        let mut handle = cl_mem::from_arc(buffer);
        Vcl::get().call_clCreateBufferMESA(
            context.get_handle(),
            flags,
            size,
            host_ptr,
            &mut handle,
        )?;
        Ok(handle)
    }

    pub fn new_image(
        context: &Arc<Context>,
        desc: cl_image_desc,
        flags: cl_mem_flags,
        image_format: *const cl_image_format,
        host_ptr: *mut c_void,
    ) -> CLResult<cl_mem> {
        let size = match desc.image_type {
            CL_MEM_OBJECT_IMAGE2D => desc.image_height * desc.image_row_pitch,
            CL_MEM_OBJECT_IMAGE3D => desc.image_depth * desc.image_slice_pitch,
            _ => unimplemented!(),
        };

        let image = Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            flags,
            size,
        });

        let mut handle = cl_mem::from_arc(image);

        match desc.image_type {
            CL_MEM_OBJECT_IMAGE2D => Vcl::get().call_clCreateImage2DMESA(
                context.get_handle(),
                flags,
                image_format,
                desc.image_width,
                desc.image_height,
                desc.image_row_pitch,
                size,
                host_ptr,
                &mut handle,
            )?,
            CL_MEM_OBJECT_IMAGE3D => Vcl::get().call_clCreateImage3DMESA(
                context.get_handle(),
                flags,
                image_format,
                desc.image_width,
                desc.image_height,
                desc.image_depth,
                desc.image_row_pitch,
                desc.image_slice_pitch,
                size,
                host_ptr,
                &mut handle,
            )?,
            _ => unimplemented!(),
        }

        Ok(handle)
    }
}

pub struct Sampler {
    pub base: CLObjectBase<CL_INVALID_SAMPLER>,
    pub context: Arc<Context>,
    pub normalized_coords: bool,
    pub addressing_mode: cl_addressing_mode,
    pub filter_mode: cl_filter_mode,
    pub props: Properties<cl_sampler_properties>,
}

impl_cl_type_trait!(cl_sampler, Sampler, CL_INVALID_SAMPLER);

impl Sampler {
    pub fn new(
        context: Arc<Context>,
        normalized_coords: bool,
        addressing_mode: cl_addressing_mode,
        filter_mode: cl_filter_mode,
    ) -> CLResult<Arc<Sampler>> {
        let sampler = Arc::new(Self {
            base: CLObjectBase::new(),
            context,
            normalized_coords,
            addressing_mode,
            filter_mode,
            props: Properties::default(),
        });

        Vcl::get().call_clCreateSamplerMESA(
            sampler.context.get_handle(),
            if normalized_coords { CL_TRUE } else { CL_FALSE },
            addressing_mode,
            filter_mode,
            &mut sampler.get_handle(),
        )?;

        Ok(sampler)
    }

    pub fn new_with_properties(
        context: Arc<Context>,
        normalized_coords: bool,
        addressing_mode: cl_addressing_mode,
        filter_mode: cl_filter_mode,
        props: Properties<cl_sampler_properties>,
    ) -> CLResult<Arc<Sampler>> {
        let sampler = Arc::new(Self {
            base: CLObjectBase::new(),
            context,
            normalized_coords,
            addressing_mode,
            filter_mode,
            props,
        });

        let props = sampler.props.to_raw();
        let props_ptr = if props.len() > 1 {
            props.as_ptr()
        } else {
            ptr::null()
        };

        Vcl::get().call_clCreateSamplerWithPropertiesMESA(
            sampler.context.get_handle(),
            props_ptr,
            &mut sampler.get_handle(),
        )?;

        Ok(sampler)
    }
}
