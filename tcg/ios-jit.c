/*
 * iOS Universal.js JIT bridge helpers.
 *
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "tcg/ios-jit.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

static bool universal_jit_enabled;

#if defined(__APPLE__) && defined(__aarch64__) && TARGET_OS_IPHONE && \
    !TARGET_OS_SIMULATOR
#define XEMU_IOS_UNIVERSAL_JIT_BRK 1
#else
#define XEMU_IOS_UNIVERSAL_JIT_BRK 0
#endif

#if XEMU_IOS_UNIVERSAL_JIT_BRK
__attribute__((noinline, optnone, naked, used))
static void *jit26_prepare_region(void *addr, size_t len)
{
    __asm__ volatile(
        "mov x16, #1\n"
        "brk #0xf00d\n"
        "ret\n"
    );
}
#endif

void xemu_ios_universal_jit_set_enabled(bool enabled)
{
    universal_jit_enabled = enabled;
}

bool xemu_ios_universal_jit_is_enabled(void)
{
    const char *env = getenv("XEMU_IOS_UNIVERSAL_JIT");

    return universal_jit_enabled ||
           (env != NULL && g_ascii_strcasecmp(env, "0") != 0 &&
            g_ascii_strcasecmp(env, "false") != 0 &&
            g_ascii_strcasecmp(env, "no") != 0);
}

void xemu_ios_universal_jit_prepare_region(void *addr, size_t size)
{
#if XEMU_IOS_UNIVERSAL_JIT_BRK
    void *prepared_addr;

    if (!xemu_ios_universal_jit_is_enabled() || addr == NULL || size == 0) {
        return;
    }

    prepared_addr = jit26_prepare_region(addr, size);
    if (prepared_addr != addr) {
        warn_report("Universal.js prepared unexpected JIT address %p "
                    "(requested %p, size %zu)",
                    prepared_addr, addr, size);
    }
#else
    (void)addr;
    (void)size;
#endif
}
