/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use vulkan_h::*;
use vulkan_runtime::*;

use crate::Result;
use crate::boxed::*;

use std::ptr::NonNull;

struct Device {
}

struct Image<'a> {
    dev: &'a VkObj<vk_device, Device>,
}

fn create_image(
    dev: &VkObj<vk_device, Device>,
    info: *const VkImageCreateInfo,
    alloc: *const VkAllocationCallbacks,
) -> Result<VkObjBox<vk_image, Image>> {
    let vk = unsafe {
        VkObjBaseBox::new2_cb(
            &dev.vk().alloc,
            alloc,
            vk_image_finish,
            &|vk: NonNull<vk_image>| {
                vk_image_init(dev.vk_ptr(), vk.as_ptr(), info);
                VK_SUCCESS
            },
        )
    }?;

    /* Stuff which may use vk */

    Ok(VkObjBox::new(vk, Image {
        dev: dev,
    }))
}
