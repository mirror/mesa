/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::util::*;
use crate::dev::renderer::Vcl;
use crate::impl_cl_type_trait;

use vcl_opencl_gen::*;

use std::mem::MaybeUninit;
use std::sync::Arc;

impl_cl_type_trait!(cl_event, Event, CL_INVALID_EVENT);

#[derive(Default)]
pub struct Event {
    base: CLObjectBase<CL_INVALID_EVENT>,
}

impl Event {
    pub fn new(context: cl_context) -> CLResult<Arc<Event>> {
        let event = Arc::new(Event::default());

        Vcl::get().call_clCreateUserEventMESA(context, &mut event.get_handle())?;

        Ok(event)
    }
}

impl CLInfo<cl_event_info> for cl_event {
    fn query(&self, info: cl_event_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        Ok(match info {
            CL_EVENT_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}
