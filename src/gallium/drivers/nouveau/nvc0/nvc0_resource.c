#include "drm-uapi/drm_fourcc.h"

#include "pipe/p_context.h"
#include "nvc0/nvc0_resource.h"
#include "nouveau_screen.h"


static struct pipe_resource *
nvc0_resource_create(struct pipe_screen *screen,
                     const struct pipe_resource *templ)
{
   switch (templ->target) {
   case PIPE_BUFFER:
      return nouveau_buffer_create(screen, templ);
   default:
      return nvc0_miptree_create(screen, templ);
   }
}

static void
nvc0_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *res)
{
   if (res->target == PIPE_BUFFER)
      nouveau_buffer_destroy(pscreen, res);
   else
      nv50_miptree_destroy(pscreen, res);
}

static struct pipe_resource *
nvc0_resource_from_handle(struct pipe_screen * screen,
                          const struct pipe_resource *templ,
                          struct winsys_handle *whandle,
                          unsigned usage)
{
   if (templ->target == PIPE_BUFFER) {
      return NULL;
   } else {
      struct pipe_resource *res = nv50_miptree_from_handle(screen,
                                                           templ, whandle);
      return res;
   }
}

static struct pipe_surface *
nvc0_surface_create(struct pipe_context *pipe,
                    struct pipe_resource *pres,
                    const struct pipe_surface *templ)
{
   if (unlikely(pres->target == PIPE_BUFFER))
      return nv50_surface_from_buffer(pipe, pres, templ);
   return nvc0_miptree_surface_new(pipe, pres, templ);
}

static struct pipe_resource *
nvc0_resource_from_user_memory(struct pipe_screen *pipe,
                               const struct pipe_resource *templ,
                               void *user_memory)
{
   struct nouveau_screen *screen = nouveau_screen(pipe);
   if (!screen->has_svm || templ->target != PIPE_BUFFER)
      return NULL;
   return nouveau_buffer_create_from_user(pipe, templ, user_memory);
}

void
nvc0_init_resource_functions(struct pipe_context *pcontext)
{
   pcontext->buffer_map = nouveau_buffer_transfer_map;
   pcontext->texture_map = nvc0_miptree_transfer_map;
   pcontext->transfer_flush_region = nouveau_buffer_transfer_flush_region;
   pcontext->buffer_unmap = nouveau_buffer_transfer_unmap;
   pcontext->texture_unmap = nvc0_miptree_transfer_unmap;
   pcontext->buffer_subdata = u_default_buffer_subdata;
   pcontext->texture_subdata = u_default_texture_subdata;
   pcontext->create_surface = nvc0_surface_create;
   pcontext->surface_destroy = nv50_surface_destroy;
   pcontext->invalidate_resource = nv50_invalidate_resource;
}

void
nvc0_screen_init_resource_functions(struct pipe_screen *pscreen)
{
   pscreen->resource_create = nvc0_resource_create;
   pscreen->resource_from_handle = nvc0_resource_from_handle;
   pscreen->resource_get_handle = nvc0_miptree_get_handle;
   pscreen->resource_destroy = nvc0_resource_destroy;
   pscreen->resource_from_user_memory = nvc0_resource_from_user_memory;

   pscreen->memobj_create_from_handle = nv50_memobj_create_from_handle;
   pscreen->resource_from_memobj = nv50_resource_from_memobj;
   pscreen->memobj_destroy = nv50_memobj_destroy;
}
