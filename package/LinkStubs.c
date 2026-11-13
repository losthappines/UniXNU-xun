// SPDX-License-Identifier: BSD-2-Clause-Patent
// Weak fallback definitions of probe hooks referenced when optional display/storage/pmgr subsystems are deferred.
#include <stdbool.h>
#include <stdint.h>

void native_legacy_display_probe(void);
__attribute__((weak)) void native_legacy_display_probe(void)
{
}

void sheng_pmgr_run(void);
__attribute__((weak)) void sheng_pmgr_run(void)
{
}

bool ml_static_mfree_prepare(uintptr_t start, uintptr_t size);
__attribute__((weak)) bool ml_static_mfree_prepare(uintptr_t start, uintptr_t size)
{
  (void)start;
  (void)size;
  return false;
}
