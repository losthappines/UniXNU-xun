// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>

#define SDHCI_BASE 0x04744000ULL

extern "C" {
volatile uint32_t native_emmc_probe_calls = 0;
volatile uint32_t native_emmc_matched = 0;
volatile uint32_t native_emmc_mapped = 0;
volatile uint32_t native_emmc_root_step = 0;
const char *volatile native_emmc_root_failure = nullptr;
}

class NativeEmmc : public IOService {
    OSDeclareDefaultStructors (NativeEmmc)
    IOMemoryMap *fMap;

public:
    IOService *probe (IOService *provider, SInt32 *score) override;
    bool start (IOService *provider) override;
    void stop (IOService *provider) override;
};

OSDefineMetaClassAndStructors (NativeEmmc, IOService)

IOService *NativeEmmc::probe (IOService *provider, SInt32 *score)
{
    native_emmc_probe_calls = native_emmc_probe_calls + 1;
    if (!IOService::probe (provider, score)) {
        return nullptr;
    }
    native_emmc_matched = native_emmc_matched + 1;
    *score = 2500;
    return this;
}

bool NativeEmmc::start (IOService *provider)
{
    if (!IOService::start (provider)) {
        return false;
    }
    native_emmc_root_step = 1;

    // Map SDHCI MMIO at 0x04744000
    fMap = provider->mapDeviceMemoryWithIndex (0);
    if (!fMap) {
        native_emmc_root_failure = "SDHCI MMIO map failed";
        return false;
    }
    native_emmc_mapped = 1;
    native_emmc_root_step = 2;

    IOLog("[NativeEmmc] initialized Qualcomm SDHCI MSM at 0x%08llx\n", SDHCI_BASE);

    // Register service
    registerService ();
    return true;
}

void NativeEmmc::stop (IOService *provider)
{
    if (fMap) {
        fMap->release ();
        fMap = nullptr;
    }
    IOService::stop (provider);
}
