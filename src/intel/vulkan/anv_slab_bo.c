/* Copyright © 2025 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "anv_slab_bo.h"

enum anv_bo_slab_heap {
   ANV_BO_SLAB_HEAP_BATCH_BUFFER,
   ANV_BO_SLAB_HEAP_DESCRIPTOR_POOL,
   ANV_BO_SLAB_HEAP_SMEM_CACHED_COHERENT,
   ANV_BO_SLAB_HEAP_SMEM_CACHED_INCOHERENT,
   ANV_BO_SLAB_HEAP_SMEM_COHERENT,
   ANV_BO_SLAB_HEAP_SMEM_COMPRESSED,
   ANV_BO_SLAB_HEAP_LMEM_SMEM,
   ANV_BO_SLAB_HEAP_LMEM_ONLY,
   ANV_BO_SLAB_HEAP_LMEM_ONLY_COMPRESSED,
   ANV_BO_SLAB_NOT_SUPPORTED,
};

struct anv_slab {
   struct pb_slab base;

   /** The BO representing the entire slab */
   struct anv_bo *bo;

   /** Array of anv_bo structs representing BOs allocated out of this slab */
   struct anv_bo *entries;
};

static bool disable_slab = false;

static enum anv_bo_slab_heap
anv_bo_alloc_flags_to_slab_heap(struct anv_device *device,
                                enum anv_bo_alloc_flags alloc_flags)
{
   enum anv_bo_alloc_flags not_supported = ANV_BO_ALLOC_32BIT_ADDRESS |
                                           ANV_BO_ALLOC_EXTERNAL |
                                           ANV_BO_ALLOC_CAPTURE |
                                           ANV_BO_ALLOC_FIXED_ADDRESS |
                                           ANV_BO_ALLOC_AUX_TT_ALIGNED |
                                           ANV_BO_ALLOC_CLIENT_VISIBLE_ADDRESS |
                                           ANV_BO_ALLOC_DESCRIPTOR_POOL |
                                           ANV_BO_ALLOC_SCANOUT |
                                           ANV_BO_ALLOC_PROTECTED |
                                           ANV_BO_ALLOC_DYNAMIC_VISIBLE_POOL |
                                           ANV_BO_ALLOC_IMPORTED;

   if (device->info->kmd_type == INTEL_KMD_TYPE_I915) {
      not_supported |= (ANV_BO_ALLOC_IMPLICIT_SYNC |
                        ANV_BO_ALLOC_IMPLICIT_WRITE);
   }

   /* TODO: add i915 support  */
   if ((device->info->kmd_type == INTEL_KMD_TYPE_XE) &&
       (alloc_flags & ANV_BO_ALLOC_BATCH_BUFFER))
      return ANV_BO_SLAB_HEAP_BATCH_BUFFER;

   if (alloc_flags & ANV_BO_ALLOC_DESCRIPTOR_POOL)
      return ANV_BO_SLAB_HEAP_DESCRIPTOR_POOL;

   if (alloc_flags & not_supported)
      return ANV_BO_SLAB_NOT_SUPPORTED;

   if (anv_physical_device_has_vram(device->physical)) {
      if (alloc_flags & ANV_BO_ALLOC_COMPRESSED)
         return ANV_BO_SLAB_HEAP_LMEM_ONLY_COMPRESSED;
      if (alloc_flags & ANV_BO_ALLOC_NO_LOCAL_MEM)
         return ANV_BO_SLAB_HEAP_SMEM_CACHED_COHERENT;
      if (alloc_flags & (ANV_BO_ALLOC_MAPPED | ANV_BO_ALLOC_LOCAL_MEM_CPU_VISIBLE))
         return ANV_BO_SLAB_HEAP_LMEM_SMEM;
      return ANV_BO_SLAB_HEAP_LMEM_ONLY;
   }

   if (alloc_flags & ANV_BO_ALLOC_COMPRESSED)
      return ANV_BO_SLAB_HEAP_SMEM_COMPRESSED;
   if ((alloc_flags & ANV_BO_ALLOC_HOST_CACHED_COHERENT) == ANV_BO_ALLOC_HOST_CACHED_COHERENT)
      return ANV_BO_SLAB_HEAP_SMEM_CACHED_COHERENT;
   if (alloc_flags & ANV_BO_ALLOC_HOST_CACHED)
      return ANV_BO_SLAB_HEAP_SMEM_CACHED_INCOHERENT;
   return ANV_BO_SLAB_HEAP_SMEM_COHERENT;
}

/* Return the power of two size of a slab entry matching the input size. */
static unsigned
get_slab_pot_entry_size(struct anv_device *device, unsigned size)
{
   unsigned entry_size = util_next_power_of_two(size);
   unsigned min_entry_size = 1 << device->bo_slabs[0].min_order;

   return MAX2(entry_size, min_entry_size);
}

/* Return the slab entry alignment. */
static unsigned
get_slab_entry_alignment(struct anv_device *device, unsigned size)
{
   unsigned entry_size = get_slab_pot_entry_size(device, size);

   if (size <= entry_size * 3 / 4)
      return entry_size / 4;

   return entry_size;
}

static struct pb_slabs *
get_slabs(struct anv_device *device, uint64_t size)
{
   const unsigned num_slab_allocator = ARRAY_SIZE(device->bo_slabs);

   for (unsigned i = 0; i < num_slab_allocator; i++) {
      struct pb_slabs *slabs = &device->bo_slabs[i];

      if (size <= 1ull << (slabs->min_order + slabs->num_orders - 1))
         return slabs;
   }

   unreachable("should have found a valid slab for this size");
   return NULL;
}

struct anv_bo *
anv_slab_bo_alloc(struct anv_device *device, const char *name, uint64_t size,
                  uint32_t alignment, enum anv_bo_alloc_flags alloc_flags)
{
   const enum anv_bo_slab_heap slab_heap = anv_bo_alloc_flags_to_slab_heap(device, alloc_flags);
   const unsigned num_slab_allocator = ARRAY_SIZE(device->bo_slabs);

   if ((slab_heap == ANV_BO_SLAB_NOT_SUPPORTED) || disable_slab)
      return NULL;

   struct pb_slabs *last_slab = &device->bo_slabs[num_slab_allocator - 1];
   unsigned max_slab_entry_size = 1 << (last_slab->min_order + last_slab->num_orders - 1);

   if (size > max_slab_entry_size)
      return NULL;

   uint64_t alloc_size = size;
   /* using a fixed bo alignment for now, we may need add per platform or per
    * flag alignment of add a new function with alignment parameter
    */
   uint32_t bo_alignment = MAX2(64, alignment);

   /* If it's big enough to store a tiled resource, we need 64K alignment */
   if (!anv_bo_is_small_heap(alloc_flags) && size > (64 * 1024))
      bo_alignment = MAX2(64 * 1024, bo_alignment);

   /* Always use slabs for sizes less than mem_alignment because the kernel
    * aligns everything to mem_alignment.
    */
   if (size < bo_alignment && bo_alignment <= device->info->mem_alignment)
      alloc_size = bo_alignment;

   if (bo_alignment > get_slab_entry_alignment(device, alloc_size)) {
      /* 3/4 allocations can return too small alignment.
       * Try again with a power of two allocation size.
       */
      unsigned pot_size = get_slab_pot_entry_size(device, alloc_size);

      if (bo_alignment <= pot_size) {
         /* This size works but wastes some memory to fulfill the alignment. */
         alloc_size = pot_size;
      } else {
         /* can't fulfill alignment requirements */
         return NULL;
      }
   }

   struct pb_slabs *slabs = get_slabs(device, alloc_size);
   struct pb_slab_entry *entry = pb_slab_alloc(slabs, alloc_size, slab_heap);
   if (!entry) {
      /* Clean up and try again... */
      pb_slabs_reclaim(slabs);

      entry = pb_slab_alloc(slabs, alloc_size, slab_heap);
   }
   if (!entry)
      return NULL;

   struct anv_bo *bo = container_of(entry, struct anv_bo, slab_entry);
   p_atomic_set(&bo->refcount, 1);
   bo->name = name;
   bo->size = size;
   bo->alloc_flags = alloc_flags;
   bo->flags = device->kmd_backend->bo_alloc_flags_to_bo_flags(device, alloc_flags);

   assert(bo->flags == bo->slab_parent->flags);

   return bo;
}

void
anv_slab_bo_free(struct anv_device *device, struct anv_bo *bo)
{
   assert(bo->slab_parent);

   if (bo->map) {
      anv_device_unmap_bo(device, bo, bo->map, bo->size, false /* replace */);
      bo->map = NULL;
   }

   pb_slab_free(get_slabs(device, bo->size), &bo->slab_entry);
}

static unsigned heap_max_get(struct anv_device *device)
{
   unsigned ret;

   if (anv_physical_device_has_vram(device->physical))
      ret = device->info->verx10 >= 200 ? ANV_BO_SLAB_HEAP_LMEM_ONLY_COMPRESSED :
                                          ANV_BO_SLAB_HEAP_LMEM_ONLY;
   else
      ret = device->info->verx10 >= 200 ? ANV_BO_SLAB_HEAP_SMEM_COMPRESSED :
                                          ANV_BO_SLAB_HEAP_SMEM_COHERENT;

   return (ret + 1);
}

static bool
anv_can_reclaim_slab(void *priv, struct pb_slab_entry *entry)
{
   struct anv_bo *bo = container_of(entry, struct anv_bo, slab_entry);

   return p_atomic_read(&bo->refcount) == 0;
}

static struct pb_slab *
anv_slab_alloc(void *priv,
               unsigned heap,
               unsigned entry_size,
               unsigned group_index)
{
   struct anv_device *device = priv;
   struct anv_slab *slab = calloc(1, sizeof(struct anv_slab));
   struct pb_slabs *slabs = device->bo_slabs;
   const unsigned num_slab_allocator = ARRAY_SIZE(device->bo_slabs);
   unsigned slab_size = 0;

   if (!slab)
      return NULL;

   /* Determine the slab buffer size. */
   for (unsigned i = 0; i < num_slab_allocator; i++) {
      unsigned max_entry_size = 1 << (slabs[i].min_order + slabs[i].num_orders - 1);

      if (entry_size <= max_entry_size) {
         /* The slab size is twice the size of the largest possible entry. */
         slab_size = max_entry_size * 2;

         if (!util_is_power_of_two_nonzero(entry_size)) {
            assert(util_is_power_of_two_nonzero(entry_size * 4 / 3));

            /* If the entry size is 3/4 of a power of two, we would waste
             * space and not gain anything if we allocated only twice the
             * power of two for the backing buffer:
             *
             *    2 * 3/4 = 1.5 usable with buffer size 2
             *
             * Allocating 5 times the entry size leads us to the next power
             * of two and results in a much better memory utilization:
             *
             *    5 * 3/4 = 3.75 usable with buffer size 4
             */
            if (entry_size * 5 > slab_size)
               slab_size = util_next_power_of_two(entry_size * 5);
         }

         /* The largest slab should have the same size as the PTE fragment
          * size to get faster address translation.
          *
          * TODO: move this to intel_device_info?
          */
         const unsigned pte_size = 2 * 1024 * 1024;

         if (i == num_slab_allocator - 1 && slab_size < pte_size)
            slab_size = pte_size;

         break;
      }
   }
   assert(slab_size != 0);

   const enum anv_bo_slab_heap bo_slab_heap = heap;
   enum anv_bo_alloc_flags alloc_flags;
   const struct intel_memory_class_instance *regions[2];
   uint16_t num_regions = 0;

   switch (bo_slab_heap) {
   case ANV_BO_SLAB_HEAP_SMEM_CACHED_COHERENT:
      alloc_flags = ANV_BO_ALLOC_HOST_CACHED_COHERENT |
                    ANV_BO_ALLOC_NO_LOCAL_MEM;
      regions[num_regions++] = device->physical->sys.region;
      break;
   case ANV_BO_SLAB_HEAP_SMEM_CACHED_INCOHERENT:
      alloc_flags = ANV_BO_ALLOC_HOST_CACHED |
                    ANV_BO_ALLOC_NO_LOCAL_MEM;
      regions[num_regions++] = device->physical->sys.region;
      break;
   case ANV_BO_SLAB_HEAP_SMEM_COHERENT:
      alloc_flags = ANV_BO_ALLOC_HOST_COHERENT |
                    ANV_BO_ALLOC_NO_LOCAL_MEM;
      regions[num_regions++] = device->physical->sys.region;
      break;
   case ANV_BO_SLAB_HEAP_SMEM_COMPRESSED:
      alloc_flags = ANV_BO_ALLOC_COMPRESSED |
                    ANV_BO_ALLOC_NO_LOCAL_MEM;
      regions[num_regions++] = device->physical->sys.region;
      break;
   case ANV_BO_SLAB_HEAP_LMEM_SMEM:
      alloc_flags = 0;
      regions[num_regions++] = device->physical->sys.region;
      regions[num_regions++] = device->physical->vram_non_mappable.region;
      break;
   case ANV_BO_SLAB_HEAP_LMEM_ONLY:
      alloc_flags = 0;
      regions[num_regions++] = device->physical->vram_non_mappable.region;
      break;
   case ANV_BO_SLAB_HEAP_LMEM_ONLY_COMPRESSED:
      alloc_flags = ANV_BO_ALLOC_COMPRESSED;
      regions[num_regions++] = device->physical->vram_non_mappable.region;
      break;
   case ANV_BO_SLAB_HEAP_BATCH_BUFFER:
      alloc_flags = ANV_BO_ALLOC_MAPPED |
                    ANV_BO_ALLOC_HOST_CACHED_COHERENT |
                    ANV_BO_ALLOC_CAPTURE;
      regions[num_regions++] = device->physical->sys.region;
      if (anv_physical_device_has_vram(device->physical))
         regions[num_regions++] = device->physical->vram_non_mappable.region;
      break;
   case ANV_BO_SLAB_HEAP_DESCRIPTOR_POOL:
      alloc_flags = ANV_BO_ALLOC_CAPTURE |
                    ANV_BO_ALLOC_MAPPED |
                    ANV_BO_ALLOC_HOST_CACHED_COHERENT |
                    ANV_BO_ALLOC_DESCRIPTOR_POOL;
      regions[num_regions++] = device->physical->sys.region;
      if (anv_physical_device_has_vram(device->physical))
         regions[num_regions++] = device->physical->vram_non_mappable.region;
      break;
   default:
      unreachable("Missing");
      return NULL;
   }

   uint64_t actual_size;
   uint32_t gem_handle = device->kmd_backend->gem_create(device, regions,
                                                         num_regions,
                                                         slab_size,
                                                         alloc_flags,
                                                         &actual_size);
   if (gem_handle == 0)
      return NULL;

   /* If we just got this gem_handle from anv_bo_init_new then we know no one
    * else is touching this BO at the moment so we don't need to lock here.
    */
   slab->bo = anv_device_lookup_bo(device, gem_handle);
   if (!slab->bo)
      goto error_alloc_bo;

   slab->bo->name = "slab_parent";
   slab->bo->gem_handle = gem_handle;
   slab->bo->refcount = 1;
   slab->bo->offset = -1;
   slab->bo->size = slab_size;
   slab->bo->actual_size = actual_size;
   slab->bo->alloc_flags = alloc_flags;
   slab->bo->flags = device->kmd_backend->bo_alloc_flags_to_bo_flags(device, alloc_flags);

   VkResult result = anv_bo_vma_alloc_or_close(device, slab->bo, alloc_flags,
                                               0);
   if (result != VK_SUCCESS)
      goto error_vma_alloc;

   result = device->kmd_backend->vm_bind_bo(device, slab->bo);
   if (result != VK_SUCCESS)
      goto error_vma_bind;

   slab->base.num_entries = slab_size / entry_size;
   slab->base.num_free = slab->base.num_entries;
   slab->base.group_index = group_index;
   slab->base.entry_size = entry_size;
   slab->entries = calloc(slab->base.num_entries, sizeof(*slab->entries));
   if (!slab->entries)
      goto error_alloc_entries;

   list_inithead(&slab->base.free);

   for (unsigned i = 0; i < slab->base.num_entries; i++) {
      struct anv_bo *bo = &slab->entries[i];
      uint64_t offset = intel_48b_address(slab->bo->offset);

      offset += (i * entry_size);

      bo->name = "slab_child";
      bo->gem_handle = gem_handle;
      bo->refcount = 0;
      bo->offset = intel_canonical_address(offset);
      bo->size = entry_size;
      bo->actual_size = entry_size;
      bo->alloc_flags = alloc_flags;
      bo->slab_entry.slab = &slab->base;
      bo->slab_parent = slab->bo;

      list_addtail(&bo->slab_entry.head, &slab->base.free);
   }

   ANV_RMV(bo_allocate, device, slab->bo);
   return &slab->base;

error_alloc_entries:
   device->kmd_backend->vm_unbind_bo(device, slab->bo);
error_vma_bind:
   anv_bo_vma_free(device, slab->bo);
error_vma_alloc:
error_alloc_bo:
   device->kmd_backend->gem_close(device, slab->bo);
   free(slab);

   return NULL;
}

static void
anv_slab_free(void *priv, struct pb_slab *pslab)
{
   struct anv_device *device = priv;
   struct anv_slab *slab = (void *)pslab;

   anv_device_release_bo(device, slab->bo);

   free(slab->entries);
   free(slab);
}

bool
anv_slab_bo_init(struct anv_device *device)
{
   const unsigned num_slab_allocator = ARRAY_SIZE(device->bo_slabs);
   unsigned min_slab_order = 8;/* 256 bytes */
   const unsigned max_slab_order = 20;/* 1 MB (slab size = 2 MB) */
   unsigned num_slab_orders_per_allocator = (max_slab_order - min_slab_order) /
                                            num_slab_allocator;

   disable_slab = debug_get_bool_option("ANV_DISABLE_SLAB", false);
   if (disable_slab)
      return true;

   /* Divide the size order range among slab managers. */
   for (unsigned i = 0; i < num_slab_allocator; i++) {
      const unsigned min_order = min_slab_order;
      const unsigned max_order = MIN2(min_order + num_slab_orders_per_allocator,
                                      max_slab_order);

      if (!pb_slabs_init(&device->bo_slabs[i], min_order, max_order,
                         heap_max_get(device), true, device,
                         anv_can_reclaim_slab,
                         anv_slab_alloc,
                         anv_slab_free)) {
         goto error_slabs_init;
      }
      min_slab_order = max_order + 1;
   }

   return true;

error_slabs_init:
   for (unsigned i = 0; i < num_slab_allocator; i++) {
      if (!device->bo_slabs[i].groups)
         break;

      pb_slabs_deinit(&device->bo_slabs[i]);
   }

   return false;
}

void
anv_slab_bo_deinit(struct anv_device *device)
{
   const unsigned num_slab_allocator = ARRAY_SIZE(device->bo_slabs);

   if (disable_slab)
      return;

   for (int i = 0; i < num_slab_allocator; i++) {
      if (device->bo_slabs[i].groups)
         pb_slabs_deinit(&device->bo_slabs[i]);
   }
}
