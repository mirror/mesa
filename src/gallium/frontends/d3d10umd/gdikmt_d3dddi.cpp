#include "gdikmt_d3dddi.h"
#include <string.h>
#include "gdikmt/gdikmt.h"
#include "pipe/p_state.h"
#include "util/u_debug.h"
#include "util/u_memory.h"

#include <d3dukmdt.h>
#include "winddk_compat.h"

static inline struct gdikmt_context_d3dddi *
gdikmt_context_d3dddi(struct gdikmt_context *iws)
{
   return (struct gdikmt_context_d3dddi *)iws;
};

static inline struct gdikmt_device_d3dddi *
gdikmt_device_d3dddi(struct gdikmt_device *iws)
{
   return (struct gdikmt_device_d3dddi *)iws;
}

NTSTATUS
gdikmt_d3dddi_queryadapterinfo(struct gdikmt_device *_device,
                               KMTQUERYADAPTERINFOTYPE Type,
                               VOID *pPrivateDriverData,
                               UINT PrivateDriverDataSize)
{
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_device);

   D3DDDICB_QUERYADAPTERINFO queryAdapterInfo;
   memset(&queryAdapterInfo, 0, sizeof(queryAdapterInfo));
   queryAdapterInfo.pPrivateDriverData = pPrivateDriverData;
   queryAdapterInfo.PrivateDriverDataSize = PrivateDriverDataSize;

   return device->pAdapterCallbacks->pfnQueryAdapterInfoCb(device->hRTAdapter,
                                                           &queryAdapterInfo);
}

NTSTATUS
gdikmt_d3dddi_escape(struct gdikmt_device *_device, VOID *pPrivateDriverData,
                     UINT PrivateDriverDataSize)
{
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_device);

   D3DDDICB_ESCAPE escape;
   memset(&escape, 0, sizeof(escape));
   escape.hDevice = device->hRTDevice;
   escape.pPrivateDriverData = pPrivateDriverData;
   escape.PrivateDriverDataSize = PrivateDriverDataSize;

   return device->KTCallbacks.pfnEscapeCb(device->hRTAdapter, &escape);
}

NTSTATUS
gdikmt_d3dddi_render(struct gdikmt_context *_ctx, struct gdikmt_render *options)
{
   struct gdikmt_context_d3dddi *ctx = gdikmt_context_d3dddi(_ctx);
   struct gdikmt_device_d3dddi *dev = gdikmt_device_d3dddi(ctx->base.device);

   D3DDDICB_RENDER render;
   memset(&render, 0, sizeof(render));
   render.hContext = ctx->hContext;

   render.CommandOffset = options->CommandOffset;
   render.CommandLength = options->CommandLength;
   render.NumAllocations = options->AllocationCount;
   render.NumPatchLocations = options->PatchLocationCount;

   render.NewCommandBufferSize = options->NewCommandBufferSize;
   render.NewAllocationListSize = options->NewAllocationListSize;
   render.NewPatchLocationListSize = options->NewPatchLocationListSize;

   render.Flags.ResizeCommandBuffer = options->ResizeCommandBuffer;
   render.Flags.ResizeAllocationList = options->ResizeAllocationList;
   render.Flags.ResizePatchLocationList = options->ResizePatchLocationList;

   NTSTATUS Status = dev->KTCallbacks.pfnRenderCb(dev->hRTDevice, &render);

   if(options->CompletionEvent) {
      D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 signalEvent;
      memset(&signalEvent, 0, sizeof(signalEvent));
      signalEvent.hContext = ctx->hContext;
      signalEvent.ObjectCount = 0;
      signalEvent.BroadcastContextCount = 0;
      signalEvent.Flags.EnqueueCpuEvent = TRUE;
      signalEvent.CpuEventHandle = options->CompletionEvent;
      dev->KTCallbacks.pfnSignalSynchronizationObject2Cb(dev->hRTDevice, &signalEvent);
   }

   ctx->base.pCommandBuffer = render.pNewCommandBuffer;
   ctx->base.pAllocationList = render.pNewAllocationList;
   ctx->base.pPatchLocationList = render.pNewPatchLocationList;

   ctx->base.CommandBufferSize = render.NewCommandBufferSize;
   ctx->base.AllocationListSize = render.NewAllocationListSize;
   ctx->base.PatchLocationListSize = render.NewPatchLocationListSize;

   return Status;
}

void
gdikmt_d3dddi_destroycontext(struct gdikmt_context *_ctx)
{
   struct gdikmt_context_d3dddi *ctx = gdikmt_context_d3dddi(_ctx);
   struct gdikmt_device_d3dddi *dev = gdikmt_device_d3dddi(ctx->base.device);

   D3DDDICB_DESTROYCONTEXT destroyContext;
   memset(&destroyContext, 0, sizeof(destroyContext));
   destroyContext.hContext = ctx->hContext;
   dev->KTCallbacks.pfnDestroyContextCb(dev->hRTDevice, &destroyContext);

   FREE(ctx);

   return;
}

NTSTATUS
gdikmt_d3dddi_createcontext(struct gdikmt_device *_device,
                            struct gdikmt_context **out_ctx)
{
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_device);

   struct gdikmt_context_d3dddi *ctx = CALLOC_STRUCT(gdikmt_context_d3dddi);
   if (!ctx) {
      return STATUS_NO_MEMORY;
   }
   ctx->base.device = _device;

   D3DDDICB_CREATECONTEXT createContext;
   memset(&createContext, 0, sizeof(createContext));
   NTSTATUS Status =
      device->KTCallbacks.pfnCreateContextCb(device->hRTDevice, &createContext);

   if (NT_SUCCESS(Status)) {
      ctx->hContext = createContext.hContext;

      ctx->base.pCommandBuffer = createContext.pCommandBuffer;
      ctx->base.pAllocationList = createContext.pAllocationList;
      ctx->base.pPatchLocationList = createContext.pPatchLocationList;

      ctx->base.CommandBufferSize = createContext.CommandBufferSize;
      ctx->base.AllocationListSize = createContext.AllocationListSize;
      ctx->base.PatchLocationListSize = createContext.PatchLocationListSize;

      ctx->base.destroy = gdikmt_d3dddi_destroycontext;
      ctx->base.render = gdikmt_d3dddi_render;

      *out_ctx = &ctx->base;
   }

   return Status;
}

NTSTATUS
gdikmt_d3dddi_createallocation(struct gdikmt_device *_device,
                               struct gdikmt_createallocation *options)
{
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_device);

   D3DDDICB_ALLOCATE createAllocation;
   memset(&createAllocation, 0, sizeof(createAllocation));
   createAllocation.NumAllocations = options->NumAllocations;
   createAllocation.pAllocationInfo = options->pAllocationInfo;

   createAllocation.pPrivateDriverData = options->pPrivateDriverData;
   createAllocation.PrivateDriverDataSize = options->PrivateDriverDataSize;

   createAllocation.hResource = device->hRTResource;

   if (device->isPrimary) {
      options->pAllocationInfo->VidPnSourceId = device->allocationVidPn;
      options->pAllocationInfo->Flags.Primary = 1;
   }

   NTSTATUS Status =
      device->KTCallbacks.pfnAllocateCb(device->hRTDevice, &createAllocation);

   options->hResource = (HANDLE)createAllocation.hKMResource;

   return Status;
}

NTSTATUS
gdikmt_d3dddi_destroyallocation(struct gdikmt_device *_device, HANDLE hResource,
                                D3DKMT_HANDLE hAllocation)
{
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_device);

   D3DDDICB_DEALLOCATE destroyAllocation;
   memset(&destroyAllocation, 0, sizeof(destroyAllocation));

   D3DKMT_HANDLE allocations[1];
   allocations[0] = hAllocation;
   destroyAllocation.NumAllocations = 1;
   destroyAllocation.HandleList = allocations;

   return device->KTCallbacks.pfnDeallocateCb(device->hRTDevice,
                                              &destroyAllocation);
}

NTSTATUS
gdikmt_d3dddi_lockallocation(struct gdikmt_device *_device,
                             D3DKMT_HANDLE hAllocation,
                             D3DDDICB_LOCKFLAGS flags, void **out_ptr)
{
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_device);

   D3DDDICB_LOCK lock;
   memset(&lock, 0, sizeof(lock));
   lock.Flags = flags;
   lock.Flags.LockEntire = TRUE;
   lock.hAllocation = hAllocation;
   NTSTATUS Status = device->KTCallbacks.pfnLockCb(device->hRTDevice, &lock);

   *out_ptr = lock.pData;

   return Status;
}

NTSTATUS
gdikmt_d3dddi_queryallocation(struct gdikmt_device *_device,
                              struct gdikmt_openallocation *options)
{
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_device);

   options->NumAllocations = device->pOpenResource->NumAllocations;
   options->PrivateDriverDataSize =
      device->pOpenResource->PrivateDriverDataSize;
   options->TotalBufferSize = 1;

   return STATUS_SUCCESS;
}

NTSTATUS
gdikmt_d3dddi_openallocation(struct gdikmt_device *_device,
                             struct gdikmt_openallocation *options)
{
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_device);

   for (UINT i = 0; i < options->NumAllocations; i++) {
      options->pOpenAllocation[i] =
         device->pOpenResource->pOpenAllocationInfo[i];
   }

   memcpy(options->pPrivateDriverData,
          device->pOpenResource->pPrivateDriverData,
          device->pOpenResource->PrivateDriverDataSize);

   options->PrivateDriverDataSize =
      device->pOpenResource->PrivateDriverDataSize;

   return STATUS_SUCCESS;
}

NTSTATUS
gdikmt_d3dddi_present(struct gdikmt_context *_ctx, D3DKMT_HANDLE hSrcAllocation,
                      void *winsys_drawable_handle, struct pipe_box *sub_box)
{
   struct gdikmt_context_d3dddi *ctx = gdikmt_context_d3dddi(_ctx);
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_ctx->device);

   DXGIDDICB_PRESENT kmPresent;
   memset(&kmPresent, 0, sizeof(kmPresent));
   kmPresent.hSrcAllocation = hSrcAllocation;
   kmPresent.hDstAllocation = NULL;
   kmPresent.pDXGIContext = winsys_drawable_handle;
   kmPresent.hContext = ctx->hContext;
   kmPresent.BroadcastContextCount = 0;

   HRESULT res =
      device->pDXGIBaseCallbacks->pfnPresentCb(device->hRTDevice, &kmPresent);
   return res;
};

NTSTATUS
gdikmt_d3dddi_setdisplaymode(struct gdikmt_device *_device,
                             D3DKMT_HANDLE hSrcAllocation)
{
   struct gdikmt_device_d3dddi *device = gdikmt_device_d3dddi(_device);

   D3DDDICB_SETDISPLAYMODE setMode;
   setMode.hPrimaryAllocation = hSrcAllocation;
   setMode.PrivateDriverFormatAttribute = 0;

   HRESULT res =
      device->KTCallbacks.pfnSetDisplayModeCb(device->hRTDevice, &setMode);
   return res;
};

void gdikmt_d3dddi_destroy(struct gdikmt_device *_device){};

void
gdikmt_d3dddi_fill_basefuncs(struct gdikmt_device_d3dddi *device)
{
   device->base.destroy = gdikmt_d3dddi_destroy;
   device->base.queryAdapterInfo = gdikmt_d3dddi_queryadapterinfo;
   device->base.escape = gdikmt_d3dddi_escape;

   device->base.createContext = gdikmt_d3dddi_createcontext;

   device->base.createAllocation = gdikmt_d3dddi_createallocation;
   device->base.destroyAllocation = gdikmt_d3dddi_destroyallocation;
   device->base.lockAllocation = gdikmt_d3dddi_lockallocation;
   device->base.queryAllocation = gdikmt_d3dddi_queryallocation;
   device->base.openAllocation = gdikmt_d3dddi_openallocation;

   device->base.present = gdikmt_d3dddi_present;
   device->base.setDisplayMode = gdikmt_d3dddi_setdisplaymode;
};