/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use std::slice;

use vcl_opencl_gen::*;

pub trait NullTerminated {
    type Output;

    fn as_slice_with_null(&self) -> &[Self::Output];
}

impl NullTerminated for *const cl_context_properties {
    type Output = cl_context_properties;

    fn as_slice_with_null(&self) -> &[Self::Output] {
        let mut size = 0;
        let ptr = *self;
        while unsafe { ptr.wrapping_offset(size).read() } != 0 {
            size += 1;
        }
        unsafe { slice::from_raw_parts(ptr, size as usize + 1) }
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn properties_as_slice() {
        let properties = [CL_CONTEXT_PLATFORM as isize, 42, 0];
        let properties_ptr = properties.as_ptr() as *const cl_context_properties;
        assert_eq!(properties_ptr.as_slice_with_null().len(), 3);
    }
}
