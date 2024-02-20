/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::CLResult;

use mesa_rust_util::{properties::Properties, ptr::CheckedPtr};
use vcl_opencl_gen::*;

use std::ffi::{CStr, CString};
use std::mem::{size_of, MaybeUninit};
use std::slice;

pub trait CLInfo<I> {
    fn query(&self, q: I, vals: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>>;

    fn get_info(
        &self,
        param_name: I,
        param_value_size: usize,
        param_value: *mut ::std::os::raw::c_void,
        param_value_size_ret: *mut usize,
    ) -> CLResult<()> {
        let arr = if !param_value.is_null() {
            unsafe { slice::from_raw_parts(param_value.cast(), param_value_size) }
        } else {
            &[]
        };

        let d = self.query(param_name, arr)?;
        let size: usize = d.len();

        // CL_INVALID_VALUE [...] if size in bytes specified by param_value_size is < size of return
        // type as specified in the Context Attributes table and param_value is not a NULL value.
        if param_value_size < size && !param_value.is_null() {
            return Err(CL_INVALID_VALUE);
        }

        // param_value_size_ret returns the actual size in bytes of data being queried by param_name.
        // If param_value_size_ret is NULL, it is ignored.
        param_value_size_ret.write_checked(size);

        // param_value is a pointer to memory where the appropriate result being queried is returned.
        // If param_value is NULL, it is ignored.
        unsafe {
            param_value.copy_checked(d.as_ptr().cast(), size);
        }

        Ok(())
    }
}

pub trait CLProp {
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>>;
}

macro_rules! cl_prop_for_type {
    ($ty: ty) => {
        impl CLProp for $ty {
            fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
                unsafe { slice::from_raw_parts((self as *const Self).cast(), size_of::<Self>()) }
                    .to_vec()
            }
        }
    };
}

cl_prop_for_type!(cl_char);
cl_prop_for_type!(cl_uchar);
cl_prop_for_type!(cl_ushort);
cl_prop_for_type!(cl_int);
cl_prop_for_type!(cl_uint);
cl_prop_for_type!(cl_ulong);
cl_prop_for_type!(isize);
cl_prop_for_type!(usize);

cl_prop_for_type!(cl_device_integer_dot_product_acceleration_properties_khr);
cl_prop_for_type!(cl_device_pci_bus_info_khr);
cl_prop_for_type!(cl_image_format);
cl_prop_for_type!(cl_name_version_khr);

impl CLProp for bool {
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        cl_prop::<cl_bool>(if *self { CL_TRUE } else { CL_FALSE })
    }
}

impl CLProp for &str {
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        to_maybeuninit_vec(
            CString::new(*self)
                .unwrap_or_default()
                .into_bytes_with_nul(),
        )
    }
}

impl CLProp for &CStr {
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        to_maybeuninit_vec(self.to_bytes_with_nul().to_vec())
    }
}

impl<T> CLProp for Vec<T>
where
    T: CLProp,
{
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        let mut res: Vec<MaybeUninit<u8>> = Vec::new();
        for i in self {
            res.append(&mut i.cl_vec())
        }
        res
    }
}

impl<T> CLProp for &T
where
    T: CLProp,
{
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        T::cl_vec(self)
    }
}

impl<T> CLProp for [T]
where
    T: CLProp,
{
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        let mut res: Vec<MaybeUninit<u8>> = Vec::new();
        for i in self {
            res.append(&mut i.cl_vec())
        }
        res
    }
}

impl<T, const I: usize> CLProp for [T; I]
where
    T: CLProp,
{
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        let mut res: Vec<MaybeUninit<u8>> = Vec::new();
        for i in self {
            res.append(&mut i.cl_vec())
        }
        res
    }
}

impl<T> CLProp for *const T {
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        (*self as usize).cl_vec()
    }
}

impl<T> CLProp for *mut T {
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        (*self as usize).cl_vec()
    }
}

impl<T> CLProp for Properties<T>
where
    T: CLProp + Default,
{
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        let mut res: Vec<MaybeUninit<u8>> = Vec::new();
        for (k, v) in &self.props {
            res.append(&mut k.cl_vec());
            res.append(&mut v.cl_vec());
        }
        res.append(&mut T::default().cl_vec());
        res
    }
}

impl<T> CLProp for Option<T>
where
    T: CLProp,
{
    fn cl_vec(&self) -> Vec<MaybeUninit<u8>> {
        self.as_ref().map_or(Vec::new(), |v| v.cl_vec())
    }
}

pub fn cl_prop<T: CLProp>(v: T) -> Vec<MaybeUninit<u8>>
where
    T: Sized,
{
    v.cl_vec()
}

pub fn to_maybeuninit_vec<T: Copy>(v: Vec<T>) -> Vec<MaybeUninit<T>> {
    // In my tests the compiler was smart enough to turn this into a noop
    v.into_iter().map(MaybeUninit::new).collect()
}

const CL_DEVICE_TYPES: u32 = CL_DEVICE_TYPE_ACCELERATOR
    | CL_DEVICE_TYPE_CPU
    | CL_DEVICE_TYPE_GPU
    | CL_DEVICE_TYPE_DEFAULT
    | CL_DEVICE_TYPE_CUSTOM;

pub fn check_cl_device_type(val: cl_device_type) -> CLResult<()> {
    let v: u32 = val.try_into().or(Err(CL_INVALID_DEVICE_TYPE))?;
    if v == CL_DEVICE_TYPE_ALL || v & CL_DEVICE_TYPES == v {
        return Ok(());
    }
    Err(CL_INVALID_DEVICE_TYPE)
}

pub fn check_cl_bool<T: PartialEq + TryInto<cl_uint>>(val: T) -> Option<bool> {
    let c: u32 = val.try_into().ok()?;
    if c != CL_TRUE && c != CL_FALSE {
        return None;
    }
    Some(c == CL_TRUE)
}
