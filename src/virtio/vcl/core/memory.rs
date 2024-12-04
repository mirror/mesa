/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::types::*;
use crate::core::context::*;
use crate::core::event::Event;
use crate::core::format::CLFormatInfo;
use crate::core::queue::Queue;
use crate::dev::renderer::Vcl;
use crate::impl_cl_type_trait;

use mesa_rust_util::properties::Properties;
use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;

use std::cmp;
use std::collections::HashMap;
use std::os::raw::c_void;
use std::ptr;
use std::sync::*;

pub struct Mem {
    pub base: CLObjectBase<CL_INVALID_MEM_OBJECT>,
    pub context: Arc<Context>,
    pub flags: cl_mem_flags,
    pub size: usize,
    pub image_format: cl_image_format,
    pub image_desc: cl_image_desc,

    /// List of callbacks called when this object gets destroyed
    pub callbacks: Mutex<Vec<Box<dyn Fn(cl_mem)>>>,

    pub mapped_data: Mutex<HashMap<*mut c_void, Vec<u8>>>,
}

impl_cl_type_trait!(cl_mem, Mem, CL_INVALID_MEM_OBJECT);

pub trait CLImageDescInfo {
    fn type_info(&self) -> (u8, bool);
    fn pixels(&self) -> usize;
    fn row_pitch(&self) -> CLResult<u32>;
    fn slice_pitch(&self) -> usize;
    fn width(&self) -> CLResult<u32>;
    fn height(&self) -> CLResult<u32>;
    fn size(&self) -> CLVec<usize>;

    fn dims(&self) -> u8 {
        self.type_info().0
    }

    fn dims_with_array(&self) -> u8 {
        let array: u8 = self.is_array().into();
        self.dims() + array
    }

    fn has_slice(&self) -> bool {
        self.dims() == 3 || self.is_array()
    }

    fn is_array(&self) -> bool {
        self.type_info().1
    }
}

impl CLImageDescInfo for cl_image_desc {
    fn type_info(&self) -> (u8, bool) {
        match self.image_type {
            CL_MEM_OBJECT_IMAGE1D | CL_MEM_OBJECT_IMAGE1D_BUFFER => (1, false),
            CL_MEM_OBJECT_IMAGE1D_ARRAY => (1, true),
            CL_MEM_OBJECT_IMAGE2D => (2, false),
            CL_MEM_OBJECT_IMAGE2D_ARRAY => (2, true),
            CL_MEM_OBJECT_IMAGE3D => (3, false),
            _ => panic!("unknown image_type {:x}", self.image_type),
        }
    }

    fn pixels(&self) -> usize {
        let mut res = self.image_width;
        let dims = self.dims();

        if dims > 1 {
            res *= self.image_height;
        }

        if dims > 2 {
            res *= self.image_depth;
        }

        if self.is_array() {
            res *= self.image_array_size;
        }

        res
    }

    fn size(&self) -> CLVec<usize> {
        let mut height = cmp::max(self.image_height, 1);
        let mut depth = cmp::max(self.image_depth, 1);

        match self.image_type {
            CL_MEM_OBJECT_IMAGE1D_ARRAY => height = self.image_array_size,
            CL_MEM_OBJECT_IMAGE2D_ARRAY => depth = self.image_array_size,
            _ => {}
        }

        CLVec::new([self.image_width, height, depth])
    }

    fn row_pitch(&self) -> CLResult<u32> {
        self.image_row_pitch
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)
    }

    fn slice_pitch(&self) -> usize {
        self.image_slice_pitch
    }

    fn width(&self) -> CLResult<u32> {
        self.image_width
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)
    }

    fn height(&self) -> CLResult<u32> {
        self.image_height
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)
    }
}

impl Mem {
    fn new_buffer_impl(context: &Arc<Context>, flags: cl_mem_flags, size: usize) -> Arc<Self> {
        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            flags,
            size,
            image_format: Default::default(),
            image_desc: Default::default(),
            callbacks: Default::default(),
            mapped_data: Default::default(),
        })
    }

    pub fn new_buffer(
        context: &Arc<Context>,
        flags: cl_mem_flags,
        size: usize,
        host_ptr: *mut c_void,
    ) -> CLResult<cl_mem> {
        let buffer = Self::new_buffer_impl(context, flags, size);
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

    pub fn new_sub_buffer(
        buffer: cl_mem,
        flags: cl_mem_flags,
        buffer_create_type: cl_buffer_create_type,
        buffer_create_info: *const c_void,
    ) -> CLResult<cl_mem> {
        let buf = buffer.get_ref()?;
        let buffer_region = unsafe { &mut *(buffer_create_info as *mut cl_buffer_region) };

        // Get context from parent buffer
        let sub_buffer = Self::new_buffer_impl(&buf.context, flags, buffer_region.size);

        let mut handle = cl_mem::from_arc(sub_buffer);
        Vcl::get().call_clCreateSubBufferMESA(
            buffer,
            flags,
            buffer_create_type,
            std::mem::size_of::<cl_buffer_region>(),
            buffer_create_info,
            &mut handle,
        )?;

        Ok(handle)
    }

    pub fn map_buffer(
        &self,
        queue: &Arc<Queue>,
        map_flags: cl_map_flags,
        offset: usize,
        size: usize,
        num_events_in_wait_list: cl_uint,
        event_wait_list: *const cl_event,
        event: *mut cl_event,
    ) -> CLResult<*mut c_void> {
        let mut data = Vec::with_capacity(size);
        data.resize_with(size, || 0u8);
        let ptr = data.as_mut_ptr() as _;

        let mut ev_handle = Event::maybe_new(&queue.context, event);
        let ev_ptr = if ev_handle.is_null() {
            ptr::null_mut()
        } else {
            &mut ev_handle
        };

        Vcl::get().call_clEnqueueMapBufferMESA(
            queue.get_handle(),
            self.get_handle(),
            CL_TRUE,
            map_flags,
            offset,
            size,
            ptr,
            num_events_in_wait_list,
            event_wait_list,
            ev_ptr,
        )?;

        event.write_checked(ev_handle);

        self.mapped_data.lock().unwrap().insert(ptr, data);

        Ok(ptr)
    }

    pub fn map_image(
        &self,
        queue: &Arc<Queue>,
        map_flags: cl_map_flags,
        origin: CLVec<usize>,
        region: CLVec<usize>,
        image_row_pitch: *mut usize,
        image_slice_pitch: *mut usize,
        num_events_in_wait_list: cl_uint,
        event_wait_list: *const cl_event,
        event: *mut cl_event,
    ) -> CLResult<*mut c_void> {
        let pixel_size = self.image_format.pixel_size().unwrap() as usize;
        let size = region[0] * region[1] * region[2] * pixel_size;

        let mut data = Vec::with_capacity(size);
        data.resize_with(size, || 0u8);
        let ptr = data.as_mut_ptr() as _;

        let mut ev_handle = Event::maybe_new(&queue.context, event);
        let ev_ptr = if ev_handle.is_null() {
            ptr::null_mut()
        } else {
            &mut ev_handle
        };

        Vcl::get().call_clEnqueueMapImageMESA(
            queue.get_handle(),
            self.get_handle(),
            CL_TRUE,
            map_flags,
            origin.as_ptr(),
            region.as_ptr(),
            image_row_pitch,
            image_slice_pitch,
            size,
            ptr,
            num_events_in_wait_list,
            event_wait_list,
            ev_ptr,
        )?;

        event.write_checked(ev_handle);

        self.mapped_data.lock().unwrap().insert(ptr, data);

        Ok(ptr)
    }

    pub fn is_mapped_ptr(&self, mapped_ptr: *mut c_void) -> bool {
        self.mapped_data.lock().unwrap().contains_key(&mapped_ptr)
    }

    pub fn unmap(
        &self,
        queue: &Arc<Queue>,
        mapped_ptr: *mut c_void,
        num_events_in_wait_list: cl_uint,
        event_wait_list: *const cl_event,
        event: *mut cl_event,
    ) -> CLResult<()> {
        let data_len = self
            .mapped_data
            .lock()
            .unwrap()
            .get(&mapped_ptr)
            .ok_or(CL_MAP_FAILURE)?
            .len();

        let mut ev_handle = Event::maybe_new(&queue.context, event);
        let ev_ptr = if ev_handle.is_null() {
            ptr::null_mut()
        } else {
            &mut ev_handle
        };

        Vcl::get().call_clEnqueueUnmapMemObjectMESA(
            queue.get_handle(),
            self.get_handle(),
            data_len,
            mapped_ptr,
            num_events_in_wait_list,
            event_wait_list,
            ev_ptr,
        )?;

        event.write_checked(ev_handle);

        self.mapped_data
            .lock()
            .unwrap()
            .remove_entry(&mapped_ptr)
            .ok_or(CL_MAP_FAILURE)?;

        Ok(())
    }

    fn new_image_impl(
        context: &Arc<Context>,
        flags: cl_mem_flags,
        size: usize,
        image_format: cl_image_format,
        image_desc: cl_image_desc,
    ) -> Arc<Self> {
        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            flags,
            size,
            image_format,
            image_desc,
            callbacks: Default::default(),
            mapped_data: Default::default(),
        })
    }

    pub fn new_image_2d(
        context: &Arc<Context>,
        image_desc: cl_image_desc,
        flags: cl_mem_flags,
        image_format: cl_image_format,
        host_ptr: *mut c_void,
    ) -> CLResult<Arc<Mem>> {
        let pixel_size = image_format.pixel_size().unwrap() as usize;
        let size = image_desc.pixels() * pixel_size;

        let image = Self::new_image_impl(context, flags, size, image_format, image_desc);

        assert_eq!(image_desc.image_type, CL_MEM_OBJECT_IMAGE2D);
        Vcl::get().call_clCreateImage2DMESA(
            image.context.get_handle(),
            flags,
            &image_format,
            image_desc.image_width,
            image_desc.image_height,
            image_desc.image_row_pitch,
            size,
            host_ptr,
            &mut image.get_handle(),
        )?;

        Ok(image)
    }

    pub fn new_image_3d(
        context: &Arc<Context>,
        image_desc: cl_image_desc,
        flags: cl_mem_flags,
        image_format: cl_image_format,
        host_ptr: *mut c_void,
    ) -> CLResult<Arc<Mem>> {
        let pixel_size = image_format.pixel_size().unwrap() as usize;
        let size = image_desc.pixels() * pixel_size;

        let image = Self::new_image_impl(context, flags, size, image_format, image_desc);

        assert_eq!(image_desc.image_type, CL_MEM_OBJECT_IMAGE3D);
        Vcl::get().call_clCreateImage3DMESA(
            image.context.get_handle(),
            flags,
            &image_format,
            image_desc.image_width,
            image_desc.image_height,
            image_desc.image_depth,
            image_desc.image_row_pitch,
            image_desc.image_slice_pitch,
            size,
            host_ptr,
            &mut image.get_handle(),
        )?;

        Ok(image)
    }

    pub fn new_image(
        context: Arc<Context>,
        image_desc: cl_image_desc,
        flags: cl_mem_flags,
        image_format: cl_image_format,
        host_ptr: *mut c_void,
    ) -> CLResult<Arc<Mem>> {
        let pixel_size = image_format.pixel_size().unwrap() as usize;
        let size = image_desc.pixels() * pixel_size;

        let image = Self::new_image_impl(&context, flags, size, image_format, image_desc);

        let image_desc = cl_image_desc_MESA {
            image_type: image_desc.image_type,
            image_width: image_desc.image_width,
            image_height: image_desc.image_height,
            image_depth: image_desc.image_depth,
            image_array_size: image_desc.image_array_size,
            image_row_pitch: image_desc.image_row_pitch,
            image_slice_pitch: image_desc.image_slice_pitch,
            num_mip_levels: image_desc.num_mip_levels,
            num_samples: image_desc.num_samples,
            mem_object: unsafe { image_desc.anon_1.mem_object },
        };
        Vcl::get().call_clCreateImageMESA(
            image.context.get_handle(),
            flags,
            &image_format,
            &image_desc,
            size,
            host_ptr,
            &mut image.get_handle(),
        )?;

        Ok(image)
    }

    pub fn new_image_with_properties(
        context: Arc<Context>,
        image_desc: cl_image_desc,
        flags: cl_mem_flags,
        image_format: cl_image_format,
        host_ptr: *mut c_void,
        props: Properties<cl_mem_properties>,
    ) -> CLResult<Arc<Mem>> {
        let pixel_size = image_format.pixel_size().unwrap() as usize;
        let size = image_desc.pixels() * pixel_size;

        let image = Self::new_image_impl(&context, flags, size, image_format, image_desc);

        let props = props.to_raw();
        let props_ptr = if props.len() > 1 {
            props.as_ptr()
        } else {
            ptr::null()
        };

        let image_desc = cl_image_desc_MESA {
            image_type: image_desc.image_type,
            image_width: image_desc.image_width,
            image_height: image_desc.image_height,
            image_depth: image_desc.image_depth,
            image_array_size: image_desc.image_array_size,
            image_row_pitch: image_desc.image_row_pitch,
            image_slice_pitch: image_desc.image_slice_pitch,
            num_mip_levels: image_desc.num_mip_levels,
            num_samples: image_desc.num_samples,
            mem_object: unsafe { image_desc.anon_1.mem_object },
        };

        Vcl::get().call_clCreateImageWithPropertiesMESA(
            image.context.get_handle(),
            props_ptr,
            flags,
            &image_format,
            &image_desc,
            size,
            host_ptr,
            &mut image.get_handle(),
        )?;

        Ok(image)
    }
}

impl Drop for Mem {
    fn drop(&mut self) {
        let mem = cl_mem::from_ptr(self);
        self.callbacks
            .get_mut()
            .unwrap()
            .iter()
            .rev()
            .for_each(|callback| callback(mem));
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
