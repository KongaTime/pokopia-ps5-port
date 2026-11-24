// SPDX-License-Identifier: GPL-3.0-or-later
// Ordinary zstd frames (zbic.h zstd_decompress), e.g. the shader cache's spirv.pack. The same zstd 1.5.7 decoder as zbic.cpp
// (third_party/zstd_zbic), compiled a second time here WITHOUT ZSTD_ZBIC_SUPPORT: built with it, the decoder reads ZBIC frames only
// and rejects plain ones. This copy sits in its own namespace (its system headers included first, so their guards keep them out of
// it), so its symbols cannot clash with zbic.cpp's.
#include "zbic.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#include <immintrin.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#define ZSTD_ZBIC_SUPPORT 0
#define ZSTDLIB_VISIBLE static
#define ZSTDLIB_HIDDEN static
#define ZSTDERRORLIB_VISIBLE static
#define ZSTDERRORLIB_HIDDEN static
#undef ZSTD_MULTITHREAD
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
namespace nxrt_zstd_plain {
#include "../../third_party/zstd_zbic/zstddeclib.c"
}  // namespace nxrt_zstd_plain
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace nxrt {

bool zstd_decompress(void* dst, size_t dst_size, const void* src, size_t src_size) {
    if (!dst || !src || src_size < 4 || zbic_is_frame(src, src_size)) return false;
    const size_t r = nxrt_zstd_plain::ZSTD_decompress(dst, dst_size, src, src_size);
    return !nxrt_zstd_plain::ZSTD_isError(r) && r == dst_size;
}

}  // namespace nxrt
