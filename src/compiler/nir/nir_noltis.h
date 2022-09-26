/*
 * Copyright © 2019 Broadcom
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef NIR_NOLTIS_H
#define NIR_NOLTIS_H

#include "nir.h"
#include "util/u_dynarray.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nir_noltis_node;

typedef struct nir_noltis {
   nir_shader *s;
   nir_function_impl *impl;

   struct hash_table *ht;

   struct hash_table *matched_tiles;
} nir_noltis;

typedef struct nir_noltis_tile {
   nir_noltis *noltis;

   /* Node (and thus NIR instruction) at the root of the tile */
   struct nir_noltis_node *node;

   /* User-provided cost of the tile for optimization */
   uint32_t cost;

   /* User private data about the tile */
   void *data;

   struct util_dynarray interior_nodes;
   struct util_dynarray edge_nodes;
} nir_noltis_tile;

typedef struct nir_noltis_node {
   nir_instr *instr;

   struct util_dynarray matching_tiles;
   struct util_dynarray covering_tiles;

   /* Set if the node is in fixedNodes. */
   bool fixed;

   /* Set if this node has more than one parent in the DAG.  (AKA !fixed and
    * the SSA def has more than one use).
    */
   bool shared;

   /* Set when we've put the best_choice node into matched_tiles. */
   bool selected;

   uint32_t best_cost;
   nir_noltis_tile *best_choice;
} nir_noltis_node;

nir_noltis *nir_noltis_create(void *mem_ctx, nir_function_impl *impl);

nir_noltis_tile *nir_noltis_tile_create(nir_noltis *noltis,
                                        nir_instr *instr,
                                        void *data);
void nir_noltis_tile_create_noop(nir_noltis *noltis, nir_instr *instr, int cost);

void nir_noltis_tile_add_edge(nir_noltis_tile *tile, nir_instr *instr);
void nir_noltis_tile_add_interior(nir_noltis_tile *tile, nir_instr *instr);

void nir_noltis_select(nir_noltis *noltis);
nir_noltis_tile *nir_noltis_get_tile(nir_noltis *noltis, nir_instr *instr);

/* Functions exported for unit-testing purposes. */
nir_noltis_node *nir_noltis_get_node(nir_noltis *noltis, nir_instr *instr);

void nir_noltis_print_tile(nir_noltis_tile *tile, FILE *out,
                           const char *prefix,
                           void (*print_tile_cb)(nir_noltis_tile *tile,
                                                 FILE *out,
                                                 const char *prefix,
                                                 void *data),
                           void *data);

void nir_noltis_print_selection(nir_noltis *noltis, FILE *out,
                                void (*print_tile_cb)(nir_noltis_tile *tile,
                                                      FILE *out,
                                                      const char *prefix,
                                                      void *data),
                                void *data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NIR_NOLTIS_H */
