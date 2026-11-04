// SPDX-License-Identifier: GPL-3.0-or-later
// BCryptGenRandom with the system RNG, the one call the runtime makes (services/misc.cpp: csrng), for the PS5 build.
#pragma once
#include <windows.h>

typedef PVOID BCRYPT_ALG_HANDLE;
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x2u
#define BCRYPT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE algorithm, uint8_t *buffer, ULONG size, ULONG flags);
