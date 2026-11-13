#!/usr/bin/env python3
"""Build XunEmmc (NativeUfs + NativeEmmcBlock : IOBlockStorageDevice) for UniXNU-xun.

Compiles drivers/XunEmmc/NativeEmmc.cpp against the prepared IOStorageFamily
headers, verifies NativeEmmcBlock's 190-slot vtable against the original
com.apple.iokit.IOStorageFamily ABI in the target iPadOS kernelcache, emits
original-bindings.S, and archives both:
  - build/xun-emmc/libXunEmmc.a
  - build/sheng-ufs/libShengUfs.a  (for upstream XNU_NATIVE_UFS=1 linkage)
"""
import shutil
import subprocess

from xun_storage_abi import (
    ROOT,
    build_compile_cmd,
    compile_bindings_object,
    ensure_storage_headers,
    load_cpo_record,
    resolve_storage_bindings,
    verify_and_collect_bindings,
)


def main():
    out = ROOT / 'build/xun-emmc'
    out.mkdir(parents=True, exist_ok=True)
    storage_include = ensure_storage_headers()
    record = load_cpo_record()
    audit_methods, possible = resolve_storage_bindings(record, out)

    source = ROOT / 'drivers/XunEmmc/NativeEmmc.cpp'
    obj = out / 'NativeEmmc.o'
    cmd = build_compile_cmd(
        record, source, obj,
        ['-I', str(storage_include), '-mgeneral-regs-only']
    )
    res = subprocess.run(cmd, cwd=record['directory'], capture_output=True, text=True)
    if res.returncode:
        raise SystemExit((res.stdout + res.stderr)[-6000:])

    bindings, overrides = verify_and_collect_bindings(obj, '15NativeEmmcBlock', audit_methods, possible)
    bind_obj = compile_bindings_object(bindings, out)

    archive = out / 'libXunEmmc.a'
    archive.unlink(missing_ok=True)
    members = [str(obj)] + ([str(bind_obj)] if bind_obj else [])
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *members], check=True)

    ufs_archive = ROOT / 'build/sheng-ufs/libShengUfs.a'
    ufs_archive.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, ufs_archive)

    print(f'build-xun-emmc: {archive} + {ufs_archive} '
          f'({overrides} NativeEmmcBlock overrides, {len(bindings)} IOStorageFamily bindings)')


if __name__ == '__main__':
    main()
