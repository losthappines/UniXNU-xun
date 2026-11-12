#!/usr/bin/env python3
"""Package Android boot image v4 for Xiaomi Redmi Pad SE (xun).

Hardware testing on xun established two ABL constraints:
  1. ABL rejects gzip-compressed kernels and non-zero ARM64 text_offset values,
     so the kernel image uses an uncompressed ARM64 Image header (b 0x40; 0,
     text_offset=0, image_size=64 MiB, flags=0xa).
  2. Under boot header v4 (GKI 2.0), ABL ignores boot.img's ramdisk section
     and only populates /chosen/linux,initrd-* from the device's stock
     vendor_boot partition. However, ABL loads the full kernel section (>=58 MiB)
     cleanly at 0x40000000, leaving >74 MiB clear below the DTB at 0x44A2A000.

To keep everything self-contained inside `fastboot boot boot.img` without
flashing vendor_boot, this packager embeds payload.img directly inside the kernel
section after SILICIUM_UEFI.fd (and appends xun.dtb after payload.img). At boot,
BootShim:
  - Copies SILICIUM_UEFI.fd (3 MiB) to 0x5FC00000
  - Copies payload.img to 0x70000000
  - Updates /chosen linux,initrd-start and linux,initrd-end in-place in ABL's FDT (x0)
  - Restores x0 and branches to SILICIUM_UEFI.fd at 0x5FC00000
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

FD_BASE = 0x5FC00000
FD_SIZE = 0x300000
SHIM_SIZE = 0x1000
INITRD_ADDR = 0x70000000


def mov64(rd: int, val: int) -> list[int]:
    insns = [0xD2800000 | (((val >> 0) & 0xFFFF) << 5) | rd]
    for shift, op in ((16, 0xF2A00000), (32, 0xF2C00000), (48, 0xF2E00000)):
        part = (val >> shift) & 0xFFFF
        if part:
            insns.append(op | (part << 5) | rd)
    return insns


class Asm:
    def __init__(self) -> None:
        self.insns: list[int] = []
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[str, int, str]] = []

    def label(self, name: str) -> None:
        self.labels[name] = len(self.insns)

    def emit(self, word: int) -> None:
        self.insns.append(word)

    def b(self, target: str) -> None:
        idx = len(self.insns)
        self.insns.append(0x14000000)
        self.fixups.append(("b26", idx, target))

    def bcond(self, cond: int, target: str) -> None:
        idx = len(self.insns)
        self.insns.append(0x54000000 | cond)
        self.fixups.append(("b19", idx, target))

    def cbz(self, rt: int, target: str) -> None:
        idx = len(self.insns)
        self.insns.append(0xB4000000 | rt)
        self.fixups.append(("b19", idx, target))

    def adr(self, rd: int, target: str) -> None:
        idx = len(self.insns)
        self.insns.append(0x10000000 | rd)
        self.fixups.append(("adr", idx, target))

    def finish(self, extra_data: bytes = b"") -> bytes:
        for kind, idx, target in self.fixups:
            if target not in self.labels:
                raise KeyError(f"Missing label: {target}")
            delta_insns = self.labels[target] - idx
            if kind == "b26":
                self.insns[idx] |= delta_insns & 0x3FFFFFF
            elif kind == "b19":
                self.insns[idx] |= (delta_insns & 0x7FFFF) << 5
            elif kind == "adr":
                byte_delta = delta_insns * 4
                immlo = byte_delta & 3
                immhi = (byte_delta >> 2) & 0x7FFFF
                self.insns[idx] |= (immlo << 29) | (immhi << 5)
        return b"".join(struct.pack("<I", w) for w in self.insns) + extra_data


def emit_streq(a: Asm, ptr_reg: int, target_str_label: str, fail_label: str) -> None:
    a.adr(9, target_str_label)
    a.emit(0xAA0003EA | (ptr_reg << 16))  # mov x10, x<ptr_reg>
    loop = f"_cmp_{len(a.insns)}"
    a.label(loop)
    a.emit(0x3840152B)  # ldrb w11, [x9], #1
    a.emit(0x3840154C)  # ldrb w12, [x10], #1
    a.emit(0x6B0C017F)  # cmp w11, w12
    a.bcond(1, fail_label)
    a.emit(0x7100017F)  # cmp w11, #0
    a.bcond(1, loop)


def build_shim(ramdisk_size: int) -> bytes:
    a = Asm()
    a.emit(0xAA0003F3)  # mov x19, x0 (preserve incoming FDT pointer)
    a.emit(0x10000014)  # adr x20, . (load_base + 0x44)
    a.emit(0xD1011294)  # sub x20, x20, #0x44 -> kernel load base

    # Copy SILICIUM_UEFI.fd from x20 + SHIM_SIZE to FD_BASE
    a.emit(0x91400684)  # add x4, x20, #0x1000
    for w in mov64(5, FD_BASE):
        a.emit(w)
    a.emit(0xEB05009F)  # cmp x4, x5
    a.bcond(0, "copy_initrd")
    for w in mov64(6, FD_SIZE):
        a.emit(w)
    a.label("copy_fd_loop")
    a.emit(0xA8C12889)  # ldp x9, x10, [x4], #16
    a.emit(0xA88128A9)  # stp x9, x10, [x5], #16
    a.emit(0xF10040C6)  # subs x6, x6, #16
    a.bcond(1, "copy_fd_loop")

    a.label("copy_initrd")
    if ramdisk_size > 0:
        copy_len = (ramdisk_size + 15) & ~15
        for w in mov64(9, SHIM_SIZE + FD_SIZE):
            a.emit(w)
        a.emit(0x8B090284)  # add x4, x20, x9
        for w in mov64(5, INITRD_ADDR):
            a.emit(w)
        for w in mov64(6, copy_len):
            a.emit(w)
        a.label("copy_rd_loop")
        a.emit(0xA8C12889)
        a.emit(0xA88128A9)
        a.emit(0xF10040C6)
        a.bcond(1, "copy_rd_loop")

        # Patch /chosen/linux,initrd-start and linux,initrd-end in FDT at x19
        a.cbz(19, "fdt_done")
        a.emit(0xB9400269)  # ldr w9, [x19, #0]
        for w in mov64(10, 0xEDFE0DD0):
            a.emit(w)
        a.emit(0x6B0A013F)  # cmp w9, w10
        a.bcond(1, "fdt_done")

        a.emit(0xB9400A69)  # ldr w9, [x19, #8] (off_dt_struct)
        a.emit(0x5AC00929)  # rev w9, w9
        a.emit(0x8B090277)  # add x23, x19, x9
        a.emit(0xB9400E69)  # ldr w9, [x19, #12] (off_dt_strings)
        a.emit(0x5AC00929)  # rev w9, w9
        a.emit(0x8B090278)  # add x24, x19, x9
        a.emit(0xB9402669)  # ldr w9, [x19, #36] (size_dt_struct)
        a.emit(0x5AC00929)  # rev w9, w9
        a.emit(0x8B0902F9)  # add x25, x23, x9

        a.label("fdt_loop")
        a.emit(0xEB1902FF)  # cmp x23, x25
        a.bcond(2, "fdt_done")
        a.emit(0xB84046E9)  # ldr w9, [x23], #4
        a.emit(0x5AC00929)  # rev w9, w9
        a.emit(0x7100053F)  # cmp w9, #1 (FDT_BEGIN_NODE)
        a.bcond(0, "tok_begin")
        a.emit(0x7100093F)  # cmp w9, #2 (FDT_END_NODE)
        a.bcond(0, "fdt_loop")
        a.emit(0x7100113F)  # cmp w9, #4 (FDT_NOP)
        a.bcond(0, "fdt_loop")
        a.emit(0x71000D3F)  # cmp w9, #3 (FDT_PROP)
        a.bcond(0, "tok_prop")
        a.b("fdt_done")

        a.label("tok_begin")
        a.emit(0x384016E9)  # ldrb w9, [x23], #1
        a.emit(0x7100013F)  # cmp w9, #0
        a.bcond(1, "tok_begin")
        a.emit(0x91000EF7)  # add x23, x23, #3
        a.emit(0x927EF6F7)  # and x23, x23, #~3
        a.b("fdt_loop")

        a.label("tok_prop")
        a.emit(0xB84046ED)  # ldr w13, [x23], #4 (len)
        a.emit(0x5AC009AD)  # rev w13, w13
        a.emit(0xB84046EE)  # ldr w14, [x23], #4 (nameoff)
        a.emit(0x5AC009CE)  # rev w14, w14
        a.emit(0x8B0E030F)  # add x15, x24, x14

        emit_streq(a, 15, "str_start", "chk_end")
        for w in mov64(9, INITRD_ADDR):
            a.emit(w)
        a.emit(0x5AC00929)  # rev w9, w9
        a.emit(0x710021BF)  # cmp w13, #8
        a.bcond(0, "wr_start_64")
        a.emit(0xB90002E9)  # str w9, [x23, #0]
        a.b("prop_next")
        a.label("wr_start_64")
        a.emit(0xB90002FF)  # str wzr, [x23, #0]
        a.emit(0xB90006E9)  # str w9, [x23, #4]
        a.b("prop_next")

        a.label("chk_end")
        emit_streq(a, 15, "str_end", "prop_next")
        for w in mov64(9, INITRD_ADDR + ramdisk_size):
            a.emit(w)
        a.emit(0x5AC00929)  # rev w9, w9
        a.emit(0x710021BF)  # cmp w13, #8
        a.bcond(0, "wr_end_64")
        a.emit(0xB90002E9)  # str w9, [x23, #0]
        a.b("prop_next")
        a.label("wr_end_64")
        a.emit(0xB90002FF)  # str wzr, [x23, #0]
        a.emit(0xB90006E9)  # str w9, [x23, #4]

        a.label("prop_next")
        a.emit(0x8B0D02F7)  # add x23, x23, x13
        a.emit(0x91000EF7)  # add x23, x23, #3
        a.emit(0x927EF6F7)  # and x23, x23, #~3
        a.b("fdt_loop")

        a.label("fdt_done")

    a.emit(0xD5033F9F)  # dsb sy
    a.emit(0xD5033FDF)  # isb
    a.emit(0xAA1303E0)  # mov x0, x19 (restore FDT pointer)
    for w in mov64(5, FD_BASE):
        a.emit(w)
    a.emit(0xD61F00A0)  # br x5
    a.emit(0x14000000)  # b .

    s1 = b"linux,initrd-start\x00"
    s2 = b"linux,initrd-end\x00"
    s1_padded = s1 + b"\x00" * ((-len(s1)) % 4)
    str_start_off = len(a.insns) * 4
    str_end_off = str_start_off + len(s1_padded)
    a.labels["str_start"] = str_start_off // 4
    a.labels["str_end"] = str_end_off // 4
    body = a.finish(s1_padded + s2)

    shim = bytearray(SHIM_SIZE)
    struct.pack_into("<II", shim, 0x00, 0x14000010, 0x00000000)
    struct.pack_into("<Q", shim, 0x08, 0x0)         # text_offset = 0
    struct.pack_into("<Q", shim, 0x10, 0x04000000)  # image_size = 64 MiB
    struct.pack_into("<Q", shim, 0x18, 0xA)         # flags = 0xa
    shim[0x38:0x3C] = b"ARM\x64"
    assert 0x40 + len(body) <= SHIM_SIZE
    shim[0x40:0x40 + len(body)] = body
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
    shim = build_shim(len(ramdisk))
    kernel = shim + fd + ramdisk + dtb

    hdr = bytearray(4096)
    struct.pack_into("<8sIIII4II", hdr, 0, b"ANDROID!", len(kernel), 0, 0, 1584, 0, 0, 0, 0, 4)
    struct.pack_into("<I", hdr, 44 + 1536, 0)

    k_padded = kernel + b"\x00" * (-len(kernel) % 4096)
    image = bytes(hdr) + k_padded

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)

    report = {
        "kernel_sha256": sha256_bytes(kernel),
        "kernel_compression": "none",
        "ramdisk_bytes": len(ramdisk),
        "ramdisk_address": hex(INITRD_ADDR) if ramdisk else "0x0",
        "header_version": 4,
        "text_offset": "0x0",
        "image_size": "0x4000000",
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
