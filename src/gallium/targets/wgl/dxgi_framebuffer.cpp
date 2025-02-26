/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* This file provides driver-agnoistic way to display wgl window over dxgi swapchain. */

#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/format/u_formats.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "stw_device.h"
#include "stw_pixelformat.h"
#include "stw_winsys.h"

#include "frontend/winsys_handle.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgiformat.h>
#include <libloaderapi.h>
#include <string.h>
#include <winerror.h>
#include <dxgi1_4.h>

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#define NUM_BUFFERS 2

struct wgl_dxgi_framebuffer {
   struct stw_winsys_framebuffer base;

   struct pipe_screen *screen;
   enum pipe_format pformat;
   HWND window;

   HINSTANCE d3d11;
   ComPtr<ID3D11Device> device;
   ComPtr<ID3D11DeviceContext> context;
   ComPtr<IDXGISwapChain> swapchain;

   D3D11_TEXTURE2D_DESC textureDesc;
   ComPtr<ID3D11Texture2D> d3d11_textures[NUM_BUFFERS];
   struct pipe_resource *textures[NUM_BUFFERS];
};

static struct wgl_dxgi_framebuffer *
wgl_dxgi_framebuffer(struct stw_winsys_framebuffer *fb)
{
   return (struct wgl_dxgi_framebuffer *)fb;
}

static void
wgl_dxgi_framebuffer_destroy(struct stw_winsys_framebuffer *_fb,
                              struct pipe_context *ctx)
{
   struct wgl_dxgi_framebuffer *fb = wgl_dxgi_framebuffer(_fb);
   struct pipe_fence_handle *fence = NULL;

   if (ctx) {
      /* Ensure all resources are flushed */
      ctx->flush(ctx, &fence, PIPE_FLUSH_HINT_FINISH);
      if (fence) {
         ctx->screen->fence_finish(ctx->screen, ctx, fence,
                                   OS_TIMEOUT_INFINITE);
         ctx->screen->fence_reference(ctx->screen, &fence, NULL);
      }
   }

   for (int i = 0; i < NUM_BUFFERS; ++i) {
      if (fb->textures[i]) {
         pipe_resource_reference(&fb->textures[i], NULL);
      }
      if (fb->d3d11_textures[i]) {
         fb->d3d11_textures[i].Reset();
      }
   }

   fb->context.Reset();
   fb->swapchain.Reset();
   fb->device.Reset();

   FreeLibrary(fb->d3d11);

   free(fb);
}

static const GUID IID_D3D11Texture2D = {
   0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

DXGI_FORMAT
DxgiFormatFromPipe(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      return DXGI_FORMAT_R8G8B8A8_UNORM;

   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      return DXGI_FORMAT_B8G8R8A8_UNORM;

   default:
      _debug_printf("Unsupported dxgi framebuffer format %d\n", format);
      return DXGI_FORMAT_R8G8B8A8_UNORM;
   }
}

static void
wgl_dxgi_framebuffer_resize(struct stw_winsys_framebuffer *_fb,
                             struct pipe_context *ctx,
                             struct pipe_resource *templ)
{
   struct wgl_dxgi_framebuffer *fb = wgl_dxgi_framebuffer(_fb);
   // struct dxgi_dxgi_screen *screen = dxgi_dxgi_screen(framebuffer->screen);

   DXGI_SWAP_CHAIN_DESC desc;
   ZeroMemory(&desc, sizeof desc);
   desc.BufferCount = NUM_BUFFERS;
   desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
   desc.BufferDesc.Format = DxgiFormatFromPipe(templ->format);
   desc.BufferDesc.Width = templ->width0;
   desc.BufferDesc.Height = templ->height0;
   desc.SampleDesc.Count = 1;
   desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
   desc.Windowed = true;
   desc.OutputWindow = fb->window;

   static const D3D_FEATURE_LEVEL FeatureLevels[] = {D3D_FEATURE_LEVEL_10_0};

   fb->pformat = templ->format;

   if (!fb->swapchain) {
      PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN createDeviceAndSwapchain =
         (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(
            fb->d3d11, "D3D11CreateDeviceAndSwapChain");

      HRESULT hr = createDeviceAndSwapchain(
         NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_DEBUG,
         FeatureLevels, _countof(FeatureLevels), D3D11_SDK_VERSION, &desc,
         &fb->swapchain, &fb->device, NULL, &fb->context);

      if (FAILED(hr)) {
         _debug_printf("Failed to create framebuffer dxgi device: %x\n", hr);
         return;
      }
   } else {
      if (FAILED(fb->swapchain->ResizeBuffers(
             2, desc.BufferDesc.Width, desc.BufferDesc.Height,
             desc.BufferDesc.Format, desc.Flags))) {
         debug_printf("Failed to resize dxgi swapchain\n");
      }
   }

   for (int i = 0; i < NUM_BUFFERS; ++i) {
      pipe_resource_reference(&fb->textures[i], NULL);
      if (fb->d3d11_textures[i]) {
         fb->d3d11_textures[i].Reset();
      }
   }

   ComPtr<ID3D11Texture2D> swapchainBuffer;
   fb->swapchain->GetBuffer(0, IID_D3D11Texture2D, &swapchainBuffer);
   swapchainBuffer->GetDesc(&fb->textureDesc);
   fb->textureDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED;
}

static bool
wgl_dxgi_framebuffer_present(struct stw_winsys_framebuffer *_fb, int interval,
                              struct pipe_resource *res)
{
   struct wgl_dxgi_framebuffer *fb = wgl_dxgi_framebuffer(_fb);
   if (!fb->swapchain) {
      debug_printf("Cannot present; no swapchain\n");
      return false;
   }

   ComPtr<ID3D11Texture2D> buffer;
   fb->swapchain->GetBuffer(0, IID_D3D11Texture2D, &buffer);

   // Find matching d3d11 texture
   for (UINT i = 0; i < NUM_BUFFERS; i++) {
      if (fb->textures[i] != res)
         continue;

      fb->context->CopyResource(buffer.Get(), fb->d3d11_textures[i].Get());
   }

   return S_OK == fb->swapchain->Present(
                     0, interval < 1 ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

static struct pipe_resource *
wgl_dxgi_framebuffer_get_resource(struct stw_winsys_framebuffer *pframebuffer,
                                   enum st_attachment_type statt)
{
   struct wgl_dxgi_framebuffer *fb = wgl_dxgi_framebuffer(pframebuffer);

   if (!fb->swapchain)
      return NULL;

   UINT index = statt;
   if (fb->textures[index]) {
      pipe_reference(NULL, &fb->textures[index]->reference);
      return fb->textures[index];
   }

   HRESULT hr = fb->device->CreateTexture2D(&fb->textureDesc, 0,
                                            &fb->d3d11_textures[index]);
   if (FAILED(hr)) {
      _debug_printf("Failed to create d3d11 rendering resource\n");
      return NULL;
   }

   ComPtr<IDXGIResource> idxgiRes;
   fb->d3d11_textures[index].As(&idxgiRes);
   HANDLE handle;
   hr = idxgiRes->GetSharedHandle(&handle);
   if (FAILED(hr) || handle == 0) {
      _debug_printf("Failed to acquire d3d11 handle resource\n");
      return NULL;
   }

   struct winsys_handle whandle;
   memset(&whandle, 0, sizeof(whandle));
   whandle.type = WINSYS_HANDLE_TYPE_WIN32_HANDLE;
   whandle.handle = handle;

   struct pipe_resource *texture =
      fb->screen->resource_from_handle(fb->screen, nullptr, &whandle, 0);
   fb->textures[index] = texture;
   return texture;
}

extern "C" {
struct stw_winsys_framebuffer *
wgl_create_dxgi_framebuffer(struct pipe_screen *screen, HWND hWnd,
                             int iPixelFormat)
{
   const struct stw_pixelformat_info *pfi =
      stw_pixelformat_get_info(iPixelFormat);
   if (pfi->pfd.dwFlags & PFD_SUPPORT_GDI)
      return NULL;

   if (pfi->stvis.color_format != PIPE_FORMAT_B8G8R8A8_UNORM &&
       pfi->stvis.color_format != PIPE_FORMAT_R8G8B8A8_UNORM &&
       pfi->stvis.color_format != PIPE_FORMAT_B8G8R8X8_UNORM &&
       pfi->stvis.color_format != PIPE_FORMAT_R8G8B8X8_UNORM &&
       pfi->stvis.color_format != PIPE_FORMAT_R10G10B10A2_UNORM &&
       pfi->stvis.color_format != PIPE_FORMAT_R16G16B16A16_FLOAT)
      return NULL;

   struct wgl_dxgi_framebuffer *fb = CALLOC_STRUCT(wgl_dxgi_framebuffer);
   if (!fb)
      return NULL;

   fb->d3d11 = LoadLibraryA("d3d11.dll");

   fb->window = hWnd;
   fb->screen = screen;
   fb->base.destroy = wgl_dxgi_framebuffer_destroy;
   fb->base.resize = wgl_dxgi_framebuffer_resize;
   fb->base.present = wgl_dxgi_framebuffer_present;
   fb->base.get_resource = wgl_dxgi_framebuffer_get_resource;

   return &fb->base;
}
}