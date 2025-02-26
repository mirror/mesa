/* Copyright © 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#ifndef GENX_RT_CL_PACK_H
#define GENX_RT_CL_PACK_H

#ifndef GFX_VERx10
#  error "The GFX_VERx10 macro must be defined"
#endif

#if (GFX_VERx10 == 40)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 45)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 50)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 60)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 70)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 75)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 80)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 90)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 110)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 120)
/* No RT support for this gfx ver */
#elif (GFX_VERx10 == 125)
#  include "genxml/gen125_rt_cl_pack.h"
#elif (GFX_VERx10 == 200)
#  include "genxml/gen200_rt_cl_pack.h"
#elif (GFX_VERx10 == 300)
#  include "genxml/gen300_rt_cl_pack.h"
#else
#  error "Need to add a pack header include for this gen"
#endif

#endif /* GENX_RT_CL_PACK_H */
