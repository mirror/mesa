/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::dev::renderer::*;
use crate::protocol::cs_impl::*;

use std::ffi::c_void;
use std::pin::Pin;

pub type VclObjectID = u64;

pub struct VclCsEncoder<'b> {
    imp: Box<dyn VclCsWrite + 'b>,
}

impl<'b> VclCsEncoder<'b> {
    pub fn new(buffer: &'b mut [u8]) -> Self {
        Self {
            imp: Box::new(VclCsEncoderSys::new(buffer)),
        }
    }
}

pub trait VclCsWrite {
    fn is_empty(&self) -> bool;

    fn get_slice(&self) -> &[u8];

    /// Writes `val_size` bytes reading them from memory pointed to by `val`
    fn write(&mut self, size: usize, val: *const c_void, val_size: usize);
}

impl<'b> VclCsEncoder<'b> {
    pub fn is_empty(&self) -> bool {
        self.imp.is_empty()
    }

    pub fn get_slice(&self) -> &[u8] {
        self.imp.get_slice()
    }

    pub fn encode(&mut self, size: usize, data: *const c_void, data_size: usize) {
        assert_eq!(size % 4, 0);
        self.imp.write(size, data, data_size);
    }
}

pub trait VclCsRead {
    fn peek(&self, size: usize, val: *mut c_void, val_size: usize);
    /// Reads `val_size` bytes writing them to memory pointed to by `val`
    fn read(&mut self, size: usize, val: *mut c_void, val_size: usize);
}

pub struct VclCsDecoder {
    _reply_buffer: Pin<Box<VclBuffer>>,
    imp: Box<dyn VclCsRead>,
}

impl VclCsDecoder {
    pub fn new(reply_buffer: VclBuffer) -> Self {
        // Make sure the reply buffer does not move its memory while the
        // decoder uses it as a slice
        let reply_buffer = Box::pin(reply_buffer);
        let imp = Box::new(VclCsDecoderSys::new(reply_buffer.res.get_slice()));
        Self {
            _reply_buffer: reply_buffer,
            imp,
        }
    }

    pub fn set_fatal(&mut self) {}
    pub fn get_fatal(&self) -> bool {
        false
    }
    pub fn lookup_object(&self, _id: VclObjectID) {}
    pub fn reset_temp_pool(&self) {}
    pub fn alloc_temp(&self, _size: usize) {}
    pub fn alloc_temp_array(&self, _size: usize, _count: usize) {}
    pub fn peek(&self, size: usize, val: *mut c_void, val_size: usize) {
        self.imp.peek(size, val, val_size);
    }

    pub fn decode(&mut self, size: usize, val: *mut c_void, val_size: usize) {
        assert_eq!(size % 4, 0);
        self.imp.read(size, val, val_size)
    }
}
