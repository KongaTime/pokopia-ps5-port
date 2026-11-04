// SPDX-License-Identifier: GPL-3.0-or-later
// tools/textures' ASTC -> BC7 transcoder (astc2bc7.h) as the PS5 build has it until P3 (ps5/PLAN.md): the size and format
// helpers answer as the real library does, and initialisation and the transcodes answer A2B_ERR_NO_ASTC_LIB, what the real
// one says on Windows when its astcdec DLLs are missing (the graphics shell then keeps the texture as it is). The Windows
// build links prebuilt .lib files of that library; a console build of it (astcenc + bc7e) comes with the renderer.
#include "astc2bc7.h"

extern "C" {

a2b_status a2b_init(const char*) { return A2B_ERR_NO_ASTC_LIB; }

const char* a2b_status_string(a2b_status s) {
    switch (s) {
        case A2B_OK: return "ok";
        case A2B_ERR_ARGS: return "bad arguments";
        case A2B_ERR_SIZE: return "buffer size does not match";
        case A2B_ERR_UNSUPPORTED: return "unsupported format";
        case A2B_ERR_NO_ASTC_LIB: return "no ASTC decoder in the console build yet";
        case A2B_ERR_DECODE: return "decode error";
    }
    return "unknown";
}

int a2b_is_astc(a2b_format f) { return f == A2B_ASTC_UNORM || f == A2B_ASTC_SRGB; }

int a2b_is_srgb(a2b_format f) {
    return f == A2B_ASTC_SRGB || f == A2B_BC1_SRGB || f == A2B_BC2_SRGB || f == A2B_BC3_SRGB || f == A2B_BC7_SRGB;
}

uint32_t a2b_bcn_block_bytes(a2b_format f) {
    switch (f) {
        case A2B_BC1_UNORM: case A2B_BC1_SRGB: case A2B_BC4_UNORM: case A2B_BC4_SNORM: return 8;
        case A2B_BC2_UNORM: case A2B_BC2_SRGB: case A2B_BC3_UNORM: case A2B_BC3_SRGB: case A2B_BC5_UNORM: case A2B_BC5_SNORM:
        case A2B_BC6H_UF16: case A2B_BC6H_SF16: case A2B_BC7_UNORM: case A2B_BC7_SRGB: return 16;
        default: return 0;
    }
}

// The transcode's output: BC7 (16-byte 4x4 blocks) for ASTC, the same BCn for BCn input.
uint64_t a2b_output_size(const a2b_input* in) {
    if (!in) return 0;
    const uint64_t blocks = uint64_t((in->width + 3) / 4) * ((in->height + 3) / 4);
    const uint32_t b = a2b_is_astc(in->format) ? 16 : a2b_bcn_block_bytes(in->format);
    return blocks * b;
}

uint64_t a2b_astc_size(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
    if (!bw || !bh) return 0;
    return uint64_t((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
}

a2b_status a2b_transcode(const a2b_input*, const void*, size_t, const a2b_options*, a2b_format*, void*, size_t, a2b_stats*) {
    return A2B_ERR_NO_ASTC_LIB;
}

a2b_status a2b_decode_astc_rgba8(const a2b_input*, const void*, size_t, int, uint8_t*, a2b_stats*) { return A2B_ERR_NO_ASTC_LIB; }

a2b_status a2b_encode_bc7(uint32_t, uint32_t, const uint8_t*, const a2b_options*, void*, size_t, a2b_stats*) {
    return A2B_ERR_NO_ASTC_LIB;
}

}  // extern "C"
