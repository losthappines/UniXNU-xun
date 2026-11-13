// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <stdint.h>
#include <string.h>

#include "SmmuTakeover.h"

namespace xun_emmc {

constexpr uint64_t kSdhci1CqhciBase = 0x04745000ULL;
constexpr uint64_t kSdhci1CqhciSize = 0x00001000ULL;
constexpr uint64_t kGccSdcc1Base    = 0x00138000ULL;
constexpr uint64_t kGccSdcc1Size    = 0x00001000ULL;

constexpr uint32_t kEmmcSectorBytes = 512U;
constexpr uint32_t kMaxChunkSectors = 128U;  // 64 KiB per transfer

// Standard SDHCI registers (sdhci@4744000)
enum SdhciReg : uint32_t {
  kSdmaAddr          = 0x00,
  kBlockSize         = 0x04,
  kBlockCount        = 0x06,
  kArgument          = 0x08,
  kTransferMode      = 0x0C,
  kCommand           = 0x0E,
  kResponse0         = 0x10,
  kResponse1         = 0x14,
  kResponse2         = 0x18,
  kResponse3         = 0x1C,
  kBufferDataPort    = 0x20,
  kPresentState      = 0x24,
  kHostControl       = 0x28,
  kPowerControl      = 0x29,
  kClockControl      = 0x2C,
  kTimeoutControl    = 0x2E,
  kSoftwareReset     = 0x2F,
  kIntStatus         = 0x30,
  kErrIntStatus      = 0x32,
  kIntEnable         = 0x34,
  kErrIntEnable      = 0x36,
  kSignalEnable      = 0x38,
  kErrSignalEnable   = 0x3A,
  kHostControl2      = 0x3E,
  kCapabilities0     = 0x40,
  kCapabilities1     = 0x44,
  kAdmaErrStatus     = 0x54,
  kAdmaAddrLo        = 0x58,
  kAdmaAddrHi        = 0x5C,
  kHostVersion       = 0xFE,
};

// Qualcomm SDHCI v5 vendor registers (msm_offset for sdhci-msm-v5)
enum SdhciMsmV5Reg : uint32_t {
  kCoreDllConfig     = 0x200,
  kCoreDllStatus     = 0x208,
  kCoreVendorSpec    = 0x20C,
  kCorePwrctlStatus  = 0x240,
  kCorePwrctlMask    = 0x244,
  kCorePwrctlClear   = 0x248,
  kCorePwrctlCtl     = 0x24C,
  kCoreVendorSpec3   = 0x250,
};

// Present state bits
constexpr uint32_t kStateCmdInhibit      = 1U << 0;
constexpr uint32_t kStateDatInhibit      = 1U << 1;
constexpr uint32_t kStateBufWriteEnable  = 1U << 10;
constexpr uint32_t kStateBufReadEnable   = 1U << 11;

// Transfer mode bits
constexpr uint16_t kXferDmaEn            = 1U << 0;
constexpr uint16_t kXferBlkCntEn         = 1U << 1;
constexpr uint16_t kXferAutoCmd12        = 1U << 2;
constexpr uint16_t kXferDirRead          = 1U << 4;
constexpr uint16_t kXferMultiBlk         = 1U << 5;

// Command register flags
constexpr uint16_t kCmdRespNone          = 0x00;
constexpr uint16_t kCmdResp136           = 0x01;
constexpr uint16_t kCmdResp48            = 0x02;
constexpr uint16_t kCmdResp48Busy        = 0x03;
constexpr uint16_t kCmdCrcCheck          = 1U << 3;
constexpr uint16_t kCmdIndexCheck        = 1U << 4;
constexpr uint16_t kCmdDataPresent       = 1U << 5;

// Normal / error interrupt bits
constexpr uint16_t kIntCmdComplete       = 1U << 0;
constexpr uint16_t kIntXferComplete      = 1U << 1;
constexpr uint16_t kIntDma               = 1U << 3;
constexpr uint16_t kIntBufWriteReady     = 1U << 4;
constexpr uint16_t kIntBufReadReady      = 1U << 5;
constexpr uint16_t kIntError             = 1U << 15;

// Host control 1/2
constexpr uint8_t  kHost4Bit             = 1U << 1;
constexpr uint8_t  kHostHighSpeed        = 1U << 2;
constexpr uint8_t  kHostDmaMask          = 0x18U;
constexpr uint8_t  kHostDmaAdma32        = 0x10U;
constexpr uint8_t  kHostDmaAdma64        = 0x18U;
constexpr uint8_t  kHost8Bit             = 1U << 5;

constexpr uint16_t kHost2Vdd180          = 1U << 3;
constexpr uint16_t kHost2Ver4En          = 1U << 12;
constexpr uint16_t kHost2Addr64Bit       = 1U << 13;

// Clock control
constexpr uint16_t kClkIntEn             = 1U << 0;
constexpr uint16_t kClkIntStable         = 1U << 1;
constexpr uint16_t kClkSdEn              = 1U << 2;

struct __attribute__((packed)) Adma2Desc32 {
  uint16_t attr;
  uint16_t len;
  uint32_t addr;
};

struct __attribute__((packed)) Adma2Desc64 {
  uint16_t attr;
  uint16_t len;
  uint64_t addr;
  uint32_t reserved;
};

constexpr uint16_t kAdmaValid = 1U << 0;
constexpr uint16_t kAdmaEnd   = 1U << 1;
constexpr uint16_t kAdmaTran  = 2U << 4;

inline void cleanDataCacheRange(const void *ptr, size_t len) {
  const uintptr_t line = 64;
  uintptr_t cur = reinterpret_cast<uintptr_t>(ptr) & ~(line - 1);
  const uintptr_t end = reinterpret_cast<uintptr_t>(ptr) + len;
  while (cur < end) {
    __asm__ volatile("dc cvac, %0" :: "r"(cur) : "memory");
    cur += line;
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

inline void invalidateDataCacheRange(void *ptr, size_t len) {
  const uintptr_t line = 64;
  uintptr_t cur = reinterpret_cast<uintptr_t>(ptr) & ~(line - 1);
  const uintptr_t end = reinterpret_cast<uintptr_t>(ptr) + len;
  while (cur < end) {
    __asm__ volatile("dc civac, %0" :: "r"(cur) : "memory");
    cur += line;
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

struct SdhciController {
  IOMemoryMap *hcMap = nullptr;
  IOMemoryMap *cqhciMap = nullptr;
  IOMemoryMap *gccMap = nullptr;
  IOBufferMemoryDescriptor *dmaDesc = nullptr;
  IOBufferMemoryDescriptor *admaDesc = nullptr;

  uintptr_t base = 0;
  uintptr_t cqhciBase = 0;
  uintptr_t gccBase = 0;

  uint8_t *dmaBuf = nullptr;
  uint64_t dmaPhys = 0;
  uint8_t *admaBuf = nullptr;
  uint64_t admaPhys = 0;

  bool initialized = false;
  bool useDma = false;
  bool useAdma64 = false;
  bool highCapacity = true;
  bool fallbackReinitUsed = false;

  uint16_t rca = 1;
  uint32_t sectorCount = 0;
  uint32_t lastErrInt = 0;
  uint32_t lastIntStatus = 0;
  uint32_t lastCmd = 0;
  uint32_t stage = 0;
  uint32_t caps0 = 0;
  uint32_t caps1 = 0;
  uint32_t vendorSpec = 0;
  uint16_t hostVersion = 0;
  uint8_t hostCtrl = 0;
  uint16_t clockCtrl = 0;
  uint64_t readOps = 0;
  uint64_t writeOps = 0;
  uint64_t readErrors = 0;

  uint8_t r8(uint32_t off) const {
    return *reinterpret_cast<volatile uint8_t *>(base + off);
  }
  uint16_t r16(uint32_t off) const {
    return *reinterpret_cast<volatile uint16_t *>(base + off);
  }
  uint32_t r32(uint32_t off) const {
    return *reinterpret_cast<volatile uint32_t *>(base + off);
  }
  void w8(uint32_t off, uint8_t v) {
    *reinterpret_cast<volatile uint8_t *>(base + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
  }
  void w16(uint32_t off, uint16_t v) {
    *reinterpret_cast<volatile uint16_t *>(base + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
  }
  void w32(uint32_t off, uint32_t v) {
    *reinterpret_cast<volatile uint32_t *>(base + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
  }

  void release() {
    if (dmaDesc) { dmaDesc->release(); dmaDesc = nullptr; dmaBuf = nullptr; }
    if (admaDesc) { admaDesc->release(); admaDesc = nullptr; admaBuf = nullptr; }
    if (gccMap) { gccMap->release(); gccMap = nullptr; gccBase = 0; }
    if (cqhciMap) { cqhciMap->release(); cqhciMap = nullptr; cqhciBase = 0; }
    if (hcMap) { hcMap->release(); hcMap = nullptr; base = 0; }
    initialized = false;
  }

  bool waitStateClear(uint32_t mask, uint32_t timeoutUs) const {
    for (uint32_t t = 0; t < timeoutUs; ++t) {
      if ((r32(kPresentState) & mask) == 0U) return true;
      IODelay(1);
    }
    return false;
  }

  void resetLines(uint8_t mask) {
    w8(kSoftwareReset, mask);
    for (uint32_t t = 0; t < 100000U; ++t) {
      if ((r8(kSoftwareReset) & mask) == 0U) break;
      IODelay(1);
    }
  }

  void ensureGccClocks() {
    IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
        kGccSdcc1Base, kGccSdcc1Size, kIODirectionInOut);
    if (!desc) return;
    gccMap = desc->map(kIOMapAnywhere | kIOMapInhibitCache);
    desc->release();
    if (!gccMap) return;
    gccBase = static_cast<uintptr_t>(gccMap->getVirtualAddress());

    auto enableCbcr = [&](uint32_t off) {
      volatile uint32_t *reg = reinterpret_cast<volatile uint32_t *>(gccBase + off);
      uint32_t v = *reg;
      if ((v & 1U) == 0U) {
        *reg = v | 1U;
        __asm__ volatile("dsb sy" ::: "memory");
      }
      for (uint32_t t = 0; t < 50000U; ++t) {
        if ((*reg & (1U << 31)) == 0U) break;
        IODelay(1);
      }
    };
    // gcc-khaje.c: GCC_SDCC1_AHB_CBCR = +0x08, GCC_SDCC1_APPS_CBCR = +0x04
    enableCbcr(0x08U);
    enableCbcr(0x04U);
  }

  void ackPendingPwrctl() {
    const uint32_t status = r32(kCorePwrctlStatus);
    if (!status) return;
    w32(kCorePwrctlClear, status);
    uint32_t ctl = 0;
    if (status & ((1U << 0) | (1U << 1))) ctl |= (1U << 0);  // BUS_SUCCESS
    if (status & ((1U << 2) | (1U << 3))) ctl |= (1U << 2);  // IO_SUCCESS
    if (ctl) w32(kCorePwrctlCtl, ctl);
    w32(kCorePwrctlMask, 0U);
  }

  void quiesceCqhci() {
    IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
        kSdhci1CqhciBase, kSdhci1CqhciSize, kIODirectionInOut);
    if (!desc) return;
    cqhciMap = desc->map(kIOMapAnywhere | kIOMapInhibitCache);
    desc->release();
    if (!cqhciMap) return;
    cqhciBase = static_cast<uintptr_t>(cqhciMap->getVirtualAddress());

    volatile uint32_t *cqCfg = reinterpret_cast<volatile uint32_t *>(cqhciBase + 0x08U);
    volatile uint32_t *cqCtl = reinterpret_cast<volatile uint32_t *>(cqhciBase + 0x0CU);
    if (*cqCfg & 1U) {
      *cqCtl = *cqCtl | 1U;  // HALT
      __asm__ volatile("dsb sy" ::: "memory");
      for (uint32_t t = 0; t < 100000U; ++t) {
        if (*cqCtl & 1U) break;
        IODelay(1);
      }
      *cqCfg = *cqCfg & ~1U;
      __asm__ volatile("dsb sy" ::: "memory");
      w16(kIntStatus, kIntCmdComplete);
    }
  }

  void configureInterruptMasks() {
    w16(kIntStatus, 0xFFFFU);
    w16(kErrIntStatus, 0xFFFFU);
    // Keep INT_SIGNAL_ENABLE = 0 (polled driver); enable all normal/error statuses.
    w16(kSignalEnable, 0U);
    w16(kErrSignalEnable, 0U);
    w16(kIntEnable, 0x00FFU);
    w16(kErrIntEnable, 0x03FFU);
  }

  bool sendCommand(uint16_t cmdIdx, uint32_t arg, uint16_t cmdFlags, uint32_t *resp = nullptr) {
    lastCmd = cmdIdx;
    if (!waitStateClear(kStateCmdInhibit | ((cmdFlags & kCmdDataPresent) ? kStateDatInhibit : 0U), 200000U)) {
      resetLines(0x06U);
      return false;
    }
    w16(kIntStatus, 0xFFFFU);
    w16(kErrIntStatus, 0xFFFFU);
    w32(kArgument, arg);
    w16(kCommand, static_cast<uint16_t>((static_cast<uint32_t>(cmdIdx) << 8) | (cmdFlags & 0xFFU)));

    uint16_t st = 0;
    for (uint32_t t = 0; t < 500000U; ++t) {
      st = r16(kIntStatus);
      if (st & kIntError) {
        lastIntStatus = st;
        lastErrInt = r16(kErrIntStatus);
        w16(kErrIntStatus, 0xFFFFU);
        w16(kIntStatus, 0xFFFFU);
        resetLines(0x06U);
        return false;
      }
      if (st & kIntCmdComplete) {
        w16(kIntStatus, kIntCmdComplete);
        break;
      }
      IODelay(1);
    }
    if ((st & kIntCmdComplete) == 0U) {
      lastIntStatus = st;
      resetLines(0x02U);
      return false;
    }
    if ((cmdFlags & 3U) == kCmdResp48Busy) {
      // Wait for optional DAT0 busy deassertion after R1b command
      uint16_t xferSt = 0;
      for (uint32_t t = 0; t < 500000U; ++t) {
        xferSt = r16(kIntStatus);
        if (xferSt & kIntError) {
          lastIntStatus = xferSt;
          lastErrInt = r16(kErrIntStatus);
          resetLines(0x06U);
          return false;
        }
        if ((xferSt & kIntXferComplete) || ((r32(kPresentState) & (1U << 20)) != 0U)) {
          w16(kIntStatus, kIntXferComplete);
          break;
        }
        IODelay(1);
      }
    }
    if (resp) {
      resp[0] = r32(kResponse0);
      if ((cmdFlags & 3U) == kCmdResp136) {
        resp[1] = r32(kResponse1);
        resp[2] = r32(kResponse2);
        resp[3] = r32(kResponse3);
      }
    }
    return true;
  }

  bool readExtCsd(uint8_t out[512]) {
    if (!waitStateClear(kStateCmdInhibit | kStateDatInhibit, 200000U)) {
      resetLines(0x06U);
      return false;
    }
    w16(kIntStatus, 0xFFFFU);
    w16(kErrIntStatus, 0xFFFFU);
    w16(kBlockSize, 512U);
    w16(kBlockCount, 1U);
    w32(kArgument, 0U);
    w16(kTransferMode, kXferDirRead | kXferBlkCntEn);
    w16(kCommand, static_cast<uint16_t>((8U << 8) | kCmdDataPresent | kCmdCrcCheck | kCmdIndexCheck | kCmdResp48));

    bool gotCmd = false;
    bool gotBuf = false;
    for (uint32_t t = 0; t < 500000U; ++t) {
      const uint16_t st = r16(kIntStatus);
      if (st & kIntError) {
        lastIntStatus = st;
        lastErrInt = r16(kErrIntStatus);
        resetLines(0x06U);
        return false;
      }
      if (!gotCmd && (st & kIntCmdComplete)) {
        w16(kIntStatus, kIntCmdComplete);
        gotCmd = true;
      }
      if ((st & kIntBufReadReady) || (r32(kPresentState) & kStateBufReadEnable)) {
        w16(kIntStatus, kIntBufReadReady);
        for (uint32_t w = 0; w < 128U; ++w) {
          const uint32_t word = r32(kBufferDataPort);
          memcpy(out + w * 4U, &word, sizeof(word));
        }
        gotBuf = true;
        break;
      }
      IODelay(1);
    }
    if (!gotBuf) {
      resetLines(0x06U);
      return false;
    }
    for (uint32_t t = 0; t < 200000U; ++t) {
      const uint16_t st = r16(kIntStatus);
      if (st & kIntError) {
        resetLines(0x06U);
        return false;
      }
      if (st & kIntXferComplete) {
        w16(kIntStatus, kIntXferComplete);
        return true;
      }
      IODelay(1);
    }
    resetLines(0x06U);
    return false;
  }

  bool setSdClock(uint16_t divider) {
    w16(kClockControl, 0U);
    IODelay(10);
    const uint16_t divVal = static_cast<uint16_t>(((divider & 0xFFU) << 8) | ((divider >> 8 & 3U) << 6) | kClkIntEn);
    w16(kClockControl, divVal);
    for (uint32_t t = 0; t < 100000U; ++t) {
      if (r16(kClockControl) & kClkIntStable) {
        w16(kClockControl, static_cast<uint16_t>(divVal | kClkSdEn));
        IODelay(50);
        return true;
      }
      IODelay(1);
    }
    return false;
  }

  bool sendStatus(uint16_t candidateRca, uint32_t *r1Out) {
    uint32_t r = 0;
    if (!sendCommand(13U, static_cast<uint32_t>(candidateRca) << 16,
                     kCmdCrcCheck | kCmdIndexCheck | kCmdResp48, &r)) {
      return false;
    }
    if (r1Out) *r1Out = r;
    return true;
  }

  bool switchExtCsd(uint8_t index, uint8_t value) {
    const uint32_t arg = (3U << 24) | (static_cast<uint32_t>(index) << 16) |
                         (static_cast<uint32_t>(value) << 8);
    if (!sendCommand(6U, arg, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy)) return false;
    uint32_t r1 = 0;
    for (uint32_t t = 0; t < 20000U; ++t) {
      if (sendStatus(rca, &r1) && ((r1 >> 9) & 0xFU) == 4U) {
        return (r1 & (1U << 7)) == 0U;  // SWITCH_ERROR clear
      }
      IODelay(50);
    }
    return false;
  }

  bool reinitEmmcFallback() {
    fallbackReinitUsed = true;
    resetLines(0x01U);  // SDHCI_RESET_ALL
    IODelay(2000);
    ackPendingPwrctl();
    w32(kCoreVendorSpec3, r32(kCoreVendorSpec3) & ~(1U << 10));  // Clear CORE_FIFO_ALT_EN
    configureInterruptMasks();

    w8(kTimeoutControl, 0x0EU);
    w8(kHostControl, 0U);  // 1-bit normal speed
    w16(kHostControl2, 0U);
    w8(kPowerControl, 0x0FU);  // 3.3V bus power on
    IODelay(200);
    ackPendingPwrctl();
    IODelay(2000);
    // Initial 400 kHz identification clock (divider 0xFA ~= /500 from 200 MHz, /250 from 100 MHz).
    if (!setSdClock(0x0FAU)) return false;

    // CMD0 GO_IDLE_STATE
    if (!sendCommand(0U, 0U, kCmdRespNone)) return false;
    IODelay(2000);

    // CMD1 SEND_OP_COND loop (sector mode 0x40FF8080)
    uint32_t ocr = 0;
    bool ready = false;
    for (uint32_t tries = 0; tries < 500U; ++tries) {
      if (!sendCommand(1U, 0x40FF8080U, kCmdResp48, &ocr)) return false;
      if (ocr & (1U << 31)) {
        ready = true;
        highCapacity = (ocr & (1U << 30)) != 0U;
        break;
      }
      IOSleep(2);
    }
    if (!ready) return false;

    // CMD2 ALL_SEND_CID
    uint32_t cid[4] = {};
    if (!sendCommand(2U, 0U, kCmdCrcCheck | kCmdResp136, cid)) return false;

    // CMD3 SET_RELATIVE_ADDR (assign RCA = 1)
    rca = 1;
    if (!sendCommand(3U, static_cast<uint32_t>(rca) << 16,
                     kCmdCrcCheck | kCmdIndexCheck | kCmdResp48)) {
      return false;
    }

    // CMD7 SELECT_CARD
    if (!sendCommand(7U, static_cast<uint32_t>(rca) << 16,
                     kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy)) {
      return false;
    }

    // CMD16 SET_BLOCKLEN = 512
    sendCommand(16U, kEmmcSectorBytes, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48);

    // Switch to 8-bit bus width (EXT_CSD[183] = 2)
    if (switchExtCsd(183U, 2U)) {
      w8(kHostControl, (r8(kHostControl) & ~kHost4Bit) | kHost8Bit);
    } else if (switchExtCsd(183U, 1U)) {
      w8(kHostControl, (r8(kHostControl) & ~kHost8Bit) | kHost4Bit);
    }

    // Switch to High-Speed timing (EXT_CSD[185] = 1, ~50 MHz safe without DLL tuning)
    if (switchExtCsd(185U, 1U)) {
      w8(kHostControl, r8(kHostControl) | kHostHighSpeed);
      setSdClock(2U);  // div 4 (~50 MHz from 200 MHz / ~25 MHz from 100 MHz)
    } else {
      setSdClock(4U);
    }
    IODelay(1000);
    return true;
  }

  bool allocateDmaBuffers() {
    if (dmaDesc && admaDesc) return true;
    const uint64_t mask = useAdma64 ? 0xFFFFFFFFFFFFFFFFULL : 0x00000000FFFFFFFFULL;
    const IOOptionBits bufOpts =
        static_cast<IOOptionBits>(kIODirectionInOut) |
        static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    dmaDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, bufOpts,
        kMaxChunkSectors * kEmmcSectorBytes, mask);
    admaDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, bufOpts,
        4096U, mask);
    if (!dmaDesc || !admaDesc ||
        dmaDesc->prepare() != kIOReturnSuccess ||
        admaDesc->prepare() != kIOReturnSuccess) {
      if (dmaDesc) { dmaDesc->release(); dmaDesc = nullptr; }
      if (admaDesc) { admaDesc->release(); admaDesc = nullptr; }
      return false;
    }
    IOByteCount segLen = 0;
    dmaPhys = dmaDesc->getPhysicalSegment(0, &segLen);
    admaPhys = admaDesc->getPhysicalSegment(0, &segLen);
    dmaBuf = static_cast<uint8_t *>(dmaDesc->getBytesNoCopy());
    admaBuf = static_cast<uint8_t *>(admaDesc->getBytesNoCopy());
    if (!dmaPhys || !admaPhys || !dmaBuf || !admaBuf) return false;
    return true;
  }

  bool init(IOService *provider, bool enableDma) {
    highCapacity = true;
    rca = 1;
    stage = 1;
    hcMap = provider->mapDeviceMemoryWithIndex(0, kIOMapAnywhere | kIOMapInhibitCache);
    if (!hcMap || hcMap->getLength() < 0x300U) return false;
    base = static_cast<uintptr_t>(hcMap->getVirtualAddress());

    stage = 2;
    ensureGccClocks();
    quiesceCqhci();
    ackPendingPwrctl();

    // Ensure standard SDHCI FIFO path (CORE_FIFO_ALT_EN = 0).
    w32(kCoreVendorSpec3, r32(kCoreVendorSpec3) & ~(1U << 10));
    w8(kTimeoutControl, 0x0EU);
    configureInterruptMasks();

    hostVersion = r16(kHostVersion);
    caps0 = r32(kCapabilities0);
    caps1 = r32(kCapabilities1);
    vendorSpec = r32(kCoreVendorSpec);
    hostCtrl = r8(kHostControl);
    clockCtrl = r16(kClockControl);

    stage = 3;
    // Fast path: probe whether bootloader/UEFI left eMMC powered & in TRAN state.
    bool cardReady = false;
    uint8_t extCsd[512] = {};
    if ((clockCtrl & kClkSdEn) != 0U) {
      for (uint16_t tryRca = 1; tryRca <= 2; ++tryRca) {
        uint32_t r1 = 0;
        if (sendStatus(tryRca, &r1) && ((r1 >> 9) & 0xFU) == 4U) {
          rca = tryRca;
          if (readExtCsd(extCsd)) {
            cardReady = true;
            break;
          }
        }
      }
    }

    if (!cardReady) {
      stage = 4;
      if (!reinitEmmcFallback()) return false;
      if (!readExtCsd(extCsd)) return false;
    }

    stage = 5;
    memcpy(&sectorCount, &extCsd[212], sizeof(sectorCount));
    if (sectorCount < 0x100000U) {
      // Fallback minimum if SEC_COUNT unexpectedly zero; real eMMC is 128 GB (0x0E900000+).
      return false;
    }

    // Read current host control & optional DMA setup.
    hostCtrl = r8(kHostControl);
    clockCtrl = r16(kClockControl);
    if (enableDma) {
      const uint16_t hc2 = r16(kHostControl2);
      useAdma64 = (hc2 & kHost2Addr64Bit) != 0U || (caps0 & (1U << 28)) != 0U;
      if (useAdma64) {
        w16(kHostControl2, hc2 | kHost2Ver4En | kHost2Addr64Bit);
      }
      SmmuResult smmu = configureSdhciSmmuBypass(kSdhci1StreamId);
      if (smmu.bypassed && allocateDmaBuffers()) {
        useDma = true;
        w8(kHostControl, (hostCtrl & ~kHostDmaMask) | (useAdma64 ? kHostDmaAdma64 : kHostDmaAdma32));
      } else {
        useDma = false;
      }
    } else {
      useDma = false;
    }

    stage = 6;
    initialized = true;
    return true;
  }

  bool readChunkPio(uint64_t lba, uint32_t nblks, uint8_t *dst) {
    if (!nblks || nblks > kMaxChunkSectors) return false;
    if (!waitStateClear(kStateCmdInhibit | kStateDatInhibit, 200000U)) {
      resetLines(0x06U);
      return false;
    }
    w16(kIntStatus, 0xFFFFU);
    w16(kErrIntStatus, 0xFFFFU);
    w16(kBlockSize, kEmmcSectorBytes);
    w16(kBlockCount, static_cast<uint16_t>(nblks));
    const uint32_t arg = highCapacity ? static_cast<uint32_t>(lba)
                                      : static_cast<uint32_t>(lba * kEmmcSectorBytes);
    w32(kArgument, arg);

    const bool multi = nblks > 1U;
    const uint16_t xferMode = kXferDirRead | kXferBlkCntEn |
                              (multi ? (kXferMultiBlk | kXferAutoCmd12) : 0U);
    w16(kTransferMode, xferMode);

    const uint16_t cmdIdx = multi ? 18U : 17U;
    lastCmd = cmdIdx;
    w16(kCommand, static_cast<uint16_t>((cmdIdx << 8) | kCmdDataPresent | kCmdCrcCheck | kCmdIndexCheck | kCmdResp48));

    bool gotCmd = false;
    for (uint32_t blk = 0; blk < nblks; ++blk) {
      bool bufReady = false;
      for (uint32_t t = 0; t < 1000000U; ++t) {
        const uint16_t st = r16(kIntStatus);
        if (st & kIntError) {
          lastIntStatus = st;
          lastErrInt = r16(kErrIntStatus);
          w16(kErrIntStatus, 0xFFFFU);
          w16(kIntStatus, 0xFFFFU);
          if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
          resetLines(0x06U);
          return false;
        }
        if (!gotCmd && (st & kIntCmdComplete)) {
          w16(kIntStatus, kIntCmdComplete);
          gotCmd = true;
        }
        if ((st & kIntBufReadReady) || (r32(kPresentState) & kStateBufReadEnable)) {
          w16(kIntStatus, kIntBufReadReady);
          bufReady = true;
          break;
        }
        IODelay(1);
      }
      if (!bufReady) {
        if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
        resetLines(0x06U);
        return false;
      }
      for (uint32_t w = 0; w < 128U; ++w) {
        const uint32_t word = r32(kBufferDataPort);
        memcpy(dst + blk * kEmmcSectorBytes + w * 4U, &word, sizeof(word));
      }
    }

    for (uint32_t t = 0; t < 500000U; ++t) {
      const uint16_t st = r16(kIntStatus);
      if (st & kIntError) {
        lastIntStatus = st;
        lastErrInt = r16(kErrIntStatus);
        if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
        resetLines(0x06U);
        return false;
      }
      if (st & kIntXferComplete) {
        w16(kIntStatus, kIntXferComplete);
        return true;
      }
      IODelay(1);
    }
    if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
    resetLines(0x06U);
    return false;
  }

  bool readChunkAdma2(uint64_t lba, uint32_t nblks, uint8_t *dst) {
    if (!useDma || !dmaBuf || !admaBuf || !nblks || nblks > kMaxChunkSectors) return false;
    if (!waitStateClear(kStateCmdInhibit | kStateDatInhibit, 200000U)) {
      resetLines(0x06U);
      return false;
    }

    const uint32_t bytes = nblks * kEmmcSectorBytes;
    invalidateDataCacheRange(dmaBuf, bytes);

    if (useAdma64) {
      auto *desc = reinterpret_cast<Adma2Desc64 *>(admaBuf);
      desc->attr = kAdmaValid | kAdmaEnd | kAdmaTran;
      desc->len = static_cast<uint16_t>(bytes & 0xFFFFU);
      desc->addr = dmaPhys;
      desc->reserved = 0U;
      cleanDataCacheRange(desc, sizeof(*desc));
    } else {
      auto *desc = reinterpret_cast<Adma2Desc32 *>(admaBuf);
      desc->attr = kAdmaValid | kAdmaEnd | kAdmaTran;
      desc->len = static_cast<uint16_t>(bytes & 0xFFFFU);
      desc->addr = static_cast<uint32_t>(dmaPhys);
      cleanDataCacheRange(desc, sizeof(*desc));
    }

    w16(kIntStatus, 0xFFFFU);
    w16(kErrIntStatus, 0xFFFFU);
    w32(kAdmaAddrLo, static_cast<uint32_t>(admaPhys));
    if (useAdma64) w32(kAdmaAddrHi, static_cast<uint32_t>(admaPhys >> 32));
    w16(kBlockSize, kEmmcSectorBytes);
    w16(kBlockCount, static_cast<uint16_t>(nblks));
    const uint32_t arg = highCapacity ? static_cast<uint32_t>(lba)
                                      : static_cast<uint32_t>(lba * kEmmcSectorBytes);
    w32(kArgument, arg);

    const bool multi = nblks > 1U;
    const uint16_t xferMode = kXferDmaEn | kXferDirRead | kXferBlkCntEn |
                              (multi ? (kXferMultiBlk | kXferAutoCmd12) : 0U);
    w16(kTransferMode, xferMode);

    const uint16_t cmdIdx = multi ? 18U : 17U;
    lastCmd = cmdIdx;
    w16(kCommand, static_cast<uint16_t>((cmdIdx << 8) | kCmdDataPresent | kCmdCrcCheck | kCmdIndexCheck | kCmdResp48));

    bool done = false;
    for (uint32_t t = 0; t < 1000000U; ++t) {
      const uint16_t st = r16(kIntStatus);
      if (st & kIntError) {
        lastIntStatus = st;
        lastErrInt = r16(kErrIntStatus);
        w16(kErrIntStatus, 0xFFFFU);
        w16(kIntStatus, 0xFFFFU);
        if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
        resetLines(0x06U);
        return false;
      }
      if (st & kIntXferComplete) {
        w16(kIntStatus, kIntCmdComplete | kIntXferComplete | kIntDma);
        done = true;
        break;
      }
      IODelay(1);
    }
    if (!done) {
      if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
      resetLines(0x06U);
      return false;
    }
    invalidateDataCacheRange(dmaBuf, bytes);
    memcpy(dst, dmaBuf, bytes);
    return true;
  }

  bool readSectors(uint64_t lba, uint32_t count, void *dst) {
    if (!initialized || !dst) return false;
    if (!count) return true;
    if (lba >= sectorCount || count > sectorCount - lba) return false;

    uint8_t *out = static_cast<uint8_t *>(dst);
    uint32_t done = 0;
    while (done < count) {
      const uint32_t chunk = (count - done > kMaxChunkSectors) ? kMaxChunkSectors : (count - done);
      bool ok = false;
      if (useDma) {
        ok = readChunkAdma2(lba + done, chunk, out);
        if (!ok) {
          // Graceful runtime fallback to PIO if ADMA/SMMU fails on a board state.
          useDma = false;
        }
      }
      if (!ok) {
        ok = readChunkPio(lba + done, chunk, out);
        if (!ok && chunk > 1U) {
          // Fallback to single-sector CMD17 loop if AutoCMD12 behaves unexpectedly.
          ok = true;
          for (uint32_t s = 0; s < chunk; ++s) {
            if (!readChunkPio(lba + done + s, 1U, out + s * kEmmcSectorBytes)) {
              ok = false;
              break;
            }
          }
        }
      }
      if (!ok) {
        ++readErrors;
        return false;
      }
      done += chunk;
      out += chunk * kEmmcSectorBytes;
    }
    ++readOps;
    return true;
  }

  bool writeChunkPio(uint64_t lba, uint32_t nblks, const uint8_t *src) {
    if (!nblks || nblks > kMaxChunkSectors) return false;
    if (!waitStateClear(kStateCmdInhibit | kStateDatInhibit, 200000U)) {
      resetLines(0x06U);
      return false;
    }
    w16(kIntStatus, 0xFFFFU);
    w16(kErrIntStatus, 0xFFFFU);
    w16(kBlockSize, kEmmcSectorBytes);
    w16(kBlockCount, static_cast<uint16_t>(nblks));
    const uint32_t arg = highCapacity ? static_cast<uint32_t>(lba)
                                      : static_cast<uint32_t>(lba * kEmmcSectorBytes);
    w32(kArgument, arg);

    const bool multi = nblks > 1U;
    const uint16_t xferMode = kXferBlkCntEn | (multi ? (kXferMultiBlk | kXferAutoCmd12) : 0U);
    w16(kTransferMode, xferMode);

    const uint16_t cmdIdx = multi ? 25U : 24U;
    lastCmd = cmdIdx;
    w16(kCommand, static_cast<uint16_t>((cmdIdx << 8) | kCmdDataPresent | kCmdCrcCheck | kCmdIndexCheck | kCmdResp48));

    bool gotCmd = false;
    for (uint32_t blk = 0; blk < nblks; ++blk) {
      bool bufReady = false;
      for (uint32_t t = 0; t < 1000000U; ++t) {
        const uint16_t st = r16(kIntStatus);
        if (st & kIntError) {
          lastIntStatus = st;
          lastErrInt = r16(kErrIntStatus);
          if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
          resetLines(0x06U);
          return false;
        }
        if (!gotCmd && (st & kIntCmdComplete)) {
          w16(kIntStatus, kIntCmdComplete);
          gotCmd = true;
        }
        if ((st & kIntBufWriteReady) || (r32(kPresentState) & kStateBufWriteEnable)) {
          w16(kIntStatus, kIntBufWriteReady);
          bufReady = true;
          break;
        }
        IODelay(1);
      }
      if (!bufReady) {
        if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
        resetLines(0x06U);
        return false;
      }
      for (uint32_t w = 0; w < 128U; ++w) {
        uint32_t word = 0;
        memcpy(&word, src + blk * kEmmcSectorBytes + w * 4U, sizeof(word));
        w32(kBufferDataPort, word);
      }
    }

    for (uint32_t t = 0; t < 1000000U; ++t) {
      const uint16_t st = r16(kIntStatus);
      if (st & kIntError) {
        lastIntStatus = st;
        lastErrInt = r16(kErrIntStatus);
        if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
        resetLines(0x06U);
        return false;
      }
      if (st & kIntXferComplete) {
        w16(kIntStatus, kIntXferComplete);
        return true;
      }
      IODelay(1);
    }
    if (multi) sendCommand(12U, 0U, kCmdCrcCheck | kCmdIndexCheck | kCmdResp48Busy);
    resetLines(0x06U);
    return false;
  }

  bool writeSectors(uint64_t lba, uint32_t count, const void *src) {
    if (!initialized || !src) return false;
    if (!count) return true;
    if (lba >= sectorCount || count > sectorCount - lba) return false;
    const uint8_t *in = static_cast<const uint8_t *>(src);
    uint32_t done = 0;
    while (done < count) {
      const uint32_t chunk = (count - done > kMaxChunkSectors) ? kMaxChunkSectors : (count - done);
      if (!writeChunkPio(lba + done, chunk, in)) return false;
      done += chunk;
      in += chunk * kEmmcSectorBytes;
    }
    ++writeOps;
    return true;
  }

  bool flushCache() {
    if (!initialized) return false;
    // EXT_CSD[32] FLUSH_CACHE = 1 (optional; some eMMCs ignore if cache not enabled)
    switchExtCsd(32U, 1U);
    return true;
  }
};

}  // namespace xun_emmc
