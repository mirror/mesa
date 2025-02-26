#include "gdikmt/gdikmt.h"
#include <d3d10umddi.h>

struct gdikmt_device_d3dddi {  
   gdikmt_device base;

   HANDLE hRTAdapter;
   HANDLE hRTDevice;
   
   D3DDDI_DEVICECALLBACKS KTCallbacks;
   D3DDDI_ADAPTERCALLBACKS *pAdapterCallbacks;
   DXGI_DDI_BASE_CALLBACKS *pDXGIBaseCallbacks;

   UINT allocationVidPn;
   boolean isPrimary;
   HANDLE hRTResource;

   const D3D10DDIARG_OPENRESOURCE *pOpenResource;
};


struct gdikmt_context_d3dddi {
    struct gdikmt_context base;

    HANDLE hContext;
};

void gdikmt_d3dddi_fill_basefuncs(struct gdikmt_device_d3dddi *device);