/*
 * Copyright 2024 Autodesk, Inc.
 * Copyright (c) 2024 The Foundry Visionmongers Ltd.
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from:
 * https://gitlab.freedesktop.org/mesa/demos/-/blob/0754adcba952b8fb7d6d3cddbace50637078d011/src/vulkan/wsi/metal.m
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdio.h>

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
   NSWindow *_window;
@public
   CAMetalLayer *metal_layer;
}
@end

@interface WsiWindow : NSWindow <NSWindowDelegate> {}
@end

static AppDelegate *app_delegate = NULL;

@implementation AppDelegate
- (void)init:(const char *)title
       withWidth:(NSInteger)width
      withHeight:(NSInteger)height
    isFullscreen:(BOOL)fullscreen
{
   NSRect frame = NSMakeRect(0, 0, width, height);

   _window = [[WsiWindow alloc]
      initWithContentRect:frame
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
   _window.title = [NSString stringWithUTF8String:title];
   _window.delegate = app_delegate;

   NSView *view = _window.contentView;
   view.wantsLayer = YES;
   metal_layer = [CAMetalLayer layer];
   view.layer = metal_layer;

   [_window center];
   [_window orderFrontRegardless];

   /* run will block the thread, so we'll stop immediately in
      applicationDidFinishLaunching, and implement our own loop
      in update_window(). */
   [NSApp run];
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
  [NSApp stop:nil];
}
@end

@implementation WsiWindow
@end

void* kopperGetMetalLayer(void)
{
   {
     @autoreleasepool {
        if (!app_delegate) {
           [NSApplication sharedApplication];
           app_delegate = [AppDelegate alloc];
           [NSApp setDelegate:app_delegate];
           [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }

        [app_delegate init:"title"
               withWidth:300
               withHeight:300
               isFullscreen:false];
      }
   }

   return (__bridge void*)(app_delegate->metal_layer);
}
