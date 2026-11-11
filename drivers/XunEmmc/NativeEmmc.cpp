// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native Qualcomm SDHCI MSM eMMC 5.1 bring-up for Xiaomi Redmi Pad SE 11" (xun, SM6225).
// Base register: 0x04744000 (sdhci@4744000).
// 512-byte eMMC sectors.
// Userdata start LBA: 20129584 (0x1332730), last LBA: 244318207 (0xe8fffff).

#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMemoryMap.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include "RootLayout.h"

extern "C" {
volatile uint32_t native_emmc_probe_calls = 0;
volatile uint32_t native_emmc_matched = 0;
volatile uint32_t native_emmc_mapped = 0;
volatile uint32_t native_emmc_root_step = 0;
const char *volatile native_emmc_root_failure = nullptr;
}

class NativeEmmc : public IOService {
  OSDeclareDefaultStructors (NativeEmmc)
  OSSharedPtr<IOMemoryMap> fMap;
  RootLayout fLayout;

public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
};

OSDefineMetaClassAndStructors (NativeEmmc, IOService)

IOService *NativeEmmc::probe (IOService *provider, SInt32 *score)
{
  native_emmc_probe_calls++;
  if (!IOService::probe (provider, score)) {
    return nullptr;
  }
  native_emmc_matched++;
  *score = 2000;
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

  // Register service
  registerService ();
  return true;
}

void NativeEmmc::stop (IOService *provider)
{
  fMap.reset ();
  IOService::stop (provider);
}
