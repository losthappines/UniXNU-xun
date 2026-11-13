#!/usr/bin/env python3
"""Build or inspect the userdata image for Redmi Pad SE (xun, SM6225).

Assembles the SHNGROOT manifest and SHNGPOL1 policy directory for eMMC 5.1
storage (512-byte sector size, userdata start LBA 0x1332730) using the OS
and Cryptex1,SystemOS disk images from the local IPSW.

Layout:
  0x000000..0x000FFF:  SHNGROOT Manifest Header (4096 bytes)
  0x001000..0x001FFF:  SHNGPOL1 Policy Locator Block (4096 bytes)
  0x002000..0x0FFFFF:  Zero padding to 1 MiB alignment
  0x100000..........:  Window "OS" (044-76040-038.dmg)
  ..................:  Window "SystemOS" (044-76034-038.dmg)
"""
from pathlib import Path
import argparse
import bz2
import hashlib
import json
import os
import platform
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
IPSW_PATH = ROOT.parent / 'iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw'

OS_DMG_NAME = '044-76040-038.dmg'
SYSTEMOS_DMG_NAME = '044-76034-038.dmg'

PINNED_OS_SHA = '3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d'
PINNED_OS_EXPANDED_SHA = 'e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2'
PINNED_SYSTEMOS_SHA = 'b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f'

OS_UUID = '04769D26-2CA0-4825-B443-E2E5F88300F2'
SYSTEMOS_UUID = '8846FE78-2910-4297-AF97-D59BB6F8FC16'

UDIF_ZERO_FILL = 0x00000000
UDIF_RAW = 0x00000001
UDIF_IGNORE = 0x00000002
UDIF_ZLIB = 0x80000005
UDIF_BZIP2 = 0x80000006
UDIF_LZFSE = 0x80000007
UDIF_COMMENT = 0x7FFFFFFE
UDIF_TERMINATOR = 0xFFFFFFFF


def align_1mib(val):
    return (val + 0xFFFFF) & ~0xFFFFF


def inspect_dmg(zf, member_name):
    info = zf.getinfo(member_name)
    if info.file_size < 512:
        return False, info.file_size, None, None
    with zf.open(member_name) as f:
        head = f.read(64)
        # Raw APFS container (e.g. Cryptex1,SystemOS UDRW) has NXSB at +0x20 and no koly trailer.
        if head[32:36] == b'NXSB':
            return False, info.file_size, None, None
        f.seek(info.file_size - 512)
        trailer = f.read(512)
    if trailer[:4] != b'koly':
        return False, info.file_size, None, None
    data_off = struct.unpack_from('>Q', trailer, 24)[0]
    xml_off, xml_len = struct.unpack_from('>QQ', trailer, 216)
    sector_count = struct.unpack_from('>Q', trailer, 492)[0]
    with zf.open(member_name) as f:
        f.seek(xml_off)
        plist_data = plistlib.loads(f.read(xml_len))
    return True, sector_count * 512, data_off, plist_data


def _lzfse_decode(blob):
    if platform.system() == 'Darwin' and Path('/usr/bin/compression_tool').is_file():
        cmd = ['/usr/bin/compression_tool', '-decode', '-a', 'lzfse']
    elif shutil.which('lzfse'):
        cmd = [shutil.which('lzfse'), '-decode']
    else:
        raise RuntimeError(
            'Expanding UDIF LZFSE chunks in OS DMG requires `lzfse` (or macOS `/usr/bin/compression_tool`). '
            'Writing compressed bvxn bytes would produce an unmountable OS partition. '
            'Run on macOS/Linux with `lzfse` installed or via `python3 unixnu build userdata`.'
        )
    with tempfile.NamedTemporaryFile(delete=False) as inp, tempfile.NamedTemporaryFile(delete=False) as outp:
        in_path, out_path = Path(inp.name), Path(outp.name)
    try:
        in_path.write_bytes(blob)
        subprocess.run([*cmd, '-i', str(in_path), '-o', str(out_path)], check=True)
        return out_path.read_bytes()
    finally:
        in_path.unlink(missing_ok=True)
        out_path.unlink(missing_ok=True)


def stream_udif_expanded(zf, member_name, data_off, plist_data, out_f):
    with zf.open(member_name) as src_f:
        for entry in plist_data['resource-fork']['blkx']:
            mish = entry['Data']
            if mish[:4] != b'mish':
                raise RuntimeError('Unexpected non-mish blkx entry in UDIF')
            run_count = struct.unpack_from('>I', mish, 200)[0]
            pos = 204
            for _ in range(run_count):
                kind, _, sec_off, sec_cnt, comp_off, comp_len = struct.unpack_from('>IIQQQQ', mish, pos)
                pos += 40
                if kind in (UDIF_COMMENT, UDIF_TERMINATOR):
                    continue
                if kind in (UDIF_ZERO_FILL, UDIF_IGNORE):
                    rem = sec_cnt * 512
                    zeros = bytes(min(rem, 8 << 20))
                    while rem > 0:
                        step = min(rem, len(zeros))
                        out_f.write(zeros[:step])
                        rem -= step
                elif kind in (UDIF_RAW, UDIF_ZLIB, UDIF_BZIP2, UDIF_LZFSE):
                    src_f.seek(data_off + comp_off)
                    blob = src_f.read(comp_len)
                    if kind == UDIF_ZLIB:
                        raw = zlib.decompress(blob)
                    elif kind == UDIF_BZIP2:
                        raw = bz2.decompress(blob)
                    elif kind == UDIF_LZFSE:
                        raw = _lzfse_decode(blob)
                    else:
                        raw = blob
                    if len(raw) != sec_cnt * 512:
                        raise RuntimeError(f'UDIF run size mismatch: {len(raw)} != {sec_cnt * 512}')
                    out_f.write(raw)
                else:
                    raise RuntimeError(f'Unsupported UDIF chunk kind {kind:#x}')


def build_manifest(windows, total_image_bytes):
    header = bytearray(4096)
    header[:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', header, 8, 1, len(windows), total_image_bytes)
    for idx, (name, offset, size, digest) in enumerate(windows):
        struct.pack_into('<32sQQII32s', header, 24 + idx * 128, name.encode('ascii'), offset, size, 512, 1, digest)
    digest = hashlib.sha256(header[:4064]).digest()
    header[4064:4096] = digest
    return header


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ipsw', type=Path, default=IPSW_PATH, help='Path to IPSW file')
    parser.add_argument('--manifest-only', action='store_true', help='Only generate header and print layout')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/xun/userdata.img')
    args = parser.parse_args()

    if not args.ipsw.is_file():
        print(f"Error: IPSW not found at {args.ipsw}", file=sys.stderr)
        sys.exit(1)

    print(f"Reading IPSW: {args.ipsw}")
    with zipfile.ZipFile(args.ipsw, 'r') as z:
        os_info = z.getinfo(OS_DMG_NAME)
        sys_info = z.getinfo(SYSTEMOS_DMG_NAME)

        os_is_udif, os_size, os_data_off, os_plist = inspect_dmg(z, OS_DMG_NAME)
        sys_is_udif, sys_size, sys_data_off, sys_plist = inspect_dmg(z, SYSTEMOS_DMG_NAME)

        print(f"Found OS DMG: {OS_DMG_NAME} (packed {os_info.file_size} bytes, expanded {os_size} bytes, udif={os_is_udif})")
        print(f"Found SystemOS DMG: {SYSTEMOS_DMG_NAME} (packed {sys_info.file_size} bytes, expanded {sys_size} bytes, udif={sys_is_udif})")

        # Calculate offsets
        os_offset = 1048576  # 1 MiB
        sys_offset = align_1mib(os_offset + os_size)
        total_size = align_1mib(sys_offset + sys_size)

        print(f"\nUserdata Layout (eMMC 512-byte sectors, partition base 0x1332730):")
        print(f"  0x000000 - 0x001000 : SHNGROOT Manifest Header (4 KiB)")
        print(f"  0x001000 - 0x002000 : SHNGPOL1 Policy Locator Block (4 KiB)")
        print(f"  0x002000 - {os_offset:#08x} : Padding to 1 MiB")
        print(f"  {os_offset:#08x} - {os_offset + os_size:#08x} : Window 'OS' ({os_size / (1024**3):.2f} GiB)")
        print(f"  {sys_offset:#08x} - {sys_offset + sys_size:#08x} : Window 'SystemOS' ({sys_size / (1024**3):.2f} GiB)")
        print(f"  Total flashed userdata payload size: {total_size / (1024**3):.2f} GiB ({total_size} bytes)")

        # Windows table
        os_sha = PINNED_OS_EXPANDED_SHA if os_is_udif else PINNED_OS_SHA
        windows = [
            ('OS', os_offset, os_size, bytes.fromhex(os_sha)),
            ('SystemOS', sys_offset, sys_size, bytes.fromhex(PINNED_SYSTEMOS_SHA)),
        ]
        manifest = build_manifest(windows, total_size)

        if args.manifest_only:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            manifest_file = args.output.with_suffix('.manifest.bin')
            manifest_file.write_bytes(manifest)
            print(f"\nManifest header saved to {manifest_file} (SHA-256: {hashlib.sha256(manifest).hexdigest()})")
            return

        print(f"\nWriting userdata image to {args.output}...")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('wb') as out_f:
            # 1. Manifest
            out_f.write(manifest)
            # 2. Pad to 1 MiB
            out_f.write(bytes(os_offset - 4096))
            # 3. Write OS DMG (UDIF-expanded if compressed)
            if os_is_udif:
                print(f"Expanding UDIF {OS_DMG_NAME}...")
                stream_udif_expanded(z, OS_DMG_NAME, os_data_off, os_plist, out_f)
            else:
                print(f"Streaming {OS_DMG_NAME}...")
                with z.open(OS_DMG_NAME) as src_f:
                    while chunk := src_f.read(8 << 20):
                        out_f.write(chunk)
            # Pad between OS and SystemOS
            pad_between = sys_offset - (os_offset + os_size)
            if pad_between > 0:
                out_f.write(bytes(pad_between))
            # 4. Write SystemOS DMG
            if sys_is_udif:
                print(f"Expanding UDIF {SYSTEMOS_DMG_NAME}...")
                stream_udif_expanded(z, SYSTEMOS_DMG_NAME, sys_data_off, sys_plist, out_f)
            else:
                print(f"Streaming {SYSTEMOS_DMG_NAME}...")
                with z.open(SYSTEMOS_DMG_NAME) as src_f:
                    while chunk := src_f.read(8 << 20):
                        out_f.write(chunk)

        print(f"[PASS] Successfully created userdata image: {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == '__main__':
    main()
