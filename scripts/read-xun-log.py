#!/usr/bin/env python3
"""Fetch the UniXNU diagnostic log window from a Redmi Pad SE (xun) in fastboot.

Boot the log reader with `fastboot boot`, wait for its read-only USB disk
(volume UNIXNULOG), copy LOG.BIN and STATUS.TXT, eject the disk (restarting into fastboot),
and extract retained ramoops (0x5d000000, 2 MiB) or userdata log ring.
Sector size: 512 bytes, userdata start LBA: 0x1332730.
"""
from pathlib import Path
import argparse
import glob
import os
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
UNIXNU = ROOT.parent / 'UniXNU'
SERIAL = os.environ.get('XUN_SERIAL', '')


def fastboot_serials():
    out = subprocess.run(['fastboot', 'devices'], capture_output=True, text=True).stdout
    return [line.split()[0] for line in out.splitlines() if line.strip().endswith('fastboot')]


def in_fastboot():
    serials = fastboot_serials()
    return SERIAL in serials if SERIAL else len(serials) == 1


def fastboot(*args):
    return ['fastboot'] + (['-s', SERIAL] if SERIAL else []) + list(args)


def find_volume():
    candidates = []
    if sys.platform == 'darwin':
        candidates = glob.glob('/Volumes/UNIXNULOG*')
    elif sys.platform == 'win32':
        # On Windows, search removable drive letters
        import string
        for letter in string.ascii_uppercase:
            drive = f"{letter}:\\"
            if os.path.exists(os.path.join(drive, 'LOG.BIN')) and os.path.exists(os.path.join(drive, 'STATUS.TXT')):
                candidates.append(drive)
    else:
        candidates = glob.glob('/media/*/UNIXNULOG*') + glob.glob('/run/media/*/*/UNIXNULOG*')

    for vol in candidates:
        if os.path.isfile(os.path.join(vol, 'LOG.BIN')) and os.path.isfile(os.path.join(vol, 'STATUS.TXT')):
            return vol
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reader', type=Path, default=ROOT / 'build/xun/reader.img')
    parser.add_argument('--out', type=Path, default=ROOT / 'logs')
    parser.add_argument('--no-boot', action='store_true')
    parser.add_argument('--wait', type=int, default=120)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    print("Checking for xun device in fastboot...")
    devices = fastboot_serials()
    if not devices:
        print("No devices detected in fastboot. Ensure Redmi Pad SE is in fastboot mode ('adb reboot bootloader').")
        sys.exit(1)
    print(f"Detected fastboot devices: {devices}")

    if not args.no_boot:
        if not args.reader.is_file():
            print(f"Reader image not found at {args.reader}. Use --no-boot if logs are already pulled.")
            sys.exit(1)
        print(f"Booting diagnostic reader: {args.reader}...")
        subprocess.run(fastboot('boot', str(args.reader)), check=True)

    print(f"Waiting up to {args.wait}s for UNIXNULOG volume...")
    start = time.time()
    volume = None
    while time.time() - start < args.wait:
        volume = find_volume()
        if volume:
            break
        time.sleep(1)

    if not volume:
        print("Timeout waiting for UNIXNULOG disk volume.")
        sys.exit(1)

    print(f"Found log volume at {volume}. Copying log artifacts to {args.out}...")
    import shutil
    timestamp = time.strftime('%Y%m%d-%H%M%S')
    dest = args.out / timestamp
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(os.path.join(volume, 'LOG.BIN'), dest / 'LOG.BIN')
    shutil.copy2(os.path.join(volume, 'STATUS.TXT'), dest / 'STATUS.TXT')
    print(f"Log artifacts saved in {dest}")


if __name__ == '__main__':
    main()
