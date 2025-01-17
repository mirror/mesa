/*
 * Copyright © 2021 Google
 * Copyright © 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */
#include "lvp_acceleration_structure.h"

static float3
unpack_lvp_vec3(lvp_vec3 v)
{
   return (float3)(v.x, v.y, v.z);
}

static float3
bvh_triangle_coord(global struct lvp_bvh_triangle_node *node, uint index)
{
   return (float3)(node->coords[index][0], node->coords[index][1],
                   node->coords[index][2]);
}

float3
lvp_load_vertex_position(global struct lvp_bvh_instance_node *node,
                         uint32_t primitive_id, uint32_t index)
{
   global struct lvp_bvh_header *bvh = (global void *)node->bvh_ptr;
   global struct lvp_bvh_triangle_node *leaves =
      (global void *)(node->bvh_ptr + bvh->leaf_nodes_offset);

   return bvh_triangle_coord(leaves + primitive_id, index);
}

uint2
lvp_build_intersect_ray_box(global struct lvp_bvh_box_node *node,
                            float ray_tmax, float3 origin, float3 dir,
                            float3 inv_dir)
{
   float2 distances = INFINITY;
   uint2 child_indices = 0xffffffffu;

   inv_dir = (dir == 0) ? FLT_MAX : inv_dir;

   for (int i = 0; i < 2; i++) {
      float3 bound_min = unpack_lvp_vec3(node->bounds[i].min);
      float3 bound_max = unpack_lvp_vec3(node->bounds[i].max);

      float3 bound0 = (bound_min - origin) * inv_dir;
      float3 bound1 = (bound_max - origin) * inv_dir;

      float3 bmin = min(bound0, bound1);
      float3 bmax = max(bound0, bound1);

      float tmin = MAX3(bmin.x, bmin.y, bmin.z);
      float tmax = MIN3(bmax.x, bmax.y, bmax.z);

      /* If x of the aabb min is NaN, then this is an inactive aabb. We don't
       * need to care about any other components being NaN as that is UB.
       * https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#acceleration-structure-inactive-prims
       */
      if (!isnan(bound_min.x) && tmax >= max(0.0f, tmin) && tmin < ray_tmax) {
         child_indices[i] = node->children[i];
         distances[i] = tmin;
      }
   }

   return (distances.y < distances.x) ? child_indices.yx : child_indices.xy;
}

/*
 * Based on watertight Ray/Triangle intersection from
 * http://jcgt.org/published/0002/01/05/paper.pdf
 */
float4
lvp_build_intersect_ray_tri(global struct lvp_bvh_triangle_node *node,
                            float ray_tmax, float3 origin, float3 dir,
                            float3 inv_dir)
{
   /* Calculate vertices relative to ray origin */
   float3 v_a = bvh_triangle_coord(node, 0) - origin;
   float3 v_b = bvh_triangle_coord(node, 1) - origin;
   float3 v_c = bvh_triangle_coord(node, 2) - origin;

   /* Find the dimension where the ray direction is largest and put as kz */
   bool xy = fabs(dir.x) >= fabs(dir.y);
   bool xz = fabs(dir.x) >= fabs(dir.z);
   bool yz = fabs(dir.y) >= fabs(dir.z);

   uint3 k = xy ? (xz ? (uint3)(1, 2, 0) : (uint3)(0, 1, 2))
                : (yz ? (uint3)(2, 0, 1) : (uint3)(0, 1, 2));

   /* Swap k.x and k.y dimensions to preserve winding order */
   if (dir[k.z] < 0.0f) {
      k.xy = k.yx;
   }

   /* Calculate shear constants */
   float3 s = (1.0f / dir[k.z]) * (float3)(dir[k.x], dir[k.y], 1.0f);

   /* Swap dimensions so we can ignore this later */
   v_a = (float3)(v_a[k.x], v_a[k.y], v_a[k.z]);
   v_b = (float3)(v_b[k.x], v_b[k.y], v_b[k.z]);
   v_c = (float3)(v_c[k.x], v_c[k.y], v_c[k.z]);

   /* Perform shear and scale */
   double3 a = convert_double3(v_a - (s * v_a.z));
   double3 b = convert_double3(v_b - (s * v_b.z));
   double3 c = convert_double3(v_c - (s * v_c.z));

   double u = (c.x * b.y) - (c.y * b.x);
   double v = (a.x * c.y) - (a.y * c.x);
   double w = (b.x * a.y) - (b.y * a.x);

   /* Perform edge tests. */
   if (MIN3(u, v, w) >= 0.0f || MAX3(u, v, w) <= 0.0f) {
      double det = u + v + w;
      double3 vz = (double)s.z * (double3)(v_a.z, v_b.z, v_c.z);
      double t = dot((double3)(u, v, w), vz);

      if (sign(det) * t >= 0.0) {
         float fdet = det;
         return (float4)(t / det, det, (float)v / fdet, (float)w / fdet);
      }
   }

   return (float4)(INFINITY, 1.0f, 0.0f, 0.0f);
}
