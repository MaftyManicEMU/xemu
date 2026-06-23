/*
 * iOS Universal.js JIT bridge helpers.
 *
 * This is intentionally tiny: StikDebug's Universal.js script expects arm64
 * apps to request JIT-region preparation through brk #0xf00d with x16 = 1.
 */
#ifndef TCG_IOS_JIT_H
#define TCG_IOS_JIT_H

#include "qemu/osdep.h"

void xemu_ios_universal_jit_set_enabled(bool enabled);
bool xemu_ios_universal_jit_is_enabled(void);
void xemu_ios_universal_jit_prepare_region(void *addr, size_t size);

#endif /* TCG_IOS_JIT_H */
