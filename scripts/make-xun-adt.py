#!/usr/bin/env python3
"""Generate the minimal Redmi Pad SE 11" (xun, SM6225) Apple-format DT for boot.

The tree follows the UniXNU Apple Device Tree structure with verified SM6225 / xun values:
  - GICv3 distributor 0x0f200000/0x10000, redistributor 0x0f300000/0x100000
    for eight cores, stride 0x20000; arm-io child offsets under a range at 0x0f200000
  - CPU timebase 19200000 Hz (19.2 MHz virtual timer, INTID 19)
  - Cores 0x0..0x3 Cortex-A53 (Silver) and 0x100..0x103 Cortex-A73 (Gold)
  - DRAM base 0x80000000 (DarwinBoot window base) or 0x40000000 physical DRAM base
  - Framebuffer at 0x5c000000 (1200x1920, stride 4800, 32 bpp ARGB)
  - Ramoops / pstore at 0x5d000000 (2 MiB, record 256K, console 512K, pmsg 1M)
  - eMMC controller node (sdhci@4744000, NativeEmmc driver)
"""
from pathlib import Path
import argparse
import os
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

DEFAULT_DRAM_BASE = 0x80000000   # DarwinBoot window base on SM6225 (0x80000000..0x1c0000000)
ARMIO_BASE = 0x04000000          # 16 MiB-aligned SoC window covering SDHCI (0x04744000) & GICv3 (0x0f200000..0x0f400000)
ARMIO_SIZE = 0x10000000          # 0x04000000..0x14000000
GIC_DIST = 0x0F200000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x0F300000
GIC_REDIST_SIZE = 0x100000
GIC_REDIST_STRIDE = 0x20000      # GICv3 without virtual LPIs: two 64 KiB frames per core
TIMEBASE = 19200000              # 19.2 MHz virtual timer
LITTLE_CORES = 0x100             # Regs below 0x100 are Cortex-A53 Silver cluster

FB_BASE = 0x5C000000
FB_SIZE = 0x00F00000             # 15 MiB
FB_WIDTH = 1200
FB_HEIGHT = 1920
FB_STRIDE = 4800                 # 1200 * 4
FB_DEPTH = 32

RAMOOPS_BASE = 0x5D000000
RAMOOPS_SIZE = 0x00200000        # 2 MiB
RAMOOPS_RECORD = 0x00040000      # 256 KiB
RAMOOPS_CONSOLE = 0x00080000     # 512 KiB
RAMOOPS_PMSG = 0x00100000        # 1 MiB

SDHCI_BASE = 0x04744000
SDHCI_SIZE = 0x1000


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy():
    """A fresh volatile CHRP bank for upstream IODTNVRAM, with no controller."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))),
            ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)),
            ('nvram-proxy-data', bytes(image))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/xun/boot/kernel.macho.adt')
    parser.add_argument('--dram-base', type=lambda value: int(value, 0), default=DEFAULT_DRAM_BASE,
                        help='DRAM base address (default 0x80000000 DarwinBoot window base)')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), required=True,
                        help='Actual observed DRAM size, e.g. 0x180000000')
    parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=0x80000000,
                        help='Explicit DarwinBoot contiguous window request, default 0x80000000 (2 GiB)')
    parser.add_argument('--root-match-volume-uuid', default=None,
                        help='/chosen/root-matching: select the root by the UUID of an IOMedia')
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None,
                        help='/chosen/unique-chip-id (8 bytes) and --chip-id (4 bytes) seed XNU IOPlatformUUID')
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--ipad-identity', action='store_true',
                        help='/product and /buttons describe an iPad-class device without a Home button')
    parser.add_argument('--cpu-regs', default='0',
                        help='Comma-separated CPU MPIDR affinity values; the first is the running boot CPU. '
                             'xun numbers clusters: 0x0..0x3 (A53) and 0x100..0x103 (A73).')
    parser.add_argument('--psci-method', choices=['smc', 'hvc'], default='smc',
                        help='/psci method used by secondary-CPU start')
    parser.add_argument('--packed-data-ramdisk', action='store_true', help='Hand off validated DBRDISK1 bytes')
    parser.add_argument('--data-ramdisk', action='store_true',
                        help='/chosen/memory-map RAMDisk placeholder')
    parser.add_argument('--trust-cache', action='store_true', help='Zeroed TrustCache sidecar handoff placeholder')
    parser.add_argument('--gpu-addon', action='store_true',
                        help='Zeroed GpuAddon sidecar handoff placeholder')
    args = parser.parse_args()
    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error('--unique-chip-id and --chip-id must be supplied together')
    if args.unique_chip_id is not None and not (0 < args.unique_chip_id < 1 << 64 and 0 < args.chip_id < 1 << 32):
        parser.error('--unique-chip-id must fit 8 bytes and --chip-id 4 bytes, both nonzero')
    if args.owned_memory_size is not None and (
            not 0x10000000 <= args.owned_memory_size <= 0x200000000
            or args.owned_memory_size % 0x200000):
        parser.error('Owned memory must be 256 MiB to 8 GiB, aligned to 2 MiB')

    try:
        cpu_regs = [int(value, 0) for value in args.cpu_regs.split(',')]
    except ValueError:
        parser.error('--cpu-regs must be integers')
    if (not cpu_regs or cpu_regs[0] != 0 or len(set(cpu_regs)) != len(cpu_regs) or len(cpu_regs) > 8
            or any(reg not in range(0, 4) and reg not in range(0x100, 0x104) for reg in cpu_regs)):
        parser.error('--cpu-regs: up to eight distinct cores 0..3 (A53) and 0x100..0x103 (A73), boot CPU 0 first')
    cpus = [node('cpu%d' % index, [('state', b'running\0' if index == 0 else b'waiting\0'),
                                   ('reg', struct.pack('<I', reg)),
                                   ('device_type', b'cpu\0'),
                                   ('compatible', b'arm,cortex-a53\0' if reg < LITTLE_CORES else b'arm,cortex-a73\0'),
                                   # Omit cluster-type: UniXNU's native-smp-secondary.patch sets MAX_CPU_CLUSTERS=1,
                                   # so 'P' (phys_cluster_id=2) or two clusters panics _ml_parse_cpu_topology.
                                   ('timebase-frequency', struct.pack('<I', TIMEBASE))])
            for index, reg in enumerate(cpu_regs)]
    tree = node('device-tree', [
        ('model', b'iPad7,1\0' if args.ipad_identity else b'xun-first-boot\0'),
        *([('target-type', b'J120\0')] if args.ipad_identity else []),
        ('compatible', b'xiaomi,xun\0qcom,khaje\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', [('random-seed', os.urandom(64)),
                        *([('unique-chip-id', struct.pack('<Q', args.unique_chip_id)), ('chip-id', struct.pack('<I', args.chip_id))]
                          if args.unique_chip_id is not None else []),
                        ('osenvironment', b'main\0'),
                        *([('certificate-production-status', struct.pack('<I', 0)), ('effective-production-status-ap', struct.pack('<I', 0)),
                           ('certificate-security-mode', struct.pack('<I', 1)), ('effective-security-mode-sep', struct.pack('<I', 1))]
                          if args.ipad_identity else []),
                        *([('ephemeral-storage', struct.pack('<I', 1))] if (args.data_ramdisk or args.packed_data_ramdisk) else []),
                        *([('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size))]
                          if args.owned_memory_size else []),
                        ('dram-base', struct.pack('<Q', args.dram_base)),
                        ('dram-size', struct.pack('<Q', args.dram_size)),
                        ('crypto-hash-method', b'sha2-384\0'),
                        *([('root-matching', ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
                                              '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                                              args.root_match_volume_uuid.upper() + '</string></dict></dict>\0').encode())]
                          if args.root_match_volume_uuid else []),
                        ('amfi-only-platform-code', struct.pack('<I', 1)),
                        *empty_nvram_proxy()],
             [node('memory-map', ([("PackedRAMDisk" if args.packed_data_ramdisk else "RAMDisk", bytes(16))] if (args.data_ramdisk or args.packed_data_ramdisk) else []) +
                                 ([('TrustCache', bytes(16))] if args.trust_cache else []) +
                                 ([('GpuAddon', bytes(16))] if args.gpu_addon else [])),
              node('manifest-properties')]),
        node('cpus', children=cpus),
        *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', args.psci_method.encode() + b'\0')])] if len(cpus) > 1 else []),
        node('defaults', [('sleep-disabled', struct.pack('<I', 1)),
                          ('kern.maxvnodes', struct.pack('<I', (args.owned_memory_size or (4 << 30)) // 65536 + 1024))]),
        node('options', []),
        *([node('buttons', [('compatible', b'buttons\0'), ('device_type', b'buttons\0'),
                            ('button-names', b'hold\0voldown\0volup\0'),
                            ('home-button-type', struct.pack('<I', 2))])] if args.ipad_identity else []),
        node('product', [('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0' if args.ipad_identity else b'Xiaomi Redmi Pad SE (xun)\0'),
                         ('model', b'xiaomi,xun\0'),
                         *([('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                            ('artwork-device-idiom', b'pad\0'),
                            ('artwork-device-subtype', struct.pack('<I', 2732)),
                            ('artwork-scale-factor', struct.pack('<I', 2)),
                            ('artwork-display-gamut', b'P3\0'),
                            ('compatible-device-fallback', b'iPad6,7\0'),
                            ('thin-bezel', struct.pack('<I', 1))] if args.ipad_identity else [])]),
        node('arm-io', [('device_type', b'arm-io\0'),
                        ('#address-cells', struct.pack('<I', 2)),
                        ('#size-cells', struct.pack('<I', 2)),
                        ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, ARMIO_SIZE))],
             [node('intc', [('compatible', b'arm,gic-v3\0'),
                            ('reg', struct.pack('<4Q',
                                                GIC_DIST - ARMIO_BASE, GIC_DIST_SIZE,
                                                GIC_REDIST - ARMIO_BASE, GIC_REDIST_SIZE)),
                            ('redistributor-stride', struct.pack('<Q', GIC_REDIST_STRIDE))]),
              node('sdhci', [('compatible', b'qcom,sdhci-msm-v5\0qcom,sdhci-msm\0'),
                             ('reg', struct.pack('<QQ', SDHCI_BASE - ARMIO_BASE, SDHCI_SIZE)),
                             ('bus-width', struct.pack('<I', 8))])]),
        node('framebuffer', [('compatible', b'simple-framebuffer\0'),
                             ('reg', struct.pack('<QQ', FB_BASE, FB_SIZE)),
                             ('width', struct.pack('<I', FB_WIDTH)),
                             ('height', struct.pack('<I', FB_HEIGHT)),
                             ('stride', struct.pack('<I', FB_STRIDE)),
                             ('depth', struct.pack('<I', FB_DEPTH)),
                             ('format', b'a8r8g8b8\0'),
                             ('status', b'okay\0')]),
        node('ramoops', [('compatible', b'ramoops\0'),
                         ('reg', struct.pack('<QQ', RAMOOPS_BASE, RAMOOPS_SIZE)),
                         ('record-size', struct.pack('<I', RAMOOPS_RECORD)),
                         ('console-size', struct.pack('<I', RAMOOPS_CONSOLE)),
                         ('pmsg-size', struct.pack('<I', RAMOOPS_PMSG)),
                         ('status', b'okay\0')]),
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f'xun ADT: {args.output} ({len(tree)} bytes, dram_base {args.dram_base:#x}, dram_size {args.dram_size:#x}, cpus {len(cpus)}, timebase {TIMEBASE})')


if __name__ == '__main__':
    main()
