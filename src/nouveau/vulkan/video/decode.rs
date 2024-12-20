// Copyright © 2024 Collabora, Ltd
// SPDX-License-Identifier: MIT

use nvk_video_bindings::nvk_cmd_buffer;
use nvk_video_bindings::nvk_video_session;

use std::sync::Mutex;

mod h264;

struct SessionData {
    decoder: Box<dyn VideoDecoder>,
}

pub(crate) trait VideoDecoder {
    fn begin(
        &mut self,
        _nvk_cmd_buffer: *mut nvk_cmd_buffer,
        _begin_info: &nvk_video_bindings::VkVideoBeginCodingInfoKHR,
    ) {
    }

    fn decode(
        &mut self,
        nvk_cmd_buffer: *mut nvk_cmd_buffer,
        frame_info: &nvk_video_bindings::VkVideoDecodeInfoKHR,
    );
}

#[no_mangle]
pub extern "C" fn nvk_video_create_video_session(vid: &mut nvk_video_session) {
    vid.rust = Box::into_raw(Box::new(Mutex::new(SessionData {
        decoder: Box::new(h264::Decoder::default()),
    })))
    .cast();
}
#[no_mangle]
pub extern "C" fn nvk_video_destroy_video_session(vid: &mut nvk_video_session) {
    drop(unsafe { Box::from_raw(vid.rust as *mut Mutex<SessionData>) });
}

/// Cast the opaque `nvk_video_session::rust` pointer to a mutable reference to
/// a `SessionData`.
///
/// # Safety:
///
/// The caller must ensure that the pointer is valid when accessing the
/// reference and that Rust's aliasing rules are followed.
///
fn to_session<'a>(cmd_buf: *mut nvk_cmd_buffer) -> &'a Mutex<SessionData> {
    unsafe {
        let video = (*cmd_buf).video;
        let rust_ptr = (*video.vid).rust as *mut Mutex<SessionData>;

        // SAFETY: We are the only ones that should be accessing the `rust`
        // field. At no point we return mutable references, since we are using
        // struct Mutex's interior mutability to access the data, so we upheld
        // Rust's aliasing rules.
        &*rust_ptr
    }
}

#[no_mangle]
pub extern "C" fn nvk_video_cmd_begin_video_coding_khr(
    cmd: nvk_video_bindings::VkCommandBuffer,
    begin_info: *const nvk_video_bindings::VkVideoBeginCodingInfoKHR,
) {
    let cmd = unsafe { nvk_video_bindings::nvk_cmd_buffer_from_handle(cmd) };
    let begin_info = unsafe { *begin_info };

    let mut session = to_session(cmd).lock().unwrap();

    session.decoder.begin(cmd, &begin_info);
}

#[no_mangle]
pub extern "C" fn nvk_video_cmd_decode_video_khr(
    cmd: nvk_video_bindings::VkCommandBuffer,
    frame_info: *const nvk_video_bindings::VkVideoDecodeInfoKHR,
) {
    let frame_info = unsafe { *frame_info };
    let cmd = unsafe { nvk_video_bindings::nvk_cmd_buffer_from_handle(cmd) };
    let mut session = to_session(cmd).lock().unwrap();

    session.decoder.decode(cmd, &frame_info);
}
