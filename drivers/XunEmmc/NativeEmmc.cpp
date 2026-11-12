// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native Qualcomm SDHCI MSM eMMC 5.1 root block driver for Xiaomi Redmi Pad SE 11" (xun, SM6225).
// Base register: 0x04744000 (sdhci@4744000).
// 512-byte eMMC sectors.
// Userdata start LBA: 20129584 (0x1332730), last LBA: 244318207 (0xe8fffff).
// Target Root volume UUID: 04769D26-2CA0-4825-B443-E2E5F88300F2 (iPadOS 17.7.11 21H461).

#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include "RootLayout.h"

extern "C" {
volatile uint32_t native_emmc_probe_calls = 0;
volatile uint32_t native_emmc_matched = 0;
volatile uint32_t native_emmc_mapped = 0;
volatile uint32_t native_emmc_root_step = 0;
volatile uint64_t native_emmc_blocks_read = 0;
volatile uint64_t native_emmc_read_errors = 0;
const char *volatile native_emmc_root_failure = nullptr;
}

class NativeEmmc : public IOBlockStorageDevice {
    OSDeclareDefaultStructors (NativeEmmc)

    IOMemoryMap *fMap = nullptr;
    uintptr_t fBase = 0;
    uint64_t fCapacity = 0;
    bool fOnline = false;

    inline uint32_t read32(unsigned offset) const {
        return *reinterpret_cast<volatile uint32_t *>(fBase + offset);
    }
    inline uint16_t read16(unsigned offset) const {
        return *reinterpret_cast<volatile uint16_t *>(fBase + offset);
    }
    inline void write32(unsigned offset, uint32_t val) {
        *reinterpret_cast<volatile uint32_t *>(fBase + offset) = val;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    inline void write16(unsigned offset, uint16_t val) {
        *reinterpret_cast<volatile uint16_t *>(fBase + offset) = val;
        __asm__ volatile("dsb oshst" ::: "memory");
    }

    bool readSectorPIO(uint64_t lba, void *dest);

public:
    IOService *probe (IOService *provider, SInt32 *score) override;
    bool start (IOService *provider) override;
    void stop (IOService *provider) override;
    void free () override;

    // IOBlockStorageDevice implementation
    IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
    IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
    char *getVendorString() override { return const_cast<char *>("Qualcomm"); }
    char *getProductString() override { return const_cast<char *>("eMMC 5.1 SDHCI"); }
    char *getRevisionString() override { return const_cast<char *>("1.0"); }
    char *getAdditionalDeviceInfoString() override { return const_cast<char *>("Redmi Pad SE xun eMMC"); }
    IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
    IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
    IOReturn reportMaxValidBlock(UInt64 *value) override {
        *value = fCapacity ? (fCapacity - 1) : (XUN_USERDATA_LAST_LBA - XUN_USERDATA_FIRST_LBA);
        return kIOReturnSuccess;
    }
    IOReturn reportMediaState(bool *present, bool *changed) override {
        *present = fOnline; if (changed) *changed = false; return kIOReturnSuccess;
    }
    IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
    IOReturn reportWriteProtection(bool *value) override { *value = true; return kIOReturnSuccess; }
    IOReturn doSynchronizeCache() override { return fOnline ? kIOReturnSuccess : kIOReturnNotReady; }
    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
    IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
    IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                              IOStorageAttributes *attributes, IOStorageCompletion *completion) override;

    bool authenticatesVolume(const char *uuid, uint8_t *digest = nullptr) const;
};

OSDefineMetaClassAndStructors (NativeEmmc, IOBlockStorageDevice)

IOService *NativeEmmc::probe (IOService *provider, SInt32 *score)
{
    native_emmc_probe_calls = native_emmc_probe_calls + 1;
    if (!IOBlockStorageDevice::probe (provider, score)) {
        return nullptr;
    }
    native_emmc_matched = native_emmc_matched + 1;
    *score = 2500;
    return this;
}

bool NativeEmmc::readSectorPIO(uint64_t lba, void *dest)
{
    if (!fBase) return false;

    // Wait for Command & Data Inhibit to clear in Present State (0x24)
    unsigned timeout = 100000;
    while ((read32(0x24) & 0x00000003) && --timeout) {
        IODelay(1);
    }
    if (!timeout) {
        // Reset CMD and DAT lines if hung
        write16(0x2F, 0x0006);
        return false;
    }

    // Clear interrupt status flags
    write32(0x30, 0xFFFFFFFF);

    // Set Block Size (512 bytes) and Block Count (1)
    write16(0x04, 512);
    write16(0x06, 1);

    // Set Argument to sector LBA (eMMC HC devices use sector addressing)
    write32(0x08, static_cast<uint32_t>(lba));

    // Transfer mode: Read direction, single block
    write16(0x0C, 0x0010);

    // Command: CMD17 (READ_SINGLE_BLOCK) with 48-bit R1 response and data present
    write16(0x0E, 0x113A);

    // Wait for Command Complete (bit 0 of 0x30)
    timeout = 50000;
    while (!(read16(0x30) & 0x0001) && --timeout) {
        if (read16(0x32) & 0x8000) return false; // Error interrupt
        IODelay(1);
    }
    write16(0x30, 0x0001); // Clear Command Complete

    // Wait for Buffer Read Ready (bit 5 of 0x30 or bit 11 of 0x24)
    timeout = 100000;
    while (!(read16(0x30) & 0x0020) && !(read32(0x24) & 0x00000800) && --timeout) {
        IODelay(1);
    }
    write16(0x30, 0x0020); // Clear Buffer Read Ready

    // Read 128 32-bit words (512 bytes) from the internal Buffer Data Port (0x20)
    uint32_t *p = static_cast<uint32_t *>(dest);
    for (int w = 0; w < 128; w++) {
        p[w] = read32(0x20);
    }

    // Wait for Transfer Complete (bit 1 of 0x30)
    timeout = 50000;
    while (!(read16(0x30) & 0x0002) && --timeout) {
        IODelay(1);
    }
    write16(0x30, 0x0002); // Clear Transfer Complete

    return true;
}

bool NativeEmmc::start (IOService *provider)
{
    if (!IOBlockStorageDevice::start (provider)) {
        return false;
    }
    native_emmc_root_step = 1;

    // Map SDHCI MMIO at 0x04744000
    fMap = provider->mapDeviceMemoryWithIndex (0);
    if (!fMap) {
        native_emmc_root_failure = "SDHCI MMIO map failed";
        return false;
    }
    fBase = static_cast<uintptr_t>(fMap->getVirtualAddress());
    native_emmc_mapped = 1;
    native_emmc_root_step = 2;

    // Verify and configure SD clock in Clock Control register (0x2C)
    uint16_t clk = read16(0x2C);
    if ((clk & 0x0007) != 0x0007) {
        write16(0x2C, clk | 0x0001); // Internal Clock Enable
        IODelay(100);
        write16(0x2C, read16(0x2C) | 0x0006); // SD Clock Enable & Frequency Select
    }

    // Userdata partition capacity on xun: 224,188,624 sectors (approx 106.9 GiB)
    fCapacity = XUN_USERDATA_LAST_LBA - XUN_USERDATA_FIRST_LBA + 1;
    fOnline = true;
    native_emmc_root_step = 3;

    IOLog("NativeEmmc: published eMMC block storage device (start LBA 0x%llx, capacity %llu blocks)\n",
          XUN_USERDATA_FIRST_LBA, fCapacity);

    // Register this device with IOKit so IOBlockStorageDriver binds and creates IOMedia
    registerService ();
    return true;
}

IOReturn NativeEmmc::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                      IOStorageAttributes *, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !completion->action || !count || block >= fCapacity ||
        count > fCapacity - block) {
        return kIOReturnBadArgument;
    }
    if (buffer->getDirection() != kIODirectionIn) {
        return kIOReturnNotWritable;
    }

    uint64_t actual = 0;
    IOReturn result = kIOReturnSuccess;
    uint8_t scratch[512];

    for (UInt64 i = 0; i < count; i++) {
        uint64_t lba = XUN_USERDATA_FIRST_LBA + block + i;
        if (!readSectorPIO(lba, scratch)) {
            native_emmc_read_errors = native_emmc_read_errors + 1;
            result = kIOReturnIOError;
            break;
        }
        if (buffer->writeBytes(actual, scratch, 512) != 512) {
            result = kIOReturnIOError;
            break;
        }
        actual += 512;
        native_emmc_blocks_read = native_emmc_blocks_read + 1;
    }

    completion->action(completion->target, completion->parameter, result, actual);
    return kIOReturnSuccess;
}

bool NativeEmmc::authenticatesVolume (const char *uuid, uint8_t *) const
{
    if (!uuid) return false;
    // Pinned OS volume UUID: 04769D26-2CA0-4825-B443-E2E5F88300F2
    if (!strcasecmp(uuid, "04769D26-2CA0-4825-B443-E2E5F88300F2")) {
        return true;
    }
    return false;
}

void NativeEmmc::stop (IOService *provider)
{
    fOnline = false;
    if (fMap) {
        fMap->release ();
        fMap = nullptr;
    }
    fBase = 0;
    IOBlockStorageDevice::stop (provider);
}

void NativeEmmc::free ()
{
    if (fMap) {
        fMap->release ();
        fMap = nullptr;
    }
    IOBlockStorageDevice::free ();
}
