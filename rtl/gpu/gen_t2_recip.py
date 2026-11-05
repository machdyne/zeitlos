#!/usr/bin/env python3
# T2's reciprocal table for textures (rtl/gpu/gpu_t2.v, RECIP_HEX): entry
# i is 2^28 / (2049 + 2i), rounded -- 1/(1 + f) for f's first ten bits,
# their middle, in 17 bits. sw/common/zgl/zgl_t2.h's t2_recip() is the
# same, and the model uses that.
#
#   python3 rtl/gpu/gen_t2_recip.py > rtl/gpu/t2_recip.hex
for i in range(1024):
    d = 2049 + 2 * i
    print('%05x' % (((1 << 28) + d // 2) // d))
