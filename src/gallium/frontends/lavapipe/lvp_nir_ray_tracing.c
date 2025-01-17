/*
 * Copyright © 2021 Google
 * Copyright © 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "lvp_nir_ray_tracing.h"
#include "lvp_acceleration_structure.h"
#include "lvp_private.h"

#include "compiler/spirv/spirv.h"

#include <float.h>
#include <math.h>

nir_def *
lvp_mul_vec3_mat(nir_builder *b, nir_def *vec, nir_def *matrix[], bool translation)
{
   nir_def *result_components[3] = {
      nir_channel(b, matrix[0], 3),
      nir_channel(b, matrix[1], 3),
      nir_channel(b, matrix[2], 3),
   };
   for (unsigned i = 0; i < 3; ++i) {
      for (unsigned j = 0; j < 3; ++j) {
         nir_def *v =
            nir_fmul(b, nir_channels(b, vec, 1 << j), nir_channels(b, matrix[i], 1 << j));
         result_components[i] = (translation || j) ? nir_fadd(b, result_components[i], v) : v;
      }
   }
   return nir_vec(b, result_components, 3);
}

void
lvp_load_wto_matrix(nir_builder *b, nir_def *instance_addr, nir_def **out)
{
   unsigned offset = offsetof(struct lvp_bvh_instance_node, wto_matrix);
   for (unsigned i = 0; i < 3; ++i) {
      out[i] = nir_build_load_global(b, 4, 32, nir_iadd_imm(b, instance_addr, offset + i * 16));
   }
}

static nir_def *
lvp_build_hit_is_opaque(nir_builder *b, nir_def *sbt_offset_and_flags,
                        const struct lvp_ray_flags *ray_flags, nir_def *geometry_id_and_flags)
{
   nir_def *opaque = nir_uge_imm(b, nir_ior(b, geometry_id_and_flags, sbt_offset_and_flags),
                                     LVP_INSTANCE_FORCE_OPAQUE | LVP_INSTANCE_NO_FORCE_NOT_OPAQUE);
   opaque = nir_bcsel(b, ray_flags->force_opaque, nir_imm_true(b), opaque);
   opaque = nir_bcsel(b, ray_flags->force_not_opaque, nir_imm_false(b), opaque);
   return opaque;
}

static void
lvp_build_triangle_case(nir_builder *b, const struct lvp_ray_traversal_args *args,
                        const struct lvp_ray_flags *ray_flags, nir_def *result,
                        nir_def *node_addr)
{
   if (!args->triangle_cb)
      return;

   struct lvp_triangle_intersection intersection;
   intersection.t = nir_channel(b, result, 0);
   intersection.barycentrics = nir_channels(b, result, 0xc);

   nir_push_if(b, nir_flt(b, intersection.t, nir_load_deref(b, args->vars.tmax)));
   {
      intersection.frontface = nir_fgt_imm(b, nir_channel(b, result, 1), 0);
      nir_def *switch_ccw = nir_test_mask(b, nir_load_deref(b, args->vars.sbt_offset_and_flags),
                                              LVP_INSTANCE_TRIANGLE_FLIP_FACING);
      intersection.frontface = nir_ixor(b, intersection.frontface, switch_ccw);

      nir_def *not_cull = ray_flags->no_skip_triangles;
      nir_def *not_facing_cull =
         nir_bcsel(b, intersection.frontface, ray_flags->no_cull_front, ray_flags->no_cull_back);

      not_cull =
         nir_iand(b, not_cull,
                  nir_ior(b, not_facing_cull,
                          nir_test_mask(b, nir_load_deref(b, args->vars.sbt_offset_and_flags),
                                        LVP_INSTANCE_TRIANGLE_FACING_CULL_DISABLE)));

      nir_push_if(b, nir_iand(b, nir_flt(b, args->tmin, intersection.t), not_cull));
      {
         intersection.base.node_addr = node_addr;
         nir_def *triangle_info = nir_build_load_global(
            b, 2, 32,
            nir_iadd_imm(b, intersection.base.node_addr,
                         offsetof(struct lvp_bvh_triangle_node, primitive_id)));
         intersection.base.primitive_id = nir_channel(b, triangle_info, 0);
         intersection.base.geometry_id_and_flags = nir_channel(b, triangle_info, 1);
         intersection.base.opaque =
            lvp_build_hit_is_opaque(b, nir_load_deref(b, args->vars.sbt_offset_and_flags), ray_flags,
                                    intersection.base.geometry_id_and_flags);

         not_cull = nir_bcsel(b, intersection.base.opaque, ray_flags->no_cull_opaque,
                              ray_flags->no_cull_no_opaque);
         nir_push_if(b, not_cull);
         {
            args->triangle_cb(b, &intersection, args, ray_flags);
         }
         nir_pop_if(b, NULL);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);
}

static void
lvp_build_aabb_case(nir_builder *b, const struct lvp_ray_traversal_args *args,
                           const struct lvp_ray_flags *ray_flags, nir_def *node_addr)
{
   if (!args->aabb_cb)
      return;

   struct lvp_leaf_intersection intersection;
   intersection.node_addr = node_addr;
   nir_def *triangle_info = nir_build_load_global(
      b, 2, 32,
      nir_iadd_imm(b, intersection.node_addr, offsetof(struct lvp_bvh_aabb_node, primitive_id)));
   intersection.primitive_id = nir_channel(b, triangle_info, 0);
   intersection.geometry_id_and_flags = nir_channel(b, triangle_info, 1);
   intersection.opaque = lvp_build_hit_is_opaque(b, nir_load_deref(b, args->vars.sbt_offset_and_flags),
                                                 ray_flags, intersection.geometry_id_and_flags);

   nir_def *not_cull =
      nir_bcsel(b, intersection.opaque, ray_flags->no_cull_opaque, ray_flags->no_cull_no_opaque);
   not_cull = nir_iand(b, not_cull, ray_flags->no_skip_aabbs);
   nir_push_if(b, not_cull);
   {
      args->aabb_cb(b, &intersection, args, ray_flags);
   }
   nir_pop_if(b, NULL);
}

static void
lvp_build_push_stack(nir_builder *b, const struct lvp_ray_traversal_args *args, nir_def *node)
{
   nir_def *stack_ptr = nir_load_deref(b, args->vars.stack_ptr);
   nir_store_deref(b, nir_build_deref_array(b, args->vars.stack, stack_ptr), node, 0x1);
   nir_store_deref(b, args->vars.stack_ptr, nir_iadd_imm(b, nir_load_deref(b, args->vars.stack_ptr), 1), 0x1);
}

static nir_def *
lvp_build_pop_stack(nir_builder *b, const struct lvp_ray_traversal_args *args)
{
   nir_def *stack_ptr = nir_iadd_imm(b, nir_load_deref(b, args->vars.stack_ptr), -1);
   nir_store_deref(b, args->vars.stack_ptr, stack_ptr, 0x1);
   return nir_load_deref(b, nir_build_deref_array(b, args->vars.stack, stack_ptr));
}

nir_def *
lvp_build_ray_traversal(nir_builder *b, const struct lvp_ray_traversal_args *args)
{
   nir_variable *incomplete = nir_local_variable_create(b->impl, glsl_bool_type(), "incomplete");
   nir_store_var(b, incomplete, nir_imm_true(b), 0x1);

   nir_def *vec3ones = nir_imm_vec3(b, 1.0, 1.0, 1.0);

   struct lvp_ray_flags ray_flags = {
      .force_opaque = nir_test_mask(b, args->flags, SpvRayFlagsOpaqueKHRMask),
      .force_not_opaque = nir_test_mask(b, args->flags, SpvRayFlagsNoOpaqueKHRMask),
      .terminate_on_first_hit =
         nir_test_mask(b, args->flags, SpvRayFlagsTerminateOnFirstHitKHRMask),
      .no_cull_front = nir_ieq_imm(
         b, nir_iand_imm(b, args->flags, SpvRayFlagsCullFrontFacingTrianglesKHRMask), 0),
      .no_cull_back =
         nir_ieq_imm(b, nir_iand_imm(b, args->flags, SpvRayFlagsCullBackFacingTrianglesKHRMask), 0),
      .no_cull_opaque =
         nir_ieq_imm(b, nir_iand_imm(b, args->flags, SpvRayFlagsCullOpaqueKHRMask), 0),
      .no_cull_no_opaque =
         nir_ieq_imm(b, nir_iand_imm(b, args->flags, SpvRayFlagsCullNoOpaqueKHRMask), 0),
      .no_skip_triangles =
         nir_ieq_imm(b, nir_iand_imm(b, args->flags, SpvRayFlagsSkipTrianglesKHRMask), 0),
      .no_skip_aabbs = nir_ieq_imm(b, nir_iand_imm(b, args->flags, SpvRayFlagsSkipAABBsKHRMask), 0),
   };

   nir_push_loop(b);
   {
      nir_push_if(b, nir_ieq_imm(b, nir_load_deref(b, args->vars.current_node), LVP_BVH_INVALID_NODE));
      {
         nir_push_if(b, nir_ieq_imm(b, nir_load_deref(b, args->vars.stack_ptr), 0));
         {
            nir_store_var(b, incomplete, nir_imm_false(b), 0x1);
            nir_jump(b, nir_jump_break);
         }
         nir_pop_if(b, NULL);

         nir_push_if(b, nir_ige(b, nir_load_deref(b, args->vars.stack_base), nir_load_deref(b, args->vars.stack_ptr)));
         {
            nir_store_deref(b, args->vars.stack_base, nir_imm_int(b, -1), 1);

            nir_store_deref(b, args->vars.bvh_base, args->root_bvh_base, 1);
            nir_store_deref(b, args->vars.origin, args->origin, 7);
            nir_store_deref(b, args->vars.dir, args->dir, 7);
            nir_store_deref(b, args->vars.inv_dir, nir_fdiv(b, vec3ones, args->dir), 7);
         }
         nir_pop_if(b, NULL);

         nir_store_deref(b, args->vars.current_node, lvp_build_pop_stack(b, args), 0x1);
      }
      nir_pop_if(b, NULL);

      nir_def *bvh_node = nir_load_deref(b, args->vars.current_node);
      nir_store_deref(b, args->vars.current_node, nir_imm_int(b, LVP_BVH_INVALID_NODE), 0x1);

      nir_def *node_addr = nir_iadd(b, nir_load_deref(b, args->vars.bvh_base), nir_u2u64(b, nir_iand_imm(b, bvh_node, ~3u)));

      nir_def *node_type = nir_iand_imm(b, bvh_node, 3);
      nir_push_if(b, nir_uge_imm(b, node_type, lvp_bvh_node_internal));
      {
         nir_push_if(b, nir_uge_imm(b, node_type, lvp_bvh_node_instance));
         {
            nir_push_if(b, nir_ieq_imm(b, node_type, lvp_bvh_node_aabb));
            {
               lvp_build_aabb_case(b, args, &ray_flags, node_addr);
            }
            nir_push_else(b, NULL);
            {
               /* instance */
               nir_store_deref(b, args->vars.instance_addr, node_addr, 1);

               nir_def *instance_data = nir_build_load_global(
                  b, 4, 32,
                  nir_iadd_imm(b, node_addr, offsetof(struct lvp_bvh_instance_node, bvh_ptr)));

               nir_def *wto_matrix[3];
               lvp_load_wto_matrix(b, node_addr, wto_matrix);

               nir_store_deref(b, args->vars.sbt_offset_and_flags, nir_channel(b, instance_data, 3),
                               1);

               nir_def *instance_and_mask = nir_channel(b, instance_data, 2);
               nir_push_if(b, nir_ult(b, nir_iand(b, instance_and_mask, args->cull_mask),
                                      nir_imm_int(b, 1 << 24)));
               {
                  nir_jump(b, nir_jump_continue);
               }
               nir_pop_if(b, NULL);

               nir_store_deref(b, args->vars.bvh_base,
                               nir_pack_64_2x32(b, nir_trim_vector(b, instance_data, 2)), 1);

               nir_store_deref(b, args->vars.stack_base, nir_load_deref(b, args->vars.stack_ptr), 0x1);

               /* Push the instance root node onto the stack */
               nir_store_deref(b, args->vars.current_node, nir_imm_int(b, LVP_BVH_ROOT_NODE), 0x1);

               /* Transform the ray into object space */
               nir_store_deref(b, args->vars.origin,
                               lvp_mul_vec3_mat(b, args->origin, wto_matrix, true), 7);
               nir_store_deref(b, args->vars.dir,
                               lvp_mul_vec3_mat(b, args->dir, wto_matrix, false), 7);
               nir_store_deref(b, args->vars.inv_dir,
                               nir_fdiv(b, vec3ones, nir_load_deref(b, args->vars.dir)), 7);
            }
            nir_pop_if(b, NULL);
         }
         nir_push_else(b, NULL);
         {
            nir_def *result = lvp_build_intersect_ray_box(
               b, node_addr, nir_load_deref(b, args->vars.tmax),
               nir_load_deref(b, args->vars.origin), nir_load_deref(b, args->vars.dir),
               nir_load_deref(b, args->vars.inv_dir));

            nir_store_deref(b, args->vars.current_node, nir_channel(b, result, 0), 0x1);

            nir_push_if(b, nir_ine_imm(b, nir_channel(b, result, 1), LVP_BVH_INVALID_NODE));
            {
               lvp_build_push_stack(b, args, nir_channel(b, result, 1));
            }
            nir_pop_if(b, NULL);
         }
         nir_pop_if(b, NULL);
      }
      nir_push_else(b, NULL);
      {
         nir_def *result = lvp_build_intersect_ray_tri(
            b, node_addr, nir_load_deref(b, args->vars.tmax), nir_load_deref(b, args->vars.origin),
            nir_load_deref(b, args->vars.dir), nir_load_deref(b, args->vars.inv_dir));

         lvp_build_triangle_case(b, args, &ray_flags, result, node_addr);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_loop(b, NULL);

   return nir_load_var(b, incomplete);
}
