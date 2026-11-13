// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>

#include "RootLayout.h"
#include "SdhciMsm.h"

#ifndef XNU_OWNER_ROOT_VOLUME_UUID
#define XNU_OWNER_ROOT_VOLUME_UUID "04769D26-2CA0-4825-B443-E2E5F88300F2"
#endif

#ifndef XNU_OWNER_CACHE_VOLUME_UUID
#define XNU_OWNER_CACHE_VOLUME_UUID "8846FE78-2910-4297-AF97-D59BB6F8FC16"
#endif

extern "C" {
volatile uint32_t native_emmc_ready = 0;
volatile uint32_t native_emmc_stage = 0;
volatile uint32_t native_emmc_controller_stage = 0;
volatile uint32_t native_emmc_layout_fail = 0;
volatile uint32_t native_emmc_rca = 0;
volatile uint32_t native_emmc_dma_enabled = 0;
volatile uint32_t native_emmc_fallback_reinit = 0;
volatile uint32_t native_emmc_host_version = 0;
volatile uint32_t native_emmc_caps0 = 0;
volatile uint32_t native_emmc_vendor_spec = 0;
volatile uint32_t native_emmc_last_err_int = 0;
volatile uint32_t native_emmc_last_int = 0;
volatile uint64_t native_emmc_sectors = 0;
volatile uint64_t native_emmc_userdata_first_lba = 0;
volatile uint64_t native_emmc_userdata_last_lba = 0;
volatile uint64_t native_emmc_reads = 0;
volatile uint64_t native_emmc_read_errors = 0;

volatile uint32_t native_verified_block_root_device = 0;
volatile uint32_t native_verified_block_root_media_matches = 0;
volatile uint32_t native_verified_block_root_provider_depth = 0;
volatile uint32_t native_verified_block_root_uuid_match = 0;
volatile uint32_t native_verified_block_wrong_uuid_refused = 0;
char native_verified_block_root_uuid[37] = {};

extern const uint8_t verified_root_key[] __attribute__((weak_import));

int native_verified_block_root_authorized(dev_t device);
int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]);
int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size);
int native_ufs_block_authenticates(IOService *provider, const char *uuid, uint8_t *digest);

unsigned native_ufs_store_mode(void);
int native_ufs_store_open(uint64_t minBytes, uint64_t *actualBytes, const char **kind);
int native_ufs_store_read(uint64_t offset, uint32_t bytes, void *dst);
int native_ufs_store_write(uint64_t offset, uint32_t bytes, const void *src);
int native_ufs_store_write_cached(uint64_t offset, uint32_t bytes, const void *src);
int native_ufs_store_sync(void);
unsigned native_ufs_store_gpt_check(void);
}

namespace {

using xun_emmc::SdhciController;

struct ControllerState {
  SdhciController sdhci;
  IOLock *gate = nullptr;
  EmmcRootLayout layout = {};
  uint8_t *bounceBuf = nullptr;
  static constexpr size_t kBounceBytes = xun_emmc::kMaxChunkSectors * kEmmcSectorBytes;
  bool dataStoreOpen = false;
  RootWindowDesc dataWindow = {};
};

ControllerState *gCtrlState = nullptr;

void sha256Adapter(const void *data, size_t len, uint8_t out[32]) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, data, len);
  SHA256_Final(out, &ctx);
}

bool asciiCaseEquals(const char *a, const char *b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char ca = *a;
    char cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
    if (ca != cb) return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

bool dataContainsToken(OSData *data, const char *token) {
  if (!data || !token) return false;
  const size_t tlen = strlen(token);
  const uint8_t *bytes = static_cast<const uint8_t *>(data->getBytesNoCopy());
  const size_t len = data->getLength();
  if (!bytes || len < tlen) return false;
  size_t pos = 0;
  while (pos < len) {
    const size_t rem = len - pos;
    const size_t slen = strnlen(reinterpret_cast<const char *>(bytes + pos), rem);
    if (slen == tlen && memcmp(bytes + pos, token, tlen) == 0) return true;
    pos += slen + 1;
  }
  return false;
}

}  // namespace

class NativeEmmcBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeEmmcBlock)

  ControllerState *ctrl = nullptr;
  RootWindowDesc window = {};
  char expectedUuid[37] = {};
  char deviceName[40] = {};
  bool online = false;
  bool readOnly = true;

 public:
  bool initWithWindow(ControllerState *state, const RootWindowDesc &win, const char *uuid, bool ro);
  bool authenticatesVolume(const char *uuid, uint8_t *manifestDigest = nullptr) const;

  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  void free() override;

  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *capacities, UInt32 capacitiesMaxCount) const override {
    if (capacities && capacitiesMaxCount >= 1) {
      capacities[0] = window.byteSize;
      return 1;
    }
    return 0;
  }
  char *getVendorString() override { return const_cast<char *>("Qualcomm"); }
  char *getProductString() override { return deviceName; }
  char *getRevisionString() override { return const_cast<char *>("5.1"); }
  char *getAdditionalDeviceInfoString() override {
    return const_cast<char *>("SDHCI MSM eMMC 5.1");
  }
  IOReturn reportBlockSize(UInt64 *value) override {
    *value = kEmmcSectorBytes;
    return kIOReturnSuccess;
  }
  IOReturn reportEjectability(bool *value) override {
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportMaxValidBlock(UInt64 *value) override {
    *value = window.sectorCount ? (window.sectorCount - 1ULL) : 0ULL;
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState(bool *mediaPresent, bool *changedState) override {
    *mediaPresent = online;
    if (changedState) *changedState = false;
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override {
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportWriteProtection(bool *value) override {
    *value = readOnly;
    return kIOReturnSuccess;
  }
  IOReturn doSynchronizeCache() override {
    if (!online) return kIOReturnNotReady;
    if (readOnly) return kIOReturnSuccess;
    IOLockLock(ctrl->gate);
    bool ok = ctrl->sdhci.flushCache();
    IOLockUnlock(ctrl->gate);
    return ok ? kIOReturnSuccess : kIOReturnIOError;
  }
  IOReturn doSynchronize(UInt64, UInt64, IOStorageSynchronizeOptions) override {
    return doSynchronizeCache();
  }
  IOReturn getWriteCacheState(bool *value) override {
    *value = !readOnly;
    return kIOReturnSuccess;
  }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                            IOStorageAttributes *attributes,
                            IOStorageCompletion *completion) override;
};

OSDefineMetaClassAndStructors(NativeEmmcBlock, IOBlockStorageDevice)

bool NativeEmmcBlock::initWithWindow(ControllerState *state, const RootWindowDesc &win,
                                     const char *uuid, bool ro) {
  if (!IOBlockStorageDevice::init(nullptr)) return false;
  if (!state || !win.present || !win.sectorCount || !uuid) return false;
  ctrl = state;
  window = win;
  readOnly = ro;
  strlcpy(expectedUuid, uuid, sizeof(expectedUuid));
  snprintf(deviceName, sizeof(deviceName), "Xun eMMC %s", win.name);
  setName(win.name);
  setProperty("EmmcWindowName", win.name);
  setProperty("EmmcStartLBA", static_cast<unsigned long long>(win.startLba), 64);
  setProperty("EmmcSectorCount", static_cast<unsigned long long>(win.sectorCount), 64);
  return true;
}

IOService *NativeEmmcBlock::probe(IOService *provider, SInt32 *score) {
  return IOBlockStorageDevice::probe(provider, score);
}

bool NativeEmmcBlock::start(IOService *provider) {
  if (!IOBlockStorageDevice::start(provider)) return false;
  if (!ctrl || !window.sectorCount) return false;

  // Smoke-read sector 0 of this window before publishing IOMedia.
  IOLockLock(ctrl->gate);
  bool ok = ctrl->sdhci.readSectors(window.startLba, 1U, ctrl->bounceBuf);
  IOLockUnlock(ctrl->gate);
  if (!ok) return false;

  online = true;
  registerService();
  return true;
}

void NativeEmmcBlock::stop(IOService *provider) {
  online = false;
  IOBlockStorageDevice::stop(provider);
}

void NativeEmmcBlock::free() {
  IOBlockStorageDevice::free();
}

bool NativeEmmcBlock::authenticatesVolume(const char *uuid, uint8_t *manifestDigest) const {
  if (!online || !uuid || !asciiCaseEquals(uuid, expectedUuid)) return false;
  if (!ctrl || !ctrl->layout.manifestValid || !window.present) return false;
  if (manifestDigest) {
    static constexpr char domain[] = "Xun.ShngRoot.v1";
    SHA384_CTX ctx;
    SHA384_Init(&ctx);
    SHA384_Update(&ctx, domain, sizeof(domain));
    SHA384_Update(&ctx, window.name, sizeof(window.name));
    SHA384_Update(&ctx, window.sha256, sizeof(window.sha256));
    SHA384_Update(&ctx, &window.byteSize, sizeof(window.byteSize));
    SHA384_Final(manifestDigest, &ctx);
  }
  return true;
}

IOReturn NativeEmmcBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                           IOStorageAttributes *, IOStorageCompletion *completion) {
  if (!buffer || !completion || !completion->action || !count ||
      block >= window.sectorCount || count > window.sectorCount - block ||
      count > UINT64_MAX / kEmmcSectorBytes ||
      buffer->getLength() < count * kEmmcSectorBytes) {
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection() != kIODirectionIn) {
    return kIOReturnNotWritable;
  }

  UInt64 actual = 0;
  IOReturn result = kIOReturnNotReady;
  const UInt64 totalBytes = count * kEmmcSectorBytes;

  IOLockLock(ctrl->gate);
  while (online && actual < totalBytes) {
    const uint32_t remainSectors = static_cast<uint32_t>((totalBytes - actual) / kEmmcSectorBytes);
    const uint32_t chunkSectors = (remainSectors > xun_emmc::kMaxChunkSectors)
                                      ? xun_emmc::kMaxChunkSectors
                                      : remainSectors;
    const uint64_t lba = window.startLba + block + (actual / kEmmcSectorBytes);
    if (!ctrl->sdhci.readSectors(lba, chunkSectors, ctrl->bounceBuf)) {
      native_emmc_last_err_int = ctrl->sdhci.lastErrInt;
      native_emmc_last_int = ctrl->sdhci.lastIntStatus;
      native_emmc_read_errors = ctrl->sdhci.readErrors;
      result = kIOReturnIOError;
      break;
    }
    const uint32_t chunkBytes = chunkSectors * kEmmcSectorBytes;
    if (buffer->writeBytes(actual, ctrl->bounceBuf, chunkBytes) != chunkBytes) {
      result = kIOReturnIOError;
      break;
    }
    actual += chunkBytes;
    result = kIOReturnSuccess;
  }
  native_emmc_reads = ctrl->sdhci.readOps;
  IOLockUnlock(ctrl->gate);

  completion->action(completion->target, completion->parameter, result, actual);
  return kIOReturnSuccess;
}

// ============================================================================
// NativeUfs controller class (matches upstream KernelConfigTables XNU_NATIVE_UFS)
// ============================================================================

class NativeUfs : public IOService {
  OSDeclareDefaultStructors(NativeUfs)

  ControllerState *state = nullptr;
  NativeEmmcBlock *systemOsDev = nullptr;
  NativeEmmcBlock *osDev = nullptr;

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  void free() override;
};

OSDefineMetaClassAndStructors(NativeUfs, IOService)

IOService *NativeUfs::probe(IOService *provider, SInt32 *score) {
  if (!provider) return nullptr;
  const char *name = provider->getName();
  OSData *compat = OSDynamicCast(OSData, provider->getProperty("compatible"));
  const bool matched =
      (name && strcmp(name, "sdhci") == 0) ||
      dataContainsToken(compat, "qcom,sdhci-msm-v5") ||
      dataContainsToken(compat, "qcom,sdhci-msm");
  if (!matched) return nullptr;
  if (score) *score = 2000;
  return this;
}

bool NativeUfs::start(IOService *provider) {
  if (!IOService::start(provider)) return false;

  native_emmc_stage = 1;
  state = static_cast<ControllerState *>(IOMallocZero(sizeof(ControllerState)));
  if (!state) return false;
  state->gate = IOLockAlloc();
  state->bounceBuf = static_cast<uint8_t *>(IOMallocZero(ControllerState::kBounceBytes));
  if (!state->gate || !state->bounceBuf) return false;

  uint32_t dmaArg = 0;
  PE_parse_boot_argn("native-emmc-dma", &dmaArg, sizeof(dmaArg));
  native_emmc_dma_enabled = dmaArg ? 1U : 0U;

  native_emmc_stage = 2;
  if (!state->sdhci.init(provider, dmaArg != 0U)) {
    native_emmc_controller_stage = state->sdhci.stage;
    native_emmc_last_err_int = state->sdhci.lastErrInt;
    native_emmc_last_int = state->sdhci.lastIntStatus;
    native_emmc_vendor_spec = state->sdhci.vendorSpec;
    return false;
  }

  native_emmc_controller_stage = state->sdhci.stage;
  native_emmc_rca = state->sdhci.rca;
  native_emmc_sectors = state->sdhci.sectorCount;
  native_emmc_fallback_reinit = state->sdhci.fallbackReinitUsed ? 1U : 0U;
  native_emmc_host_version = state->sdhci.hostVersion;
  native_emmc_caps0 = state->sdhci.caps0;
  native_emmc_vendor_spec = state->sdhci.vendorSpec;

  native_emmc_stage = 3;
  uint8_t *scratch = static_cast<uint8_t *>(IOMallocZero(16384U));
  if (!scratch) return false;

  auto readFn = [this](uint64_t lba, uint32_t cnt, void *dst) -> bool {
    return state->sdhci.readSectors(lba, cnt, dst);
  };
  bool layoutOk = parseEmmcRootLayout(readFn, sha256Adapter, scratch, 16384U, &state->layout);
  IOFree(scratch, 16384U);
  if (!layoutOk) {
    native_emmc_layout_fail = state->layout.failStep;
    native_emmc_last_err_int = state->sdhci.lastErrInt;
    native_emmc_last_int = state->sdhci.lastIntStatus;
    return false;
  }

  native_emmc_userdata_first_lba = state->layout.userdataFirstLba;
  native_emmc_userdata_last_lba = state->layout.userdataLastLba;
  state->dataWindow = state->layout.data;
  gCtrlState = state;

  native_emmc_stage = 4;
  // Ordered publication: SystemOS first, OS last so IOKitBSDInit mounts OS
  // only once SystemOS cryptex media is also ready in the registry.
  systemOsDev = new NativeEmmcBlock;
  if (!systemOsDev ||
      !systemOsDev->initWithWindow(state, state->layout.systemOs, XNU_OWNER_CACHE_VOLUME_UUID, true) ||
      !systemOsDev->attach(this) ||
      !systemOsDev->start(this)) {
    return false;
  }

  native_emmc_stage = 5;
  osDev = new NativeEmmcBlock;
  if (!osDev ||
      !osDev->initWithWindow(state, state->layout.os, XNU_OWNER_ROOT_VOLUME_UUID, true) ||
      !osDev->attach(this) ||
      !osDev->start(this)) {
    return false;
  }

  setProperty("EmmcSectors", static_cast<unsigned long long>(state->sdhci.sectorCount), 64);
  setProperty("UserdataFirstLBA", static_cast<unsigned long long>(state->layout.userdataFirstLba), 64);
  setProperty("UserdataLastLBA", static_cast<unsigned long long>(state->layout.userdataLastLba), 64);
  setProperty("EmmcPioDefault", state->sdhci.useDma ? kOSBooleanFalse : kOSBooleanTrue);

  native_emmc_stage = 6;
  native_emmc_ready = 1;
  registerService();
  return true;
}

void NativeUfs::stop(IOService *provider) {
  if (osDev) { osDev->stop(this); osDev->detach(this); OSSafeReleaseNULL(osDev); }
  if (systemOsDev) { systemOsDev->stop(this); systemOsDev->detach(this); OSSafeReleaseNULL(systemOsDev); }
  IOService::stop(provider);
}

void NativeUfs::free() {
  if (gCtrlState == state) gCtrlState = nullptr;
  if (state) {
    state->sdhci.release();
    if (state->bounceBuf) { IOFree(state->bounceBuf, ControllerState::kBounceBytes); state->bounceBuf = nullptr; }
    if (state->gate) { IOLockFree(state->gate); state->gate = nullptr; }
    IOFree(state, sizeof(ControllerState));
    state = nullptr;
  }
  IOService::free();
}

// ============================================================================
// XNU VerifiedRoot / OwnerRoot & Store hooks
// ============================================================================

namespace {

int verifiedVolumeQuery(dev_t device, const char *requestedUuid, uint8_t manifestDigest[48]) {
  native_verified_block_root_device = static_cast<uint32_t>(device);
  native_verified_block_root_media_matches = 0;
  native_verified_block_root_provider_depth = 0;
  native_verified_block_root_uuid_match = 0;
  native_verified_block_wrong_uuid_refused = 0;
  bzero(native_verified_block_root_uuid, sizeof(native_verified_block_root_uuid));

  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(2);
  const uint32_t devRaw = static_cast<uint32_t>(device);
  OSNumber *wantedMajor = OSNumber::withNumber(static_cast<unsigned long long>(major(devRaw)), 32);
  OSNumber *wantedMinor = OSNumber::withNumber(static_cast<unsigned long long>(minor(devRaw)), 32);
  if (!matching || !properties || !wantedMajor || !wantedMinor) {
    OSSafeReleaseNULL(wantedMinor);
    OSSafeReleaseNULL(wantedMajor);
    OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(matching);
    return 0;
  }

  bool ready = properties->setObject(kIOBSDMajorKey, wantedMajor) &&
               properties->setObject(kIOBSDMinorKey, wantedMinor) &&
               matching->setObject("IOPropertyMatch", properties);
  wantedMajor->release();
  wantedMinor->release();
  properties->release();

  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  matching->release();
  if (!iterator) return 0;

  unsigned matches = 0;
  unsigned depth = 0;
  bool authenticated = false;

  while (OSObject *object = iterator->getNextObject()) {
    IOService *service = OSDynamicCast(IOService, object);
    if (!service) continue;
    if (++matches != 1) {
      authenticated = false;
      break;
    }

    OSString *uuid = OSDynamicCast(OSString, service->getProperty("UUID"));
    if (!uuid || uuid->getLength() != 36) continue;
    if (requestedUuid && !asciiCaseEquals(uuid->getCStringNoCopy(), requestedUuid)) continue;
    strlcpy(native_verified_block_root_uuid, uuid->getCStringNoCopy(),
            sizeof(native_verified_block_root_uuid));

    IOService *provider = service->getProvider();
    for (unsigned step = 0; provider && step < 16; ++step, provider = provider->getProvider()) {
      depth = step + 1;
      NativeEmmcBlock *block = OSDynamicCast(NativeEmmcBlock, provider);
      if (block) {
        authenticated = block->authenticatesVolume(uuid->getCStringNoCopy(), manifestDigest);
        native_verified_block_root_uuid_match = authenticated ? 1U : 0U;
        native_verified_block_wrong_uuid_refused =
            !block->authenticatesVolume("00000000-0000-0000-0000-000000000000");
        break;
      }
    }
  }

  iterator->release();
  native_verified_block_root_media_matches = matches;
  native_verified_block_root_provider_depth = depth;
  return authenticated && matches == 1 ? 1 : 0;
}

}  // namespace

extern "C" int native_verified_block_root_authorized(dev_t device) {
  return verifiedVolumeQuery(device, nullptr, nullptr);
}

extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]) {
  if (!digest) return 0;
  bzero(digest, SHA384_DIGEST_LENGTH);
  if (!uuid || strlen(uuid) != 36) return 0;
  return verifiedVolumeQuery(device, uuid, digest);
}

extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size) {
  if (!uuid || !device || !path || size < 6 || strlen(uuid) != 36) return 0;
  *device = NODEV;
  path[0] = '\0';

  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *wanted = OSString::withCString(uuid);
  if (!matching || !properties || !wanted) {
    OSSafeReleaseNULL(wanted);
    OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(matching);
    return 0;
  }

  bool ready = properties->setObject("UUID", wanted) && matching->setObject("IOPropertyMatch", properties);
  wanted->release();
  properties->release();
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  matching->release();
  if (!iterator) return 0;

  unsigned matches = 0;
  bool authenticated = false;
  dev_t candidate = NODEV;
  char candidatePath[64] = {};

  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    if (++matches != 1) {
      authenticated = false;
      break;
    }
    OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
    OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
    OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
    if (!identity || !asciiCaseEquals(identity->getCStringNoCopy(), uuid) ||
        !name || !mjr || !mnr || !name->getLength() ||
        name->getLength() >= sizeof(candidatePath) - 6 ||
        name->getLength() >= size - 6 ||
        mjr->unsigned64BitValue() > 255 || mnr->unsigned64BitValue() > 0xffffff) {
      continue;
    }
    candidate = static_cast<dev_t>(makedev(static_cast<int>(mjr->unsigned32BitValue()),
                                           static_cast<int>(mnr->unsigned32BitValue())));
    if (!native_verified_block_root_authorized(candidate)) continue;
    snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name->getCStringNoCopy());
    authenticated = true;
  }

  iterator->release();
  if (!authenticated || matches != 1) return 0;
  *device = candidate;
  strlcpy(path, candidatePath, size);
  return 1;
}

extern "C" int native_ufs_block_authenticates(IOService *provider, const char *uuid, uint8_t *digest) {
  NativeEmmcBlock *block = OSDynamicCast(NativeEmmcBlock, provider);
  if (!block) return -1;
  return block->authenticatesVolume(uuid, digest) ? 1 : 0;
}

// Persistent Data window hooks (used if NativeRamBlock / Data store is enabled).
extern "C" unsigned native_ufs_store_mode(void) {
  return (gCtrlState && gCtrlState->dataWindow.present) ? 1U : 0U;
}

extern "C" int native_ufs_store_open(uint64_t minBytes, uint64_t *actualBytes, const char **kind) {
  if (kind) *kind = "emmc";
  if (!gCtrlState || !gCtrlState->dataWindow.present) return -1;
  if (gCtrlState->dataWindow.byteSize < minBytes) return -1;
  if (actualBytes) *actualBytes = gCtrlState->dataWindow.byteSize;
  gCtrlState->dataStoreOpen = true;
  return 0;
}

extern "C" int native_ufs_store_read(uint64_t offset, uint32_t bytes, void *dst) {
  if (!gCtrlState || !gCtrlState->dataStoreOpen || !dst) return -1;
  if ((offset % kEmmcSectorBytes) || (bytes % kEmmcSectorBytes)) return -1;
  if (offset > gCtrlState->dataWindow.byteSize || bytes > gCtrlState->dataWindow.byteSize - offset) return -1;
  IOLockLock(gCtrlState->gate);
  bool ok = gCtrlState->sdhci.readSectors(
      gCtrlState->dataWindow.startLba + (offset / kEmmcSectorBytes),
      bytes / kEmmcSectorBytes, dst);
  IOLockUnlock(gCtrlState->gate);
  return ok ? 0 : -1;
}

extern "C" int native_ufs_store_write(uint64_t offset, uint32_t bytes, const void *src) {
  if (!gCtrlState || !gCtrlState->dataStoreOpen || !src) return -1;
  if ((offset % kEmmcSectorBytes) || (bytes % kEmmcSectorBytes)) return -1;
  if (offset > gCtrlState->dataWindow.byteSize || bytes > gCtrlState->dataWindow.byteSize - offset) return -1;
  IOLockLock(gCtrlState->gate);
  bool ok = gCtrlState->sdhci.writeSectors(
      gCtrlState->dataWindow.startLba + (offset / kEmmcSectorBytes),
      bytes / kEmmcSectorBytes, src);
  if (ok) gCtrlState->sdhci.flushCache();
  IOLockUnlock(gCtrlState->gate);
  return ok ? 0 : -1;
}

extern "C" int native_ufs_store_write_cached(uint64_t offset, uint32_t bytes, const void *src) {
  if (!gCtrlState || !gCtrlState->dataStoreOpen || !src) return -1;
  if ((offset % kEmmcSectorBytes) || (bytes % kEmmcSectorBytes)) return -1;
  if (offset > gCtrlState->dataWindow.byteSize || bytes > gCtrlState->dataWindow.byteSize - offset) return -1;
  IOLockLock(gCtrlState->gate);
  bool ok = gCtrlState->sdhci.writeSectors(
      gCtrlState->dataWindow.startLba + (offset / kEmmcSectorBytes),
      bytes / kEmmcSectorBytes, src);
  IOLockUnlock(gCtrlState->gate);
  return ok ? 0 : -1;
}

extern "C" int native_ufs_store_sync(void) {
  if (!gCtrlState || !gCtrlState->dataStoreOpen) return -1;
  IOLockLock(gCtrlState->gate);
  bool ok = gCtrlState->sdhci.flushCache();
  IOLockUnlock(gCtrlState->gate);
  return ok ? 0 : -1;
}

extern "C" unsigned native_ufs_store_gpt_check(void) {
  return (gCtrlState && gCtrlState->layout.gptFound) ? 0U : 1U;
}
