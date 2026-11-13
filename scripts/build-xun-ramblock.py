#!/usr/bin/env python3
"""Build NativeRamBlock (RamBlock : IOBlockStorageDevice) for UniXNU-xun CI/local builds.

Compiles drivers/RamBlock/NativeRamBlock.cpp against the prepared IOStorageFamily
headers, verifies NativeRamBlock's 190-slot vtable against the original
com.apple.iokit.IOStorageFamily ABI in the target iPadOS kernelcache without needing
QEMU gdb-stop.json, emits any IOBlockStorageDevice bindings not already provided by
build/sheng-ufs/libShengUfs.a, and archives build/ram-block/libRamBlock.a.
"""
import subprocess

from xun_storage_abi import (
    ROOT,
    build_compile_cmd,
    compile_bindings_object,
    ensure_storage_headers,
    load_cpo_record,
    read_defined_symbols,
    resolve_storage_bindings,
    verify_and_collect_bindings,
)


def main():
    out = ROOT / 'build/ram-block'
    out.mkdir(parents=True, exist_ok=True)
    storage_include = ensure_storage_headers()
    record = load_cpo_record()
    audit_methods, possible = resolve_storage_bindings(record, out)

    source = ROOT / 'drivers/RamBlock/NativeRamBlock.cpp'
    obj = out / 'NativeRamBlock.o'
    cmd = build_compile_cmd(
        record, source, obj,
        ['-I', str(storage_include), '-mgeneral-regs-only']
    )
    res = subprocess.run(cmd, cwd=record['directory'], capture_output=True, text=True)
    if res.returncode:
        raise SystemExit((res.stdout + res.stderr)[-6000:])

    bindings, overrides = verify_and_collect_bindings(obj, '14NativeRamBlock', audit_methods, possible)
    already_defined = read_defined_symbols(ROOT / 'build/sheng-ufs/libShengUfs.a')
    bindings = {k: v for k, v in bindings.items() if k not in already_defined}
    bind_obj = compile_bindings_object(bindings, out)

    archive = out / 'libRamBlock.a'
    archive.unlink(missing_ok=True)
    members = [str(obj)] + ([str(bind_obj)] if bind_obj else [])
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *members], check=True)

    print(f'build-xun-ramblock: {archive} '
          f'({overrides} NativeRamBlock overrides, {len(bindings)} extra IOStorageFamily bindings)')


if __name__ == '__main__':
    main()
