/*
 * Based on amdgpu_bo.c
 *
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
 * Copyright © 2015 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS, AUTHORS
 * AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 */

#include <inttypes.h>

#include "pipebuffer/pb_cache.h"
#include "pipebuffer/pb_slab.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/list.h"

#include "nouveau_winsys.h"
#include "nouveau_mm.h"

#define NUM_SLAB_ALLOCATORS 3

struct nouveau_mm_allocation {
   struct pb_buffer base;

   /* Note that cached allocations own a reference to bo,
    * but slab allocations do not. */
   struct nouveau_bo *bo;
   uint32_t offset;

   union {
      struct pb_cache_entry cache_entry;
      struct pb_slab_entry slab_entry;
   } u;
};

struct nouveau_mman {
   struct nouveau_device *dev;
   uint32_t domain;
   union nouveau_bo_config config;

   struct pb_cache bo_cache;

   /* Each slab buffer can only contain suballocations of equal sizes, so we
    * need to layer the allocators, so that we don't waste too much memory.
    */
   struct pb_slabs bo_slabs[NUM_SLAB_ALLOCATORS];
};

struct slab {
   struct pb_slab base;
   unsigned entry_size;
   struct nouveau_mman *mm;
   struct nouveau_mm_allocation *buffer;
   struct nouveau_mm_allocation *entries;
};

static void
cache_buffer_dtor(void *priv, struct pb_buffer *buf)
{
   struct nouveau_mm_allocation *alloc = (struct nouveau_mm_allocation*) buf;
   pb_cache_add_buffer(&alloc->u.cache_entry);
}

static const struct pb_vtbl cache_vtbl = {
   cache_buffer_dtor
   /* other functions are never called */
};

static void
destroy_buffer_cache(void *priv, struct pb_buffer *buf)
{
   struct nouveau_mm_allocation *alloc = (struct nouveau_mm_allocation*) buf;
   nouveau_bo_ref(NULL, &alloc->bo);
   FREE(alloc);
}

static bool
can_reclaim_cache(void *, struct pb_buffer *)
{
   // Logic in nouveau_buffer.c ensures that buffers are not in use
   // at this point
   return true;
}

/* Return the power of two size of a slab entry matching the input size. */
static unsigned
get_slab_pot_entry_size(struct nouveau_mman *mm, unsigned size)
{
   unsigned entry_size = util_next_power_of_two(size);
   unsigned min_entry_size = 1 << mm->bo_slabs[0].min_order;

   return MAX2(entry_size, min_entry_size);
}

/* Return the slab entry alignment. */
static unsigned
get_slab_entry_alignment(struct nouveau_mman *mm, unsigned size)
{
   unsigned entry_size = get_slab_pot_entry_size(mm, size);

   if (size <= entry_size * 3 / 4)
      return entry_size / 4;

   return entry_size;
}

static struct pb_slabs*
get_slabs(struct nouveau_mman *mm, uint64_t size)
{
   /* Find the correct slab allocator for the given size. */
   for (unsigned i = 0; i < NUM_SLAB_ALLOCATORS; i++) {
      struct pb_slabs *slabs = &mm->bo_slabs[i];

      if (size <= 1 << (slabs->min_order + slabs->num_orders - 1))
         return slabs;
   }

   assert(0);
   return NULL;
}

static void
subslab_dtor(void *priv, struct pb_buffer *buf)
{
   struct nouveau_mm_allocation *alloc = (struct nouveau_mm_allocation*) buf;
   struct slab *slab = (struct slab*) alloc->u.slab_entry.slab;
   struct nouveau_mman *mm = slab->mm;

   struct pb_slabs *slabs;
   slabs = get_slabs(mm, alloc->base.size);
   pb_slab_free(slabs, &alloc->u.slab_entry);
}

static const struct pb_vtbl subslab_vtbl = {
   subslab_dtor
   /* other functions are never called */
};

static struct pb_slab*
slab_alloc(void *priv, unsigned heap, unsigned entry_size, unsigned group_index)
{
   struct nouveau_mman *mm = priv;
   struct slab *slab = CALLOC_STRUCT(slab);
   unsigned slab_size = 0;

   if (!slab)
      return NULL;

   /* Determine the slab buffer size. */
   for (unsigned i = 0; i < NUM_SLAB_ALLOCATORS; i++) {
      unsigned max_entry_size = 1 << (mm->bo_slabs[i].min_order + mm->bo_slabs[i].num_orders - 1);

      if (entry_size <= max_entry_size) {
         /* The slab size is twice the size of the largest possible entry. */
         slab_size = max_entry_size * 2;

         if (!util_is_power_of_two_nonzero(entry_size)) {
            assert(util_is_power_of_two_nonzero(entry_size * 4 / 3));

            /* If the entry size is 3/4 of a power of two, we would waste space and not gain
             * anything if we allocated only twice the power of two for the backing buffer:
             *   2 * 3/4 = 1.5 usable with buffer size 2
             *
             * Allocating 5 times the entry size leads us to the next power of two and results
             * in a much better memory utilization:
             *   5 * 3/4 = 3.75 usable with buffer size 4
             */
            if (entry_size * 5 > slab_size)
               slab_size = util_next_power_of_two(entry_size * 5);
         }

         break;
      }
   }
   assert(slab_size != 0);

   slab->buffer = nouveau_mm_allocate(mm, slab_size, NULL, NULL);
   if (!slab->buffer)
      goto fail;

   slab_size = slab->buffer->base.size;

   slab->base.num_entries = slab_size / entry_size;
   slab->base.num_free = slab->base.num_entries;
   slab->mm = mm;
   slab->entry_size = entry_size;
   slab->entries = CALLOC(slab->base.num_entries, sizeof(*slab->entries));
   if (!slab->entries)
      goto fail_buffer;

   list_inithead(&slab->base.free);

   for (unsigned i = 0; i < slab->base.num_entries; ++i) {
      struct nouveau_mm_allocation *alloc = &slab->entries[i];

      alloc->base.alignment_log2 = util_logbase2(get_slab_entry_alignment(mm, entry_size));
      alloc->base.size = entry_size;
      alloc->base.vtbl = &subslab_vtbl;
      alloc->base.placement = 0;
      alloc->u.slab_entry.slab = &slab->base;
      alloc->u.slab_entry.group_index = group_index;
      alloc->u.slab_entry.entry_size = entry_size;

      alloc->bo = slab->buffer->bo;
      alloc->offset = slab->buffer->offset + i * entry_size;

      list_addtail(&alloc->u.slab_entry.head, &slab->base.free);
   }

   return &slab->base;

fail_buffer:
   nouveau_mm_free(slab->buffer);
fail:
   FREE(slab);
   return NULL;
}

static void
slab_free(void *priv, struct pb_slab *pb_slab)
{
   struct slab *slab = (struct slab *) pb_slab;

   FREE(slab->entries);
   nouveau_mm_free(slab->buffer);
   FREE(slab);
}

static bool
slab_can_reclaim(void *, struct pb_slab_entry *)
{
   // Logic in nouveau_buffer.c ensures that buffers are not in use
   // at this point
   return true;
}

static void clean_up_buffer_managers(struct nouveau_mman *mm)
{
   for (unsigned i = 0; i < NUM_SLAB_ALLOCATORS; i++)
      pb_slabs_reclaim(&mm->bo_slabs[i]);

   pb_cache_release_all_buffers(&mm->bo_cache);
}

/* @return token to identify slab or NULL if we just allocated a new bo */
struct nouveau_mm_allocation *
nouveau_mm_allocate(struct nouveau_mman *mm,
                    uint32_t size, struct nouveau_bo **bo, uint32_t *offset)
{
   struct nouveau_mm_allocation *alloc = NULL;

   const int alignment = 64;

   struct pb_slabs *last_slab = &mm->bo_slabs[NUM_SLAB_ALLOCATORS - 1];
   unsigned max_slab_entry_size = 1 << (last_slab->min_order + last_slab->num_orders - 1);

   /* Sub-allocate small buffers from slabs. */
   if (size <= max_slab_entry_size) {
      struct pb_slab_entry *entry;
      unsigned alloc_size = size;

      assert(alignment <= 4096);

      if (size < alignment && alignment <= 4 * 1024)
         alloc_size = alignment;

      if (alignment > get_slab_entry_alignment(mm, alloc_size)) {
         /* 3/4 allocations can return too small alignment. Try again with a power of two
          * allocation size.
          */
         unsigned pot_size = get_slab_pot_entry_size(mm, alloc_size);

         if (alignment <= pot_size) {
            /* This size works but wastes some memory to fulfil the alignment. */
            alloc_size = pot_size;
         } else {
            goto no_slab; /* can't fulfil alignment requirements */
         }
      }

      struct pb_slabs *slabs = get_slabs(mm, alloc_size);
      entry = pb_slab_alloc(slabs, alloc_size, 0);
      if (!entry) {
         /* Clean up buffer managers and try again. */
         clean_up_buffer_managers(mm);

         entry = pb_slab_alloc(slabs, alloc_size, 0);
      }
      if (!entry)
         return NULL;

      alloc = container_of(entry, struct nouveau_mm_allocation, u.slab_entry);
      pipe_reference_init(&alloc->base.reference, 1);
      alloc->base.size = size;
      assert(alignment <= 1 << alloc->base.alignment_log2);

      if (bo)
         nouveau_bo_ref(alloc->bo, bo);
      if (offset)
         *offset = alloc->offset;

      return alloc;
   }

no_slab:
   if (!alloc) {
      alloc = (struct nouveau_mm_allocation*)
         pb_cache_reclaim_buffer(&mm->bo_cache, size, alignment, 0, 0);
   }

   if (!alloc) {
      // Allocate a new buffer
      alloc = CALLOC_STRUCT(nouveau_mm_allocation);
      if (!alloc)
         return NULL;

      pipe_reference_init(&alloc->base.reference, 1);
      alloc->base.alignment_log2 = util_logbase2(alignment);
      alloc->base.size = size;
      alloc->base.vtbl = &cache_vtbl;

      pb_cache_init_entry(&mm->bo_cache, &alloc->u.cache_entry, &alloc->base, 0);

      int ret = nouveau_bo_new(mm->dev, mm->domain, alignment, size,
                               &mm->config, &alloc->bo);
      if (ret)
         debug_printf("bo_new(%x, %x): %i\n",
                      size, mm->config.nv50.memtype, ret);

      alloc->offset = 0;
   }

   if (bo)
      nouveau_bo_ref(alloc->bo, bo);
   if (offset)
      *offset = alloc->offset;

   return alloc;

#if 0
   ret = nouveau_bo_new(mm->dev, mm->domain, 0, size, &mm->config,
                        bo);
   if (ret)
      debug_printf("bo_new(%x, %x): %i\n",
                   size, mm->config.nv50.memtype, ret);

   *offset = 0;
   return NULL;
#endif
}

void
nouveau_mm_free(struct nouveau_mm_allocation *alloc)
{
   struct pb_buffer *base = &alloc->base;
   pb_reference(&base, NULL);
}

void
nouveau_mm_free_work(void *data)
{
   nouveau_mm_free(data);
}

struct nouveau_mman *
nouveau_mm_create(struct nouveau_device *dev, uint32_t domain,
                  union nouveau_bo_config *config)
{
   struct nouveau_mman *mm = CALLOC_STRUCT(nouveau_mman);

   if (!mm)
      return NULL;

   mm->dev = dev;
   mm->domain = domain;
   mm->config = *config;

   /* Cache size heuristic */
   uint64_t memory_size;  // in bytes
   if (domain & NOUVEAU_BO_VRAM) {
      memory_size = dev->vram_limit;
   } else if (domain & NOUVEAU_BO_GART) {
      uint64_t physical_memory = 0;
      os_get_total_physical_memory(&physical_memory);
      memory_size = MIN2(physical_memory, dev->gart_size);
   } else {
      unreachable("unknown domain");
      memory_size = 0;
   }
   uint64_t cache_size = memory_size / 8;  // in bytes
   if (cache_size == 0) {
      cache_size = 256 * 1024 * 1024;
   }

   /* Create managers. */
   pb_cache_init(&mm->bo_cache, 1,
                 500000, 2.0f, 0,
                 cache_size, mm,
                 destroy_buffer_cache, can_reclaim_cache);

   unsigned min_slab_order = 8;  /* 256 bytes */
   unsigned max_slab_order = 20; /* 1 MB (slab size = 2 MB) */
   unsigned num_slab_orders_per_allocator = (max_slab_order - min_slab_order) /
                                             NUM_SLAB_ALLOCATORS;

   /* Divide the size order range among slab managers. */
   for (unsigned i = 0; i < NUM_SLAB_ALLOCATORS; i++) {
      unsigned min_order = min_slab_order;
      unsigned max_order = MIN2(min_order + num_slab_orders_per_allocator,
                                 max_slab_order);

      int ret = pb_slabs_init(&mm->bo_slabs[i],
                              min_order, max_order,
                              1, true, mm,
                              slab_can_reclaim,
                              slab_alloc,
                              slab_free);

      if (!ret) {
         pb_cache_deinit(&mm->bo_cache);
         for (unsigned j = 0; j < i; j++) {
            pb_slabs_deinit(&mm->bo_slabs[j]);
         }
         FREE(mm);
         return NULL;
      }

      min_slab_order = max_order + 1;
   }

   return mm;
}

void
nouveau_mm_destroy(struct nouveau_mman *mm)
{
   if (!mm)
      return;

   for (unsigned i = 0; i < NUM_SLAB_ALLOCATORS; i++) {
      pb_slabs_deinit(&mm->bo_slabs[i]);
   }
   pb_cache_deinit(&mm->bo_cache);

   FREE(mm);
}
