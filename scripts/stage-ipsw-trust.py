#!/usr/bin/env python3
"""Extract and stage StaticTrustCache and SystemTrustCache from the local IPSW.

Verifies:
- StaticTrustCache payload sha256 == da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2
- SystemTrustCache payload sha256 == d121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651
- Combined bundle sha256 == 7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7
- Stages static-trustcache.bin and platform-seed
"""
from pathlib import Path
import hashlib
import json
import os
import plistlib
import struct
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
UNIXNU_ROOT = ROOT.parent / 'UniXNU'
IPSW_PATH = ROOT.parent / 'iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw'

EXPECTED_STATIC_SHA = 'da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2'
EXPECTED_SYSTEM_SHA = 'd121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651'
EXPECTED_BUNDLE_SHA = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'


def der(data, at):
    if at + 2 > len(data):
        raise ValueError('Truncated DER')
    tag, size = data[at:at + 2]
    at += 2
    if size & 128:
        count = size & 127
        if not 0 < count <= 4 or at + count > len(data):
            raise ValueError('Invalid DER length')
        size = int.from_bytes(data[at:at + count], 'big')
        at += count
    if at + size > len(data):
        raise ValueError('DER exceeds input')
    return tag, data[at:at + size], at + size


def im4p_payload(data):
    tag, sequence, end = der(data, 0)
    if tag != 0x30 or end != len(data):
        raise ValueError('Not a DER sequence')
    fields, cursor = [], 0
    while cursor < len(sequence):
        kind, val, cursor = der(sequence, cursor)
        fields.append((kind, val))
    if len(fields) < 4 or fields[0][1] != b'IM4P':
        raise ValueError('Not an IM4P')
    return fields[3][1]


def main():
    if not IPSW_PATH.is_file():
        print(f"Error: IPSW not found at {IPSW_PATH}", file=sys.stderr)
        sys.exit(1)

    print(f"Opening IPSW: {IPSW_PATH}")
    with zipfile.ZipFile(IPSW_PATH, 'r') as z:
        manifest = plistlib.loads(z.read('BuildManifest.plist'))
        identities = [i for i in manifest['BuildIdentities']
                      if i['Info'].get('DeviceClass') == 'j120ap' and i['Info'].get('RestoreBehavior') == 'Erase']
        if not identities:
            print("Error: Erase j120ap identity not found in BuildManifest.plist", file=sys.stderr)
            sys.exit(1)
        ident = identities[0]

        static_path = ident['Manifest']['StaticTrustCache']['Info']['Path']
        system_path = ident['Manifest']['Cryptex1,SystemTrustCache']['Info']['Path']

        print(f"Extracting StaticTrustCache from {static_path}...")
        static_raw = z.read(static_path)
        static_payload = im4p_payload(static_raw)
        static_sha = hashlib.sha256(static_payload).hexdigest()
        print(f"  StaticTrustCache: {len(static_payload)} bytes, sha256: {static_sha}")
        assert static_sha == EXPECTED_STATIC_SHA, f"Static sha mismatch: {static_sha} != {EXPECTED_STATIC_SHA}"

        print(f"Extracting Cryptex1,SystemTrustCache from {system_path}...")
        system_raw = z.read(system_path)
        system_payload = im4p_payload(system_raw)
        system_sha = hashlib.sha256(system_payload).hexdigest()
        print(f"  SystemTrustCache: {len(system_payload)} bytes, sha256: {system_sha}")
        assert system_sha == EXPECTED_SYSTEM_SHA, f"System sha mismatch: {system_sha} != {EXPECTED_SYSTEM_SHA}"

    # Build bundle
    bundle = struct.pack('<3I', 2, 12, 12 + len(static_payload)) + static_payload + system_payload
    bundle_sha = hashlib.sha256(bundle).hexdigest()
    print(f"Combined Trust Bundle: {len(bundle)} bytes, sha256: {bundle_sha}")
    assert bundle_sha == EXPECTED_BUNDLE_SHA, f"Bundle sha mismatch: {bundle_sha} != {EXPECTED_BUNDLE_SHA}"

    # Stage into targets
    for base in [ROOT, UNIXNU_ROOT]:
        target_dir = base / '.cache/firmware/target-volumes'
        target_dir.mkdir(parents=True, exist_ok=True)
        (target_dir / 'static-trustcache.bin').write_bytes(static_payload)
        print(f"Staged {target_dir / 'static-trustcache.bin'}")

        bundle_dir = base / 'build/original-static-system-trust'
        bundle_dir.mkdir(parents=True, exist_ok=True)
        (bundle_dir / 'original-static-system.bundle').write_bytes(bundle)
        (bundle_dir / 'inputs.json').write_text(json.dumps({
            'original_static_sha256': static_sha,
            'original_system_sha256': system_sha,
            'bundle_sha256': bundle_sha
        }, indent=2) + '\n')
        print(f"Staged {bundle_dir / 'original-static-system.bundle'}")

        seed_file = base / '.cache/platform-seed'
        if not seed_file.exists():
            seed = os.urandom(8).hex()
            seed_file.write_text(seed + '\n')
            print(f"Generated platform-seed: {seed} at {seed_file}")

    print("[PASS] IPSW trust caches and platform-seed successfully verified and staged!")


if __name__ == '__main__':
    main()
