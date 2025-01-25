#include "compiler/libcl/libcl.h"
#include "genxml/gen_macros.h"

uint32_t GENX(panlib_dummy_pack)(private struct mali_blend_equation_packed *dst)
{
    pan_pack(dst, BLEND_EQUATION, cfg) {
    };

    return dst->opaque[0];
}

void GENX(panlib_dummy_unpack)(private struct mali_blend_equation_packed *src)
{
    pan_unpack(src, BLEND_EQUATION, unpacked);
}

static void panlib_dummy_write32(global uint32_t *dst, uint32_t value)
{
    *dst = value;
}

#if PAN_ARCH >= 6
KERNEL(1) panlib_dummy(global uint32_t *dst)
{
    panlib_dummy_write32(dst, 42);
}
#endif
