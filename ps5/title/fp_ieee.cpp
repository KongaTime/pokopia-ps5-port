// The floating-point environment a title starts in (from KongaTime's switch2-port ps5/m1/fp_ieee.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5platform/fp.h declares ps5_fp_ieee(), and the title CRT calls it as the first
// thing _start does: the console starts a title with flush-to-zero (MXCSR bit 15) and
// denormals-are-zero (bit 6) enabled, while code built here expects the IEEE state.
// Clearing those two bits is the whole job.
//
// The SDK declares it but no stub library in the payload SDK exports it (checked with
// nm -D across target/lib/*.so), so the definition has to come from us. It is weak
// deliberately: if a platform implementation is ever linked in, that one wins.
extern "C" __attribute__((weak)) void ps5_fp_ieee(void)
{
    unsigned int control;
    __asm__ volatile("stmxcsr %0" : "=m"(control));
    control &= ~0x8040u;
    __asm__ volatile("ldmxcsr %0" : : "m"(control));
}
