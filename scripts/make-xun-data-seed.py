#!/usr/bin/env python3
"""Create a <= 2 GiB packed DBRDISK1 APFS Data seed for UniXNU-xun sparse RAM mode.

Uses an already-fetched iPadOS OS disk image (044-76040-038.dmg), formats a 1 GiB
APFS Data volume (<= 2 GiB so NativeRamBlock uses RamSparseDisk without requiring
persistent SHNGDATA eMMC backing), copies /private/var (excluding staged_system_apps),
packs it as DBRDISK1, and optionally updates CONFIG_XNU_OWNER_DATA_VOLUME_UUID in .config.
"""
import argparse
import os
from pathlib import Path
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile


def require(cond, msg):
    if not cond:
        raise SystemExit(f'make-xun-data-seed: {msg}')


def find_unixnu_root(explicit):
    if explicit:
        root = Path(explicit).resolve()
        require((root / 'scripts/unixnu/runtime.py').is_file(), f'Invalid UniXNU root: {root}')
        return root
    here = Path(__file__).resolve()
    candidates = [
        Path.cwd(),
        here.parents[2] / 'UniXNU',
        here.parents[1].parent / 'UniXNU',
    ]
    for c in candidates:
        if (c / 'scripts/unixnu/runtime.py').is_file():
            return c.resolve()
    require(False, 'Cannot locate UniXNU checkout (pass --unixnu)')


def find_os_dmg(unixnu_root, device_root, explicit):
    if explicit:
        p = Path(explicit).resolve()
        require(p.is_file(), f'OS image not found: {p}')
        return p
    patterns = [
        'out/ipsw/*/044-76040-038.dmg',
        '.cache/ipsw/*/044-76040-038.dmg',
        'out/ipsw/*/*.dmg',
        '.cache/ipsw/*/*.dmg',
    ]
    for base in (unixnu_root, device_root):
        for pat in patterns:
            hits = sorted(base.glob(pat))
            if hits:
                # Pick largest DMG if wildcard matched multiple.
                return max(hits, key=lambda h: h.stat().st_size)
    require(False, 'Fetched iPadOS OS .dmg not found (run unixnu ipsw fetch OS first)')


def hdiutil_attach_plist(args):
    res = subprocess.run(['hdiutil', 'attach', '-plist', *args], check=True, capture_output=True)
    return plistlib.loads(res.stdout)


def detach_best_effort(dev_node):
    if not dev_node:
        return
    subprocess.run(['hdiutil', 'detach', '-force', dev_node], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def merge_pref(mount, home, name, values):
    path = mount / home / 'Library/Preferences' / (name + '.plist')
    path.parent.mkdir(parents=True, exist_ok=True)
    content = plistlib.loads(path.read_bytes()) if path.is_file() else {}
    content.update(values)
    path.write_bytes(plistlib.dumps(content, fmt=plistlib.FMT_BINARY))
    path.chmod(0o644)


def populate_data_root(os_var, data_root):
    for current, dirs, files in os.walk(os_var, followlinks=False):
        dirs[:] = [name for name in dirs if name != 'staged_system_apps']
        dest = data_root / Path(current).relative_to(os_var)
        dest.mkdir(exist_ok=True)
        dest.chmod(stat.S_IMODE(Path(current).stat().st_mode))
        for name in list(dirs) + files:
            src, dst = Path(current) / name, dest / name
            if src.is_symlink():
                if not dst.exists() and not dst.is_symlink():
                    dst.symlink_to(os.readlink(src))
                if name in dirs:
                    dirs.remove(name)
            elif src.is_file():
                shutil.copy2(src, dst)

    runtime_dirs = [
        ('keybags', 0o755),
        ('Keychains', 0o755),
        ('mobile', 0o755),
        ('mobile/tmp', 0o755),
        ('mobile/Library', 0o755),
        ('mobile/Library/Preferences', 0o755),
        ('root', 0o755),
        ('tmp', 0o1777),
        ('run', 0o755),
        ('folders', 0o755),
        ('db', 0o755),
        ('log', 0o755),
        ('logs', 0o755),
        ('empty', 0o755),
        ('containers/Bundle', 0o755),
        ('containers/Data/System', 0o755),
        ('containers/Shared/SystemGroup', 0o755),
        ('containers/Temp', 0o755),
    ]
    for rel, mode in runtime_dirs:
        target = data_root / rel
        target.mkdir(parents=True, exist_ok=True)
        os.chmod(target, mode)

    for home in ('root', 'mobile'):
        for name in ('backboardd', '.GlobalPreferences'):
            merge_pref(data_root, home, name, {
                'CAEnableDeepFramebuffer': False,
                'CAEnableFramebufferCompression': False,
                'CAEnableIOSurfaceCompression': False,
            })


def update_dot_config_uuid(config_path, uuid_str):
    text = config_path.read_text()
    line = f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{uuid_str}"'
    if re.search(r'^CONFIG_XNU_OWNER_DATA_VOLUME_UUID=.*$', text, re.M):
        updated = re.sub(r'^CONFIG_XNU_OWNER_DATA_VOLUME_UUID=.*$', line, text, flags=re.M)
    else:
        updated = text.rstrip() + '\n' + line + '\n'
    config_path.write_text(updated)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', required=True, type=Path, help='Output DBRDISK1 .dbr file')
    ap.add_argument('--device', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--unixnu', type=Path, default=None)
    ap.add_argument('--os-dmg', type=Path, default=None)
    ap.add_argument('--size-mb', type=int, default=1024, help='Logical container size in MiB (<= 2048)')
    ap.add_argument('--config', type=str, default=None, help='Optional .config path (or "auto") to patch DATA_VOLUME_UUID')
    args = ap.parse_args()

    require(32 <= args.size_mb <= 2048, f'--size-mb must be 32..2048 for sparse RAM mode (got {args.size_mb})')
    unixnu_root = find_unixnu_root(args.unixnu)
    device_root = args.device.resolve()
    os_dmg = find_os_dmg(unixnu_root, device_root, args.os_dmg)

    sys.path.insert(0, str(unixnu_root / 'scripts'))
    sys.path.insert(0, str(unixnu_root / 'drivers/DarwinBoot/Test'))
    import packed_ramdisk
    from unixnu import runtime

    out_path = args.output.resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.unlink(missing_ok=True)

    os_dev = None
    raw_dev = None

    with tempfile.TemporaryDirectory(prefix='xun-data-seed-') as tmp_str:
        tmp = Path(tmp_str)
        os_mnt = tmp / 'os_mnt'
        data_mnt = tmp / 'data_mnt'
        os_mnt.mkdir()
        data_mnt.mkdir()
        raw_img = tmp / 'seed.cdr'

        try:
            os_plist = hdiutil_attach_plist([
                '-nobrowse', '-readonly', '-noverify', '-mountpoint', str(os_mnt), str(os_dmg),
            ])
            os_dev = next(
                e['dev-entry'] for e in os_plist.get('system-entities', [])
                if e.get('dev-entry', '').startswith('/dev/disk')
            )

            # Bare APFS container (-layout NONE -> NXSB at offset 0, matching seed-data-skip-setup.py).
            subprocess.run([
                'hdiutil', 'create', '-size', f'{args.size_mb}m', '-layout', 'NONE',
                '-fs', 'APFS', '-volname', 'Data', '-type', 'UDTO', str(raw_img),
            ], check=True)
            data_plist = hdiutil_attach_plist([
                '-nobrowse', '-owners', 'off', '-mountpoint', str(data_mnt), str(raw_img),
            ])
            raw_dev = data_plist['system-entities'][0]['dev-entry']

            src_var = os_mnt / 'private/var'
            require(src_var.is_dir(), f'Missing /private/var inside OS DMG {os_dmg}')
            populate_data_root(src_var, data_mnt)
            require((data_mnt / 'mobile').is_dir(), f'Data seed missing /mobile after copy into {data_mnt}')

            subprocess.run(['sync'], check=True)
            subprocess.run(['hdiutil', 'detach', raw_dev], check=True)
            raw_dev = None
        finally:
            detach_best_effort(raw_dev)
            detach_best_effort(os_dev)

        header = raw_img.read_bytes()[:4096]
        require(header[32:36] == b'NXSB', f'Expected bare APFS NXSB at offset 32 in {raw_img}')
        block_size = int.from_bytes(header[36:40], 'little')
        require(block_size == 4096, f'Expected 4096-byte APFS block size, got {block_size}')

        packed_ramdisk.pack(raw_img, out_path)

    vol_uuid = runtime.seed_uuid(unixnu_root, out_path)
    if args.config:
        if args.config == 'auto':
            from unixnu.cli import parser
            from unixnu.config import Context
            ctx = Context(parser().parse_args(['tree', '-D', str(device_root)]))
            cfg_path = ctx.config_path
        else:
            cfg_path = Path(args.config)
        require(cfg_path.is_file(), f'Missing .config: {cfg_path}')
        update_dot_config_uuid(cfg_path, vol_uuid)
        print(f'make-xun-data-seed: updated {cfg_path} -> {vol_uuid}')

    print(f'make-xun-data-seed: {out_path} ({out_path.stat().st_size} bytes, '
          f'{args.size_mb} MiB logical, Data UUID={vol_uuid})')


if __name__ == '__main__':
    main()
