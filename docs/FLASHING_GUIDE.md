# Redmi Pad SE 11" (`xun` / SM6225) iPadOS Port: Complete Flashing & Bring-Up Guide

> **Target Device:** Xiaomi Redmi Pad SE 11" (`23073RPBFG`, codename `xun`)  
> **Platform:** Qualcomm Snapdragon 680 4G (`SM6225` / `"khaje"` / `"divar"`, `soc_id = 518`)  
> **Target OS:** iPadOS 17.7.11 (`21H461`), `iPad7,3` userland  
> **Firmware:** Mu-Silicium `xunPkg` + UniXNU DarwinBoot + 4K XNU Kernel  

---

## 1. Safety Guardrails & Anti-Brick Rules

> [!CAUTION]
> **CRITICAL PARTITION SAFETY:**
> * **NEVER** flash, format, or erase low-level firmware partitions:
>   `xbl`, `xbl_config`, `abl`, `tz`, `hyp`, `devcfg`, `persist`, `modem`, `modemst1`, `modemst2`, `fsg`, `fsc`.
> * Flashing any third-party payload to these partitions will hard-brick the device and require Qualcomm Emergency Download (EDL 9008) recovery with authorized service credentials.
> * The UniXNU port strictly targets **only two partitions**:
>   1. `boot_a` (or `boot_b` depending on active slot) — holds the unified UEFI + XNU kernel boot image (`< 57 MiB`).
>   2. `userdata` — holds the signed Apple APFS root slices (`OS`, `SystemOS`, and `Data`).

> [!WARNING]
> **SM6225 ABL FASTBOOT DOWNLOAD BUFFER QUIRK:**
> The stock Xiaomi ABL bootloader on Snapdragon 680 tablets allocates an in-memory fastboot download buffer smaller than the partition size.
> * Images **under 57 MiB** (such as our 8 KiB `boot-green.img` diagnostic) can be booted directly in RAM using `fastboot boot <image.img>`.
> * Full OS images (57–96 MiB) will trigger `Failed to load/authenticate boot image: Load Error` if booted via `fastboot boot`. You must write them directly with `fastboot flash boot_a <image.img>` after backing up your stock boot partition.

---

## 2. Prerequisites & Preparation

1. **Unlocked Bootloader:** Ensure the tablet's bootloader is officially unlocked via Xiaomi Community / Mi Unlock tool.
2. **Android Platform Tools:** Ensure `fastboot` and `adb` are installed on your host computer (Windows, macOS, or Linux) and in your system `PATH`.
3. **USB Cable & Port:** Connect the tablet to a reliable USB-A or USB-C port directly on your PC (avoid unpowered USB hubs).
4. **Custom Recovery Installed:** Ensure TWRP (`twrp_3.7.1_xun.img`) or OrangeFox is installed on the `recovery` partition so you can always recover, back up, or extract retained `pstore`/`ramoops` logs.

---

## 3. Step 0: Backup Stock Boot & State

Before flashing anything, always create a full backup of your active stock boot partition:

1. Reboot the tablet to Fastboot mode:
   ```bash
   adb reboot bootloader
   ```
2. Check your active slot:
   ```bash
   fastboot getvar current-slot
   ```
3. Boot into TWRP recovery to take a block-level backup:
   ```bash
   fastboot boot twrp_3.7.1_xun.img
   ```
4. In TWRP, open an ADB shell on your host PC:
   ```bash
   adb shell "dd if=/dev/block/bootdevice/by-name/boot_a of=/sdcard/stock_boot_a.img"
   adb pull /sdcard/stock_boot_a.img ./stock_boot_a.img
   ```
   *(Keep `stock_boot_a.img` safely stored. If you ever need to return to HyperOS, simply run `fastboot flash boot_a stock_boot_a.img`.)*

---

## 4. Step 1: Verify Hardware Framebuffer (`boot-green.img`)

The repository includes a standalone 8 KiB diagnostic image [`out/xun/boot-green.img`](file:///d:/IpadOS/out/xun/boot-green.img) that tests the continuous splash scanout buffer at `0x5C000000` (1200×1920 portrait, 32 bpp ARGB).

1. Put the tablet in Fastboot mode:
   ```bash
   adb reboot bootloader
   ```
2. Boot the diagnostic image directly into RAM:
   ```bash
   fastboot boot out/xun/boot-green.img
   ```
3. **Expected Result:**
   The tablet's screen will immediately fill with a bright solid green canvas (`0xFF00FF00`). This proves that:
   * The panel is initialized in 1200×1920 video mode.
   * Continuous splash memory at `0x5C000000` is active and scanout-mapped.
   * The CPU is executing AArch64 code cleanly from fastboot handoff.

---

## 5. Step 2: Build UEFI Firmware & 4K XNU Kernel

Because compiling XNU requires Apple's `iphoneos` SDK (available on macOS) and compiling Mu-Silicium requires Clang + EDK2 BaseTools, the repository provides two turnkey pipelines:

### Option A: Cloud CI Build (GitHub Actions)
1. Push `UniXNU-xun` to your GitHub repository.
2. The workflow [`.github/workflows/build.yml`](file:///d:/IpadOS/UniXNU-xun/.github/workflows/build.yml) triggers automatically:
   * **`build-uefi` (Ubuntu runner):** Compiles `SILICIUM_UEFI.fd` (3 MiB) using Clang and EDK2 BaseTools.
   * **`build-xnu` (macOS 14 runner):** Compiles `kernel.macho` and generates `xun-single.adt` and `xun-smp.adt` using Xcode's `iphoneos` SDK with `MACHINE_CONFIGS=BCM2837`.
3. Download the zipped artifacts (`xun-uefi` and `xun-kernel-payload`) from the GitHub Actions run page.

### Option B: Local Build on macOS / Hackintosh
1. Boot into your macOS or Hackintosh partition.
2. Clone or mount `D:\IpadOS`.
3. Run the turnkey local build script:
   ```bash
   chmod +x UniXNU-xun/scripts/build-macos-local.sh
   ./UniXNU-xun/scripts/build-macos-local.sh
   ```
4. Output boot artifacts will be assembled into `UniXNU-xun/out/xun/boot.img`.

---

## 6. Step 3: Generate and Flash `userdata.img`

The `userdata` partition holds the root filesystem slices from `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw` behind the `SHNGROOT` manifest.

1. Generate the userdata payload image:
   ```bash
   python UniXNU-xun/scripts/make-xun-userdata.py
   ```
   *This extracts `044-76040-038.dmg` (OS slice, 4.80 GiB) and `044-76034-038.dmg` (SystemOS slice, 3.08 GiB), prepends the `SHNGROOT` 512-byte eMMC header, and outputs `out/xun/userdata.img` (7.88 GiB).*
2. Put the tablet in Fastboot mode:
   ```bash
   adb reboot bootloader
   ```
3. Flash the userdata image:
   ```bash
   fastboot flash userdata out/xun/userdata.img
   ```

---

## 7. Step 4: Flash the Unified DarwinBoot Boot Image

1. Ensure your unified boot payload `out/xun/boot.img` is ready (< 57 MiB).
2. Flash to the active boot partition:
   ```bash
   fastboot flash boot_a out/xun/boot.img
   ```
3. Reboot the tablet:
   ```bash
   fastboot reboot
   ```

---

## 8. Step 5: Boot Sequence & What to Expect

1. **ABL Phase (0–2s):** Xiaomi logo appears briefly.
2. **Mu-Silicium UEFI Phase (2–5s):** SimpleFbDxe displays the UniXNU boot menu or banner on the 1200×1920 continuous splash buffer.
3. **DarwinBoot Phase (5–8s):** DarwinBoot loads `kernel.macho`, hands off `xun-single.adt`, maps memory, and transfers control to XNU.
4. **XNU Kernel Bring-Up (8–30s):**
   * Early boot color bands draw in 1/16th height steps:
     * Band 1 (blue): kernel entry (`_start`, MMU off)
     * Band 2 (magenta): MMU enable transition
     * Band 3 (white/progress): `_arm_vm_init` passed
   * `NativeEmmc` probes Qualcomm SDHCI MSM at `0x04744000`, finds the `userdata` partition at LBA `0x1332730`, verifies the `SHNGROOT` signature against the compiled RSA-3072 key, and mounts `OS` and `SystemOS`.
   * On-screen boot progress bar (`boot-progress=2`) renders diagnostic status text.
5. **SpringBoard UI (70–95s):** QuartzCore draws the iPadOS home screen on the 1200×1920 frame buffer.

---

## 9. Closed-Loop Panic & Log Extraction Protocol

If a boot stalls or panics:

1. **Retained RAM Logs (`ramoops`):**
   * The kernel maintains a 2 MiB pstore region at `0x5D000000` (record: 256 KiB, console: 512 KiB, pmsg: 1 MiB) that survives warm reboot.
   * Hold **Power + Volume Down** to enter Fastboot mode.
   * Run the automated log fetcher:
     ```bash
     python UniXNU-xun/scripts/read-xun-log.py
     ```
2. **Reading Logs via Recovery:**
   * Boot into TWRP:
     ```bash
     fastboot boot twrp_3.7.1_xun.img
     ```
   * Mount and inspect pstore logs:
     ```bash
     adb shell "mount -t pstore pstore /sys/fs/pstore && ls -la /sys/fs/pstore && cat /sys/fs/pstore/console-ramoops-0"
     ```
3. Record panic traces or register dumps in `UniXNU-xun/NOTES.md` and `porting.toml`.

---

## 10. Emergency Rollback to Stock Android / HyperOS

If you ever wish to restore the tablet to stock condition:

1. Put the tablet in Fastboot mode:
   ```bash
   adb reboot bootloader
   ```
2. Restore your stock boot image:
   ```bash
   fastboot flash boot_a stock_boot_a.img
   ```
3. Format userdata to restore Android ext4/f2fs storage:
   ```bash
   fastboot erase userdata
   ```
4. Reboot into stock system:
   ```bash
   fastboot reboot
   ```
