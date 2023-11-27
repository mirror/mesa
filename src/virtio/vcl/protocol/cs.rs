/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::protocol::cs_impl::*;

use std::ffi::c_void;

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
    fn peek(&mut self, size: usize, val: *mut c_void, val_size: usize);
    /// Reads `val_size` bytes writing them to memory pointed to by `val`
    fn read(&mut self, size: usize, val: *mut c_void, val_size: usize);
}

pub struct VclCsDecoder {
    imp: Box<dyn VclCsRead>,
}

impl VclCsDecoder {
    pub fn new(buffer: &[u8]) -> Self {
        Self {
            imp: Box::new(VclCsDecoderSys::new(buffer)),
        }
    }

    pub fn set_fatal(&mut self) {}
    pub fn get_fatal(&mut self) -> bool {
        false
    }
    pub fn lookup_object(&mut self, _id: VclObjectID) {}
    pub fn reset_temp_pool(&mut self) {}
    pub fn alloc_temp(&mut self, _size: usize) {}
    pub fn alloc_temp_array(&mut self, _size: usize, _count: usize) {}
    pub fn peek(&mut self, size: usize, val: *mut c_void, val_size: usize) {
        self.imp.peek(size, val, val_size);
    }

    pub fn decode(&mut self, size: usize, data: *mut c_void, data_size: usize) {
        assert_eq!(size % 4, 0);
        self.imp.read(size, data, data_size);
    }
}
