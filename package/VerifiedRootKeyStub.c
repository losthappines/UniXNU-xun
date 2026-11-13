// SPDX-License-Identifier: BSD-2-Clause-Patent
// Weak fallback for CONFIG_XNU_VERIFIED_ROOT_PROBE when NativeEmmc already exports native_verified_block_*.
void native_verified_root_probe(void);
__attribute__((weak)) void native_verified_root_probe(void)
{
}
