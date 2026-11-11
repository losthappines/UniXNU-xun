# Porting Notes: iPadOS 17.7.11 (`21H461`) on Xiaomi Redmi Pad SE 11" (`xun` / SM6225)

## Target Device & Platform
- **Device:** Xiaomi Redmi Pad SE 11" (`23073RPBFG`, codename `xun`)
- **Platform:** Qualcomm Snapdragon 680 4G (`SM6225` / `khaje` / `divar`)
- **CPU Topology:** 4× Kryo 265 Gold (Cortex-A73 @ 2.4 GHz, `MPIDR 0x100–0x103`) + 4× Kryo 265 Silver (Cortex-A53 @ 1.9 GHz, `MPIDR 0x0–0x3`)
- **Target OS:** iPadOS 17.7.11 (`21H461`), `iPad7,3` `arm64` userland (`iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`)
- **Baseline Framework:** UniXNU Format-2 Device Repository (`schema_version = 2`), seeded from `xVanTuring/UniXNU-tb128fu`

## Current Milestone
- **Milestone:** Stage 1 — Workspace Initialization & Hardware Extraction Complete
- **Last Confirmed Stage:** Stage 1 (DTB extraction & verification)
- **Active Blocker:** None (Stage 1 complete, moving to Stage 2 UEFI bring-up)

## Verified Hardware Parameters (Extracted from Official HyperOS ROM Live DTBs)

### 1. Memory & Reserved Memory Carve-Outs
- **DRAM Base:** `0x40000000`
- **Continuous Splash Region (`cont_splash_region`):**
  - Base: `0x5c000000`
  - Size: `0x00f00000` (15 MiB = 15,728,640 bytes)
  - Verified from: `dtbo.img_dtb_45.dtb` (`/fragment@41/__overlay__/splash_region@5c000000`)
- **PStore / Ramoops:**
  - Base: `0x5d000000`
  - Size: `0x00200000` (2 MiB)
  - Record size: `0x00040000` (256 KiB)
  - Console size: `0x00080000` (512 KiB)
  - Pmsg size: `0x00100000` (1 MiB)
  - Verified from: `vendor_boot.img_dtb_0.dtb` (`/reserved-memory/ramoops@5D000000`)
- **SMEM Region:** `0x46000000`
- **GPU Region:** `0x55B15000`

### 2. Display & Framebuffer Geometry
- **Resolution:** 1200 × 1920 portrait scanout (1920 × 1200 landscape)
- **Refresh Rates:** 30 Hz, 48 Hz, 50 Hz, 60 Hz, 90 Hz
- **Format:** 32 bpp (`B8G8R8A8` / `RGBX_8888`)
- **DSI Interface:** DSI video mode on `qcom,mdss_dsi_ctrl0@5e94000`
- **Verified from:** `dtbo.img_dtb_45.dtb` (`/fragment@35/__overlay__/qcom,mdss_mdp/qcom,mdss_dsi_panel_m84_35_03_0b_fhdp_video`)

### 3. Storage (eMMC 5.1 vs UFS)
- **Storage Subsystem:** eMMC 5.1 on Qualcomm SDHCI (`sdhci_msm`)
- **Controller Base:** `0x04744000` (`sdhci@4744000` / `mmcblk0`)
- **Block / Sector Size:** 512 bytes (`SECTOR_SIZE_IN_BYTES="512"`)
- **Userdata Partition Start Sector:** `20129584` (`0x1332730`)
- **Userdata Start Byte Hex:** `0x2664e6000`
- **Verified from:** `rawprogram0.xml`, `gpt_main0.bin`, and `vendor_boot.img_dtb_0.dtb`
- **Porting Action:** Replace `Tb128fuUfs` with `NativeEmmc`

### 4. Interrupts & Timers
- **GICv3 Distributor:** `0x0f200000`
- **GICv3 Redistributors:** `0x0f300000` (stride `0x20000`)
- **Virtual Timer INTID:** `19` @ 19.2 MHz
- **Software SPI IPI Base:** `536` (`INTID 536..543`)
- **Verified from:** `vendor_boot.img_dtb_0.dtb` (`interrupt-controller@f200000`, `timer@f120000`)

### 5. Touch & Input
- **Touch Controller:** FocalTech / Novatek (`focaltech,fts` / `novatek,NVT-ts` / `novatek,NVT-ts-spi`) on QUP SE
- **I2C Slave Address:** `0x62`
- **IRQ GPIO:** `80` (`0x50`)
- **Reset GPIO:** `86` (`0x56`)
- **Max Touch Points:** 10
- **Hardware Keys:** PM6125 SPMI PON resin/kpdpwr (Power + Volume)
- **Verified from:** `dtbo.img_dtb_45.dtb` (`fragment@43/__overlay__/touch@0`, `fragment@44/__overlay__/novatek@62`)

### 6. GPU
- **Architecture:** Qualcomm Adreno 610 (`a610` / `a6xx`)
- **Base Address:** `0x05900000` (`qcom,kgsl-3d0@5900000`)
- **SMMU:** `0x059a0000` (`kgsl-smmu@0x59a0000`)
- **Firmware:** `a630_sqe.fw` + zap shader

## Safety Guardrails Reminder
- **NEVER** flash or modify critical boot partitions: `xbl`, `xbl_config`, `abl`, `tz`, `hyp`, `devcfg`, `persist`, `modem`, `modemst1`, `modemst2`, `fsg`, `fsc`.
- **ABL Download Buffer Limit:** Keep boot payload image under 57 MiB.
- Always verify images in QEMU prior to hardware flashing.
- Keep TWRP / OrangeFox intact on `recovery` partition for log extraction via `pstore` / `ramoops`.

## Next Steps
- Stage 2: Scaffold `xunPkg` in Mu-Silicium and produce diagnostic `boot-green.img` framebuffer test.
- Stage 3: Prepare `dts/xun.dts`, `scripts/make-xun-adt.py`, and `configs/xun_defconfig` with 4 KiB kernel patches.
