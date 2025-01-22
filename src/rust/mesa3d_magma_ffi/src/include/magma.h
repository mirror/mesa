/*
 * Copyright 2025 Mesa3D authors
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef MAGMA_H
#define MAGMA_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Versioning
 */
#define MAGMA_VERSION_MAJOR 0
#define MAGMA_VERSION_MINOR 1
#define MAGMA_VERSION_PATCH 3

#define MAGMA_MAX_PHYSICAL_DEVICES 8

struct magma_physical_device;
struct magma_device;
struct magma_buffer;

typedef struct *magma_physical_device magma_physical_device_t;
typedef struct *magma_device magma_device_t;
typedef struct *magma_buffer magma_buffer_t;

/**
 * Enumerates physical devices on the system.
 *
 * # Safety
 * - `physical_devices` must be an array of type magma_physical_device_t with size `MAGMA_MAX_PHYSICAL_DEVICES`.
 */
int32_t
magma_enumerate_physical_devices(magma_physical_device_t physical_devices[MAGMA_MAX_PHYSICAL_DEVICES],
                                 uint32_t *num_devices);
int32_t magma_create_device(const magma_physical_device_t physical_device, magma_device_t *device);
int32_t magma_physical_device_close(magma_physical_device_t *physical_devices);

int32_t magma_device_get_memory_properties(const magma_device_t device,
                                           struct magma_memory_properties *mem_props);
int32_t magma_device_get_memory_budget(const magma_device_t device, const uint32_t heap_idx,
                                       struct magma_heap_budget *budget);
int32_t magma_device_create_buffer(const magma_device_t device,
                                   const struct magma_create_buffer_info *info,
                                   magma_buffer_t *buffer);
int32_t magma_device_create_context(const magma_device_t device,
                                    const struct magma_create_context_info *info,
                                    magma_context_t *context);
int32_t magma_device_close(magma_physical_device_t *physical_devices);

#ifdef __cplusplus
}
#endif

#endif
