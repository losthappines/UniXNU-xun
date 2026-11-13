#!/usr/bin/env python3
"""Archive no-op probe hooks (libLinkStubs.a) and verified-root stub (libVerifiedRootKey.a)."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def compile_archive(source_name, out_subdir, obj_name, archive_name):
    kit_src = ROOT / 'kits/xun' / source_name
    pkg_src = ROOT / 'package' / source_name
    source = kit_src if kit_src.exists() else pkg_src
    out = ROOT / out_subdir
    out.mkdir(parents=True, exist_ok=True)
    obj, archive = out / obj_name, out / archive_name
    subprocess.run([
        'xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64', '-mkernel',
        '-miphoneos-version-min=17.0', '-Os', '-Wall', '-Werror',
        '-c', source, '-o', obj,
    ], check=True)
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
    print(archive)


def main():
    compile_archive('LinkStubs.c', 'build/link-stubs', 'LinkStubs.o', 'libLinkStubs.a')
    compile_archive('VerifiedRootKeyStub.c', 'build/verified-root-key', 'VerifiedRootKey.o', 'libVerifiedRootKey.a')


if __name__ == '__main__':
    main()
