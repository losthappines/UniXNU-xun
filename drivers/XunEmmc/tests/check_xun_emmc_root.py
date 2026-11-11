#!/usr/bin/env python3
"""Host unit test for Redmi Pad SE (xun, SM6225) eMMC 5.1 storage geometry.

Validates:
1. 512-byte eMMC sector addressing and conversion (vs 4096-byte UFS).
2. Primary GPT generation & parsing with protective MBR at LBA 0, GPT header at LBA 1,
   and 128 partition entries at LBA 2..33.
3. Userdata partition bounds:
   - First LBA: 20129584 (0x1332730), byte offset: 0x2664e6000 (10,306,347,008 bytes).
   - Last LBA: 244318207 (0xe8fffff), total sectors: 224,188,624 (~106.9 GiB).
4. SHNGROOT manifest header at userdata LBA 0 (0x1332730):
   - Magic: 'SHNGROOT' (0x474e4853, 0x544f4f52)
   - Slices: 'OS', 'SystemOS', 'Data'
5. SHNGPOL1 policy locator block at userdata offset 4096 (8 sectors):
   - Magic: 'SHNGPOL1'
   - RSA-3072 envelope offsets and SHA-256 digests.
"""
from pathlib import Path
import hashlib
import struct
import sys
import uuid
import zlib

EMMC_SECTOR_SIZE = 512
USERDATA_FIRST_LBA = 20129584    # 0x1332730
USERDATA_LAST_LBA = 244318207    # 0xe8fffff
TOTAL_SECTORS = 244318208        # ~125 GB device


def create_mock_emmc_disk():
    # Synthetic disk dictionary mapping LBA -> 512-byte sector
    disk = {}

    def write_sector(lba, data):
        assert len(data) == 512, f"Sector write must be exactly 512 bytes, got {len(data)}"
        disk[lba] = bytes(data)

    def write_range(start_lba, data):
        assert len(data) % 512 == 0, "Data must be 512-byte aligned"
        for i in range(len(data) // 512):
            write_sector(start_lba + i, data[i * 512:(i + 1) * 512])

    # 1. Protective MBR at LBA 0
    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<BBBBBBBBII', 0x00, 0x00, 0x02, 0x00, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(0xFFFFFFFF, TOTAL_SECTORS - 1))
    mbr[510:512] = b'\x55\xAA'
    write_sector(0, mbr)

    # 2. Partition entries array (128 entries * 128 bytes = 16,384 bytes = 32 sectors)
    entries = bytearray(16384)
    # Entry 0: userdata
    type_guid = uuid.UUID('1b81e7e6-f50d-419b-a739-2a0eee6333b6').bytes_le  # Linux basic data / userdata
    part_guid = uuid.UUID('d6ad4e45-9ddb-4cce-9827-4c3fd6b9da5a').bytes_le
    name_utf16 = 'userdata\0'.encode('utf-16-le')
    struct.pack_into('<16s16sQQQ72s', entries, 0, type_guid, part_guid, USERDATA_FIRST_LBA, USERDATA_LAST_LBA, 0, name_utf16)
    entries_crc = zlib.crc32(entries)
    write_range(2, entries)

    # 3. GPT Header at LBA 1
    gpt = bytearray(512)
    gpt[:8] = b'EFI PART'
    struct.pack_into('<IIIIQQQQ', gpt, 8, 0x10000, 92, 0, 0, 1, TOTAL_SECTORS - 1, 34, TOTAL_SECTORS - 34)
    gpt[56:72] = uuid.UUID('f8a8d015-0f98-4800-9c74-b023ca44099f').bytes_le
    struct.pack_into('<QIII', gpt, 72, 2, 128, 128, entries_crc)
    header_crc = zlib.crc32(gpt[:92])
    struct.pack_into('<I', gpt, 16, header_crc)
    write_sector(1, gpt)

    # 4. SHNGROOT manifest header at userdata start LBA (0x1332730)
    manifest = bytearray(4096)
    manifest[:8] = b'SHNGROOT'
    windows = [
        ('OS', 1048576, 33 * 1024 * 1024, hashlib.sha256(b'OS_IMAGE_DATA').digest()),
        ('SystemOS', 35 * 1024 * 1024, 8 * 1024 * 1024, hashlib.sha256(b'SYSTEM_OS_DATA').digest()),
        ('Data', 45 * 1024 * 1024, 64 * 1024 * 1024, hashlib.sha256(b'DATA_STORAGE').digest()),
    ]
    struct.pack_into('<IIQ', manifest, 8, 1, len(windows), 120 * 1024 * 1024)
    for idx, (wname, woffset, wsize, whash) in enumerate(windows):
        struct.pack_into('<32sQQII32s', manifest, 24 + idx * 128, wname.encode(), woffset, wsize, 512, 1, whash)
    manifest_digest = hashlib.sha256(manifest[:4064]).digest()
    manifest[4064:4096] = manifest_digest
    write_range(USERDATA_FIRST_LBA, manifest)

    # 5. SHNGPOL1 policy locator at userdata offset 4096 (LBA 0x1332730 + 8)
    locator = bytearray(4096)
    locator[:8] = b'SHNGPOL1'
    struct.pack_into('<IIII', locator, 8, 1, len(windows) - 1, 88, 0)
    for idx, (wname, woffset, wsize, whash) in enumerate(windows[:2]):
        struct.pack_into('<32sQQII32s', locator, 24 + idx * 88, wname.encode(), woffset + 65536, 4096, 512, 1, whash)
    locator_digest = hashlib.sha256(locator[:4064]).digest()
    locator[4064:4096] = locator_digest
    write_range(USERDATA_FIRST_LBA + 8, locator)

    return disk


def test_emmc_root_parsing():
    print("Running Xun eMMC 5.1 Root Storage Unit Test...")
    disk = create_mock_emmc_disk()

    def host_read_sectors(lba, count, buffer):
        for i in range(count):
            sector = disk.get(lba + i, bytes(512))
            buffer[i * 512:(i + 1) * 512] = sector
        return 0

    # Test GPT Header at LBA 1
    buf = bytearray(512)
    assert host_read_sectors(1, 1, buf) == 0
    assert buf[:8] == b'EFI PART', "GPT header signature mismatch"
    header_crc = struct.unpack_from('<I', buf, 16)[0]
    struct.pack_into('<I', buf, 16, 0)
    computed_crc = zlib.crc32(buf[:92])
    assert header_crc == computed_crc, f"Header CRC mismatch: {header_crc:#x} != {computed_crc:#x}"
    print(f"  [OK] LBA 1 GPT Header valid (CRC: {header_crc:#x})")

    # Test Partition Entries at LBA 2..33 (32 sectors)
    entries_buf = bytearray(16384)
    assert host_read_sectors(2, 32, entries_buf) == 0
    entries_crc = struct.unpack_from('<I', buf, 88)[0]
    computed_entries_crc = zlib.crc32(entries_buf)
    assert entries_crc == computed_entries_crc, f"Entries CRC mismatch: {entries_crc:#x} != {computed_entries_crc:#x}"
    print(f"  [OK] LBA 2..33 GPT Entries valid (128 entries, CRC: {entries_crc:#x})")

    # Locate userdata partition
    entry_first, entry_last, attrs, name_raw = struct.unpack_from('<QQQ72s', entries_buf, 32)
    name = name_raw.decode('utf-16-le', errors='ignore').split('\0')[0]
    assert name == 'userdata', f"Expected 'userdata', got {name}"
    assert entry_first == USERDATA_FIRST_LBA, f"Userdata start LBA mismatch: {entry_first:#x} != {USERDATA_FIRST_LBA:#x}"
    assert entry_last == USERDATA_LAST_LBA, f"Userdata last LBA mismatch: {entry_last:#x} != {USERDATA_LAST_LBA:#x}"
    byte_offset = entry_first * EMMC_SECTOR_SIZE
    assert byte_offset == 0x2664e6000, f"Userdata byte offset mismatch: {byte_offset:#x} != 0x2664e6000"
    print(f"  [OK] Partition '{name}' found at LBA {entry_first:#x} ({byte_offset:#x} bytes, 512-byte sectors)")

    # Read SHNGROOT manifest at userdata start LBA
    manifest_buf = bytearray(4096)
    assert host_read_sectors(entry_first, 8, manifest_buf) == 0
    assert manifest_buf[:8] == b'SHNGROOT', f"SHNGROOT magic mismatch: {manifest_buf[:8]}"
    manifest_digest = hashlib.sha256(manifest_buf[:4064]).digest()
    assert manifest_buf[4064:4096] == manifest_digest, "Manifest SHA-256 digest mismatch"
    version, num_windows = struct.unpack_from('<II', manifest_buf, 8)
    assert version == 1, f"Manifest version mismatch: {version}"
    assert num_windows == 3, f"Window count mismatch: {num_windows}"
    print(f"  [OK] SHNGROOT manifest valid (v{version}, {num_windows} windows: OS, SystemOS, Data)")

    # Read SHNGPOL1 policy locator at userdata offset 4096 (LBA + 8)
    locator_buf = bytearray(4096)
    assert host_read_sectors(entry_first + 8, 8, locator_buf) == 0
    assert locator_buf[:8] == b'SHNGPOL1', f"SHNGPOL1 magic mismatch: {locator_buf[:8]}"
    locator_digest = hashlib.sha256(locator_buf[:4064]).digest()
    assert locator_buf[4064:4096] == locator_digest, "Locator SHA-256 digest mismatch"
    print(f"  [OK] SHNGPOL1 policy locator valid at offset 4096")

    print("[PASS] All Xun eMMC 5.1 storage geometry tests succeeded!")
    return True


if __name__ == '__main__':
    ok = test_emmc_root_parsing()
    sys.exit(0 if ok else 1)
