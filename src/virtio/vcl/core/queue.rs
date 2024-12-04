/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::context::Context;
use crate::core::device::Device;
use crate::dev::renderer::Vcl;
use crate::impl_cl_type_trait;

use mesa_rust_util::properties::Properties;
use vcl_opencl_gen::*;

use std::ptr;
use std::sync::Arc;

impl_cl_type_trait!(cl_command_queue, Queue, CL_INVALID_COMMAND_QUEUE);

pub struct Queue {
    base: CLObjectBase<CL_INVALID_COMMAND_QUEUE>,
    pub context: Arc<Context>,
    pub device: &'static Device,
}

impl Queue {
    /// Deprecated API
    pub fn new(
        context: Arc<Context>,
        device: &'static Device,
        props: cl_command_queue_properties,
    ) -> CLResult<Arc<Queue>> {
        let queue = Arc::new(Queue {
            base: Default::default(),
            context: context.clone(),
            device,
        });

        Vcl::get().call_clCreateCommandQueueMESA(
            context.get_handle(),
            device.get_handle(),
            props,
            &mut queue.get_handle(),
        )?;

        Ok(queue)
    }

    /// New API
    pub fn new_with_properties(
        context: Arc<Context>,
        device: &'static Device,
        properties: Properties<cl_queue_properties>,
    ) -> CLResult<Arc<Queue>> {
        let queue = Arc::new(Queue {
            base: Default::default(),
            context,
            device,
        });

        let props = properties.to_raw();
        let props_ptr = if props.len() > 1 {
            props.as_ptr()
        } else {
            ptr::null()
        };

        Vcl::get().call_clCreateCommandQueueWithPropertiesMESA(
            queue.context.get_handle(),
            device.get_handle(),
            props.len(),
            props_ptr,
            &mut queue.get_handle(),
        )?;

        Ok(queue)
    }
}
