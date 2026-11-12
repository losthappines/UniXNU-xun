#!/usr/bin/env python3
"""Package Android boot image v4 for Xiaomi Redmi Pad SE (xun).

On xun, ABL rejects gzip-compressed kernels and non-zero ARM64 text_offset
values (staying at a black screen). Hardware testing confirmed ABL enters an
uncompressed ARM64 Image with:
  - b 0x40; .long 0 at 0x00..0x07
  - text_offset = 0, image_size = 0x400000, flags = 0xa, magic = ARM\x64
  - BootShim preserving x0 (ABL FDT pointer) while copying SILICIUM_UEFI.fd to 0x5FC00000
  - xun.dtb appended after SILICIUM_UEFI.fd
  - Android boot image header v4 (pagesize=4096, header_size=1584) + XUNESP ramdisk
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

FD_BASE = 0x5FC00000
FD_SIZE = 0x300000


def build_shim() -> bytes:
    shim = bytearray(0x100)
    insns = [
        0x10000604,  # adr x4, _Payload (+0xC0 from 0x40 -> 0x100)
        0xD2ABF805,  # movz x5, #0x5fc0, lsl #16 (FD_BASE = 0x5FC00000)
        0xEB05009F,  # cmp x4, x5
        0x540000C0,  # b.eq jump (+6 insns -> idx 9)
        0xD2A00606,  # movz x6, #0x30, lsl #16 (FD_SIZE = 0x300000)
        0xA8C12889,  # ldp x9, x10, [x4], #16
        0xA88128A9,  # stp x9, x10, [x5], #16
        0xF10040C6,  # subs x6, x6, #16
        0x54FFFFA1,  # b.ne copy_loop (-3 insns -> idx 5)
        0xD5033F9F,  # dsb sy
        0xD5033FDF,  # isb
        0xD2ABF805,  # movz x5, #0x5fc0, lsl #16
        0xD61F00A0,  # br x5
        0x14000000,  # b .
    ]
    assert 0x40 + len(insns) * 4 <= len(shim)
    struct.pack_into("<II", shim, 0x00, 0x14000010, 0x00000000)
    struct.pack_into("<Q", shim, 0x08, 0x0)         # text_offset = 0
    struct.pack_into("<Q", shim, 0x10, 0x00400000)  # image_size = 4 MiB
    struct.pack_into("<Q", shim, 0x18, 0xA)         # flags = 0xa
    shim[0x38:0x3C] = b"ARM\x64"
    for idx, word in enumerate(insns):
        struct.pack_into("<I", shim, 0x40 + idx * 4, word)
    return bytes(shim)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fd", type=Path, required=True)
    parser.add_argument("--dtb", type=Path, required=True)
    parser.add_argument("--ramdisk", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()

    fd = args.fd.read_bytes()
    if len(fd) != FD_SIZE:
        raise SystemExit(f"{args.fd} is {len(fd):#x} bytes, expected {FD_SIZE:#x}")

    dtb = args.dtb.read_bytes()
    if dtb[:4] != b"\xd0\x0d\xfe\xed" or struct.unpack_from(">I", dtb, 4)[0] != len(dtb):
        raise SystemExit(f"Invalid DTB: {args.dtb}")

    ramdisk = args.ramdisk.read_bytes() if args.ramdisk else b""
    shim = build_shim()
    kernel = shim + fd + dtb

    hdr = bytearray(4096)
    struct.pack_into("<8sIIII4II", hdr, 0, b"ANDROID!", len(kernel), len(ramdisk), 0, 1584, 0, 0, 0, 0, 4)
    struct.pack_into("<I", hdr, 44 + 1536, 0)

    k_padded = kernel + b"\x00" * (-len(kernel) % 4096)
    r_padded = ramdisk + b"\x00" * (-len(ramdisk) % 4096)
    image = bytes(hdr) + k_padded + r_padded

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)

    report = {
        "kernel_sha256": sha256_bytes(kernel),
        "kernel_compression": "none",
        "ramdisk_bytes": len(ramdisk),
        "header_version": 4,
        "text_offset": "0x0",
        "image_size": "0x400000",
        "image_sha256": sha256_bytes(image),
        "image_bytes": len(image),
        "fd_sha256": sha256_bytes(fd),
        "dtb_sha256": sha256_bytes(dtb),
        "shim_bytes": len(shim),
        "hardware_tested": False,
    }
    (args.output.parent / "boot.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Wrote {args.output} ({len(image)} bytes, {len(image) / 1048576:.2f} MiB)")


if __name__ == "__main__":
    main()
