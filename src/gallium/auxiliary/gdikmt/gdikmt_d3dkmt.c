#include <string.h>
#include "gdikmt/gdikmt.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "gdikmt.h"

#include <d3dkmthk.h>
#include <d3dukmdt.h>
#include <winnt.h>
#include <winternl.h>

struct d3dkmt_callbacks {
   PFND3DKMT_QUERYADAPTERINFO queryAdapterInfo;
   PFND3DKMT_ESCAPE escape;
   PFND3DKMT_RENDER render;
   PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signalSynchronizationObject2;
   PFND3DKMT_CREATECONTEXT createContext;
   PFND3DKMT_DESTROYCONTEXT destroyContext;
   PFND3DKMT_CREATEALLOCATION createAllocation;
   PFND3DKMT_DESTROYALLOCATION destroyAllocation;
   PFND3DKMT_LOCK lock;
   PFND3DKMT_QUERYRESOURCEINFO queryResourceInfo;
   PFND3DKMT_OPENRESOURCE openResource;
   PFND3DKMT_CREATEDEVICE createDevice;
   PFND3DKMT_DESTROYDEVICE destroyDevice;
   PFND3DKMT_OPENADAPTERFROMHDC openAdapterFromHdc;
   PFND3DKMT_CLOSEADAPTER closeAdapter;
};

void
gdikmt_load_callbacks(HINSTANCE gdi32lib, struct d3dkmt_callbacks *cb)
{
   cb->queryAdapterInfo = (PFND3DKMT_QUERYADAPTERINFO)GetProcAddress(
      gdi32lib, "D3DKMTQueryAdapterInfo");
   cb->escape = (PFND3DKMT_ESCAPE)GetProcAddress(gdi32lib, "D3DKMTEscape");
   cb->render = (PFND3DKMT_RENDER)GetProcAddress(gdi32lib, "D3DKMTRender");
   cb->signalSynchronizationObject2 =
      (PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECT2)GetProcAddress(
         gdi32lib, "D3DKMTSignalSynchronizationObject2");
   cb->createContext =
      (PFND3DKMT_CREATECONTEXT)GetProcAddress(gdi32lib, "D3DKMTCreateContext");
   cb->destroyContext = (PFND3DKMT_DESTROYCONTEXT)GetProcAddress(
      gdi32lib, "D3DKMTDestroyContext");
   cb->createAllocation = (PFND3DKMT_CREATEALLOCATION)GetProcAddress(
      gdi32lib, "D3DKMTCreateAllocation");
   cb->destroyAllocation = (PFND3DKMT_DESTROYALLOCATION)GetProcAddress(
      gdi32lib, "D3DKMTDestroyAllocation");
   cb->lock = (PFND3DKMT_LOCK)GetProcAddress(gdi32lib, "D3DKMTLock");
   cb->queryResourceInfo = (PFND3DKMT_QUERYRESOURCEINFO)GetProcAddress(
      gdi32lib, "D3DKMTQueryResourceInfo");
   cb->openResource =
      (PFND3DKMT_OPENRESOURCE)GetProcAddress(gdi32lib, "D3DKMTOpenResource");
   cb->createDevice =
      (PFND3DKMT_CREATEDEVICE)GetProcAddress(gdi32lib, "D3DKMTCreateDevice");
   cb->destroyDevice =
      (PFND3DKMT_DESTROYDEVICE)GetProcAddress(gdi32lib, "D3DKMTDestroyDevice");
   cb->openAdapterFromHdc = (PFND3DKMT_OPENADAPTERFROMHDC)GetProcAddress(
      gdi32lib, "D3DKMTOpenAdapterFromHdc");
   cb->closeAdapter =
      (PFND3DKMT_CLOSEADAPTER)GetProcAddress(gdi32lib, "D3DKMTCloseAdapter");
}

struct gdikmt_context_d3dkmt {
   struct gdikmt_context base;

   D3DKMT_HANDLE hContext;
};

static inline struct gdikmt_context_d3dkmt *
gdikmt_context_d3dkmt(struct gdikmt_context *iws)
{
   return (struct gdikmt_context_d3dkmt *)iws;
}

struct gdikmt_device_d3dkmt {
   struct gdikmt_device base;

   D3DKMT_HANDLE hAdapter;
   D3DKMT_HANDLE hDevice;

   HINSTANCE gdi32lib;
   struct d3dkmt_callbacks cb;
};

static inline struct gdikmt_device_d3dkmt *
gdikmt_device_d3dkmt(struct gdikmt_device *iws)
{
   return (struct gdikmt_device_d3dkmt *)iws;
}

NTSTATUS
gdikmt_d3dkmt_queryadapterinfo(struct gdikmt_device *_device,
                               KMTQUERYADAPTERINFOTYPE Type,
                               VOID *pPrivateDriverData,
                               UINT PrivateDriverDataSize)
{
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_device);

   D3DKMT_QUERYADAPTERINFO queryAdapterInfo;
   memset(&queryAdapterInfo, 0, sizeof(queryAdapterInfo));
   queryAdapterInfo.hAdapter = device->hAdapter;
   queryAdapterInfo.Type = Type;
   queryAdapterInfo.pPrivateDriverData = pPrivateDriverData;
   queryAdapterInfo.PrivateDriverDataSize = PrivateDriverDataSize;

   return device->cb.queryAdapterInfo(&queryAdapterInfo);
}

NTSTATUS
gdikmt_d3dkmt_escape(struct gdikmt_device *_device, VOID *pPrivateDriverData,
                     UINT PrivateDriverDataSize)
{
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_device);

   D3DKMT_ESCAPE escape;
   memset(&escape, 0, sizeof(escape));
   escape.hAdapter = device->hAdapter;
   escape.hDevice = device->hDevice;
   escape.pPrivateDriverData = pPrivateDriverData;
   escape.PrivateDriverDataSize = PrivateDriverDataSize;

   return device->cb.escape(&escape);
}

NTSTATUS
gdikmt_d3dkmt_render(struct gdikmt_context *_ctx, struct gdikmt_render *options)
{
   struct gdikmt_context_d3dkmt *ctx = gdikmt_context_d3dkmt(_ctx);
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_ctx->device);

   D3DKMT_RENDER render;
   memset(&render, 0, sizeof(render));
   render.hContext = ctx->hContext;

   render.CommandOffset = options->CommandOffset;
   render.CommandLength = options->CommandLength;
   render.AllocationCount = options->AllocationCount;
   render.PatchLocationCount = options->PatchLocationCount;

   render.NewCommandBufferSize = options->NewCommandBufferSize;
   render.NewAllocationListSize = options->NewAllocationListSize;
   render.NewPatchLocationListSize = options->NewPatchLocationListSize;

   render.Flags.ResizeCommandBuffer = options->ResizeCommandBuffer;
   render.Flags.ResizeAllocationList = options->ResizeAllocationList;
   render.Flags.ResizePatchLocationList = options->ResizePatchLocationList;

   NTSTATUS Status = device->cb.render(&render);

   if (options->CompletionEvent) {
      D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signalEvent;
      memset(&signalEvent, 0, sizeof(signalEvent));
      signalEvent.hContext = ctx->hContext;
      signalEvent.ObjectCount = 0;
      signalEvent.BroadcastContextCount = 0;
      signalEvent.Flags.EnqueueCpuEvent = TRUE;
      signalEvent.CpuEventHandle = options->CompletionEvent;
      device->cb.signalSynchronizationObject2(&signalEvent);
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
gdikmt_d3dkmt_destroycontext(struct gdikmt_context *_ctx)
{
   struct gdikmt_context_d3dkmt *ctx = gdikmt_context_d3dkmt(_ctx);
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_ctx->device);

   D3DKMT_DESTROYCONTEXT destroyContext;
   memset(&destroyContext, 0, sizeof(destroyContext));
   destroyContext.hContext = ctx->hContext;
   device->cb.destroyContext(&destroyContext);

   FREE(ctx);

   return;
}

NTSTATUS
gdikmt_d3dkmt_createcontext(struct gdikmt_device *_device,
                            struct gdikmt_context **out_ctx)
{
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_device);

   struct gdikmt_context_d3dkmt *ctx = CALLOC_STRUCT(gdikmt_context_d3dkmt);
   if (!ctx) {
      return STATUS_NO_MEMORY;
   }
   ctx->base.device = _device;

   D3DKMT_CREATECONTEXT createContext;
   memset(&createContext, 0, sizeof(createContext));
   createContext.hDevice = device->hDevice;
   NTSTATUS Status = device->cb.createContext(&createContext);

   if (NT_SUCCESS(Status)) {
      ctx->hContext = createContext.hContext;

      ctx->base.pCommandBuffer = createContext.pCommandBuffer;
      ctx->base.pAllocationList = createContext.pAllocationList;
      ctx->base.pPatchLocationList = createContext.pPatchLocationList;

      ctx->base.CommandBufferSize = createContext.CommandBufferSize;
      ctx->base.AllocationListSize = createContext.AllocationListSize;
      ctx->base.PatchLocationListSize = createContext.PatchLocationListSize;

      ctx->base.destroy = gdikmt_d3dkmt_destroycontext;
      ctx->base.render = gdikmt_d3dkmt_render;

      *out_ctx = &ctx->base;
   }

   return Status;
}

NTSTATUS
gdikmt_d3dkmt_createallocation(struct gdikmt_device *_device,
                               struct gdikmt_createallocation *options)
{
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_device);

   D3DKMT_CREATEALLOCATION createAllocation;
   memset(&createAllocation, 0, sizeof(createAllocation));
   createAllocation.hDevice = device->hDevice;
   createAllocation.NumAllocations = options->NumAllocations;
   createAllocation.pAllocationInfo = options->pAllocationInfo;

   createAllocation.Flags.CreateResource = TRUE;
   createAllocation.pPrivateDriverData = options->pPrivateDriverData;
   createAllocation.PrivateDriverDataSize = options->PrivateDriverDataSize;

   NTSTATUS Status = device->cb.createAllocation(&createAllocation);

   options->hResource = (HANDLE)createAllocation.hResource;

   return Status;
}

NTSTATUS
gdikmt_d3dkmt_lockallocation(struct gdikmt_device *_device,
                             D3DKMT_HANDLE hAllocation,
                             D3DDDICB_LOCKFLAGS flags, void **out_ptr)
{
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_device);

   D3DKMT_LOCK lock;
   memset(&lock, 0, sizeof(lock));
   lock.hDevice = device->hDevice;
   lock.Flags = flags;
   lock.Flags.LockEntire = TRUE;
   lock.hAllocation = hAllocation;
   NTSTATUS Status = device->cb.lock(&lock);

   *out_ptr = lock.pData;

   return Status;
}

NTSTATUS
gdikmt_d3dkmt_destroyallocation(struct gdikmt_device *_device, HANDLE hResource,
                                D3DKMT_HANDLE hAllocation)
{
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_device);

   D3DKMT_DESTROYALLOCATION destroyAllocation;
   memset(&destroyAllocation, 0, sizeof(destroyAllocation));
   destroyAllocation.hDevice = device->hDevice;

   if (hResource != NULL) {
      destroyAllocation.hResource = (D3DKMT_HANDLE)hResource;
   } else {
      D3DKMT_HANDLE handles[1];
      handles[0] = hAllocation;
      destroyAllocation.phAllocationList = handles;
      destroyAllocation.AllocationCount = 1;
   }

   return device->cb.destroyAllocation(&destroyAllocation);
}

NTSTATUS
gdikmt_d3dddi_queryallocation(struct gdikmt_device *_device,
                              struct gdikmt_openallocation *openAllocation)
{
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_device);

   D3DKMT_QUERYRESOURCEINFO queryResource;
   memset(&queryResource, 0, sizeof(queryResource));
   queryResource.hDevice = device->hDevice;
   queryResource.hGlobalShare = openAllocation->hGlobalHandle;

   NTSTATUS Status = device->cb.queryResourceInfo(&queryResource);

   openAllocation->PrivateDriverDataSize =
      queryResource.ResourcePrivateDriverDataSize;
   openAllocation->TotalBufferSize = queryResource.TotalPrivateDriverDataSize;
   openAllocation->NumAllocations = queryResource.NumAllocations;
   openAllocation->PrivateRuntimeSize = queryResource.PrivateRuntimeDataSize;

   return Status;
}

NTSTATUS
gdikmt_d3dddi_openallocation(struct gdikmt_device *_device,
                             struct gdikmt_openallocation *openAllocation)
{
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_device);

   void *privateRuntimeData = malloc(openAllocation->PrivateRuntimeSize);

   D3DKMT_OPENRESOURCE openResource;
   memset(&openResource, 0, sizeof(openResource));
   openResource.hDevice = device->hDevice;
   openResource.hGlobalShare = openAllocation->hGlobalHandle;
   openResource.NumAllocations = openAllocation->NumAllocations;
   openResource.pOpenAllocationInfo = openAllocation->pOpenAllocation;
   openResource.pResourcePrivateDriverData = openAllocation->pPrivateDriverData;
   openResource.ResourcePrivateDriverDataSize =
      openAllocation->PrivateDriverDataSize;
   openResource.pTotalPrivateDriverDataBuffer = openAllocation->pTotalBuffer;
   openResource.TotalPrivateDriverDataBufferSize =
      openAllocation->TotalBufferSize;
   openResource.PrivateRuntimeDataSize = openAllocation->PrivateRuntimeSize;
   openResource.pPrivateRuntimeData = privateRuntimeData;

   NTSTATUS Status = device->cb.openResource(&openResource);

   free(privateRuntimeData);

   return Status;
}

NTSTATUS
gdikmt_d3dkmt_present(struct gdikmt_context *_ctx, D3DKMT_HANDLE hSrcAllocation,
                      void *winsys_drawable_handle, struct pipe_box *sub_box)
{
   /* STUB */
   return 0;
};

NTSTATUS
gdikmt_d3dkmt_setdisplaymode(struct gdikmt_device *ctx,
                             D3DKMT_HANDLE hSrcAllocation)
{
   /* STUB */
   return 0;
};

void
gdikmt_d3dkmt_destroy(struct gdikmt_device *_device)
{
   struct gdikmt_device_d3dkmt *device = gdikmt_device_d3dkmt(_device);

   D3DKMT_DESTROYDEVICE destroyDevice;
   destroyDevice.hDevice = device->hDevice;
   device->cb.destroyDevice(&destroyDevice);

   D3DKMT_CLOSEADAPTER closeAdapter;
   closeAdapter.hAdapter = device->hAdapter;
   device->cb.closeAdapter(&closeAdapter);

   FreeLibrary(device->gdi32lib);

   FREE(device);
}

struct gdikmt_device *
gdikmt_create_from_hdc(HDC hDC)
{
   struct gdikmt_device_d3dkmt *device = CALLOC_STRUCT(gdikmt_device_d3dkmt);
   if (!device)
      return NULL;

   device->gdi32lib = LoadLibraryA("GDI32.dll");
   gdikmt_load_callbacks(device->gdi32lib, &device->cb);

   D3DKMT_OPENADAPTERFROMHDC openFromHdc;
   NTSTATUS Status;
   openFromHdc.hDc = hDC;
   Status = device->cb.openAdapterFromHdc(&openFromHdc);
   if (!NT_SUCCESS(Status)) {
      _debug_printf(
         "Failed to open device(D3DKMTOpenAdapterFromHdc) with status code: %lx\n",
         Status);
      return NULL;
   }
   device->hAdapter = openFromHdc.hAdapter;

   D3DKMT_CREATEDEVICE createDevice;
   memset(&createDevice, 0, sizeof(createDevice));
   createDevice.hAdapter = device->hAdapter;

   Status = device->cb.createDevice(&createDevice);

   if (!NT_SUCCESS(Status)) {
      printf("Failed to create D3DKMTCreateDevice with status code: %lx\n",
             Status);
      FREE(device);
      return NULL;
   }
   device->hDevice = createDevice.hDevice;

   device->base.destroy = gdikmt_d3dkmt_destroy;
   device->base.queryAdapterInfo = gdikmt_d3dkmt_queryadapterinfo;
   device->base.escape = gdikmt_d3dkmt_escape;

   device->base.createContext = gdikmt_d3dkmt_createcontext;

   device->base.createAllocation = gdikmt_d3dkmt_createallocation;
   device->base.destroyAllocation = gdikmt_d3dkmt_destroyallocation;
   device->base.lockAllocation = gdikmt_d3dkmt_lockallocation;
   device->base.queryAllocation = gdikmt_d3dddi_queryallocation;
   device->base.openAllocation = gdikmt_d3dddi_openallocation;

   device->base.present = gdikmt_d3dkmt_present;
   device->base.setDisplayMode = gdikmt_d3dkmt_setdisplaymode;

   return &device->base;
};