// SPDX-License-Identifier: BSD-2-Clause-Patent
// Touch controller bring-up for Xiaomi Redmi Pad SE (xun, SM6225 QUPv3 SE0 SPI).
// Supports all three m84 (1200x1920) panels over SPI SE0 (GPIO0..3, CS0, IRQ GPIO80, RST GPIO86):
//   - BOE / Huaxing (CSOT): FocalTech FT8203 SPI (1200x1920 12-bit coords)
//   - Tianma: Novatek NT36532 SPI (65-byte checksummed coordinate packets)
// Boot args:
//   xun-touch=<1..5>   (or touch=<1..5> / tb128fu-touch=<1..5>; 5 = live touch)
//   xun-touch-fw=<0|1|2>  (0 = auto [Huaxing/BOE FT8203 or Tianma NVT], 1 = force BOE FT8203, 2 = force Huaxing FT8203)
//   xun-touch-flip=<0|1>  (0 = default [auto-synced with xun-fb-rotate], 1 = toggle 180-deg X/Y flip)
//   xun-touch-orient=<0..7> (bitmask: 1=flip_x, 2=flip_y, 4=swap_xy; XORed with 180-deg display rotation)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "GeniSpi.h"
#include "Fts8203.h"
#include "NvtProtocol.h"
#include "TouchBoard.h"
extern const uint8_t native_xun_fw_boe[];
extern const uint32_t native_xun_fw_boe_size;
extern const uint8_t native_xun_fw_huaxing[];
extern const uint32_t native_xun_fw_huaxing_size;
extern const uint8_t native_xun_fw_tianma[];
extern const uint32_t native_xun_fw_tianma_size;
extern volatile uint32_t native_touch_seq, native_touch_count;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
void native_touch_published(void);
int native_legacy_fb_rotate_180(void) __attribute__((weak_import));
}

extern "C" {
char native_touch_rows[4][120];
char native_touch_trace[8][120];
volatile uint32_t native_touch_loop_step;
volatile unsigned native_touch_stage_req;
volatile uint32_t native_touch_stage_done, native_touch_step, native_touch_frames, native_touch_valid, native_touch_contacts;
volatile int native_touch_error;
volatile int native_touch_last_x = -1, native_touch_last_y = -1;
volatile int32_t native_touch_strokes[16][7];
volatile uint32_t native_touch_stroke_count;
volatile int32_t native_touch_extent[4] = {0x7fffffff, -1, 0x7fffffff, -1};
volatile uint32_t native_touch_auto_done;
}

namespace {
constexpr uint32_t kFtsRawWidth = XUN_PANEL_WIDTH, kFtsRawHeight = XUN_PANEL_HEIGHT;
constexpr int kHidAxisMaxX = 12000, kHidAxisMaxY = 19200;
constexpr uint32_t kIntrCfg = 0x8, kIntrStatus = 0xc;
constexpr uint32_t kIntrFallingLatch = (1u << 4) | (2u << 2) | (1u << 1);
constexpr uint32_t kProbeHz = 1000000u;
constexpr size_t kDmaBufBytes = 65536u;
constexpr unsigned kForceDmaUnset = 0xffffffffu;

enum class ChipKind { None = 0, Fts8203 = 1, Nvt36532 = 2 };

struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};

size_t dcacheLineBytes() {
    uint64_t ctr = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return static_cast<size_t>(4u << ((ctr >> 16) & 15u));
}

void flushDcache(void *virt, size_t len, bool inval) {
    if (!virt || !len) return;
    const size_t line = dcacheLineBytes();
    const uintptr_t start = reinterpret_cast<uintptr_t>(virt) & ~(line - 1u);
    const uintptr_t end = reinterpret_cast<uintptr_t>(virt) + len;
    for (uintptr_t a = start; a < end; a += line) {
        if (inval) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

void dmaClean(void *, void *virt, size_t len) {flushDcache(virt, len, false);}
void dmaCleanInval(void *, void *virt, size_t len) {flushDcache(virt, len, true);}

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}

uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
void ioDelayUs(void *, unsigned us) {
    if (us > 50) {IODelay(us); return;}
    uint64_t freq = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (freq ? freq : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));} while (now - start < ticks);
}

struct Session {
    Region gccLow, gccQup, gccVote, tlmmSpi, tlmmIrq, tlmmRst, wrapper, se, smmu;
    uint32_t vals[24] = {};
    uint64_t levelHz[TB_DFS_LEVELS] = {};
    unsigned levels = 0, level = 0, div = 0;
    int dfs = 0;
    uint64_t actualHz = 0;
    struct geni_spi spi = {};
    IOBufferMemoryDescriptor *dmaTxDesc = nullptr, *dmaRxDesc = nullptr;
    uint8_t *dmaTxCpu = nullptr, *dmaRxCpu = nullptr;
    uint64_t dmaTxPhys = 0, dmaRxPhys = 0;
    uint32_t smmuS2cr = 0;
    int32_t smmuSlot = -1;
    bool smmuBypassed = false;
    unsigned forceDma = kForceDmaUnset;
    int loopRc = 99;
    uint8_t loopRx[8] = {};
    unsigned traceN = 0, ringN = 0;
    uint32_t traceLen[4] = {}, ringLen[4] = {};
    uint8_t traceTx[4][8] = {}, traceRx[4][8] = {}, ringTx[4][8] = {}, ringRx[4][8] = {};
    int lastRc = 0;
    uint32_t lastLen = 0, xIrq = 0, xGeni = 0, xTx = 0, xRx = 0;
    ChipKind kind = ChipKind::None;
    unsigned fwMode = 0;   // 0 auto, 1 boe, 2 huaxing
    unsigned flip = 0;
    unsigned orient = 0;   // bit0=flip_x, bit1=flip_y, bit2=swap_xy
    unsigned rawWOverride = 0, rawHOverride = 0;
    uint32_t rawW = XUN_PANEL_WIDTH, rawH = XUN_PANEL_HEIGHT;
    const uint8_t *activeFw = nullptr;
    size_t activeFwSize = 0;
    const char *chipName = "none";
    struct Fts8203 fts = {};
    struct Fts8203Frame ftsFrame = {};
    struct nvt_bus nvtBus = {};
    struct nvt_chip nvt = {};
    uint8_t *nvtScratch = nullptr, *nvtRxBuf = nullptr;
    struct nvt_fw nvtFw = {};
    struct nvt_fw_info nvtInfo = {};
    struct nvt_point_frame nvtPoints = {};
    uint32_t edges = 0, lowSeen = 0, reads = 0, bad = 0, idleReads = 0, nonzero = 0;
};

struct Out {
    char *p;
    size_t cap, n;
    void add(const char *fmt, unsigned a = 0, unsigned b = 0) {
        if (n + 1 >= cap) return;
        int w = snprintf(p + n, cap - n, fmt, a, b);
        if (w < 0) return;
        n = (n + static_cast<size_t>(w) >= cap) ? cap - 1 : n + static_cast<size_t>(w);
    }
    void hex(uint8_t v) {add("%02x", v);}
};

void traceRows(const Session &s) {
    for (unsigned r = 0; r < 2; ++r) {
        Out o = {native_touch_trace[r], sizeof native_touch_trace[r], 0};
        o.p[0] = 0;
        o.add("TCH xfer");
        for (unsigned j = 0; j < 2; ++j) {
            unsigned i = r * 2 + j;
            if (i >= s.traceN) break;
            o.add(" #%u len%u tx", i, s.traceLen[i]);
            for (unsigned k = 0; k < 8 && k < s.traceLen[i]; ++k) o.hex(s.traceTx[i][k]);
            o.add(" rx");
            for (unsigned k = 0; k < 8 && k < s.traceLen[i]; ++k) o.hex(s.traceRx[i][k]);
            o.add(" |");
        }
    }
    snprintf(native_touch_trace[2], sizeof native_touch_trace[2],
             "TCH chip=%s raw=%ux%u fts=%02x%02x/v%02x nvt=%02x%02x%02x/v%02x",
             s.chipName, s.rawW, s.rawH, s.fts.rom[0], s.fts.rom[1], s.fts.version,
             s.nvt.id[0], s.nvt.id[1], s.nvt.id[2], s.nvtInfo.fw_ver);

    for (unsigned r = 0; r < 2; ++r) {
        Out o = {native_touch_trace[3 + r], sizeof native_touch_trace[3 + r], 0};
        o.p[0] = 0;
        o.add("TCH last");
        for (unsigned j = 0; j < 2; ++j) {
            unsigned k = r * 2 + j;
            if (s.ringN < 4 && k >= s.ringN) break;
            unsigned seq = s.ringN >= 4 ? s.ringN - 4 + k : k;
            unsigned idx = seq % 4;
            o.add(" #%u len%u tx", seq, s.ringLen[idx]);
            for (unsigned q = 0; q < 8 && q < s.ringLen[idx]; ++q) o.hex(s.ringTx[idx][q]);
            o.add(" rx");
            for (unsigned q = 0; q < 8 && q < s.ringLen[idx]; ++q) o.hex(s.ringRx[idx][q]);
            o.add(" |");
        }
    }
    snprintf(native_touch_trace[5], sizeof native_touch_trace[5],
             "TCH lp%u ed%u low%u rd%u idl%u bad%u nz%u lrc%d/%u",
             native_touch_loop_step, s.edges, s.lowSeen, s.reads, s.idleReads, s.bad, s.nonzero, s.lastRc, s.lastLen);
}

void rows(const Session &s) {
    traceRows(s);
    const uint32_t *v = s.vals;
    if (native_touch_stage_done == 5 && !native_touch_error) {
        snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
                 "TCH s5/5 %s fw%u %uHz dm%u fr%u ok%u c%u (%d,%d) ed%u lo%u rd%u bad%u",
                 s.chipName, static_cast<unsigned>(s.activeFwSize), static_cast<unsigned>(s.actualHz),
                 static_cast<unsigned>(s.spi.use_dma), native_touch_frames, native_touch_valid,
                 native_touch_contacts, native_touch_last_x, native_touch_last_y,
                 s.edges, s.lowSeen, s.reads, s.bad);
    } else if (native_touch_error && native_touch_stage_done >= 2) {
        snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
                 "TCH s%u/%u st%u e%d %s if%x dm%u fw%x hz%u L%u/%u irq%x st%x tx%x rx%x sm%x",
                 native_touch_stage_done, native_touch_stage_req, native_touch_step, native_touch_error,
                 s.chipName, v[20], static_cast<unsigned>(s.spi.use_dma), v[19],
                 static_cast<unsigned>(s.actualHz), s.level, s.div,
                 s.spi.snap_irq, s.spi.snap_status, s.spi.snap_dma_txin, s.spi.snap_dma_rxin, s.smmuS2cr);
    } else {
        snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
                 "TCH s%u/%u st%u e%d vote%08x h%08x %08x %08x %08x %08x gp%08x",
                 native_touch_stage_done, native_touch_stage_req, native_touch_step, native_touch_error,
                 v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    }
    snprintf(native_touch_rows[1], sizeof native_touch_rows[1],
             "TCH spi=%s %uHz L%u/%u fw%u if%08x hw%08x loop%d(%02x%02x) dm%u sm%d:%x",
             s.chipName, static_cast<unsigned>(s.actualHz), s.level, s.div,
             static_cast<unsigned>(s.activeFwSize), v[20], v[21], s.loopRc, s.loopRx[0], s.loopRx[1],
             static_cast<unsigned>(s.spi.use_dma), s.smmuSlot, s.smmuS2cr);
    snprintf(native_touch_rows[2], sizeof native_touch_rows[2],
             "TCH gp %08x %08x %08x %08x irq%08x rst%08x rcg%08x/%08x",
             v[10], v[11], v[12], v[13], v[14], v[17], v[8], v[9]);
    snprintf(native_touch_rows[3], sizeof native_touch_rows[3],
             "TCH fr%u ok%u c%u (%d,%d) ed%u lo%u rd%u bad%u xirq%08x gs%08x",
             native_touch_frames, native_touch_valid, native_touch_contacts,
             native_touch_last_x, native_touch_last_y, s.edges, s.lowSeen, s.reads, s.bad, s.xIrq, s.xGeni);
}

void mark(const Session &s, unsigned step) {native_touch_step = step; rows(s);}
void fail(unsigned step, int code) {
    if (!native_touch_error) {native_touch_step = step; native_touch_error = code ? code : -1;}
}

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Session *s = static_cast<Session *>(ctx);
    int rc = geni_spi_transfer(&s->spi, tx, rx, len, 0);
    unsigned ringIdx = s->ringN++ % 4;
    s->ringLen[ringIdx] = static_cast<uint32_t>(len);
    for (unsigned k = 0; k < 8; ++k) {
        s->ringTx[ringIdx][k] = (tx && k < len) ? tx[k] : 0;
        s->ringRx[ringIdx][k] = (rx && k < len && !rc) ? rx[k] : 0;
    }
    if (!rc && s->traceN < 4) {
        unsigned ti = s->traceN++;
        s->traceLen[ti] = static_cast<uint32_t>(len);
        for (unsigned k = 0; k < 8 && k < len && tx; ++k) s->traceTx[ti][k] = tx[k];
        for (unsigned k = 0; k < 8 && k < len && rx; ++k) s->traceRx[ti][k] = rx[k];
    }
    if (rc && !s->lastRc) {
        s->lastRc = rc;
        s->lastLen = static_cast<uint32_t>(len);
        s->xIrq = s->spi.snap_irq;
        s->xGeni = s->spi.snap_status;
        s->xTx = s->spi.snap_txfs;
        s->xRx = s->spi.snap_rxfs;
    }
    return rc;
}

void gpioReset(void *ctx, int high) {
    Session *s = static_cast<Session *>(ctx);
    wr32(s->tlmmRst.va, 4, high ? TB_GPIO_OUT : 0u);
}

void ftsDelayUs(void *, unsigned us) {
    if (us >= 1000) IOSleep((us + 999) / 1000);
    else ioDelayUs(nullptr, us);
}

void ftsStage(void *ctx, const char *name, unsigned index, int rc) {
    Session *s = static_cast<Session *>(ctx);
    snprintf(native_touch_trace[6], sizeof native_touch_trace[6], "TCH fts %s #%u rc=%d", name ? name : "?", index, rc);
    s->vals[22] = static_cast<uint32_t>(rc);
}

void busDelayMs(void *, unsigned ms) {IOSleep(ms);}

bool setSpiRate(Session &s, uint32_t targetHz) {
    const unsigned os = s.spi.oversampling ? s.spi.oversampling : 16;
    struct tb_clock_choice choice = {};
    if (tb_choose_clock(s.levelHz, s.levels, os, targetHz, &choice)) return false;
    s.level = choice.level;
    s.div = choice.div;
    s.actualHz = choice.actual_hz;
    if (geni_spi_set_clock(&s.spi, targetHz, static_cast<unsigned>(choice.sclk_hz), s.dfs ? choice.level : 0)) {
        return false;
    }
    return true;
}

bool snapshot(Session &s) {
    if (!mapRegion(TB_GCC_BASE + (TB_GCC_GPLL0_MODE & ~0xfffULL), 0x1000, s.gccLow) ||
        !mapRegion(TB_GCC_BASE + (TB_HALT_M_AHB & ~0xfffULL), 0x1000, s.gccQup) ||
        !mapRegion(TB_GCC_BASE + (TB_GCC_GPLL0_ENABLE & ~0xfffULL), 0x1000, s.gccVote) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_SPI_FIRST), 4 * TB_GPIO_STRIDE, s.tlmmSpi) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_IRQ), 0x1000, s.tlmmIrq) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_RESET), 0x1000, s.tlmmRst)) {
        fail(1, -12);
        return false;
    }
    uint32_t *v = s.vals;
    v[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    v[1] = rd32(s.gccQup.va, TB_HALT_M_AHB & 0xfff);
    v[2] = rd32(s.gccQup.va, TB_HALT_S_AHB & 0xfff);
    v[3] = rd32(s.gccQup.va, TB_HALT_CORE & 0xfff);
    v[4] = rd32(s.gccQup.va, TB_HALT_CORE_2X & 0xfff);
    v[5] = rd32(s.gccQup.va, TB_HALT_S0 & 0xfff);
    v[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE & 0xfff);
    v[7] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CMD) & 0xfff);
    v[8] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CFG) & 0xfff);
    v[9] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_DFSR_CMD) & 0xfff);
    for (unsigned i = 0; i < 4; ++i) v[10 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    v[14] = rd32(s.tlmmIrq.va, 0); v[15] = rd32(s.tlmmIrq.va, 4); v[16] = rd32(s.tlmmIrq.va, kIntrCfg);
    v[17] = rd32(s.tlmmRst.va, 0); v[18] = rd32(s.tlmmRst.va, 4);

    s.dfs = (v[9] & 1u) != 0;
    if (s.dfs) {
        s.levels = TB_DFS_LEVELS;
        for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
            s.levelHz[l] = tb_rcg_hz(rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_PERF_CFG(l)) & 0xfff),
                                     rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_PERF_M(l)) & 0xfff),
                                     rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_PERF_N(l)) & 0xfff));
        }
    } else {
        s.levels = 1;
    }
    return true;
}

bool configureSmmuBypass(Session &s) {
    if (!mapRegion(TB_APPS_SMMU_BASE, TB_APPS_SMMU_SIZE, s.smmu)) {s.smmuSlot = -10; return false;}
    const uintptr_t base = s.smmu.va;
    if (rd32(base, 0x0u) & 1u) {
        s.smmuBypassed = true;
        return true;
    }
    const uint32_t idr0 = rd32(base, 0x20u);
    const uint32_t idr1 = rd32(base, 0x24u);
    uint32_t numSmrg = idr0 & 0xffu;
    if (numSmrg > 50u) numSmrg = 50u;   // vendor_boot qcom,num-smr-override = 0x32
    const uint32_t pagesize = (idr1 & (1u << 31)) ? 0x10000u : 0x1000u;
    const uint32_t numPages = 1u << (((idr1 >> 28) & 7u) + 1u);
    const uint32_t cbBase = numPages * pagesize;
    uint32_t numCb = idr1 & 0xffu;
    if (numCb > 48u) numCb = 48u;       // vendor_boot qcom,num-context-banks-override = 0x30
    if (numSmrg == 0u || numCb == 0u || cbBase + numCb * pagesize > TB_APPS_SMMU_SIZE) {
        s.smmuSlot = -11;
        s.smmuS2cr = idr1;
        return false;
    }

    uint64_t cbUsedMask = 0;
    int32_t matched = -1, freeSlot = -1;
    for (uint32_t i = 0; i < numSmrg; ++i) {
        const uint32_t smr = rd32(base, 0x800u + i * 4u);
        if (smr & (1u << 31)) {
            const uint32_t id = smr & 0x7fffu;
            const uint32_t mask = (smr >> 16) & 0x7fffu;
            const uint32_t s2cr = rd32(base, 0xC00u + i * 4u);
            if (((s2cr >> 16) & 3u) == 0u) {
                const uint32_t cb = s2cr & 0xffu;
                if (cb < numCb) cbUsedMask |= (1ULL << cb);
            }
            if (id == TB_QUP_SMMU_SID && mask == 0u) {
                if (matched < 0) matched = static_cast<int32_t>(i);
            } else if ((TB_QUP_SMMU_SID & ~mask) == (id & ~mask)) {
                const uint32_t type = (s2cr >> 16) & 3u;
                const uint32_t cb = s2cr & 0xffu;
                const bool cbBypass = (type == 0u && cb < numCb && (rd32(base, cbBase + cb * pagesize) & 1u) == 0u);
                s.smmuSlot = static_cast<int32_t>(i);
                s.smmuS2cr = s2cr;
                if (type == 1u || cbBypass) {
                    s.smmuBypassed = true;
                    return true;
                }
                return false;
            }
        } else if (freeSlot < 0) {
            freeSlot = static_cast<int32_t>(i);
        }
    }
    const int32_t slot = (matched >= 0) ? matched : freeSlot;
    if (slot < 0) {s.smmuSlot = -12; return false;}
    s.smmuSlot = slot;
    if (matched >= 0) {
        const uint32_t curS2cr = rd32(base, 0xC00u + static_cast<uint32_t>(slot) * 4u);
        const uint32_t type = (curS2cr >> 16) & 3u;
        const uint32_t cb = curS2cr & 0xffu;
        const bool cbBypass = (type == 0u && cb < numCb && (rd32(base, cbBase + cb * pagesize) & 1u) == 0u);
        if (type == 1u || cbBypass) {
            s.smmuS2cr = curS2cr;
            s.smmuBypassed = true;
            return true;
        }
    }

    int32_t bank = -1;
    for (uint32_t b = 0; b < numCb; ++b) {
        if (cbUsedMask & (1ULL << b)) continue;
        if ((rd32(base, cbBase + b * pagesize) & 1u) != 0u) continue;
        bank = static_cast<int32_t>(b);
        break;
    }
    if (bank < 0) {s.smmuSlot = -13; return false;}

    const uint32_t cbOff = cbBase + static_cast<uint32_t>(bank) * pagesize;
    const uint32_t cbarOff = pagesize + static_cast<uint32_t>(bank) * 4u;
    const uint32_t cba2rOff = pagesize + 0x800u + static_cast<uint32_t>(bank) * 4u;
    wr32(base, cbOff + 0x0u, 0u);
    const uint32_t sctlrRb = rd32(base, cbOff + 0x0u);
    if ((sctlrRb & 1u) != 0u) {s.smmuS2cr = 0xE1000000u | (sctlrRb & 0xffffffu); return false;}
    wr32(base, cba2rOff, 1u);
    const uint32_t cba2rRb = rd32(base, cba2rOff);
    if ((cba2rRb & 1u) != 1u) {s.smmuS2cr = 0xE2000000u | (cba2rRb & 0xffffffu); return false;}
    wr32(base, cbarOff, 0x10000u);
    const uint32_t cbarRb = rd32(base, cbarOff);
    if ((cbarRb & 0x30000u) != 0x10000u) {s.smmuS2cr = 0xE3000000u | (cbarRb & 0xffffffu); return false;}
    __asm__ volatile("dsb sy" ::: "memory");

    const uint32_t s2crOff = 0xC00u + static_cast<uint32_t>(slot) * 4u;
    const uint32_t smrOff = 0x800u + static_cast<uint32_t>(slot) * 4u;
    wr32(base, s2crOff, static_cast<uint32_t>(bank));
    s.smmuS2cr = rd32(base, s2crOff);
    if ((s.smmuS2cr & 0x300ffu) != static_cast<uint32_t>(bank)) return false;
    wr32(base, smrOff, (1u << 31) | TB_QUP_SMMU_SID);
    const uint32_t smrRb = rd32(base, smrOff);
    if ((smrRb & 0x80007fffu) != ((1u << 31) | TB_QUP_SMMU_SID)) {
        s.smmuS2cr = 0xE5000000u | (smrRb & 0xffffffu);
        return false;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    s.smmuBypassed = true;
    return true;
}

bool allocateDmaBuf(IOBufferMemoryDescriptor **descOut, uint8_t **cpuOut, uint64_t *physOut) {
    const IOOptionBits opts = static_cast<IOOptionBits>(kIODirectionInOut) |
                              static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    IOBufferMemoryDescriptor *desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, opts, kDmaBufBytes, 0xFFFFF000ULL);
    if (!desc || desc->prepare() != kIOReturnSuccess) {
        if (desc) desc->release();
        return false;
    }
    IOByteCount segLen = 0;
    const uint64_t phys = desc->getPhysicalSegment(0, &segLen);
    uint8_t *cpu = static_cast<uint8_t *>(desc->getBytesNoCopy());
    if (!cpu || !phys || (phys >> 32) || segLen < kDmaBufBytes) {
        desc->complete();
        desc->release();
        return false;
    }
    bzero(cpu, kDmaBufBytes);
    *descOut = desc;
    *cpuOut = cpu;
    *physOut = phys;
    return true;
}

bool allocateDmaBuffers(Session &s) {
    if (!allocateDmaBuf(&s.dmaTxDesc, &s.dmaTxCpu, &s.dmaTxPhys)) return false;
    if (!allocateDmaBuf(&s.dmaRxDesc, &s.dmaRxCpu, &s.dmaRxPhys)) return false;
    s.spi.dma = {
        nullptr, dmaClean, dmaCleanInval, dmaCleanInval,
        s.dmaTxCpu, s.dmaRxCpu, s.dmaTxPhys, s.dmaRxPhys, kDmaBufBytes
    };
    return true;
}

bool enableBus(Session &s) {
    mark(s, 2);
    wr32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
    for (unsigned i = 0; i < 10000 && !(rd32(s.gccLow.va, TB_GCC_GPLL0_MODE & 0xfff) & (1u << 31)); ++i) IODelay(10);
    s.vals[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE & 0xfff);
    wr32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff) | TB_VOTE_ALL);

    auto running = [](uint32_t cbcr) {return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u);};
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !ok; ++i) {
        ok = running(rd32(s.gccQup.va, TB_HALT_M_AHB & 0xfff)) &&
             running(rd32(s.gccQup.va, TB_HALT_S_AHB & 0xfff)) &&
             !(rd32(s.gccQup.va, TB_HALT_CORE & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccQup.va, TB_HALT_CORE_2X & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccQup.va, TB_HALT_S0 & 0xfff) & TB_CBCR_CLK_OFF);
        if (!ok) IODelay(10);
    }
    s.vals[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    s.vals[1] = rd32(s.gccQup.va, TB_HALT_M_AHB & 0xfff);
    s.vals[2] = rd32(s.gccQup.va, TB_HALT_S_AHB & 0xfff);
    s.vals[3] = rd32(s.gccQup.va, TB_HALT_CORE & 0xfff);
    s.vals[4] = rd32(s.gccQup.va, TB_HALT_CORE_2X & 0xfff);
    s.vals[5] = rd32(s.gccQup.va, TB_HALT_S0 & 0xfff);
    if (!ok) {fail(2, -110); return false;}

    const uint32_t spiPin = tb_pin_cfg(TB_FUNC_QUP0, TB_DRIVE_6MA, TB_PULL_UP);
    for (unsigned i = 0; i < 4; ++i) {
        if (TB_GPIO_SPI_FIRST + i == TB_GPIO_SPI_CS) {
            wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE + 4, TB_GPIO_OUT);
            wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE, spiPin | TB_GPIO_OE);
        } else {
            wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE, spiPin);
        }
    }
    wr32(s.tlmmIrq.va, 0, tb_pin_cfg(TB_FUNC_GPIO, TB_DRIVE_8MA, TB_PULL_UP));
    wr32(s.tlmmIrq.va, kIntrCfg, (rd32(s.tlmmIrq.va, kIntrCfg) & ~0x1fu) | kIntrFallingLatch);
    wr32(s.tlmmIrq.va, kIntrStatus, 0);
    wr32(s.tlmmRst.va, 4, TB_GPIO_OUT);
    wr32(s.tlmmRst.va, 0, tb_pin_cfg(TB_FUNC_GPIO, TB_DRIVE_8MA, TB_PULL_UP) | TB_GPIO_OE);
    for (unsigned i = 0; i < 4; ++i) s.vals[10 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    s.vals[14] = rd32(s.tlmmIrq.va, 0);
    s.vals[17] = rd32(s.tlmmRst.va, 0);

    mark(s, 3);
    if (!mapRegion(TB_QUP_WRAPPER_BASE, 0x2000, s.wrapper) || !mapRegion(TB_SE_BASE, TB_SE_SIZE, s.se)) {
        fail(3, -12);
        return false;
    }
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    s.spi.poll_only = 1;
    if (geni_spi_read_info(&s.spi)) {fail(3, -5); return false;}
    s.vals[19] = s.spi.info.fw_revision;
    s.vals[20] = s.spi.info.if_disable;
    s.vals[21] = s.spi.info.qup_hw_version;

    const bool wantDma = (s.forceDma == 1u) || (s.forceDma == kForceDmaUnset && s.spi.info.fifo_disabled != 0);
    if (wantDma) {
        if (!configureSmmuBypass(s)) {fail(3, -5); return false;}
        if (!allocateDmaBuffers(s)) {fail(3, -12); return false;}
        rows(s);
        s.spi.use_dma = 1;
        s.spi.allow_fifo_disabled = 0;
    } else {
        s.spi.use_dma = 0;
        s.spi.allow_fifo_disabled = (s.forceDma == 0u && s.spi.info.fifo_disabled) ? 1 : 0;
    }
    return true;
}

bool ftsProbeRom(Session &s) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        gpioReset(&s, 0);
        IOSleep(2);
        gpioReset(&s, 1);
        IOSleep(8);
        const uint8_t cmd55[4] = {0x55, 0x00, 0x00, 0x00};
        if (busXfer(&s, cmd55, nullptr, sizeof cmd55) != 0) continue;
        IOSleep(8);
        uint8_t rom[2] = {0, 0};
        if (fts8203_read(&s.fts, 0x90, rom, sizeof rom) == 0 && rom[0] == 0x82 && rom[1] == 0x01) {
            s.fts.rom[0] = rom[0];
            s.fts.rom[1] = rom[1];
            return true;
        }
    }
    return false;
}

bool openAndIdentify(Session &s) {
    mark(s, 4);
    if (!s.dfs) {
        s.vals[7] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CMD) & 0xfff);
        s.vals[8] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CFG) & 0xfff);
        s.levelHz[0] = tb_rcg_hz(s.vals[8],
                                 rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_M) & 0xfff),
                                 rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_N) & 0xfff));
        if (!s.levelHz[0]) {
            wr32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CFG) & 0xfff, 1);
            wr32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CMD) & 0xfff, s.vals[7] | 1u);
            for (unsigned i = 0; i < 1000 && (rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CMD) & 0xfff) & 1u); ++i) IODelay(10);
            s.levelHz[0] = 19200000ULL;
        }
    }
    if (rd32(s.se.va, SE_GENI_STATUS) & GENI_STATUS_M_ACTIVE) {
        wr32(s.se.va, SE_GENI_M_CMD_CTRL_REG, GENI_M_CMD_ABORT);
        for (unsigned i = 0; i < 5000 && (rd32(s.se.va, SE_GENI_STATUS) & GENI_STATUS_M_ACTIVE); ++i) ioDelayUs(nullptr, 10);
    }
    int r = geni_spi_init(&s.spi);
    if (r) {fail(4, r); return false;}
    if (geni_spi_setup(&s.spi, 0, 0)) {fail(4, -22); return false;}
    if (!setSpiRate(s, kProbeHz)) {fail(4, -22); return false;}

    // Internal engine loopback check before touching the external panel bus.
    const uint8_t pat[4] = {0xa5, 0x5a, 0x3c, 0xc3};
    geni_spi_setup(&s.spi, GENI_SPI_LOOP, 0);
    int lrc = geni_spi_transfer(&s.spi, pat, s.loopRx, sizeof pat, 0);
    geni_spi_setup(&s.spi, 0, 0);
    s.loopRc = (lrc == 0 && memcmp(pat, s.loopRx, sizeof pat) == 0) ? 0 : (lrc ? lrc : -5);
    if (s.loopRc) {fail(5, s.loopRc); return false;}

    s.fts.io.context = &s;
    s.fts.io.transfer = busXfer;
    s.fts.io.delay_us = ftsDelayUs;
    s.fts.io.reset = gpioReset;
    s.fts.io.stage = ftsStage;

    s.nvtScratch = static_cast<uint8_t *>(IOMallocZero(NVT_TRANSFER_LEN + 8u));
    s.nvtRxBuf = static_cast<uint8_t *>(IOMallocZero(NVT_READ_MAX + 2u));
    s.nvtBus.ctx = &s;
    s.nvtBus.xfer = busXfer;
    s.nvtBus.delay_ms = busDelayMs;
    s.nvt.bus = &s.nvtBus;
    s.nvt.scratch = s.nvtScratch;
    s.nvt.rxbuf = s.nvtRxBuf;

    for (unsigned attempt = 0; attempt < 2 && s.kind == ChipKind::None; ++attempt) {
        if (attempt > 0) IOSleep(20);
        int fst = fts8203_state(&s.fts, 1);
        if (fst >= 0) {
            s.kind = ChipKind::Fts8203;
            s.chipName = "ft8203";
            s.rawW = kFtsRawWidth;
            s.rawH = kFtsRawHeight;
            break;
        }
        gpioReset(&s, 0);
        IOSleep(10);
        gpioReset(&s, 1);
        IOSleep(10);
        if (s.nvtScratch && s.nvtRxBuf && !nvt_identify(&s.nvt)) {
            s.kind = ChipKind::Nvt36532;
            s.chipName = "nt36532";
            s.rawW = XUN_PANEL_WIDTH;
            s.rawH = XUN_PANEL_HEIGHT;
            break;
        }
        if (ftsProbeRom(s)) {
            s.kind = ChipKind::Fts8203;
            s.chipName = "ft8203";
            s.rawW = kFtsRawWidth;
            s.rawH = kFtsRawHeight;
            break;
        }
    }
    if (s.kind != ChipKind::Nvt36532) {
        if (s.nvtScratch) {IOFree(s.nvtScratch, NVT_TRANSFER_LEN + 8u); s.nvtScratch = nullptr; s.nvt.scratch = nullptr;}
        if (s.nvtRxBuf) {IOFree(s.nvtRxBuf, NVT_READ_MAX + 2u); s.nvtRxBuf = nullptr; s.nvt.rxbuf = nullptr;}
    }
    if (s.kind == ChipKind::None) {fail(6, -19); return false;}
    return true;
}

bool downloadFirmware(Session &s) {
    mark(s, 7);
    if (s.kind == ChipKind::Fts8203) {
        if (s.fwMode == 1) {
            s.activeFw = native_xun_fw_boe;
            s.activeFwSize = native_xun_fw_boe_size;
            s.chipName = "ft8203-boe";
        } else if (s.fwMode == 2 || native_xun_fw_huaxing_size > 0) {
            s.activeFw = native_xun_fw_huaxing;
            s.activeFwSize = native_xun_fw_huaxing_size;
            s.chipName = "ft8203-csot";
        } else {
            s.activeFw = native_xun_fw_boe;
            s.activeFwSize = native_xun_fw_boe_size;
            s.chipName = "ft8203-boe";
        }
        if (!s.activeFw || !s.activeFwSize) {fail(8, -2); return false;}
        int rc = fts8203_start(&s.fts, s.activeFw, s.activeFwSize);
        if (rc) {fail(8, rc); return false;}
        setSpiRate(s, TB_SPI_DEFAULT_HZ);
        return true;
    }

    s.activeFw = native_xun_fw_tianma;
    s.activeFwSize = native_xun_fw_tianma_size;
    s.chipName = "nt36532-tm";
    if (!s.activeFw || !s.activeFwSize) {fail(8, -2); return false;}
    int rc = nvt_fw_parse(&s.nvtFw, s.activeFw, s.activeFwSize);
    if (rc) {fail(8, rc); return false;}
    rc = nvt_download_firmware(&s.nvt, &s.nvtFw);
    if (rc) {fail(9, rc); return false;}
    setSpiRate(s, TB_SPI_DEFAULT_HZ);
    IOSleep(20);
    nvt_get_fw_info(&s.nvt, &s.nvtInfo);
    if (s.nvtInfo.abs_x_max >= 400 && s.nvtInfo.abs_x_max <= 4096) s.rawW = s.nvtInfo.abs_x_max;
    if (s.nvtInfo.abs_y_max >= 400 && s.nvtInfo.abs_y_max <= 4096) s.rawH = s.nvtInfo.abs_y_max;
    return true;
}

struct Stroke {int id, x0, y0, x1, y1, frames; uint32_t t0;};
Stroke gStrokes[12];
unsigned gStrokeCount;

uint32_t touchMs() {
    uint64_t f = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return static_cast<uint32_t>(mach_absolute_time() / ((f ? f : 19200000) / 1000));
}

void trackStrokes(unsigned count, const int *ids, const int *pxs, const int *pys) {
    const uint32_t now = touchMs();
    for (unsigned i = 0; i < count; ++i) {
        const int x10 = pxs[i] * 10, y10 = pys[i] * 10;
        if (x10 < native_touch_extent[0]) native_touch_extent[0] = x10;
        if (x10 > native_touch_extent[1]) native_touch_extent[1] = x10;
        if (y10 < native_touch_extent[2]) native_touch_extent[2] = y10;
        if (y10 > native_touch_extent[3]) native_touch_extent[3] = y10;
        unsigned k = 0;
        while (k < gStrokeCount && gStrokes[k].id != ids[i]) ++k;
        if (k == gStrokeCount) {
            if (gStrokeCount == 12) continue;
            gStrokes[gStrokeCount++] = {ids[i], x10, y10, x10, y10, 0, now};
        }
        gStrokes[k].x1 = x10;
        gStrokes[k].y1 = y10;
        gStrokes[k].frames++;
    }
    for (unsigned k = 0; k < gStrokeCount;) {
        bool live = false;
        for (unsigned i = 0; i < count && !live; ++i) live = ids[i] == gStrokes[k].id;
        if (live) {++k; continue;}
        volatile int32_t *r = native_touch_strokes[native_touch_stroke_count & 15];
        r[0] = static_cast<int32_t>(gStrokes[k].t0);
        r[1] = static_cast<int32_t>(now - gStrokes[k].t0);
        r[2] = gStrokes[k].x0; r[3] = gStrokes[k].y0;
        r[4] = gStrokes[k].x1; r[5] = gStrokes[k].y1;
        r[6] = gStrokes[k].frames;
        native_touch_stroke_count = native_touch_stroke_count + 1;
        gStrokes[k] = gStrokes[--gStrokeCount];
    }
}

void publishContacts(const Session &s, unsigned count, const int *ids, const int *rawXs, const int *rawYs) {
    int xs[12], ys[12], pxs[12], pys[12];
    int rw = static_cast<int>(s.rawWOverride ? s.rawWOverride : (s.rawW ? s.rawW : XUN_PANEL_WIDTH));
    int rh = static_cast<int>(s.rawHOverride ? s.rawHOverride : (s.rawH ? s.rawH : XUN_PANEL_HEIGHT));
    const unsigned fbRot = (native_legacy_fb_rotate_180 && native_legacy_fb_rotate_180()) ? 3u : 0u;
    const unsigned effOrient = (s.orient ^ (s.flip ? 3u : 0u) ^ fbRot) & 7u;
    if (count > 12) count = 12;
    for (unsigned i = 0; i < count; ++i) {
        int rx = rawXs[i] < 0 ? 0 : (rawXs[i] >= rw ? rw - 1 : rawXs[i]);
        int ry = rawYs[i] < 0 ? 0 : (rawYs[i] >= rh ? rh - 1 : rawYs[i]);
        int cw = rw, ch = rh;
        if (effOrient & 4u) {
            int tmp = rx; rx = ry; ry = tmp;
            tmp = cw; cw = ch; ch = tmp;
        }
        if (effOrient & 1u) rx = cw - 1 - rx;
        if (effOrient & 2u) ry = ch - 1 - ry;
        xs[i] = (cw > 1) ? static_cast<int>(static_cast<int64_t>(rx) * (kHidAxisMaxX - 1) / (cw - 1)) : 0;
        ys[i] = (ch > 1) ? static_cast<int>(static_cast<int64_t>(ry) * (kHidAxisMaxY - 1) / (ch - 1)) : 0;
        pxs[i] = (cw > 1) ? static_cast<int>(static_cast<int64_t>(rx) * (XUN_PANEL_WIDTH - 1) / (cw - 1)) : 0;
        pys[i] = (ch > 1) ? static_cast<int>(static_cast<int64_t>(ry) * (XUN_PANEL_HEIGHT - 1) / (ch - 1)) : 0;
    }
    trackStrokes(count, ids, pxs, pys);
    native_touch_seq = native_touch_seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count; ++i) {
        native_touch_xs[i] = xs[i];
        native_touch_ys[i] = ys[i];
        native_touch_ids[i] = ids[i];
    }
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;
    native_touch_published();
    native_touch_contacts = count;
    if (count > 0) {native_touch_last_x = pxs[0]; native_touch_last_y = pys[0];}
}

void eventLoop(Session &s) {
    unsigned lastCount = 0;
    unsigned pollIter = 0;
    unsigned lastX[12] = {}, lastY[12] = {}, lastId[12] = {};
    for (;;) {
        native_touch_loop_step = 1;
        const bool edge = (rd32(s.tlmmIrq.va, kIntrStatus) & 1u) != 0;
        if (edge) {wr32(s.tlmmIrq.va, kIntrStatus, 0); s.edges++;}
        const bool low = (rd32(s.tlmmIrq.va, 4) & TB_GPIO_IN) == 0;
        if (low) s.lowSeen++;
        ++pollIter;

        // Always read when IRQ asserts or contacts are down; fall back to 16ms polling if IRQ line is idle/unwired.
        const bool irqObserved = (s.edges > 0 || s.lowSeen > 0);
        const bool shouldRead = edge || low || lastCount > 0 || (!irqObserved && (pollIter % 2 == 0));
        if (!shouldRead) {
            IOSleep(8);
            continue;
        }

        native_touch_loop_step = 2;
        s.reads++;
        int ids[12] = {}, rxs[12] = {}, rys[12] = {};
        unsigned n = 0;
        int rc = 0;
        if (s.kind == ChipKind::Fts8203) {
            rc = fts8203_frame(&s.fts, &s.ftsFrame);
            if (rc == FTS8203_FRAME_BOOT) {
                s.bad++;
                if (s.activeFw && s.activeFwSize) {
                    setSpiRate(s, kProbeHz);
                    fts8203_start(&s.fts, s.activeFw, s.activeFwSize);
                    setSpiRate(s, TB_SPI_DEFAULT_HZ);
                }
                IOSleep(8);
                continue;
            }
            if (rc == FTS8203_FRAME_EMPTY) {
                s.idleReads++;
                if (lastCount > 0) {
                    lastCount = 0;
                    native_touch_frames = native_touch_frames + 1;
                    publishContacts(s, 0, nullptr, nullptr, nullptr);
                    rows(s);
                } else if (s.reads <= 4 || (s.reads & 63u) == 0u) {
                    rows(s);
                }
                IOSleep(4);
                continue;
            }
            if (rc != 0) {
                s.bad++;
                IOSleep(8);
                continue;
            }
            for (unsigned i = 0; i < FTS8203_CONTACTS; ++i) {
                if (!(s.ftsFrame.active & (1u << i))) continue;
                ids[n] = static_cast<int>(i);
                rxs[n] = s.ftsFrame.x[i];
                rys[n] = s.ftsFrame.y[i];
                n++;
            }
        } else {
            rc = nvt_read_points(&s.nvt, &s.nvtPoints);
            if (rc != 0) {
                s.bad++;
                IOSleep(8);
                continue;
            }
            for (unsigned i = 0; i < s.nvtPoints.count && n < 12; ++i) {
                ids[n] = s.nvtPoints.points[i].id;
                rxs[n] = s.nvtPoints.points[i].x;
                rys[n] = s.nvtPoints.points[i].y;
                n++;
            }
        }

        native_touch_loop_step = 3;
        native_touch_valid = native_touch_valid + 1;
        if (n > 0) s.nonzero++;
        bool changed = (n != lastCount);
        for (unsigned i = 0; i < n && !changed; ++i) {
            if (static_cast<unsigned>(ids[i]) != lastId[i] ||
                static_cast<unsigned>(rxs[i]) != lastX[i] ||
                static_cast<unsigned>(rys[i]) != lastY[i]) {
                changed = true;
            }
        }
        if (changed || n > 0) {
            native_touch_frames = native_touch_frames + 1;
            publishContacts(s, n, ids, rxs, rys);
            lastCount = n;
            for (unsigned i = 0; i < n; ++i) {
                lastId[i] = static_cast<unsigned>(ids[i]);
                lastX[i] = static_cast<unsigned>(rxs[i]);
                lastY[i] = static_cast<unsigned>(rys[i]);
            }
        }
        if (s.reads <= 4 || (s.reads & 63u) == 0u || changed) rows(s);
        IOSleep(n > 0 ? 4 : 8);
    }
}

void touchThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "xun-touch");
    Session *s = new Session();
    if (!s) {fail(1, -12); return;}
    PE_parse_boot_argn("xun-touch-fw", &s->fwMode, sizeof s->fwMode);
    PE_parse_boot_argn("xun-touch-flip", &s->flip, sizeof s->flip);
    PE_parse_boot_argn("xun-touch-orient", &s->orient, sizeof s->orient);
    PE_parse_boot_argn("xun-touch-w", &s->rawWOverride, sizeof s->rawWOverride);
    PE_parse_boot_argn("xun-touch-h", &s->rawHOverride, sizeof s->rawHOverride);
    PE_parse_boot_argn("xun-touch-dma", &s->forceDma, sizeof s->forceDma);

    const unsigned stage = native_touch_stage_req;
    if (!snapshot(*s)) {rows(*s); delete s; return;}
    native_touch_stage_done = 1;
    if (stage < 2 || !enableBus(*s)) {rows(*s); delete s; return;}
    native_touch_stage_done = 2;
    if (stage < 3 || !openAndIdentify(*s)) {rows(*s); delete s; return;}
    native_touch_stage_done = 3;
    if (stage < 4 || !downloadFirmware(*s)) {rows(*s); delete s; return;}
    native_touch_stage_done = 4;
    if (stage < 5) {rows(*s); delete s; return;}
    native_touch_stage_done = 5;
    rows(*s);
    eventLoop(*s);
}
}  // namespace

extern "C" void native_touch_probe(void) {
    unsigned value = 0;
    if (!PE_parse_boot_argn("xun-touch", &value, sizeof value) &&
        !PE_parse_boot_argn("touch", &value, sizeof value) &&
        !PE_parse_boot_argn("tb128fu-touch", &value, sizeof value)) {
        return;
    }
    const unsigned stage = value & 0xffu;
    if (stage < 1 || native_touch_stage_req) return;
    native_touch_stage_req = stage;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_touch_error = -12;
}
