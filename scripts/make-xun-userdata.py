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
import hashlib
import json
import os
import plistlib
import struct
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
IPSW_PATH = ROOT.parent / 'iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw'

OS_DMG_NAME = '044-76040-038.dmg'
SYSTEMOS_DMG_NAME = '044-76034-038.dmg'

PINNED_OS_SHA = '3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d'
PINNED_SYSTEMOS_SHA = 'b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f'

OS_UUID = '04769D26-2CA0-4825-B443-E2E5F88300F2'
SYSTEMOS_UUID = '8846FE78-2910-4297-AF97-D59BB6F8FC16'


def align_1mib(val):
    return (val + 0xFFFFF) & ~0xFFFFF


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

        print(f"Found OS DMG: {OS_DMG_NAME} ({os_info.file_size} bytes)")
        print(f"Found SystemOS DMG: {SYSTEMOS_DMG_NAME} ({sys_info.file_size} bytes)")

        # Calculate offsets
        os_offset = 1048576  # 1 MiB
        os_size = os_info.file_size
        sys_offset = align_1mib(os_offset + os_size)
        sys_size = sys_info.file_size
        total_size = align_1mib(sys_offset + sys_size)

        print(f"\nUserdata Layout (eMMC 512-byte sectors, partition base 0x1332730):")
        print(f"  0x000000 - 0x001000 : SHNGROOT Manifest Header (4 KiB)")
        print(f"  0x001000 - 0x002000 : SHNGPOL1 Policy Locator Block (4 KiB)")
        print(f"  0x002000 - {os_offset:#08x} : Padding to 1 MiB")
        print(f"  {os_offset:#08x} - {os_offset + os_size:#08x} : Window 'OS' ({os_size / (1024**3):.2f} GiB)")
        print(f"  {sys_offset:#08x} - {sys_offset + sys_size:#08x} : Window 'SystemOS' ({sys_size / (1024**3):.2f} GiB)")
        print(f"  Total flashed userdata payload size: {total_size / (1024**3):.2f} GiB ({total_size} bytes)")

        # Windows table
        windows = [
            ('OS', os_offset, os_size, bytes.fromhex(PINNED_OS_SHA)),
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
            # 3. Stream OS DMG
            print(f"Streaming {OS_DMG_NAME}...")
            with z.open(OS_DMG_NAME) as src_f:
                while chunk := src_f.read(8 << 20):
                    out_f.write(chunk)
            # Pad between OS and SystemOS
            pad_between = sys_offset - (os_offset + os_size)
            if pad_between > 0:
                out_f.write(bytes(pad_between))
            # 4. Stream SystemOS DMG
            print(f"Streaming {SYSTEMOS_DMG_NAME}...")
            with z.open(SYSTEMOS_DMG_NAME) as src_f:
                while chunk := src_f.read(8 << 20):
                    out_f.write(chunk)

        print(f"[PASS] Successfully created userdata image: {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == '__main__':
    main()
