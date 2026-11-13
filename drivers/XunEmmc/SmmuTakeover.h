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
};

static inline uint32_t smmuRead32(uintptr_t base, uint32_t off) {
  return *reinterpret_cast<volatile uint32_t *>(base + off);
}

static inline void smmuWrite32(uintptr_t base, uint32_t off, uint32_t val) {
  *reinterpret_cast<volatile uint32_t *>(base + off) = val;
  __asm__ volatile("dsb sy" ::: "memory");
}

// Optionally configures APPS_SMMU stream 0x00C0 (sdhci@4744000) for bypass.
// Note: Default PIO transfer mode does not touch SMMU at all; this helper is
// used only when native-emmc-dma=1 explicitly requests SDHCI ADMA2 DMA.
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
  res.idr0 = smmuRead32(base, 0x20);
  res.idr1 = smmuRead32(base, 0x24);
  res.numSmrg = res.idr0 & 0xFFU;
  if (res.numSmrg > 128U) res.numSmrg = 128U;

  // sCR0 bit 0 (CLIENTPD): if SMMU global client port disable is set, DMA already bypasses.
  const uint32_t scr0 = smmuRead32(base, 0x0);
  if (scr0 & 1U) {
    res.bypassed = true;
    map->release();
    return res;
  }

  // Search existing SMR entries matching streamId, or claim an unused SMR slot.
  int32_t freeSlot = -1;
  for (uint32_t i = 0; i < res.numSmrg; ++i) {
    const uint32_t smr = smmuRead32(base, 0x800U + i * 4U);
    if (smr & (1U << 31)) {
      const uint16_t id = static_cast<uint16_t>(smr & 0x7FFFU);
      const uint16_t mask = static_cast<uint16_t>((smr >> 16) & 0x7FFFU);
      if ((streamId & ~mask) == (id & ~mask)) {
        res.matchedSlot = static_cast<int32_t>(i);
        break;
      }
    } else if (freeSlot < 0) {
      freeSlot = static_cast<int32_t>(i);
    }
  }

  const int32_t slot = (res.matchedSlot >= 0) ? res.matchedSlot : freeSlot;
  if (slot >= 0) {
    // Program SMR[slot] = VALID | ID = streamId (mask = 0), S2CR[slot] = TYPE_BYPASS (1 << 16).
    if (res.matchedSlot < 0) {
      smmuWrite32(base, 0x800U + static_cast<uint32_t>(slot) * 4U,
                  (1U << 31) | static_cast<uint32_t>(streamId));
    }
    const uint32_t s2cr = (1U << 16);  // TYPE = 01b (Bypass)
    smmuWrite32(base, 0xC00U + static_cast<uint32_t>(slot) * 4U, s2cr);
    // Invalidate all NS TLBs so stale faults/translations for the SID clear.
    smmuWrite32(base, 0x70U, 0U);
    smmuWrite32(base, 0x74U, 0U);
    for (uint32_t wait = 0; wait < 10000U; ++wait) {
      if ((smmuRead32(base, 0x78U) & 1U) == 0U) break;
    }
    res.matchedSlot = slot;
    res.bypassed = true;
  }

  map->release();
  return res;
}

}  // namespace xun_emmc
