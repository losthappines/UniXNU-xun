# Porting Notes: iPadOS 17.7.11 (`21H461`) on Xiaomi Redmi Pad SE 11" (`xun` / SM6225)

## Target Device & Platform
- **Device:** Xiaomi Redmi Pad SE 11" (`23073RPBFG`, codename `xun`)
- **Platform:** Qualcomm Snapdragon 680 4G (`SM6225` / `khaje` / `divar`)
- **CPU Topology:** 4× Kryo 265 Gold (Cortex-A73 @ 2.4 GHz, `MPIDR 0x100–0x103`) + 4× Kryo 265 Silver (Cortex-A53 @ 1.9 GHz, `MPIDR 0x0–0x3`)
- **Target OS:** iPadOS 17.7.11 (`21H461`), `iPad7,3` `arm64` userland (`iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`)
- **Baseline Framework:** UniXNU Format-2 Device Repository (`schema_version = 2`), seeded from `xVanTuring/UniXNU-tb128fu`

## Current Milestone
- **Milestone:** Stage 3 & 4 Complete — Apple Device Trees Generated, Framebuffer Patched to 1200×1920, IPSW Trust Caches & Platform Seed Staged, eMMC 5.1 Storage Verified, Turnkey CI Workflow & macOS Build Scripts Deployed
- **Last Confirmed Stage:** Stage 4 (eMMC 5.1 512-byte sector unit tests passed; `xun-single.adt` and `xun-smp.adt` validated)
- **Porting Record Status:** 13/13 structured areas checked and validated (`python UniXNU/scripts/porting-record.py check --device UniXNU-xun`)
- **Active Blocker:** None for deterministic code; ready for cloud CI compilation (`.github/workflows/build.yml`) or Hackintosh execution (`scripts/build-macos-local.sh`) to produce `SILICIUM_UEFI.fd` and `kernel.macho`.

## Stage 3 & 4 Implemented Deliverables

### 1. Apple Device Tree Generation & Validation
- **Tool:** `UniXNU-xun/scripts/make-xun-adt.py`
- **Single-Core Boot ADT:** `out/xun/xun-single.adt` (11,992 bytes)
  - `cpu_regs = "0"`, Cortex-A53 Silver cluster-type `E`
  - GICv3 Distributor `0x0F200000`, Redistributors `0x0F300000` (stride `0x20000`)
  - Virtual timer frequency: `19200000` Hz (19.2 MHz, INTID 19)
  - DRAM base: `0x80000000`, DarwinBoot window: `0x80000000` (2 GiB)
  - Continuous splash framebuffer: `0x5C000000` (1200×1920 portrait scanout, stride 4800, 32 bpp ARGB)
  - Retained panic RAM: `0x5D000000` (`ramoops` 2 MiB)
  - eMMC storage: `sdhci@4744000` (`qcom,sdhci-msm-v5`)
- **8-Core SMP ADT:** `out/xun/xun-smp.adt` (14,292 bytes)
  - All 8 cores: 4× Silver `0x0..0x3` (`E`) + 4× Gold `0x100..0x103` (`P`)
  - `/psci`: method `smc`, compatible `arm,psci-1.0`
- **Validation:** `UniXNU-xun/scripts/inspect-adt.py` parses binary format, 4-byte padding alignment, node hierarchy, and asserts all hardware registers.

### 2. Resolution & Framebuffer Constants (1200×2000 -> 1200×1920)
- `UniXNU-xun/package/LegacyDisplayProbe.cpp`: updated comments and display parameters to `1200x1920`.
- `UniXNU-xun/package/TouchProbe.cpp`: updated panel bounds to `1200x1920`.
- `UniXNU-xun/package/HimaxProtocol.h`: updated `#define HX_Y_MAX 1919` (from 1999) and resolution comments.
- `UniXNU-xun/package/BootLog.c`: updated log format string to `1200x1920`.
- `UniXNU-xun/drivers/AdrenoMetal/shaders/scale_bgra.ll`: updated scanout presentation target to `1200x1920`.
- `UniXNU-xun/drivers/GopFramebuffer/NativeLegacyDisplay.cpp`: updated render server scaling to `900x1440` on `1200x1920`.
- `UniXNU-xun/README.md`: updated GOP framebuffer and panel entries to `1200×1920`.

### 3. IPSW Trust Caches & Platform Seed
- **Script:** `UniXNU-xun/scripts/stage-ipsw-trust.py`
- Extracted `StaticTrustCache` from `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw` (`Firmware/044-76040-038.dmg.trustcache` payload):
  - Size: 59,496 bytes, SHA-256: `da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2`
- Extracted `Cryptex1,SystemTrustCache` (`Firmware/044-76034-038.dmg.trustcache` payload):
  - Size: 1,848 bytes, SHA-256: `d121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651`
- Built combined trust bundle `original-static-system.bundle`:
  - Size: 61,356 bytes, SHA-256: `7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7` (matches board pin exactly)
- Staged `.cache/firmware/target-volumes/static-trustcache.bin` and `.cache/platform-seed`.

### 4. eMMC 5.1 Storage Driver & 512-Byte Sector Unit Tests
- **Driver:** `UniXNU-xun/drivers/XunEmmc`
  - `Info.plist`, `RootLayout.h`, `NativeEmmc.cpp`
  - Qualcomm SDHCI MSM at `0x04744000`
  - 512-byte sector size
  - Userdata start LBA: `20129584` (`0x1332730`), byte offset: `0x2664e6000` (10,306,347,008 bytes)
  - Userdata last LBA: `244318207` (`0xe8fffff`)
- **Unit Test:** `UniXNU-xun/drivers/XunEmmc/tests/check_xun_emmc_root.py`
  - Validates LBA 1 GPT Header, LBA 2..33 Partition Entries array (128 entries)
  - Validates `userdata` partition resolution and 512-byte byte offset translation
  - Validates `SHNGROOT` manifest header (magic, 3 slices: OS, SystemOS, Data) and `SHNGPOL1` policy locator block at offset 4096.

### 5. Turnkey CI & macOS Local Build Pipelines
- **GitHub Actions Workflow:** `UniXNU-xun/.github/workflows/build.yml`
  - `build-uefi` (`ubuntu-latest`): builds `SILICIUM_UEFI.fd` using Clang + EDK2 BaseTools
  - `build-xnu` (`macos-14`): compiles `kernel.macho` and `.adt` with Xcode `iphoneos` SDK and `MACHINE_CONFIGS=BCM2837`
- **macOS Build Script:** `UniXNU-xun/scripts/build-macos-local.sh`
  - Automates toolchain checks, ADT generation, trust cache staging, eMMC unit tests, and kernel compilation on local Hackintosh.
- **Panic & Retained Log Diagnostic Reader:** `UniXNU-xun/scripts/read-xun-log.py`

## Verified Hardware Parameters
- **SoC:** Qualcomm Snapdragon 680 4G (`SM6225`, `soc_id = 518`, `khaje` / `divar`)
- **CPU:** 4× Silver (Cortex-A53, `0x0–0x3`) + 4× Gold (Cortex-A73, `0x100–0x103`)
- **DRAM:** Base `0x40000000`, DarwinBoot window at `0x80000000..0x100000000` (2 GiB)
- **Continuous Splash:** `0x5C000000` (15 MiB inside 16 MiB Display Reserved), `1200×1920`, 32 bpp
- **PStore / Ramoops:** `0x5D000000` (2 MiB)
- **Storage:** eMMC 5.1 on Qualcomm SDHCI MSM `0x04744000`, 512-byte sector size, userdata start LBA `20129584` (`0x1332730`)
- **Interrupts & Timer:** GICv3 Dist `0x0F200000`, Redist `0x0F300000` (stride `0x20000`), Virtual Timer `INTID 19` @ 19.2 MHz
- **Touch & Buttons:** FocalTech / Novatek (`0x62`), IRQ GPIO 80, Reset GPIO 86; PM6125 SPMI PON resin/kpdpwr
- **GPU:** Adreno 610 (`0x05900000`, SMMU `0x059A0000`, no GMU)

## Safety Guardrails Reminder
- **NEVER** flash or modify critical boot partitions: `xbl`, `xbl_config`, `abl`, `tz`, `hyp`, `devcfg`, `persist`, `modem`, `modemst1`, `modemst2`, `fsg`, `fsc`.
- **ABL Download Buffer Limit:** Keep boot payload image under 57 MiB.
- Keep TWRP / OrangeFox intact on `recovery` partition for log extraction via `pstore` / `ramoops`.
