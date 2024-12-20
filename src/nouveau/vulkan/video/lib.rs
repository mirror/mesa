// Copyright © 2024 Collabora, Ltd.
// SPDX-License-Identifier: MIT
//
// Thanks to Averne for the work on the Nvdec tracer. None of the decode work
// would have been possible otherwise.

use nv_push_rs::Push;
use nvidia_headers::classes::cl906f::mthd as cl906f;
use nvidia_headers::classes::clc5b0::mthd as clc5b0;
use nvk_video_bindings::nvk_cmd_buffer;
use nvk_video_bindings::VK_SUCCESS;

pub mod decode;

#[cfg(debug_assertions)]
pub(crate) mod trace;

/// A Rust version of the `vk_find_struct` macro. Uses the `paste` crate to call
/// the equivalent C function.
macro_rules! vk_find_struct_const(
    ($p:expr, $s:ident, $vendor:ident) => {
        {
            let s = unsafe {
                paste::paste! {
                    nvk_video_bindings::__vk_find_struct(
                        $p as *mut _,
                        nvk_video_bindings::[<VK_STRUCTURE_TYPE_ $s _ $vendor>]
                    ) as *const nvk_video_bindings::[<Vk $s:lower:camel $vendor>]
                }
            };

            unsafe { *s }
        }
    }
);
pub(crate) use vk_find_struct_const;

fn align_u32(value: u32, alignment: u32) -> u32 {
    (value + alignment - 1) & !(alignment - 1)
}

/// Append the given push buffer to the command buffer.
pub(crate) fn append_rust_push(
    push: Push,
    nvk_cmd_buffer: *mut nvk_cmd_buffer,
) {
    let ret = unsafe {
        nvk_video_bindings::nvk_cmd_buffer_append_rust_push(
            nvk_cmd_buffer,
            push.as_ptr().cast_mut(),
            push.len().try_into().unwrap(),
        )
    };

    assert!(ret == VK_SUCCESS);
}

pub(crate) fn use_video_engine(push: &mut Push) {
    let set_object = cl906f::SetObject {
        nvclass: clc5b0::VIDEO_DECODER,
        engine: cl906f::SetObjectEngine::Sw,
    };

    push.push_method(set_object);
}
