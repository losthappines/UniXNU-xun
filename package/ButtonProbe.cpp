// SPDX-License-Identifier: BSD-2-Clause-Patent
// Power and volume buttons of the xun board (SM6225 + PM6125 SPMI v5, boot argument buttons=1).
// Delivered as Consumer report 2 bits to drivers/ShengHidInput:
//   bit 1: Power (PM6125 PON PPID 0x008, IRQ bit 0 "kpdpwr")
//   bit 2: Volume Up (PM6125 GPIO5 PPID 0x0c4, active low IRQ bit 0)
//   bit 3: Volume Down (PM6125 PON PPID 0x008, IRQ bit 1 "resin")
// Register layout follows spmi-pmic-arb v5 (qcom,ee = 0, periph_irq GIC SPI 183 -> INTID 215):
//   obsrvr: 0x10000 * ee + 0x80 * apid
//   chnls:  0x10000 * apid (with +0x100 ACC_ENABLE, +0x104 IRQ_STATUS, +0x108 IRQ_CLEAR)
//   intr:   0x10000 * ee + 0x4 * word
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "BootResetCore.h"
}

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" int armcpu_spi_mask(uint32_t, bool) __attribute__((weak_import));

// [0] stage (1 mapped, 2 configured, 3 armed, -9 masked after an interrupt storm, other negatives: step failed)
// [1] interrupts, [2] reports, [3] pressed mask, [4] PON APID, [5] GPIO APID,
// [6] unexpected APIDs cleared, [7] reserved, [8] last error, [9] spurious interrupts
extern "C" {volatile int32_t native_buttons_stats[10];}

namespace {
constexpr uint64_t kCore = 0x1c40000, kChnls = 0x1e00000, kObsrvr = 0x3e00000, kIntr = 0x3f00000, kCnfg = 0x1c0a000;
constexpr uint32_t kEe = 0;
constexpr uint32_t kPonPpid = 0x008, kGpioPpid = 0x0c4;
constexpr uint8_t kPowerBit = 1u << 0, kResinBit = 1u << 1, kGpioBit = 1u << 0;
constexpr uint32_t kAccEnable = 0x100, kIrqStatus = 0x104, kIrqClear = 0x108;
constexpr uint8_t kRtSts = 0x10, kSetType = 0x11, kPolHigh = 0x12, kPolLow = 0x13, kLatchedClr = 0x14, kEnSet = 0x15;
constexpr uint32_t kSpi = 183, kIntid = 32u + kSpi;
constexpr uint32_t kReportPower = 1u << 1, kReportVolumeUp = 1u << 2, kReportVolumeDown = 1u << 3;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
Window gCore, gCnfg, gObs, gChn, gIntr;
uint32_t gApids;
int gPon = -1, gGpio = -1;
bool gGpioIrqOwned = false;
volatile uint32_t gEvents;
volatile uint32_t gIrqSeen;
void (*volatile gSink)(void *, uint32_t);
void *volatile gSinkContext;

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

volatile uint32_t *obsPtr(int apid) {
    return gObs.va + (BR_OBS_EE_STRIDE * kEe + BR_OBS_APID_STRIDE * static_cast<uint32_t>(apid)) / 4;
}

volatile uint32_t *chnPtr(int apid) {
    return gChn.va + (BR_CHNL_APID_STRIDE * static_cast<uint32_t>(apid)) / 4;
}

volatile uint32_t *apidReg(int apid, uint32_t reg) {
    return chnPtr(apid) + reg / 4;
}

void fail(int step, int error) {
    native_buttons_stats[8] = error;
    native_buttons_stats[0] = -step;
}

int findApid(uint32_t ppid, bool requireIrqOwner) {
    int found = -1;
    for (uint32_t i = 0; i < gApids; ++i) {
        uint32_t map = gCore.va[(BR_APID_MAP + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        const bool irqOwner = ((map >> 24) & 1u) != 0 &&
                              (gCnfg.va[(BR_CNFG_OWNER + 4 * i) / 4] & 7u) == kEe;
        if (!requireIrqOwner || irqOwner) found = static_cast<int>(i);
    }
    return found;
}

int armPeripheral(int apid, uint8_t bits) {
    for (unsigned attempt = 0; ; ++attempt) {
        uint8_t type = 0, high = 0, low = 0;
        volatile uint32_t *obs = obsPtr(apid);
        volatile uint32_t *chn = chnPtr(apid);
        unsigned step = 1;
        int rc = br_read8(obs, kSetType, &type);
        if (!rc) {step = 2; rc = br_read8(obs, kPolHigh, &high);}
        if (!rc) {step = 3; rc = br_read8(obs, kPolLow, &low);}
        if (!rc) {step = 4; rc = br_write8(chn, kSetType, static_cast<uint8_t>(type | bits));}
        if (!rc) {step = 5; rc = br_write8(chn, kPolHigh, static_cast<uint8_t>(high | bits));}
        if (!rc) {step = 6; rc = br_write8(chn, kPolLow, static_cast<uint8_t>(low | bits));}
        if (!rc) {step = 7; rc = br_write8(chn, kLatchedClr, bits);}
        if (!rc) {step = 8; rc = br_write8(chn, kEnSet, bits);}
        if (!rc) {*apidReg(apid, kAccEnable) = 1; return 0;}
        if (attempt == 3) return static_cast<int>((step << 2) | static_cast<unsigned>(rc));
        IOSleep(25);
    }
}

void buttonsIrq(void *) {
    bool ours = false;
    volatile uint32_t *intr = gIntr.va + (0x10000u * kEe) / 4;
    for (uint32_t word = 0; word * 32 < gApids; ++word) {
        uint32_t pending = intr[word];
        while (pending) {
            const int apid = static_cast<int>(word * 32 + __builtin_ctz(pending));
            pending &= pending - 1;
            if (apid != gPon && !(gGpioIrqOwned && apid == gGpio)) {
                *apidReg(apid, kIrqClear) = 0xff;
                native_buttons_stats[6] = native_buttons_stats[6] + 1;
                continue;
            }
            const uint32_t status = *apidReg(apid, kIrqStatus);
            *apidReg(apid, kIrqClear) = status;
            if (status) br_write8(chnPtr(apid), kLatchedClr, static_cast<uint8_t>(status));
            ours = true;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    static uint32_t idle;
    if (!ours) {
        native_buttons_stats[9] = native_buttons_stats[9] + 1;
        if (++idle >= 1000) {
            if (armcpu_spi_mask) armcpu_spi_mask(kIntid, true);
            native_buttons_stats[0] = -9;
        }
        return;
    }
    idle = 0;
    __atomic_store_n(&gIrqSeen, 1u, __ATOMIC_RELEASE);
    native_buttons_stats[1] = native_buttons_stats[1] + 1;
    __atomic_fetch_add(&gEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gEvents)));
}

uint32_t readPressedMask() {
    uint8_t pon = 0, gpio = 0xff;
    uint32_t mask = 0;
    if (gPon >= 0 && !br_read8(obsPtr(gPon), kRtSts, &pon)) {
        if (pon & kPowerBit) mask |= kReportPower;
        if (pon & kResinBit) mask |= kReportVolumeDown;
    }
    if (gGpio >= 0 && !br_read8(obsPtr(gGpio), kRtSts, &gpio) && !(gpio & kGpioBit)) {
        mask |= kReportVolumeUp;
    }
    return mask;
}

bool setup() {
    if (!mapWindow(gCore, kCore, 0x3000) || !mapWindow(gCnfg, kCnfg, 0x4000) || !mapWindow(gIntr, kIntr, 0x1000)) {
        fail(1, -12);
        return false;
    }
    const uint32_t version = gCore.va[0];
    gApids = gCore.va[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
    if (version < BR_VERSION_V5_MIN || version >= BR_VERSION_V7_MIN || !gApids || gApids > BR_MAX_APIDS) {
        fail(2, static_cast<int>(version));
        return false;
    }
    const uint64_t obsSize = ((static_cast<uint64_t>(BR_OBS_EE_STRIDE) * kEe +
                               static_cast<uint64_t>(BR_OBS_APID_STRIDE) * gApids) + 0xfff) & ~0xfffull;
    if (!mapWindow(gObs, kObsrvr, obsSize) ||
        !mapWindow(gChn, kChnls, static_cast<uint64_t>(BR_CHNL_APID_STRIDE) * gApids)) {
        fail(3, -12);
        return false;
    }
    native_buttons_stats[0] = 1;

    gPon = findApid(kPonPpid, true);
    if (gPon < 0) gPon = findApid(kPonPpid, false);
    gGpio = findApid(kGpioPpid, true);
    if (gGpio >= 0) {
        gGpioIrqOwned = true;
    } else {
        gGpio = findApid(kGpioPpid, false);
        gGpioIrqOwned = false;
    }
    native_buttons_stats[4] = gPon;
    native_buttons_stats[5] = gGpio;
    if (gPon < 0) {fail(4, -2); return false;}

    int rc = armPeripheral(gPon, kPowerBit | kResinBit);
    if (!rc && gGpio >= 0 && gGpioIrqOwned) rc = armPeripheral(gGpio, kGpioBit);
    if (rc) {fail(5, rc); return false;}
    native_buttons_stats[0] = 2;

    if (!armcpu_spi_attach) {
        native_buttons_stats[8] = -78;
        native_buttons_stats[0] = 2;
        return true;
    }
    for (unsigned tries = 0; ; ++tries) {
        rc = armcpu_spi_attach(kIntid, false, buttonsIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || tries == 240) break;
        IOSleep(250);
    }
    if (rc) {
        // Keep observer-channel polling active even if GIC SPI attach fails.
        native_buttons_stats[8] = rc;
        native_buttons_stats[0] = 2;
        return true;
    }
    native_buttons_stats[0] = 3;
    return true;
}

void buttonsThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "xun-buttons");
    if (!setup()) return;
    uint32_t seen = gEvents, last = 0;
    uint64_t pollTicks = 0;
    nanoseconds_to_absolutetime(25ull * 1000 * 1000, &pollTicks);
    for (;;) {
        const uint32_t mask = readPressedMask();
        if (mask != last) {
            last = mask;
            native_buttons_stats[3] = static_cast<int32_t>(mask);
            void (*sink)(void *, uint32_t) = __atomic_load_n(&gSink, __ATOMIC_ACQUIRE);
            if (sink) {
                sink(gSinkContext, mask);
                native_buttons_stats[2] = native_buttons_stats[2] + 1;
            }
        }
        const bool irqActive = __atomic_load_n(&gIrqSeen, __ATOMIC_ACQUIRE) != 0 && gGpioIrqOwned;
        if (irqActive) {
            assert_wait(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gEvents)), THREAD_UNINT);
        } else {
            assert_wait_deadline(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gEvents)), THREAD_UNINT,
                                 mach_absolute_time() + pollTicks);
        }
        if (__atomic_load_n(&gEvents, __ATOMIC_ACQUIRE) != seen) {
            clear_wait(current_thread(), THREAD_AWAKENED);
        } else {
            thread_block(THREAD_CONTINUE_NULL);
        }
        seen = __atomic_load_n(&gEvents, __ATOMIC_ACQUIRE);
    }
}
}  // namespace

extern "C" void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {
    gSinkContext = context;
    __atomic_store_n(&gSink, sink, __ATOMIC_RELEASE);
}

extern "C" bool xun_buttons_press(uint32_t mask, uint32_t ms) {
    void (*sink)(void *, uint32_t) = __atomic_load_n(&gSink, __ATOMIC_ACQUIRE);
    if (!sink) return false;
    sink(gSinkContext, mask);
    IOSleep(ms);
    sink(gSinkContext, 0);
    native_buttons_stats[2] = native_buttons_stats[2] + 2;
    return true;
}

extern "C" void xun_buttons_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("buttons", &enabled, sizeof enabled) || enabled != 1 || native_buttons_stats[0]) return;
    native_buttons_stats[0] = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(buttonsThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(7, -12);
}
