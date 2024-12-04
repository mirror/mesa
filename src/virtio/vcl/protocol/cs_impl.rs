/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::protocol::cs::*;

use std::ffi::c_void;
use std::ptr;

pub struct VclCsEncoderSys<'b> {
    buffer: &'b [u8],
    cur: *mut u8,
    end: *const u8,
}

impl<'b> VclCsEncoderSys<'b> {
    pub fn new(buffer: &'b mut [u8]) -> Self {
        let cur = buffer.as_mut_ptr();
        let end = unsafe { buffer.as_ptr().add(buffer.len()) };
        Self { buffer, cur, end }
    }
}

#[derive(Default)]
pub struct VclObject {
    pub handle: u64,
}

impl<'b> VclCsWrite for VclCsEncoderSys<'b> {
    fn is_empty(&self) -> bool {
        return self.cur as *const _ == self.buffer.as_ptr();
    }

    fn get_slice(&self) -> &[u8] {
        let count = self.cur as usize - self.buffer.as_ptr() as usize;
        &self.buffer[..count]
    }

    fn write(&mut self, size: usize, val: *const c_void, val_size: usize) {
        assert!(val_size <= size);
        assert!(size <= self.end as usize - self.cur as usize);
        unsafe {
            ptr::copy_nonoverlapping(val as *const u8, self.cur, val_size);
            self.cur = self.cur.add(size);
        }
    }
}

pub struct VclCsDecoderSys {
    cur: *const u8,
    end: *const u8,
}

impl VclCsDecoderSys {
    pub fn new(buffer: &[u8]) -> Self {
        let cur = buffer.as_ptr();
        let end = unsafe { cur.add(buffer.len()) };
        Self { cur, end }
    }

    fn peek_impl(&self, size: usize, val: *mut c_void, val_size: usize) -> bool {
        assert!(val_size <= size);

        if size > (self.end as usize - self.cur as usize) {
            panic!();
        }

        unsafe {
            ptr::copy_nonoverlapping(self.cur, val as *mut u8, val_size);
        }
        true
    }
}

impl VclCsRead for VclCsDecoderSys {
    fn peek(&self, size: usize, val: *mut c_void, val_size: usize) {
        self.peek_impl(size, val, val_size);
    }

    fn read(&mut self, size: usize, val: *mut c_void, val_size: usize) {
        if self.peek_impl(size, val, val_size) {
            self.cur = unsafe { self.cur.add(size) };
        }
    }
}

pub fn vcl_cs_handle_indirect_id(_ty: *const c_void) -> bool {
    false
}

pub fn vcl_cs_handle_load_id(handle: *const *const c_void) -> VclObjectID {
    if unsafe { *handle }.is_null() {
        return 0;
    } else {
        (unsafe { *handle }) as _
    }
}

pub fn vcl_cs_handle_store_id(_handle: *const *mut c_void, _id: VclObjectID) {}
