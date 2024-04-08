/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::context::Context;
use crate::dev::renderer::Vcl;
use crate::impl_cl_type_trait;

use vcl_opencl_gen::*;

use std::sync::Arc;

impl_cl_type_trait!(cl_event, Event, CL_INVALID_EVENT);

pub struct Event {
    base: CLObjectBase<CL_INVALID_EVENT>,
    pub context: Arc<Context>,
}

impl Event {
    pub fn new(context: &Arc<Context>) -> CLResult<Arc<Event>> {
        let event = Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
        });

        Vcl::get().call_clCreateUserEventMESA(context.get_handle(), &mut event.get_handle())?;

        Ok(event)
    }
}
