#!/usr/bin/env bash
# Turnkey local build script for building SILICIUM_UEFI.fd and 4K XNU kernel on macOS / Hackintosh.
# Prerequisites: Xcode with iphoneos SDK, Homebrew with dtc, lzfse, llvm.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${DEVICE_DIR}/.." && pwd)"
UNIXNU_DIR="${ROOT_DIR}/UniXNU"
MU_DIR="${ROOT_DIR}/Mu-Silicium"

echo "=== Redmi Pad SE (xun) macOS Local Build Pipeline ==="
echo "Device Directory: ${DEVICE_DIR}"
echo "UniXNU Directory: ${UNIXNU_DIR}"
echo "Mu-Silicium Directory: ${MU_DIR}"

# 1. Check Toolchain Requirements
echo "--> Checking toolchain..."
if ! command -v xcrun &>/dev/null; then
    echo "Error: xcrun not found. Please install Xcode." >&2
    exit 1
fi

IPHONEOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null || true)"
if [ -z "${IPHONEOS_SDK}" ]; then
    echo "Error: Xcode iphoneos SDK not found. Install iOS support in Xcode." >&2
    exit 1
fi
echo "  Using iphoneos SDK: ${IPHONEOS_SDK}"

for tool in dtc lzfse python3; do
    if ! command -v "${tool}" &>/dev/null; then
        echo "Error: ${tool} not found. Run 'brew install dtc lzfse'." >&2
        exit 1
    fi
done

# 2. Build Apple Device Trees (.adt)
echo "--> Generating Apple Device Trees..."
mkdir -p "${DEVICE_DIR}/out/xun"
python3 "${DEVICE_DIR}/scripts/make-xun-adt.py" \
    --dram-size 0x180000000 \
    --ipad-identity \
    --output "${DEVICE_DIR}/out/xun/xun-single.adt" \
    --cpu-regs 0

python3 "${DEVICE_DIR}/scripts/make-xun-adt.py" \
    --dram-size 0x180000000 \
    --ipad-identity \
    --output "${DEVICE_DIR}/out/xun/xun-smp.adt" \
    --cpu-regs 0,1,2,3,0x100,0x101,0x102,0x103

python3 "${DEVICE_DIR}/scripts/inspect-adt.py" "${DEVICE_DIR}/out/xun/xun-single.adt"
python3 "${DEVICE_DIR}/scripts/inspect-adt.py" "${DEVICE_DIR}/out/xun/xun-smp.adt" --smp

# 3. Stage IPSW Trust Caches
if [ -f "${ROOT_DIR}/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw" ]; then
    echo "--> Staging IPSW trust caches..."
    python3 "${DEVICE_DIR}/scripts/stage-ipsw-trust.py"
fi

# 4. Check eMMC Root Path Unit Tests
echo "--> Running eMMC 5.1 root storage verification..."
python3 "${DEVICE_DIR}/drivers/XunEmmc/tests/check_xun_emmc_root.py"

# 5. Build UEFI Firmware (SILICIUM_UEFI.fd)
if [ -d "${MU_DIR}" ]; then
    echo "--> Building Mu-Silicium UEFI firmware volume..."
    export UNIXNU_UEFI="${MU_DIR}"
    python3 "${DEVICE_DIR}/scripts/check-xun.py"
    pushd "${UNIXNU_DIR}" >/dev/null
    python3 unixnu build firmware --uefi "${MU_DIR}" -D "${DEVICE_DIR}"
    popd >/dev/null
fi

# 6. Build 4K XNU Kernel and Package Payload
if [ -d "${UNIXNU_DIR}" ]; then
    echo "--> Configuring and building 4K XNU kernel..."
    pushd "${UNIXNU_DIR}" >/dev/null
    python3 unixnu defconfig xun_defconfig -D "${DEVICE_DIR}"
    python3 unixnu build kernel -D "${DEVICE_DIR}"
    python3 unixnu build payload -D "${DEVICE_DIR}" || true
    popd >/dev/null
fi

echo "=== Build complete! Boot artifacts located in out/xun/ ==="
