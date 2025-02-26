/*
 * Copyright 2025 Mesa3D authors
 * SPDX-License-Identifier: MIT
 */

#ifndef MAGMA_DEFINES_H
#define MAGMA_DEFINES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAGMA_MAX_MEMORY_HEAPS 8
#define MAGMA_MAX_MEMORY_TYPES 16

struct magma_pci_info {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subvendor_id;
    uint16_t subdevice_id;
    uint16_t revision_id;
};

struct magma_memory_heap {
    uint64_t heap_size;
    uint64_t heap_flags;
};

struct magma_memory_type {
    uint32_t property_flags;
    uint32_t heap_idx;
};

struct magma_memory_properties {
    uint32_t memory_type_count;
    uint32_t memory_heap_count;
    struct magma_memory_type memory_types[MAGMA_MAX_MEMORY_TYPES];
    struct magma_memory_heaps memory_heaps[MAGMA_MAX_MEMORY_HEAPS];
};

struct magma_heap_budget {
    uint64_t budget;
    uint64_t usage;
};

uint32_t MAGMA_BUFFER_FLAG_AMD_OA = 0x000000001;
uint32_t MAGMA_BUFFER_FLAG_AMD_GDS = 0x000000002;
struct magma_create_buffer_info {
    uint32_t memory_type_idx;
    uint32_t alignment;
    uint32_t common_flags;
    uint32_t vendor_flags;
    uint32_t size;
};

uint16_t MAGMA_VENDOR_ID_INTEL = 0x8086;
uint16_t MAGMA_VENDOR_ID_AMD = 0x1002;
uint16_t MAGMA_VENDOR_ID_MALI = 0x13B5;
uint16_t MAGMA_VENDOR_ID_QCOM = 0x5413;

#endif
