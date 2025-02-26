// Helper to iteract with windows kernel-mode drivers
// Required because of difference between D3DKMT**** functions and callbacks used in umd

#ifndef gdikmt_h
#define gdikmt_h

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include "pipe/p_state.h"

typedef unsigned char boolean;

struct gdikmt_render {
   UINT CommandLength;
   UINT CommandOffset;
   UINT AllocationCount;
   UINT PatchLocationCount;
   
   UINT NewCommandBufferSize;
   UINT NewAllocationListSize;
   UINT NewPatchLocationListSize;
   
   boolean ResizeCommandBuffer;
   boolean ResizeAllocationList;
   boolean ResizePatchLocationList;

   HANDLE CompletionEvent;
};

struct gdikmt_createallocation {
  VOID *pPrivateDriverData;
  UINT PrivateDriverDataSize;
  HANDLE hResource;
  UINT NumAllocations;
  D3DDDI_ALLOCATIONINFO  *pAllocationInfo;
};

struct gdikmt_openallocation {
   D3DKMT_HANDLE hGlobalHandle;
   UINT NumAllocations;
   
   VOID *pPrivateDriverData;
   UINT PrivateDriverDataSize;
   
   VOID *pTotalBuffer;
   UINT TotalBufferSize;

   D3DDDI_OPENALLOCATIONINFO *pOpenAllocation;

   UINT PrivateRuntimeSize;
};


struct gdikmt_context {
   struct gdikmt_device *device;
   
   void *pCommandBuffer;
   D3DDDI_ALLOCATIONLIST *pAllocationList;
   D3DDDI_PATCHLOCATIONLIST *pPatchLocationList;

   UINT CommandBufferSize;
   UINT AllocationListSize;
   UINT PatchLocationListSize;

   void (*destroy)(struct gdikmt_context* device);
   NTSTATUS (*render)(struct gdikmt_context* device, struct gdikmt_render *options);
};

struct gdikmt_device {
   void (*destroy)(struct gdikmt_device* device);

   NTSTATUS (*queryAdapterInfo)(
    struct gdikmt_device*   device,
    KMTQUERYADAPTERINFOTYPE Type,
    VOID*                   pPrivateDriverData,
    UINT                    PrivateDriverDataSize
   );

   NTSTATUS (*escape)(
    struct gdikmt_device*   device,
    VOID*                   pPrivateDriverData,
    UINT                    PrivateDriverDataSize
   );

   NTSTATUS (*createContext)(struct gdikmt_device* device, struct gdikmt_context** out_ctx);

   NTSTATUS (*createAllocation)(struct gdikmt_device* device, struct gdikmt_createallocation *options);
   NTSTATUS (*destroyAllocation)(struct gdikmt_device* device, HANDLE hResource, D3DKMT_HANDLE hAllocation);
   NTSTATUS (*lockAllocation)(struct gdikmt_device* device, D3DKMT_HANDLE hAllocation, D3DDDICB_LOCKFLAGS flags, void **out_ptr);
   NTSTATUS (*queryAllocation)(struct gdikmt_device* device, struct gdikmt_openallocation* options);
   NTSTATUS (*openAllocation)(struct gdikmt_device* device, struct gdikmt_openallocation* options);

   NTSTATUS (*present)(struct gdikmt_context* ctx, D3DKMT_HANDLE hSrcAllocation, void *winsys_drawable_handle, struct pipe_box *sub_box);
   NTSTATUS (*setDisplayMode)(struct gdikmt_device* device, D3DKMT_HANDLE hSrcAllocation);
};


struct gdikmt_device *gdikmt_create_from_hdc(HDC hDC);

#endif