/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use vulkan_h::*;

use crate::Result;

use std::alloc::Layout;
use std::convert::{AsMut, AsRef};
use std::mem::MaybeUninit;
use std::ops::{Deref, DerefMut};
use std::os::raw::c_void;
use std::ptr::NonNull;

trait VkAllocScoped {
    unsafe fn alloc_scoped_raw(
        &self,
        layout: Layout,
        scope: VkSystemAllocationScope,
    ) -> *mut u8;

    unsafe fn alloc_scoped<T>(&self, scope: VkSystemAllocationScope) -> *mut T {
        self.alloc_scoped_raw(Layout::new::<T>(), scope) as *mut T
    }
}

trait VkFree {
    unsafe fn free_raw(&self, mem: *mut u8);

    unsafe fn free<T>(&self, mem: *mut T) {
        self.free_raw(mem as *mut u8);
    }
}

impl VkAllocScoped for VkAllocationCallbacks {
    unsafe fn alloc_scoped_raw(
        &self,
        layout: Layout,
        scope: VkSystemAllocationScope,
    ) -> *mut u8 {
        self.pfnAllocation.unwrap()(
            self.pUserData,
            layout.align(),
            layout.size(),
            scope,
        ) as *mut u8
    }
}

impl VkFree for VkAllocationCallbacks {
    unsafe fn free_raw(&self, mem: *mut u8) {
        self.pfnFree.unwrap()(self.pUserData, mem as *mut c_void);
    }
}

#[derive(Copy, Clone)]
struct FreeCb {
    user_data: *mut c_void,
    free: unsafe extern "C" fn(pUserData: *mut c_void, pMemory: *mut c_void),
}

impl FreeCb {
    pub fn new(alloc: &VkAllocationCallbacks) -> FreeCb {
        FreeCb {
            user_data: alloc.pUserData,
            free: alloc.pfnFree.unwrap(),
        }
    }
}

impl VkFree for FreeCb {
    unsafe fn free_raw(&self, mem: *mut u8) {
        (self.free)(self.user_data, mem as *mut c_void);
    }
}

pub struct VkBox<T> {
    ptr: NonNull<T>,
    free: FreeCb,
}

impl<T> VkBox<MaybeUninit<T>> {
    pub unsafe fn assume_init(self) -> VkBox<T> {
        let ub = std::mem::ManuallyDrop::new(self);
        VkBox {
            ptr: NonNull::new_unchecked(ub.ptr.as_ptr() as *mut T),
            free: ub.free,
        }
    }

    pub fn new_uninit(
        alloc: &VkAllocationCallbacks,
        scope: VkSystemAllocationScope,
    ) -> Result<VkBox<MaybeUninit<T>>> {
        unsafe {
            let ptr = alloc.alloc_scoped::<MaybeUninit<T>>(scope);
            if let Some(ptr) = NonNull::new(ptr) {
                Ok(VkBox {
                    ptr: ptr,
                    free: FreeCb::new(alloc),
                })
            } else {
                Err(VK_ERROR_OUT_OF_HOST_MEMORY)
            }
        }
    }

    pub fn write(self, x: T) -> VkBox<T> {
        unsafe {
            (self.ptr.as_ptr() as *mut T).write(x);
            self.assume_init()
        }
    }
}

impl<T> VkBox<T> {
    pub fn new(
        x: T,
        alloc: &VkAllocationCallbacks,
        scope: VkSystemAllocationScope,
    ) -> Result<VkBox<T>> {
        match VkBox::<MaybeUninit<T>>::new_uninit(alloc, scope) {
            Ok(b) => Ok(b.write(x)),
            Err(e) => Err(e),
        }
    }

    pub unsafe fn new_cb<F: FnOnce(NonNull<T>) -> VkResult>(
        alloc: &VkAllocationCallbacks,
        scope: VkSystemAllocationScope,
        f: F,
    ) -> Result<VkBox<T>> {
        match VkBox::<MaybeUninit<T>>::new_uninit(alloc, scope) {
            Ok(b) => {
                match f(NonNull::new_unchecked(b.ptr.as_ptr() as *mut T)) {
                    VK_SUCCESS => Ok(b.assume_init()),
                    e => Err(e),
                }
            },
            Err(e) => Err(e),
        }
    }

    pub fn new2(
        x: T,
        parent_alloc: &VkAllocationCallbacks,
        alloc: *const VkAllocationCallbacks,
        scope: VkSystemAllocationScope,
    ) -> Result<VkBox<T>> {
        let alloc = if alloc.is_null() {
            parent_alloc
        } else {
            unsafe {&*alloc }
        };
        VkBox::new(x, alloc, scope)
    }
}

impl<T> Drop for VkBox<T> {
    fn drop(&mut self) {
        unsafe {
            std::ptr::drop_in_place(self.ptr.as_ptr());
            self.free.free(self.ptr.as_ptr() as *mut c_void);
        }
    }
}

impl<T> AsRef<T> for VkBox<T> {
    fn as_ref(&self) -> &T {
        unsafe { self.ptr.as_ref() }
    }
}

impl<T> AsMut<T> for VkBox<T> {
    fn as_mut(&mut self) -> &mut T {
        unsafe { self.ptr.as_mut() }
    }
}

impl<T> Deref for VkBox<T> {
    type Target = T;

    fn deref(&self) -> &T {
        self.as_ref()
    }
}

impl<T> DerefMut for VkBox<T> {
    fn deref_mut(&mut self) -> &mut T {
        self.as_mut()
    }
}

type VkFinishFn<V> = unsafe extern "C" fn(obj: *mut V);

#[repr(C)]
pub struct VkObj<V, T> {
    vk: V,
    finish: VkFinishFn<V>,
    data: Option<T>,
    _pin: std::marker::PhantomPinned,
}

impl<V, T> VkObj<V, T> {
    unsafe fn init_ptr<F: FnOnce(NonNull<V>) -> VkResult>(
        mut ptr: NonNull<Self>,
        finish: VkFinishFn<V>,
        f: F,
    ) -> VkResult {
        let vk_ptr = (&mut ptr.as_mut().vk) as *mut V;
        let finish_ptr = (&mut ptr.as_mut().finish) as *mut VkFinishFn<V>;
        let data_ptr = (&mut ptr.as_mut().data) as *mut Option<T>;

        match f(NonNull::new_unchecked(vk_ptr)) {
            VK_SUCCESS => {
                finish_ptr.write(finish);
                data_ptr.write(None);
                VK_SUCCESS
            }
            err => err,
        }
    }

    pub fn vk(&self) -> &V {
        &self.vk
    }

    pub fn vk_mut(&mut self) -> &mut V {
        &mut self.vk
    }

    pub unsafe fn vk_ptr(&self) -> *mut V {
        &self.vk as *const V as *mut V
    }
}

impl<V, T> Drop for VkObj<V, T> {
    fn drop(&mut self) {
        self.data.take();
        unsafe { (self.finish)(&mut self.vk) };
    }
}

impl<V, T> Deref for VkObj<V, T> {
    type Target = T;

    fn deref(&self) -> &T {
        self.data.as_ref().unwrap()
    }
}

impl<V, T> DerefMut for VkObj<V, T> {
    fn deref_mut(&mut self) -> &mut T {
        self.data.as_mut().unwrap()
    }
}

pub struct VkObjBaseBox<V, T> {
    obj: VkBox<VkObj<V, T>>,
}

impl<V, T> VkObjBaseBox<V, T> {
    pub fn new_cb<F: FnOnce(NonNull<V>) -> VkResult>(
        alloc: &VkAllocationCallbacks,
        finish: unsafe extern "C" fn(obj: *mut V),
        f: F,
    ) -> Result<VkObjBaseBox<V, T>> {
        let obj = unsafe {
            VkBox::new_cb(alloc, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, |ptr| {
                VkObj::init_ptr(ptr, finish, f)
            })
        }?;
        Ok(VkObjBaseBox { obj: obj })
    }

    pub fn new2_cb<F: FnOnce(NonNull<V>) -> VkResult>(
        parent_alloc: &VkAllocationCallbacks,
        alloc: *const VkAllocationCallbacks,
        finish: unsafe extern "C" fn(obj: *mut V),
        f: F,
    ) -> Result<VkObjBaseBox<V, T>> {
        let alloc = if alloc.is_null() {
            parent_alloc
        } else {
            unsafe {&*alloc }
        };
        let obj = unsafe {
            VkBox::new_cb(alloc, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, |ptr| {
                VkObj::init_ptr(ptr, finish, f)
            })
        }?;
        Ok(VkObjBaseBox { obj: obj })
    }
}

impl<V, T> Deref for VkObjBaseBox<V, T> {
    type Target = V;

    fn deref(&self) -> &V {
        &self.obj.vk
    }
}

impl<V, T> DerefMut for VkObjBaseBox<V, T> {
    fn deref_mut(&mut self) -> &mut V {
        &mut self.obj.vk
    }
}

pub struct VkObjBox<V, T> {
    base: VkObjBaseBox<V, T>,
}

impl<V, T> VkObjBox<V, T> {
    pub fn new(mut base: VkObjBaseBox<V, T>, x: T) -> VkObjBox<V, T> {
        base.obj.data.replace(x);
        VkObjBox { base: base }
    }
}

impl<V, T> Deref for VkObjBox<V, T> {
    type Target = VkObj<V, T>;

    fn deref(&self) -> &VkObj<V, T> {
        &self.base.obj
    }
}

impl<V, T> DerefMut for VkObjBox<V, T> {
    fn deref_mut(&mut self) -> &mut VkObj<V, T> {
        &mut self.base.obj
    }
}
