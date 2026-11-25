// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/IOMemoryDescriptor.h>
#include <stdint.h>

namespace xun_emmc {

constexpr uint64_t kAppsSmmuBase = 0x0C600000ULL;
constexpr uint64_t kAppsSmmuSize = 0x00080000ULL;
constexpr uint16_t kSdhci1StreamId = 0x00C0U;

struct SmmuResult {
  bool probed = false;
  bool bypassed = false;
  uint32_t idr0 = 0;
  uint32_t idr1 = 0;
  uint32_t numSmrg = 0;
  int32_t matchedSlot = -1;
  int32_t chosenCb = -1;
  uint32_t s2cr = 0;
  uint32_t fsr = 0;
};

static inline uint32_t smmuRead32(uintptr_t base, uint32_t off) {
  return *reinterpret_cast<volatile uint32_t *>(base + off);
}

static inline void smmuWrite32(uintptr_t base, uint32_t off, uint32_t val) {
  *reinterpret_cast<volatile uint32_t *>(base + off) = val;
  __asm__ volatile("dsb sy" ::: "memory");
}

// Configures APPS_SMMU stream 0x00C0 (sdhci@4744000) for 1:1 passthrough.
// SM6225 apps-smmu@c600000 faults/ignores S2CR TYPE_BYPASS (1 << 16); instead we
// route the stream to a Stage-1-disabled context bank (CBAR TYPE=1, CBA2R VA64=1,
// CB_SCTLR.M=0), matching the verified pattern in package/TouchProbe.cpp.
inline SmmuResult configureSdhciSmmuBypass(uint16_t streamId = kSdhci1StreamId) {
  SmmuResult res = {};
  IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
      kAppsSmmuBase, kAppsSmmuSize, kIODirectionInOut);
  if (!desc) return res;
  IOMemoryMap *map = desc->map(kIOMapAnywhere | kIOMapInhibitCache);
  desc->release();
  if (!map) return res;

  const uintptr_t base = static_cast<uintptr_t>(map->getVirtualAddress());
  res.probed = true;
  res.idr0 = smmuRead32(base, 0x20U);
  res.idr1 = smmuRead32(base, 0x24U);

  // sCR0 bit 0 (CLIENTPD): if SMMU global client port disable is set, DMA already bypasses.
  const uint32_t scr0 = smmuRead32(base, 0x0U);
  if (scr0 & 1U) {
    res.bypassed = true;
    map->release();
    return res;
  }

  res.numSmrg = res.idr0 & 0xFFU;
  if (res.numSmrg > 50U) res.numSmrg = 50U;  // vendor_boot qcom,num-smr-override = 0x32
  const uint32_t pagesize = (res.idr1 & (1U << 31)) ? 0x10000U : 0x1000U;
  const uint32_t numPages = 1U << (((res.idr1 >> 28) & 7U) + 1U);
  const uint32_t cbBase = numPages * pagesize;
  uint32_t numCb = res.idr1 & 0xFFU;
  if (numCb > 48U) numCb = 48U;  // vendor_boot qcom,num-context-banks-override = 0x30
  if (res.numSmrg == 0U || numCb == 0U || cbBase + numCb * pagesize > kAppsSmmuSize) {
    map->release();
    return res;
  }

  // Scan existing SMRs to build CB usage mask and check if streamId already routes.
  uint64_t cbUsedMask = 0ULL;
  int32_t matched = -1, freeSlot = -1;
  for (uint32_t i = 0; i < res.numSmrg; ++i) {
    const uint32_t smr = smmuRead32(base, 0x800U + i * 4U);
    if (smr & (1U << 31)) {
      const uint32_t id = smr & 0x7FFFU;
      const uint32_t mask = (smr >> 16) & 0x7FFFU;
      const uint32_t s2cr = smmuRead32(base, 0xC00U + i * 4U);
      const uint32_t type = (s2cr >> 16) & 3U;
      const uint32_t cb = s2cr & 0xFFU;
      if (type == 0U && cb < numCb) cbUsedMask |= (1ULL << cb);

      if (id == streamId && mask == 0U) {
        if (matched < 0) matched = static_cast<int32_t>(i);
      } else if ((streamId & ~mask) == (id & ~mask)) {
        const bool cbBypass =
            (type == 0U && cb < numCb && (smmuRead32(base, cbBase + cb * pagesize) & 1U) == 0U);
        res.matchedSlot = static_cast<int32_t>(i);
        res.s2cr = s2cr;
        res.chosenCb = (type == 0U) ? static_cast<int32_t>(cb) : -1;
        if (type == 1U || cbBypass) res.bypassed = true;
        map->release();
        return res;
      }
    } else if (freeSlot < 0) {
      freeSlot = static_cast<int32_t>(i);
    }
  }

  const int32_t slot = (matched >= 0) ? matched : freeSlot;
  if (slot < 0) {
    map->release();
    return res;
  }
  res.matchedSlot = slot;

  if (matched >= 0) {
    const uint32_t curS2cr = smmuRead32(base, 0xC00U + static_cast<uint32_t>(slot) * 4U);
    const uint32_t type = (curS2cr >> 16) & 3U;
    const uint32_t cb = curS2cr & 0xFFU;
    const bool cbBypass =
        (type == 0U && cb < numCb && (smmuRead32(base, cbBase + cb * pagesize) & 1U) == 0U);
    if (type == 1U || cbBypass) {
      res.s2cr = curS2cr;
      res.chosenCb = (type == 0U) ? static_cast<int32_t>(cb) : -1;
      res.bypassed = true;
      map->release();
      return res;
    }
  }

  // Pick a free context bank with Stage-1 currently disabled.
  int32_t bank = -1;
  for (uint32_t b = 0; b < numCb; ++b) {
    if (cbUsedMask & (1ULL << b)) continue;
    if ((smmuRead32(base, cbBase + b * pagesize) & 1U) != 0U) continue;
    bank = static_cast<int32_t>(b);
    break;
  }
  if (bank < 0) {
    map->release();
    return res;
  }
  res.chosenCb = bank;

  const uint32_t cbOff = cbBase + static_cast<uint32_t>(bank) * pagesize;
  const uint32_t cbarOff = pagesize + static_cast<uint32_t>(bank) * 4U;
  const uint32_t cba2rOff = pagesize + 0x800U + static_cast<uint32_t>(bank) * 4U;

  smmuWrite32(base, cbOff + 0x0U, 0U);  // CB_SCTLR.M = 0 (Stage-1 disabled -> 1:1)
  if ((smmuRead32(base, cbOff + 0x0U) & 1U) != 0U) {
    map->release();
    return res;
  }
  smmuWrite32(base, cba2rOff, 1U);  // VA64 = 1
  if ((smmuRead32(base, cba2rOff) & 1U) != 1U) {
    map->release();
    return res;
  }
  smmuWrite32(base, cbarOff, 0x10000U);  // TYPE = 01b (Stage 1 with Stage 2 bypass)
  if ((smmuRead32(base, cbarOff) & 0x30000U) != 0x10000U) {
    map->release();
    return res;
  }
  __asm__ volatile("dsb sy" ::: "memory");

  // Program S2CR (TYPE = 0 translation -> bank) then valid SMR.
  const uint32_t s2crOff = 0xC00U + static_cast<uint32_t>(slot) * 4U;
  const uint32_t smrOff = 0x800U + static_cast<uint32_t>(slot) * 4U;
  smmuWrite32(base, s2crOff, static_cast<uint32_t>(bank));
  res.s2cr = smmuRead32(base, s2crOff);
  if ((res.s2cr & 0x300FFU) != static_cast<uint32_t>(bank)) {
    map->release();
    return res;
  }
  smmuWrite32(base, smrOff, (1U << 31) | static_cast<uint32_t>(streamId));
  if ((smmuRead32(base, smrOff) & 0x80007FFFU) != ((1U << 31) | static_cast<uint32_t>(streamId))) {
    map->release();
    return res;
  }
  __asm__ volatile("dsb sy" ::: "memory");

  res.bypassed = true;
  map->release();
  return res;
}

}  // namespace xun_emmc
