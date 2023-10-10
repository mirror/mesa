/*
 * Copyright © 2023 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_device.h"
#include "radv_entrypoints.h"
#include "radv_image.h"

struct radv_host_image_view {
   struct radv_device *device;
   struct ac_addrlib *addrlib;
   const struct radeon_info *info;
   struct radv_image *image;
   uint8_t *base;
   uint32_t plane;
   uint32_t level;
   uint32_t layer;
   bool stencil;
};

static void
radv_host_image_view_init(struct radv_host_image_view *view, struct radv_image *image,
                          const VkImageSubresource *subresource)
{
   struct radv_device *device = container_of(image->vk.base.device, struct radv_device, vk);

   view->device = device;
   view->addrlib = device->ws->get_addrlib(device->ws);
   view->info = &radv_device_physical(device)->info;
   view->image = image;

   uint32_t plane_index = 0;
   if (subresource->aspectMask & VK_IMAGE_ASPECT_PLANE_1_BIT)
      plane_index = 1;
   else if (subresource->aspectMask & VK_IMAGE_ASPECT_PLANE_2_BIT)
      plane_index = 2;

   view->plane = plane_index;
   view->level = subresource->mipLevel;
   view->layer = subresource->arrayLayer;
   view->stencil = subresource->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT;

   view->base = radv_buffer_map(device->ws, image->bindings[plane_index].bo);
   view->base += image->bindings[plane_index].offset;
}

static void *
radv_host_image_view_texel(struct radv_host_image_view *view, uint32_t x, uint32_t y, uint32_t z)
{
   struct ac_surf_info surf_info = radv_get_ac_surf_info(view->device, view->image);

   if (z == 0)
      z = view->layer;

   /* TODO: How to access depth/stencil aspects? */
   uint64_t offset =
      ac_surface_addr_from_coord(view->addrlib, view->info, &view->image->planes[view->plane].surface, &surf_info,
                                 view->level, x, y, z, view->image->vk.image_type == VK_IMAGE_TYPE_3D,
                                 view->stencil);

   assert(offset < view->image->bindings[view->plane].range);

   return view->base + offset;
}

static uint32_t
radv_get_pixel_stride(VkFormat format, VkImageAspectFlags aspects)
{
   if (vk_format_has_depth(format) && !(aspects & VK_IMAGE_ASPECT_DEPTH_BIT))
      format = vk_format_stencil_only(format);
   else if (vk_format_has_stencil(format) && !(aspects & VK_IMAGE_ASPECT_STENCIL_BIT))
      format = vk_format_depth_only(format);

   return vk_format_get_blocksize(format);
}

enum radv_copy_dst {
   RADV_COPY_DST_BUFFER,
   RADV_COPY_DST_IMAGE,
};

static void
radv_copy_image_buffer(struct radv_image *image, const VkImageToMemoryCopyEXT *region, uint32_t layer,
                       enum radv_copy_dst dst_res)
{
   VkImageSubresource subresource = {
      .aspectMask = region->imageSubresource.aspectMask,
      .mipLevel = region->imageSubresource.mipLevel,
      .arrayLayer = layer,
   };

   struct radv_host_image_view view;
   radv_host_image_view_init(&view, image, &subresource);

   uint32_t pixel_stride = radv_get_pixel_stride(image->vk.format, region->imageSubresource.aspectMask);

   uint32_t block_width = vk_format_get_blockwidth(image->vk.format);
   uint32_t block_height = vk_format_get_blockheight(image->vk.format);
   uint32_t block_depth = util_format_get_blockdepth(vk_format_to_pipe_format(image->vk.format));

   VkOffset3D offset = {
      .x = region->imageOffset.x / block_width,
      .y = region->imageOffset.y / block_height,
      .z = region->imageOffset.z / block_depth,
   };

   VkExtent3D extent = {
      .width = region->imageExtent.width / block_width,
      .height = region->imageExtent.height / block_height,
      .depth = region->imageExtent.depth / block_depth,
   };

   uint32_t buffer_y_stride = region->memoryRowLength ? region->memoryRowLength : extent.width;
   uint32_t buffer_z_stride = region->memoryImageHeight ? region->memoryImageHeight : extent.height;
   buffer_z_stride *= buffer_y_stride;

   buffer_y_stride *= pixel_stride;
   buffer_z_stride *= pixel_stride;

   uint8_t *buffer = region->pHostPointer;
   buffer += buffer_z_stride * layer;

   if (dst_res == RADV_COPY_DST_BUFFER) {
      for (uint32_t z = 0; z < extent.depth; z++) {
         for (uint32_t y = 0; y < extent.height; y++) {
            uint32_t dst_row_offset = buffer_y_stride * y + buffer_z_stride * z;
            for (uint32_t x = 0; x < extent.width; x++) {
               memcpy(buffer + dst_row_offset + x * pixel_stride,
                      radv_host_image_view_texel(&view, offset.x + x, offset.y + y, offset.z + z), pixel_stride);
            }
         }
      }
   } else {
      for (uint32_t z = 0; z < extent.depth; z++) {
         for (uint32_t y = 0; y < extent.height; y++) {
            uint32_t src_row_offset = buffer_y_stride * y + buffer_z_stride * z;
            for (uint32_t x = 0; x < extent.width; x++) {
               memcpy(radv_host_image_view_texel(&view, offset.x + x, offset.y + y, offset.z + z),
                      buffer + src_row_offset + x * pixel_stride, pixel_stride);
            }
         }
      }
   }
}

static void
radv_copy_image_rect(struct radv_image *dst, struct radv_image *src, const VkImageCopy2 *region, uint32_t layer)
{
   VkImageSubresource src_subresource = {
      .aspectMask = region->srcSubresource.aspectMask,
      .mipLevel = region->srcSubresource.mipLevel,
      .arrayLayer = layer,
   };

   struct radv_host_image_view src_view;
   radv_host_image_view_init(&src_view, src, &src_subresource);

   VkImageSubresource dst_subresource = {
      .aspectMask = region->dstSubresource.aspectMask,
      .mipLevel = region->dstSubresource.mipLevel,
      .arrayLayer = layer,
   };

   struct radv_host_image_view dst_view;
   radv_host_image_view_init(&dst_view, dst, &dst_subresource);

   uint32_t pixel_stride = radv_get_pixel_stride(src->vk.format, region->dstSubresource.aspectMask);

   uint32_t block_width = vk_format_get_blockwidth(src->vk.format);
   uint32_t block_height = vk_format_get_blockheight(src->vk.format);
   uint32_t block_depth = util_format_get_blockdepth(vk_format_to_pipe_format(src->vk.format));

   VkOffset3D src_offset = {
      .x = region->srcOffset.x / block_width,
      .y = region->srcOffset.y / block_height,
      .z = region->srcOffset.z / block_depth,
   };

   VkOffset3D dst_offset = {
      .x = region->dstOffset.x / block_width,
      .y = region->dstOffset.y / block_height,
      .z = region->dstOffset.z / block_depth,
   };

   VkExtent3D extent = {
      .width = region->extent.width / block_width,
      .height = region->extent.height / block_height,
      .depth = region->extent.depth / block_depth,
   };

   for (uint32_t z = 0; z < extent.depth; z++) {
      for (uint32_t y = 0; y < extent.height; y++) {
         for (uint32_t x = 0; x < extent.height; x++) {
            memcpy(radv_host_image_view_texel(&dst_view, dst_offset.x + x, dst_offset.y + y, dst_offset.z + z),
                   radv_host_image_view_texel(&src_view, src_offset.x + x, src_offset.y + y, src_offset.z + z),
                   pixel_stride);
         }
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyMemoryToImageEXT(VkDevice _device, const VkCopyMemoryToImageInfoEXT *pCopyMemoryToImageInfo)
{
   VK_FROM_HANDLE(radv_image, image, pCopyMemoryToImageInfo->dstImage);

   for (uint32_t i = 0; i < pCopyMemoryToImageInfo->regionCount; i++) {
      const VkMemoryToImageCopyEXT *region = &pCopyMemoryToImageInfo->pRegions[i];
      VkImageToMemoryCopyEXT tmp_region = {
         .pHostPointer = (void *)region->pHostPointer,
         .memoryRowLength = region->memoryRowLength,
         .memoryImageHeight = region->memoryImageHeight,
         .imageSubresource = region->imageSubresource,
         .imageOffset = region->imageOffset,
         .imageExtent = region->imageExtent,
      };

      uint32_t layer_count = vk_image_subresource_layer_count(&image->vk, &region->imageSubresource);
      for (uint32_t layer = 0; layer < layer_count; layer++)
         radv_copy_image_buffer(image, &tmp_region, layer, RADV_COPY_DST_IMAGE);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyImageToMemoryEXT(VkDevice _device, const VkCopyImageToMemoryInfoEXT *pCopyImageToMemoryInfo)
{
   VK_FROM_HANDLE(radv_image, image, pCopyImageToMemoryInfo->srcImage);

   for (uint32_t i = 0; i < pCopyImageToMemoryInfo->regionCount; i++) {
      const VkImageToMemoryCopyEXT *region = &pCopyImageToMemoryInfo->pRegions[i];

      uint32_t layer_count = vk_image_subresource_layer_count(&image->vk, &region->imageSubresource);
      for (uint32_t layer = 0; layer < layer_count; layer++)
         radv_copy_image_buffer(image, region, layer, RADV_COPY_DST_BUFFER);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyImageToImageEXT(VkDevice _device, const VkCopyImageToImageInfoEXT *pCopyImageToImageInfo)
{
   VK_FROM_HANDLE(radv_image, src, pCopyImageToImageInfo->srcImage);
   VK_FROM_HANDLE(radv_image, dst, pCopyImageToImageInfo->dstImage);

   for (uint32_t i = 0; i < pCopyImageToImageInfo->regionCount; i++) {
      const VkImageCopy2 *region = &pCopyImageToImageInfo->pRegions[i];

      uint32_t src_layer_count = vk_image_subresource_layer_count(&src->vk, &region->srcSubresource);
      uint32_t dst_layer_count = vk_image_subresource_layer_count(&dst->vk, &region->dstSubresource);

      uint32_t layer_count = MIN2(src_layer_count, dst_layer_count);
      for (uint32_t layer = 0; layer < layer_count; layer++)
         radv_copy_image_rect(dst, src, region, layer);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_TransitionImageLayoutEXT(VkDevice _device, uint32_t transitionCount,
                              const VkHostImageLayoutTransitionInfoEXT *pTransitions)
{
   return VK_SUCCESS;
}
